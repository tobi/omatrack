//! C ABI bridge exposing the upstream `motorsport-telemetry-rs` parser crates
//! to Omatrack.
//!
//! Design notes (bulk-first):
//! - `omatrack_decode_range` pulls a contiguous run of decoded samples across one
//!   chunk in a single FFI call; the C++ side caches whole channel arrays.
//! - `omatrack_sample_at` exists only for cheap per-cursor readouts.
//! - Strings returned by `omatrack_channel_name`/`omatrack_channel_unit` are owned by the
//!   handle and valid until `omatrack_close`.
//! - Errors are reported via `omatrack_last_error` (thread-local) and non-zero/handle
//!   return codes; no panics cross the FFI boundary.

use motorsport_telemetry_core::{read_source_metadata, TelemetrySource};
use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::fs::File;
use std::io::{self, Read};
use std::os::raw::{c_char, c_int, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};

const CACHE_NAMESPACE: &[u8] = b"omatrack-session-index-blake3-v1\0";
const FINGERPRINT_LIMIT: u64 = 1024 * 1024;

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap());
}

fn set_error(msg: impl Into<String>) {
    LAST_ERROR.with(|cell| {
        *cell.borrow_mut() = CString::new(msg.into()).unwrap_or_default();
    });
}

fn panic_message(payload: &(dyn std::any::Any + Send)) -> &str {
    payload
        .downcast_ref::<&str>()
        .copied()
        .or_else(|| payload.downcast_ref::<String>().map(String::as_str))
        .unwrap_or("unknown panic")
}

fn ffi_guard<T>(function: &str, failure: T, body: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(payload) => {
            set_error(format!("{function}: panic: {}", panic_message(&*payload)));
            failure
        }
    }
}

/// Opaque handle wrapping a parsed telemetry source plus owned C-string tables
/// for channel names/units.
pub struct BridgeFile {
    src: Box<dyn TelemetrySource>,
    names: Vec<CString>,
    units: Vec<CString>,
    source_laps: Vec<OmatrackSourceLap>,
    video_names: Vec<CString>,
    sidecar: SidecarTables,
}

struct SidecarTables {
    is_extension: u8,
    group_visible: u8,
    utc_start_ns: i64,
    duration_ns: u64,
    name: CString,
    timezone: CString,
    chrome: Vec<OwnedChrome>,
    spans: Vec<OwnedSpan>,
    channel_visible: Vec<u8>,
}

struct OwnedChrome {
    kind: u8,
    text: CString,
    label: CString,
    value: CString,
}

struct OwnedSpan {
    start_ns: u64,
    end_ns: u64,
    visible: u8,
    name: CString,
    title: CString,
    subtitle: CString,
    color: CString,
    meta: Vec<(CString, CString)>,
}

/// Stable C representation of one reliable format-neutral lap.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OmatrackSourceLap {
    pub number: i64,
    pub start_ns: u64,
    pub end_ns: u64,
    pub duration_ns: u64,
    pub first_video_frame: u64,
    pub complete: u8,
    pub has_first_video_frame: u8,
}

/// One video file linked to the open telemetry recording.
///
/// `filename` and the optional 32-byte `blake3` digest are owned by the
/// handle and remain valid until `omatrack_close`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct OmatrackVideoFileRef {
    pub filename: *const c_char,
    pub index: u32,
    pub blake3: *const u8,
    pub frame_count: u64,
    pub presentation_offset_ns: i64,
    pub has_presentation_offset: u8,
}

/// All video linkage resolved for one file-relative telemetry timestamp.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct OmatrackVideoReference {
    pub file_index: u32,
    pub sync_time: f64,
    pub presentation_time_ns: u64,
    pub frame_index: u64,
    pub has_file_index: u8,
    pub has_sync_time: u8,
    pub has_presentation_time: u8,
    pub has_frame_index: u8,
}

/// MTX / catalog placement for an open handle.
///
/// `utc_start_ns` is `-1` when the source has no Unix-epoch stamp at `t = 0`.
/// String pointers are owned by the handle and stay valid until
/// `omatrack_close`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct OmatrackSidecarInfo {
    pub is_extension: u8,
    pub group_visible: u8,
    pub utc_start_ns: i64,
    pub duration_ns: u64,
    pub name: *const c_char,
    pub timezone: *const c_char,
}

/// One right-aligned header chrome element.
///
/// `kind` is `0` for description text and `1` for a fact pill. String
/// pointers are owned by the handle.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct OmatrackSidecarChrome {
    pub kind: u8,
    pub text: *const c_char,
    pub label: *const c_char,
    pub value: *const c_char,
}

/// One MTX span on the sidecar's file-relative timeline.
///
/// String pointers are owned by the handle. Hover fields come from
/// `omatrack_span_meta`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct OmatrackSpan {
    pub start_ns: u64,
    pub end_ns: u64,
    pub visible: u8,
    pub name: *const c_char,
    pub title: *const c_char,
    pub subtitle: *const c_char,
    pub color: *const c_char,
    pub meta_count: usize,
}

fn as_file<'a>(handle: *mut c_void) -> Option<&'a BridgeFile> {
    if handle.is_null() {
        None
    } else {
        Some(unsafe { &*(handle as *const BridgeFile) })
    }
}

fn parse_path(open_path: &Path, index_only: bool) -> Result<Box<dyn TelemetrySource>, String> {
    if telemetry_format::is_jsonl_path(open_path) {
        return telemetry_format::JsonlRecording::open(open_path)
            .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
            .map_err(|error| error.to_string());
    }
    let ext = open_path
        .extension()
        .and_then(|value| value.to_str())
        .map(|value| value.to_ascii_lowercase())
        .unwrap_or_default();
    match ext.as_str() {
        "mp4" => {
            let source = if index_only {
                aim_telemetry::AimFile::open_index(open_path)
            } else {
                aim_telemetry::AimFile::open(open_path)
            };
            source
                .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
                .map_err(|error| error.to_string())
        }
        "pds" => cosworth_telemetry::CosworthFile::open(open_path)
            .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
            .map_err(|error| error.to_string()),
        "ld" => motec_telemetry::MotecFile::open(open_path)
            .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
            .map_err(|error| error.to_string()),
        "vbo" => {
            let source = if index_only {
                racelogic_telemetry::RacelogicFile::open_metadata(open_path)
            } else {
                racelogic_telemetry::RacelogicFile::open(open_path)
            };
            source
                .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
                .map_err(|error| error.to_string())
        }
        "telemetry" => telemetry_format::NativeRecording::open(open_path)
            .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
            .map_err(|error| error.to_string()),
        other => Err(format!("unsupported telemetry format: {other:?}")),
    }
}

fn canonical_or_absolute(path: &Path) -> io::Result<PathBuf> {
    match path.canonicalize() {
        Ok(path) => Ok(path),
        Err(_) if path.is_absolute() => Ok(path.to_path_buf()),
        Err(_) => Ok(std::env::current_dir()?.join(path)),
    }
}

fn add_file_fingerprint(hasher: &mut blake3::Hasher, path: &Path) -> io::Result<()> {
    let canonical = canonical_or_absolute(path)?;
    let mut file = File::open(&canonical)?;
    let size = file.metadata()?.len();
    hasher.update(canonical.as_os_str().as_encoded_bytes());
    hasher.update(b"\0");
    hasher.update(size.to_string().as_bytes());
    hasher.update(b"\0");

    let mut buffer = [0u8; 64 * 1024];
    let mut remaining = FINGERPRINT_LIMIT;
    while remaining > 0 {
        let read_limit = remaining.min(buffer.len() as u64) as usize;
        let count = file.read(&mut buffer[..read_limit])?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
        remaining -= count as u64;
    }
    Ok(())
}

fn fingerprint(path: &Path) -> io::Result<blake3::Hash> {
    let canonical = canonical_or_absolute(path)?;
    let mut hasher = blake3::Hasher::new();
    hasher.update(CACHE_NAMESPACE);
    hasher.update(b"primary\0");
    add_file_fingerprint(&mut hasher, &canonical)?;

    if canonical
        .extension()
        .and_then(|extension| extension.to_str())
        .is_some_and(|extension| extension.eq_ignore_ascii_case("ld"))
    {
        hasher.update(b"sidecar\0");
        if add_file_fingerprint(&mut hasher, &canonical.with_extension("ldx")).is_err() {
            hasher.update(b"missing\0");
        }
    }
    Ok(hasher.finalize())
}

fn build_handle(src: Box<dyn TelemetrySource>) -> Box<BridgeFile> {
    let names = src
        .channels()
        .iter()
        .map(|c| CString::new(c.name.as_str()).unwrap_or_default())
        .collect::<Vec<_>>();
    let units = src
        .channels()
        .iter()
        .map(|c| CString::new(c.unit.as_str()).unwrap_or_default())
        .collect::<Vec<_>>();
    let video_names = src
        .video_files()
        .iter()
        .map(|video| CString::new(video.filename.as_str()).unwrap_or_default())
        .collect::<Vec<_>>();
    // Laps always come from upstream's `read_source_metadata`, for every
    // format and both open modes. For a `.telemetry` that is the catalog's
    // authoritative list; for a vendor file it is the same list the
    // converter will write into that catalog. Forwarding only authoritative
    // laps here let a vendor file opened directly fall back to a second lap
    // heuristic on the C++ side, so the CLI on an MP4 and the GUI on its
    // `.telemetry` named different laps as fastest.
    let source_laps = read_source_metadata(src.as_ref())
        .laps
        .into_iter()
        .map(|lap| OmatrackSourceLap {
            number: lap.number,
            start_ns: lap.start_ns,
            end_ns: lap.end_ns,
            duration_ns: lap.duration_ns,
            first_video_frame: lap.first_video_frame.unwrap_or(0),
            complete: u8::from(lap.complete),
            has_first_video_frame: u8::from(lap.first_video_frame.is_some()),
        })
        .collect();
    let sidecar = if telemetry_format::is_jsonl_path(std::path::Path::new(src.path())) {
        sidecar_tables(src.as_ref())
    } else {
        SidecarTables {
            utc_start_ns: src
                .utc_start_ns()
                .and_then(|value| i64::try_from(value).ok())
                .unwrap_or(-1),
            timezone: cstring(&src.timezone()),
            duration_ns: src
                .channels()
                .iter()
                .map(|channel| channel.duration_ns)
                .max()
                .unwrap_or(0),
            ..SidecarTables::default()
        }
    };
    Box::new(BridgeFile {
        src,
        names,
        units,
        source_laps,
        video_names,
        sidecar,
    })
}

impl Default for SidecarTables {
    fn default() -> Self {
        Self {
            is_extension: 0,
            group_visible: 1,
            utc_start_ns: -1,
            duration_ns: 0,
            name: CString::default(),
            timezone: CString::default(),
            chrome: Vec::new(),
            spans: Vec::new(),
            channel_visible: Vec::new(),
        }
    }
}

fn cstring(value: &str) -> CString {
    CString::new(value).unwrap_or_default()
}

fn sidecar_tables(src: &dyn TelemetrySource) -> SidecarTables {
    let utc = src
        .utc_start_ns()
        .and_then(|value| i64::try_from(value).ok())
        .unwrap_or(-1);
    let mut tables = SidecarTables {
        utc_start_ns: utc,
        timezone: cstring(&src.timezone()),
        duration_ns: src
            .channels()
            .iter()
            .map(|channel| channel.duration_ns)
            .max()
            .unwrap_or(0),
        channel_visible: vec![1; src.channels().len()],
        ..SidecarTables::default()
    };
    let Ok(jsonl) = telemetry_format::JsonlRecording::open(src.path()) else {
        return tables;
    };
    tables.duration_ns = jsonl.duration_ns();
    tables.is_extension = u8::from(jsonl.is_extension());
    if let Some(group) = jsonl.sidecar_groups().first() {
        let header = &group.header;
        tables.name = cstring(&header.name);
        tables.group_visible = u8::from(header.visible);
        tables.timezone = cstring(&header.timezone);
        tables.utc_start_ns = i64::try_from(header.utc_start_ns).unwrap_or(-1);
        tables.duration_ns = group.duration_ns;
        tables.chrome = header
            .right
            .iter()
            .map(|chrome| match chrome {
                telemetry_format::HeaderChrome::Text(text) => OwnedChrome {
                    kind: 0,
                    text: cstring(text),
                    label: CString::default(),
                    value: CString::default(),
                },
                telemetry_format::HeaderChrome::Pill { label, value } => OwnedChrome {
                    kind: 1,
                    text: CString::default(),
                    label: cstring(label),
                    value: cstring(value),
                },
            })
            .collect();
    }
    tables.channel_visible = jsonl
        .channel_visible()
        .iter()
        .map(|visible| u8::from(*visible))
        .collect();
    if tables.channel_visible.len() < src.channels().len() {
        tables.channel_visible.resize(src.channels().len(), 1);
    }
    tables.spans = jsonl
        .spans()
        .iter()
        .map(|span| OwnedSpan {
            start_ns: span.start_ns,
            end_ns: span.end_ns,
            visible: u8::from(span.visible),
            name: cstring(&span.name),
            title: cstring(&span.primary.title),
            subtitle: cstring(&span.primary.subtitle),
            color: cstring(&span.color),
            meta: span
                .meta
                .iter()
                .map(|(key, value)| (cstring(key), cstring(&value.display())))
                .collect(),
        })
        .collect();
    tables
}

// ── handling ─────────────────────────────────────────────────────────

/// Open a telemetry file by path (format dispatched from extension).
/// Returns a handle, or NULL on error (see `omatrack_last_error`).
///
/// # Safety
/// `path` must be NULL or a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn omatrack_open(path: *const c_char) -> *mut c_void {
    open_bridge(path, false, stringify!(omatrack_open))
}

/// Open a telemetry file for a bounded library-index summary.
///
/// AiM MP4 files retain channel schema, complete lap counters/timers, and
/// representative samples while omitting the video-frame index. Other formats
/// use their normal lightweight parser path.
///
/// # Safety
/// `path` must be NULL or a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn omatrack_open_index(path: *const c_char) -> *mut c_void {
    open_bridge(path, true, stringify!(omatrack_open_index))
}

unsafe fn open_bridge(path: *const c_char, index_only: bool, function: &str) -> *mut c_void {
    ffi_guard(function, std::ptr::null_mut(), || {
        if path.is_null() {
            set_error(format!("{function}: null path"));
            return std::ptr::null_mut();
        }
        let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(s) => s,
            Err(_) => {
                set_error(format!("{function}: path is not valid UTF-8"));
                return std::ptr::null_mut();
            }
        };
        match parse_path(Path::new(path_str), index_only) {
            Ok(src) => Box::into_raw(build_handle(src)) as *mut c_void,
            Err(e) => {
                set_error(format!("{function}: {e}"));
                std::ptr::null_mut()
            }
        }
    })
}

/// Compute Omatrack's BLAKE3 cache fingerprint into a 65-byte hex buffer.
/// Returns 1 on success and 0 on invalid input or an I/O error.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string. `output` must point to at
/// least `output_len` writable bytes; 65 bytes are required.
#[no_mangle]
pub unsafe extern "C" fn omatrack_fingerprint(
    path: *const c_char,
    output: *mut c_char,
    output_len: usize,
) -> c_int {
    ffi_guard(stringify!(omatrack_fingerprint), 0, || {
        if path.is_null() || output.is_null() || output_len < 65 {
            set_error("omatrack_fingerprint: invalid path or output buffer");
            return 0;
        }
        let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(path) => path,
            Err(_) => {
                set_error("omatrack_fingerprint: path is not valid UTF-8");
                return 0;
            }
        };
        let hash = match fingerprint(Path::new(path_str)) {
            Ok(hash) => hash,
            Err(error) => {
                set_error(format!("omatrack_fingerprint: {error}"));
                return 0;
            }
        };
        let hex = hash.to_hex();
        unsafe {
            std::ptr::copy_nonoverlapping(hex.as_bytes().as_ptr(), output.cast::<u8>(), 64);
            *output.add(64) = 0;
        }
        1
    })
}

/// Compute the full-file BLAKE3-256 digest used by `VideoFileRef`.
///
/// # Safety
/// `path` must be a valid NUL-terminated C string. `output` must point to at
/// least 32 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn omatrack_blake3_file(path: *const c_char, output: *mut u8) -> c_int {
    ffi_guard(stringify!(omatrack_blake3_file), 0, || {
        if path.is_null() || output.is_null() {
            set_error("omatrack_blake3_file: invalid path or output buffer");
            return 0;
        }
        let path_str = match CStr::from_ptr(path).to_str() {
            Ok(path) => path,
            Err(_) => {
                set_error("omatrack_blake3_file: path is not valid UTF-8");
                return 0;
            }
        };
        let mut file = match File::open(path_str) {
            Ok(file) => file,
            Err(error) => {
                set_error(format!("omatrack_blake3_file: {error}"));
                return 0;
            }
        };
        let mut hasher = blake3::Hasher::new();
        let mut buffer = [0u8; 1 << 16];
        loop {
            let count = match file.read(&mut buffer) {
                Ok(count) => count,
                Err(error) => {
                    set_error(format!("omatrack_blake3_file: {error}"));
                    return 0;
                }
            };
            if count == 0 {
                break;
            }
            hasher.update(&buffer[..count]);
        }
        std::ptr::copy_nonoverlapping(hasher.finalize().as_bytes().as_ptr(), output, 32);
        1
    })
}

/// Free a handle returned by `omatrack_open`. Safe to call with NULL.
///
/// # Safety
/// `handle` must be NULL or a handle from `omatrack_open` that has not been closed.
#[no_mangle]
pub unsafe extern "C" fn omatrack_close(handle: *mut c_void) {
    ffi_guard(stringify!(omatrack_close), (), || {
        if !handle.is_null() {
            drop(Box::from_raw(handle as *mut BridgeFile));
        }
    })
}

/// Thread-local error string from the last failing call. The pointer is valid
/// only on the calling thread and until that thread's next failing bridge call.
#[no_mangle]
pub extern "C" fn omatrack_last_error() -> *const c_char {
    ffi_guard(stringify!(omatrack_last_error), std::ptr::null(), || {
        LAST_ERROR.with(|cell| cell.borrow().as_ptr())
    })
}

// ── metadata ─────────────────────────────────────────────────────────

/// Return the source format string.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_format(handle: *mut c_void) -> *const c_char {
    ffi_guard(
        stringify!(omatrack_format),
        std::ptr::null(),
        || match as_file(handle) {
            Some(f) => {
                static AIMD: &[u8] = b"aimd\0";
                static PDS: &[u8] = b"pds\0";
                static LD: &[u8] = b"ld\0";
                static VBO: &[u8] = b"vbo\0";
                static TELEMETRY: &[u8] = b"telemetry\0";
                static JSONL: &[u8] = b"jsonl\0";
                static MTX: &[u8] = b"mtx\0";
                let fmt = f.src.format();
                let bytes: &[u8] = match fmt {
                    "aimd" => AIMD,
                    "pds" => PDS,
                    "ld" | "motec" => LD,
                    "vbo" => VBO,
                    "telemetry" => TELEMETRY,
                    "jsonl" if f.sidecar.is_extension != 0 => MTX,
                    "jsonl" => JSONL,
                    _ if f.sidecar.is_extension != 0 => MTX,
                    _ => b"unknown\0",
                };
                bytes.as_ptr() as *const c_char
            }
            None => std::ptr::null(),
        },
    )
}

/// Return the number of reliable format-neutral laps.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_source_lap_count(handle: *mut c_void) -> usize {
    ffi_guard(stringify!(omatrack_source_lap_count), 0, || {
        as_file(handle)
            .map(|file| file.source_laps.len())
            .unwrap_or(0)
    })
}

/// Copy one reliable format-neutral lap into `out`.
/// Returns 1 on success or 0 for a null pointer or out-of-range index.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must point
/// to writable `OmatrackSourceLap` storage.
#[no_mangle]
pub unsafe extern "C" fn omatrack_source_lap(
    handle: *mut c_void,
    index: usize,
    out: *mut OmatrackSourceLap,
) -> std::os::raw::c_int {
    ffi_guard(stringify!(omatrack_source_lap), Default::default(), || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        let Some(lap) = file.source_laps.get(index) else {
            return 0;
        };
        *out = *lap;
        1
    })
}

/// Write the offset satisfying
/// `video_presentation_ns = telemetry_file_relative_ns + offset`.
/// Returns 1 when the source supplies an offset and it fits in an `i64`, or 0
/// when unavailable/invalid.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must point
/// to a writable `i64`.
#[no_mangle]
pub unsafe extern "C" fn omatrack_video_presentation_offset_ns(
    handle: *mut c_void,
    out: *mut i64,
) -> std::os::raw::c_int {
    ffi_guard(stringify!(omatrack_video_presentation_offset_ns), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        let Some(offset) = file.src.video_presentation_offset_ns() else {
            return 0;
        };
        let Ok(offset) = i64::try_from(offset) else {
            set_error("omatrack_video_presentation_offset_ns: offset exceeds i64");
            return 0;
        };
        *out = offset;
        1
    })
}

/// Return the number of channels.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_channel_count(handle: *mut c_void) -> usize {
    ffi_guard(
        stringify!(omatrack_channel_count),
        Default::default(),
        || as_file(handle).map(|f| f.src.channels().len()).unwrap_or(0),
    )
}

/// Return a channel name owned by the handle.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_channel_name(handle: *mut c_void, index: usize) -> *const c_char {
    ffi_guard(
        stringify!(omatrack_channel_name),
        std::ptr::null(),
        || match as_file(handle) {
            Some(f) => f
                .names
                .get(index)
                .map(|c| c.as_ptr())
                .unwrap_or(std::ptr::null()),
            None => std::ptr::null(),
        },
    )
}

/// Return a channel unit owned by the handle.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_channel_unit(handle: *mut c_void, index: usize) -> *const c_char {
    ffi_guard(
        stringify!(omatrack_channel_unit),
        std::ptr::null(),
        || match as_file(handle) {
            Some(f) => f
                .units
                .get(index)
                .map(|c| c.as_ptr())
                .unwrap_or(std::ptr::null()),
            None => std::ptr::null(),
        },
    )
}

/// Return a channel sample-type code.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_channel_type_code(handle: *mut c_void, index: usize) -> u32 {
    ffi_guard(
        stringify!(omatrack_channel_type_code),
        Default::default(),
        || match as_file(handle) {
            Some(f) => f
                .src
                .channels()
                .get(index)
                .map(|c| c.sample_type.code())
                .unwrap_or(0),
            None => 0,
        },
    )
}

/// Return a channel duration.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_channel_duration_ns(handle: *mut c_void, index: usize) -> u64 {
    ffi_guard(
        stringify!(omatrack_channel_duration_ns),
        Default::default(),
        || match as_file(handle) {
            Some(f) => f
                .src
                .channels()
                .get(index)
                .map(|c| c.duration_ns)
                .unwrap_or(0),
            None => 0,
        },
    )
}

/// Return a channel's total sample count.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_channel_sample_count(handle: *mut c_void, index: usize) -> u64 {
    ffi_guard(
        stringify!(omatrack_channel_sample_count),
        Default::default(),
        || match as_file(handle) {
            Some(f) => f
                .src
                .channels()
                .get(index)
                .map(|c| c.sample_count)
                .unwrap_or(0),
            None => 0,
        },
    )
}

/// Return a channel's chunk count.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_channel_chunk_count(handle: *mut c_void, index: usize) -> usize {
    ffi_guard(
        stringify!(omatrack_channel_chunk_count),
        Default::default(),
        || match as_file(handle) {
            Some(f) => f
                .src
                .channels()
                .get(index)
                .map(|c| c.chunks.len())
                .unwrap_or(0),
            None => 0,
        },
    )
}

/// Return a chunk's sample period.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_chunk_period_ns(
    handle: *mut c_void,
    index: usize,
    chunk: usize,
) -> u64 {
    ffi_guard(
        stringify!(omatrack_chunk_period_ns),
        Default::default(),
        || match as_file(handle) {
            Some(f) => f
                .src
                .channels()
                .get(index)
                .and_then(|c| c.chunks.get(chunk))
                .map(|c| c.sample_period_ns)
                .unwrap_or(0),
            None => 0,
        },
    )
}

/// Return a chunk's source time base.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_chunk_time_base_ns(
    handle: *mut c_void,
    index: usize,
    chunk: usize,
) -> u64 {
    ffi_guard(
        stringify!(omatrack_chunk_time_base_ns),
        Default::default(),
        || match as_file(handle) {
            Some(f) => f
                .src
                .channels()
                .get(index)
                .and_then(|c| c.chunks.get(chunk))
                .map(|c| c.time_base_ns)
                .unwrap_or(0),
            None => 0,
        },
    )
}

/// Return a chunk's sample count.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_chunk_sample_count(
    handle: *mut c_void,
    index: usize,
    chunk: usize,
) -> u64 {
    ffi_guard(
        stringify!(omatrack_chunk_sample_count),
        Default::default(),
        || match as_file(handle) {
            Some(f) => f
                .src
                .channels()
                .get(index)
                .and_then(|c| c.chunks.get(chunk))
                .map(|c| c.sample_count)
                .unwrap_or(0),
            None => 0,
        },
    )
}

// ── bulk sample access ───────────────────────────────────────────────

/// Decode `[start, start+n)` samples of one chunk into `out`.
/// Returns the number of samples actually written (may be less than `n` when
/// the chunk ends early). `out` must have room for `n` doubles.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must point to
/// `n` writable doubles.
#[no_mangle]
pub unsafe extern "C" fn omatrack_decode_range(
    handle: *mut c_void,
    index: usize,
    chunk: usize,
    start: u64,
    n: usize,
    out: *mut f64,
) -> usize {
    ffi_guard(
        stringify!(omatrack_decode_range),
        Default::default(),
        || {
            let Some(file) = as_file(handle) else {
                return 0;
            };
            if out.is_null() {
                return 0;
            }
            let Some(channel) = file.src.channels().get(index) else {
                return 0;
            };
            let Some(c) = channel.chunks.get(chunk) else {
                return 0;
            };
            if start >= c.sample_count {
                return 0;
            }
            let count = n.min((c.sample_count - start) as usize);
            let slice = std::slice::from_raw_parts_mut(out, count);
            for (i, slot) in slice.iter_mut().enumerate() {
                *slot = file.src.decode(index, chunk, start + i as u64);
            }
            count
        },
    )
}

/// Decode one logical multichunk channel into a single contiguous buffer.
/// Samples are emitted in chunk order (which is also time order). Returns the
/// total number of samples written; `out` must have room for
/// `omatrack_channel_sample_count` doubles.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must point to
/// `capacity` writable doubles.
#[no_mangle]
pub unsafe extern "C" fn omatrack_channel_decode_all(
    handle: *mut c_void,
    index: usize,
    out: *mut f64,
    capacity: usize,
) -> usize {
    ffi_guard(
        stringify!(omatrack_channel_decode_all),
        Default::default(),
        || {
            let Some(file) = as_file(handle) else {
                return 0;
            };
            if out.is_null() {
                return 0;
            }
            let Some(channel) = file.src.channels().get(index) else {
                return 0;
            };
            let mut written = 0usize;
            let mut slot: *mut f64 = out;
            for (chunk_idx, chunk) in channel.chunks.iter().enumerate() {
                if written >= capacity {
                    break;
                }
                let count = (chunk.sample_count as usize).min(capacity - written);
                let slice = std::slice::from_raw_parts_mut(slot, count);
                for (i, s) in slice.iter_mut().enumerate() {
                    *s = file.src.decode(index, chunk_idx, i as u64);
                }
                written += count;
                slot = slot.add(count);
            }
            written
        },
    )
}

/// Sample a channel at an absolute time in ns with linear interpolation.
/// Writes the value to `out` and returns 1 on success, 0 when out of range.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must point to a
/// writable double.
#[no_mangle]
pub unsafe extern "C" fn omatrack_sample_at(
    handle: *mut c_void,
    index: usize,
    time_ns: u64,
    linear: bool,
    out: *mut f64,
) -> std::os::raw::c_int {
    ffi_guard(stringify!(omatrack_sample_at), Default::default(), || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        let Some(_) = file.src.channels().get(index) else {
            return 0;
        };
        match file.src.sample_at(index, time_ns, linear) {
            Some(v) => {
                *out = v;
                1
            }
            None => 0,
        }
    })
}

/// Return one sample's absolute source time.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_sample_time_ns(
    handle: *mut c_void,
    index: usize,
    chunk: usize,
    local: u64,
) -> u64 {
    ffi_guard(
        stringify!(omatrack_sample_time_ns),
        Default::default(),
        || {
            let Some(file) = as_file(handle) else {
                return 0;
            };
            let Some(channel) = file.src.channels().get(index) else {
                return 0;
            };
            let Some(chunk_meta) = channel.chunks.get(chunk) else {
                return 0;
            };
            if local >= chunk_meta.sample_count {
                return 0;
            }
            file.src.sample_time_ns(index, chunk, local)
        },
    )
}

struct LinkedVideoSource<'a> {
    inner: &'a dyn TelemetrySource,
    videos: Vec<motorsport_telemetry_core::VideoFileRef>,
}

impl TelemetrySource for LinkedVideoSource<'_> {
    fn path(&self) -> &str {
        self.inner.path()
    }
    fn format(&self) -> &'static str {
        self.inner.format()
    }
    fn channels(&self) -> &[motorsport_telemetry_core::Channel] {
        self.inner.channels()
    }
    fn decode(&self, channel: usize, chunk: usize, sample: u64) -> f64 {
        self.inner.decode(channel, chunk, sample)
    }
    fn chunk_bytes(&self, channel: usize, chunk: usize) -> Option<&[u8]> {
        self.inner.chunk_bytes(channel, chunk)
    }
    fn sample_affine(&self, channel: usize) -> (f64, f64) {
        self.inner.sample_affine(channel)
    }
    fn absolute_time_range(&self) -> Option<motorsport_telemetry_core::AbsoluteTimeRange> {
        self.inner.absolute_time_range()
    }
    fn utc_start_ns(&self) -> Option<u64> {
        self.inner.utc_start_ns()
    }
    fn timezone(&self) -> String {
        self.inner.timezone()
    }
    fn channel_visible(&self) -> &[bool] {
        self.inner.channel_visible()
    }
    fn spans(&self) -> &[motorsport_telemetry_core::Span] {
        self.inner.spans()
    }
    fn channel_labels(&self, channel: usize) -> &[motorsport_telemetry_core::ChannelLabel] {
        self.inner.channel_labels(channel)
    }
    fn channel_display(&self, channel: usize) -> motorsport_telemetry_core::ChannelDisplay {
        self.inner.channel_display(channel)
    }
    fn applied_passes(&self) -> &[motorsport_telemetry_core::AppliedPass] {
        self.inner.applied_passes()
    }
    fn source_origin(&self) -> Option<motorsport_telemetry_core::SourceOrigin> {
        self.inner.source_origin()
    }
    fn identity(&self) -> motorsport_telemetry_core::SourceIdentity {
        self.inner.identity()
    }
    fn source_lap_metadata(&self) -> Option<motorsport_telemetry_core::SourceLapMetadata> {
        self.inner.source_lap_metadata()
    }
    fn video_files(&self) -> &[motorsport_telemetry_core::VideoFileRef] {
        &self.videos
    }
    fn video_presentation_times_ns(&self) -> Option<&[u64]> {
        self.inner.video_presentation_times_ns()
    }
    fn video_frame_count(&self) -> Option<u64> {
        self.inner.video_frame_count()
    }
    fn video_frame_at(&self, time_ns: u64) -> Option<u64> {
        self.inner.video_frame_at(time_ns)
    }
    fn video_presentation_offset_ns(&self) -> Option<i128> {
        self.inner.video_presentation_offset_ns()
    }
    fn video_presentation_time_ns(&self, time_ns: u64) -> Option<u64> {
        self.inner.video_presentation_time_ns(time_ns)
    }
    fn video_reference_at(&self, time_ns: u64) -> motorsport_telemetry_core::VideoReference {
        self.inner.video_reference_at(time_ns)
    }
    fn sample_time_ns(&self, channel: usize, chunk: usize, sample: u64) -> u64 {
        self.inner.sample_time_ns(channel, chunk, sample)
    }
    fn sample_at(&self, channel: usize, time_ns: u64, linear: bool) -> Option<f64> {
        self.inner.sample_at(channel, time_ns, linear)
    }
    fn diagnostics(&self) -> &[motorsport_telemetry_core::Diagnostic] {
        self.inner.diagnostics()
    }
    fn sample_times(&self, channel: usize) -> motorsport_telemetry_core::SampleTimes<'_> {
        self.inner.sample_times(channel)
    }
}

/// Serialise an open handle to a native `.telemetry` recording.
/// Returns 1 on success, 0 on error (`omatrack_last_error`).
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle. `path` must be
/// NULL or a valid NUL-terminated UTF-8 path.
#[no_mangle]
pub unsafe extern "C" fn omatrack_write_telemetry(
    handle: *mut c_void,
    path: *const c_char,
) -> c_int {
    ffi_guard(stringify!(omatrack_write_telemetry), 0, || {
        let Some(file) = as_file(handle) else {
            set_error("omatrack_write_telemetry: null handle");
            return 0;
        };
        if path.is_null() {
            set_error("omatrack_write_telemetry: null path");
            return 0;
        }
        let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(path) => path,
            Err(_) => {
                set_error("omatrack_write_telemetry: path is not valid UTF-8");
                return 0;
            }
        };
        match telemetry_format::write_from_source(file.src.as_ref(), path_str) {
            Ok(()) => 1,
            Err(error) => {
                set_error(format!("omatrack_write_telemetry: {error}"));
                0
            }
        }
    })
}

/// Serialise an open single-video source while linking the recording to the
/// caller-supplied video basename. This is for cache-private remote extracts:
/// the extract is not the video and must never be hashed as one.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle. `path` and
/// `video_filename` must be valid NUL-terminated UTF-8 strings.
#[no_mangle]
pub unsafe extern "C" fn omatrack_write_telemetry_for_video(
    handle: *mut c_void,
    path: *const c_char,
    video_filename: *const c_char,
) -> c_int {
    ffi_guard(stringify!(omatrack_write_telemetry_for_video), 0, || {
        let Some(file) = as_file(handle) else {
            set_error("omatrack_write_telemetry_for_video: null handle");
            return 0;
        };
        if path.is_null() || video_filename.is_null() {
            set_error("omatrack_write_telemetry_for_video: null path");
            return 0;
        }
        let path_str = match CStr::from_ptr(path).to_str() {
            Ok(path) => path,
            Err(_) => {
                set_error("omatrack_write_telemetry_for_video: path is not valid UTF-8");
                return 0;
            }
        };
        let filename = match CStr::from_ptr(video_filename).to_str() {
            Ok(filename) if !filename.is_empty() => filename,
            _ => {
                set_error("omatrack_write_telemetry_for_video: invalid video filename");
                return 0;
            }
        };
        let mut videos = file.src.video_files().to_vec();
        if videos.len() > 1 {
            set_error("omatrack_write_telemetry_for_video: source links multiple videos");
            return 0;
        }
        if let Some(video) = videos.first_mut() {
            video.filename = filename.to_owned();
            video.blake3 = None;
        } else {
            videos.push(motorsport_telemetry_core::VideoFileRef {
                filename: filename.to_owned(),
                index: 1,
                blake3: None,
                frame_count: file.src.video_frame_count().unwrap_or(0),
                presentation_offset_ns: file.src.video_presentation_offset_ns(),
            });
        }
        let source = LinkedVideoSource {
            inner: file.src.as_ref(),
            videos,
        };
        match telemetry_format::write_from_source(&source, path_str) {
            Ok(()) => 1,
            Err(error) => {
                set_error(format!("omatrack_write_telemetry_for_video: {error}"));
                0
            }
        }
    })
}

struct UnsupportedSource {
    path: String,
}

impl TelemetrySource for UnsupportedSource {
    fn path(&self) -> &str {
        &self.path
    }
    fn format(&self) -> &'static str {
        "unsupported"
    }
    fn channels(&self) -> &[motorsport_telemetry_core::Channel] {
        &[]
    }
    fn decode(&self, _: usize, _: usize, _: u64) -> f64 {
        0.0
    }
}

/// Writes a catalog-only `.telemetry` that means "this video has no telemetry".
///
/// # Safety
/// `path` must be NULL or a valid NUL-terminated UTF-8 path.
#[no_mangle]
pub unsafe extern "C" fn omatrack_write_unsupported_telemetry(path: *const c_char) -> c_int {
    ffi_guard(stringify!(omatrack_write_unsupported_telemetry), 0, || {
        if path.is_null() {
            set_error("omatrack_write_unsupported_telemetry: null path");
            return 0;
        }
        let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(path) => path,
            Err(_) => {
                set_error("omatrack_write_unsupported_telemetry: path is not valid UTF-8");
                return 0;
            }
        };
        let source = UnsupportedSource {
            path: path_str.to_owned(),
        };
        match telemetry_format::write_from_source(&source, path_str) {
            Ok(()) => 1,
            Err(error) => {
                set_error(format!("omatrack_write_unsupported_telemetry: {error}"));
                0
            }
        }
    })
}

/// Maps file-relative telemetry time to a presentation-order video frame.
/// Returns 1 when the source knows the frame.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must
/// point to a writable `u64`.
#[no_mangle]
pub unsafe extern "C" fn omatrack_video_frame_at(
    handle: *mut c_void,
    time_ns: u64,
    out: *mut u64,
) -> c_int {
    ffi_guard(stringify!(omatrack_video_frame_at), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        let Some(frame) = file.src.video_frame_at(time_ns) else {
            return 0;
        };
        *out = frame;
        1
    })
}

/// Maps file-relative telemetry time to the video's presentation timeline.
/// Returns 1 when the source supplies an exact mapping.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must
/// point to a writable `u64`.
#[no_mangle]
pub unsafe extern "C" fn omatrack_video_presentation_time_ns(
    handle: *mut c_void,
    time_ns: u64,
    out: *mut u64,
) -> c_int {
    ffi_guard(stringify!(omatrack_video_presentation_time_ns), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        let Some(time) = file.src.video_presentation_time_ns(time_ns) else {
            return 0;
        };
        *out = time;
        1
    })
}

/// Returns all exact video linkage at a file-relative telemetry timestamp.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must
/// point to writable `OmatrackVideoReference` storage.
#[no_mangle]
pub unsafe extern "C" fn omatrack_video_reference_at(
    handle: *mut c_void,
    time_ns: u64,
    out: *mut OmatrackVideoReference,
) -> c_int {
    ffi_guard(stringify!(omatrack_video_reference_at), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        let reference = file.src.video_reference_at(time_ns);
        if reference.file_index.is_none()
            && reference.sync_time.is_none()
            && reference.presentation_time_ns.is_none()
            && reference.frame_index.is_none()
        {
            return 0;
        }
        *out = OmatrackVideoReference {
            file_index: reference.file_index.unwrap_or(0),
            sync_time: reference.sync_time.unwrap_or(0.0),
            presentation_time_ns: reference.presentation_time_ns.unwrap_or(0),
            frame_index: reference.frame_index.unwrap_or(0),
            has_file_index: u8::from(reference.file_index.is_some()),
            has_sync_time: u8::from(reference.sync_time.is_some()),
            has_presentation_time: u8::from(reference.presentation_time_ns.is_some()),
            has_frame_index: u8::from(reference.frame_index.is_some()),
        };
        1
    })
}

/// Returns the number of linked video files.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_video_file_count(handle: *mut c_void) -> usize {
    ffi_guard(stringify!(omatrack_video_file_count), 0, || {
        as_file(handle)
            .map(|file| file.src.video_files().len())
            .unwrap_or(0)
    })
}

/// Copies one linked video's identity and timing metadata.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must
/// point to writable `OmatrackVideoFileRef` storage.
#[no_mangle]
pub unsafe extern "C" fn omatrack_video_file(
    handle: *mut c_void,
    index: usize,
    out: *mut OmatrackVideoFileRef,
) -> c_int {
    ffi_guard(stringify!(omatrack_video_file), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        let Some(video) = file.src.video_files().get(index) else {
            return 0;
        };
        let offset = video
            .presentation_offset_ns
            .and_then(|value| i64::try_from(value).ok());
        *out = OmatrackVideoFileRef {
            filename: file.video_names[index].as_ptr(),
            index: video.index,
            blake3: video
                .blake3
                .as_ref()
                .map_or(std::ptr::null(), |hash| hash.as_ptr()),
            frame_count: video.frame_count,
            presentation_offset_ns: offset.unwrap_or(0),
            has_presentation_offset: u8::from(offset.is_some()),
        };
        1
    })
}

/// Copies presentation-order frame timestamps into `out`.
///
/// With `out == NULL`, returns the required element count. Otherwise copies
/// at most `capacity` timestamps and returns the copied count.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle. Non-null `out`
/// must point to `capacity` writable `u64` values.
#[no_mangle]
pub unsafe extern "C" fn omatrack_video_presentation_times_ns(
    handle: *mut c_void,
    out: *mut u64,
    capacity: usize,
) -> usize {
    ffi_guard(stringify!(omatrack_video_presentation_times_ns), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        let Some(times) = file.src.video_presentation_times_ns() else {
            return 0;
        };
        if out.is_null() {
            return times.len();
        }
        let count = capacity.min(times.len());
        std::ptr::copy_nonoverlapping(times.as_ptr(), out, count);
        count
    })
}

/// Identity of the converter that writes normalized `.telemetry` files:
/// `{native format version}-{pinned upstream rev, 12 hex}`. Omatrack keys
/// its normalized-telemetry caches by source identity *and* this string, so
/// advancing the pinned `motorsport-telemetry-rs` revision or bumping the
/// native format regenerates every cache instead of trusting a file an older
/// decoder produced. The string is static and never needs freeing.
#[no_mangle]
pub extern "C" fn omatrack_converter_generation() -> *const c_char {
    use std::sync::OnceLock;
    static GENERATION: OnceLock<CString> = OnceLock::new();
    GENERATION
        .get_or_init(|| {
            let rev = env!("OMATRACK_UPSTREAM_REV");
            let short = &rev[..rev.len().min(12)];
            CString::new(format!("{}-{}", telemetry_format::FORMAT_VERSION, short))
                .unwrap_or_default()
        })
        .as_ptr()
}

/// True when `path` is an MTJ/MTX JSONL document (plain or zstd).
///
/// # Safety
/// `path` must be NULL or a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn omatrack_is_jsonl_path(path: *const c_char) -> c_int {
    ffi_guard(stringify!(omatrack_is_jsonl_path), 0, || {
        if path.is_null() {
            return 0;
        }
        match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(value) => i32::from(telemetry_format::is_jsonl_path(Path::new(value))),
            Err(_) => 0,
        }
    })
}

/// True when `path` names an MTX sidecar (`.ext.jsonl` / `.mtx.jsonl`,
/// including zstd).
///
/// # Safety
/// `path` must be NULL or a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn omatrack_is_sidecar_path(path: *const c_char) -> c_int {
    ffi_guard(stringify!(omatrack_is_sidecar_path), 0, || {
        if path.is_null() {
            return 0;
        }
        match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(value) => i32::from(telemetry_format::is_jsonl_ext_path(Path::new(value))),
            Err(_) => 0,
        }
    })
}

/// Copy MTX / catalog placement into `out`. Returns 1 on success.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must
/// point to writable `OmatrackSidecarInfo` storage.
#[no_mangle]
pub unsafe extern "C" fn omatrack_sidecar_info(
    handle: *mut c_void,
    out: *mut OmatrackSidecarInfo,
) -> c_int {
    ffi_guard(stringify!(omatrack_sidecar_info), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        *out = OmatrackSidecarInfo {
            is_extension: file.sidecar.is_extension,
            group_visible: file.sidecar.group_visible,
            utc_start_ns: file.sidecar.utc_start_ns,
            duration_ns: file.sidecar.duration_ns,
            name: file.sidecar.name.as_ptr(),
            timezone: file.sidecar.timezone.as_ptr(),
        };
        1
    })
}

/// Unix-epoch nanoseconds at file `t = 0`, or 0 when unknown.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must
/// point to a writable `i64`.
#[no_mangle]
pub unsafe extern "C" fn omatrack_utc_start_ns(handle: *mut c_void, out: *mut i64) -> c_int {
    ffi_guard(stringify!(omatrack_utc_start_ns), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        if file.sidecar.utc_start_ns < 0 {
            return 0;
        }
        *out = file.sidecar.utc_start_ns;
        1
    })
}

/// Default visibility of sample channel `index` (`1` shown). Missing
/// indices are visible.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_channel_visible(handle: *mut c_void, index: usize) -> u8 {
    ffi_guard(stringify!(omatrack_channel_visible), 1, || {
        as_file(handle)
            .and_then(|file| file.sidecar.channel_visible.get(index).copied())
            .unwrap_or(1)
    })
}

/// Number of right-aligned header chrome elements.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_sidecar_chrome_count(handle: *mut c_void) -> usize {
    ffi_guard(stringify!(omatrack_sidecar_chrome_count), 0, || {
        as_file(handle)
            .map(|file| file.sidecar.chrome.len())
            .unwrap_or(0)
    })
}

/// Copy one chrome element into `out`. Returns 1 on success.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must
/// point to writable `OmatrackSidecarChrome` storage.
#[no_mangle]
pub unsafe extern "C" fn omatrack_sidecar_chrome(
    handle: *mut c_void,
    index: usize,
    out: *mut OmatrackSidecarChrome,
) -> c_int {
    ffi_guard(stringify!(omatrack_sidecar_chrome), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        let Some(chrome) = file.sidecar.chrome.get(index) else {
            return 0;
        };
        *out = OmatrackSidecarChrome {
            kind: chrome.kind,
            text: chrome.text.as_ptr(),
            label: chrome.label.as_ptr(),
            value: chrome.value.as_ptr(),
        };
        1
    })
}

/// Number of MTX spans.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle.
#[no_mangle]
pub unsafe extern "C" fn omatrack_span_count(handle: *mut c_void) -> usize {
    ffi_guard(stringify!(omatrack_span_count), 0, || {
        as_file(handle)
            .map(|file| file.sidecar.spans.len())
            .unwrap_or(0)
    })
}

/// Copy one span into `out`. Returns 1 on success.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle, and `out` must
/// point to writable `OmatrackSpan` storage.
#[no_mangle]
pub unsafe extern "C" fn omatrack_span(
    handle: *mut c_void,
    index: usize,
    out: *mut OmatrackSpan,
) -> c_int {
    ffi_guard(stringify!(omatrack_span), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if out.is_null() {
            return 0;
        }
        let Some(span) = file.sidecar.spans.get(index) else {
            return 0;
        };
        *out = OmatrackSpan {
            start_ns: span.start_ns,
            end_ns: span.end_ns,
            visible: span.visible,
            name: span.name.as_ptr(),
            title: span.title.as_ptr(),
            subtitle: span.subtitle.as_ptr(),
            color: span.color.as_ptr(),
            meta_count: span.meta.len(),
        };
        1
    })
}

/// Copy one hover field of a span. `key_out` / `value_out` receive
/// handle-owned pointers. Returns 1 on success.
///
/// # Safety
/// `handle` must be NULL or a live `omatrack_open` handle. `key_out` and
/// `value_out` must each point to a writable `*const c_char`.
#[no_mangle]
pub unsafe extern "C" fn omatrack_span_meta(
    handle: *mut c_void,
    span_index: usize,
    meta_index: usize,
    key_out: *mut *const c_char,
    value_out: *mut *const c_char,
) -> c_int {
    ffi_guard(stringify!(omatrack_span_meta), 0, || {
        let Some(file) = as_file(handle) else {
            return 0;
        };
        if key_out.is_null() || value_out.is_null() {
            return 0;
        }
        let Some(span) = file.sidecar.spans.get(span_index) else {
            return 0;
        };
        let Some((key, value)) = span.meta.get(meta_index) else {
            return 0;
        };
        *key_out = key.as_ptr();
        *value_out = value.as_ptr();
        1
    })
}

/// Header-only: 1 when a `.telemetry` file stores a presentation offset and
/// at least one video frame. Older companions without `video_frames.bin`
/// return 0 — rewrite them from the vendor recording.
///
/// # Safety
/// `path` must be NULL or a valid NUL-terminated UTF-8 path.
#[no_mangle]
pub unsafe extern "C" fn omatrack_telemetry_has_video_clock(path: *const c_char) -> c_int {
    ffi_guard(stringify!(omatrack_telemetry_has_video_clock), 0, || {
        if path.is_null() {
            return 0;
        }
        let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(path) => path,
            Err(_) => return 0,
        };
        match telemetry_format::read_metadata(path_str) {
            Ok(metadata)
                if metadata.video_presentation_offset_ns.is_some()
                    && metadata.video_frame_count.unwrap_or(0) > 0 =>
            {
                1
            }
            _ => 0,
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use motorsport_telemetry_core::{
        Channel, Chunk, LapMetadata, SampleType, SourceLapMetadata, UnitSource, VideoFileRef,
    };

    struct TestSource {
        channels: Vec<Channel>,
        samples: Vec<f64>,
        video_presentation_offset_ns: Option<i128>,
        video_times: Vec<u64>,
        videos: Vec<VideoFileRef>,
        source_laps: Option<Vec<LapMetadata>>,
    }

    impl TelemetrySource for TestSource {
        fn path(&self) -> &str {
            "test"
        }

        fn format(&self) -> &'static str {
            "motec"
        }

        fn channels(&self) -> &[Channel] {
            &self.channels
        }

        fn decode(&self, _: usize, _: usize, local_index: u64) -> f64 {
            self.samples[local_index as usize]
        }

        fn video_presentation_offset_ns(&self) -> Option<i128> {
            self.video_presentation_offset_ns
        }

        fn video_files(&self) -> &[VideoFileRef] {
            &self.videos
        }

        fn video_presentation_times_ns(&self) -> Option<&[u64]> {
            (!self.video_times.is_empty()).then_some(&self.video_times)
        }

        fn video_frame_at(&self, time_ns: u64) -> Option<u64> {
            let presentation = self.video_presentation_time_ns(time_ns)?;
            let index = self
                .video_times
                .partition_point(|timestamp| *timestamp <= presentation);
            Some(index.saturating_sub(1) as u64)
        }

        fn source_lap_metadata(&self) -> Option<SourceLapMetadata> {
            self.source_laps.as_ref().map(|laps| SourceLapMetadata {
                laps: laps.clone(),
                fastest_lap: None,
            })
        }
    }

    fn test_handle() -> *mut c_void {
        let channel = Channel {
            id: 0,
            name: "test".into(),
            unit: String::new(),
            unit_source: UnitSource::Unknown,
            sample_type: SampleType::F64,
            chunks: vec![Chunk {
                sample_period_ns: 1_000_000_000,
                sample_count: 1,
                data_ptr: 0,
                sample_base: 0,
                time_base_ns: 0,
            }],
            sample_count: 1,
            duration_ns: 1_000_000_000,
        };
        Box::into_raw(build_handle(Box::new(TestSource {
            channels: vec![channel],
            samples: vec![0.0],
            video_presentation_offset_ns: Some(101_500_000),
            video_times: vec![101_500_000, 601_500_000, 1_101_500_000],
            videos: vec![VideoFileRef {
                filename: "test.mp4".into(),
                index: 1,
                blake3: Some([0xab; 32]),
                frame_count: 3,
                presentation_offset_ns: Some(101_500_000),
            }],
            source_laps: Some(vec![
                LapMetadata {
                    number: 7,
                    start_ns: 1_000_000_000,
                    end_ns: 91_000_000_000,
                    duration_ns: 90_000_000_000,
                    complete: true,
                    first_video_frame: Some(60),
                },
                LapMetadata {
                    number: 8,
                    start_ns: 91_000_000_000,
                    end_ns: 100_000_000_000,
                    duration_ns: 9_000_000_000,
                    complete: false,
                    first_video_frame: None,
                },
            ]),
        }))) as *mut c_void
    }

    #[test]
    fn panic_is_contained_and_reported() {
        let value = ffi_guard("test_export", 7, || -> i32 { panic!("bad input") });
        assert_eq!(value, 7);
        LAST_ERROR.with(|cell| {
            assert!(cell
                .borrow()
                .to_string_lossy()
                .contains("test_export: panic: bad input"));
        });
    }

    #[test]
    fn sample_exports_reject_out_of_range_indices() {
        let handle = test_handle();
        let mut value = 0.0;
        assert_eq!(
            unsafe { omatrack_sample_at(handle, 1, 0, true, &mut value) },
            0
        );
        assert_eq!(unsafe { omatrack_sample_time_ns(handle, 0, 1, 0) }, 0);
        assert_eq!(unsafe { omatrack_sample_time_ns(handle, 0, 0, 1) }, 0);
        unsafe { omatrack_close(handle) };
    }

    #[test]
    fn video_presentation_offset_is_exposed() {
        let handle = test_handle();
        let mut offset = 0i64;
        assert_eq!(
            unsafe { omatrack_video_presentation_offset_ns(handle, &mut offset) },
            1
        );
        assert_eq!(offset, 101_500_000);
        assert_eq!(
            unsafe { omatrack_video_presentation_offset_ns(handle, std::ptr::null_mut()) },
            0
        );
        unsafe { omatrack_close(handle) };
    }

    #[test]
    fn complete_video_contract_is_exposed() {
        let handle = test_handle();
        let mut presentation = 0u64;
        assert_eq!(
            unsafe {
                omatrack_video_presentation_time_ns(handle, 1_000_000_000, &mut presentation)
            },
            1
        );
        assert_eq!(presentation, 1_101_500_000);

        let mut reference = OmatrackVideoReference::default();
        assert_eq!(
            unsafe { omatrack_video_reference_at(handle, 1_000_000_000, &mut reference) },
            1
        );
        assert_eq!(reference.presentation_time_ns, presentation);
        assert_eq!(reference.frame_index, 2);
        assert_eq!(reference.has_presentation_time, 1);
        assert_eq!(reference.has_frame_index, 1);

        assert_eq!(unsafe { omatrack_video_file_count(handle) }, 1);
        let mut video = OmatrackVideoFileRef {
            filename: std::ptr::null(),
            index: 0,
            blake3: std::ptr::null(),
            frame_count: 0,
            presentation_offset_ns: 0,
            has_presentation_offset: 0,
        };
        assert_eq!(unsafe { omatrack_video_file(handle, 0, &mut video) }, 1);
        assert_eq!(
            unsafe { CStr::from_ptr(video.filename) }.to_bytes(),
            b"test.mp4"
        );
        assert_eq!(video.index, 1);
        assert_eq!(video.frame_count, 3);
        assert_eq!(video.presentation_offset_ns, 101_500_000);
        assert_eq!(video.has_presentation_offset, 1);
        assert_eq!(unsafe { *video.blake3 }, 0xab);

        let count =
            unsafe { omatrack_video_presentation_times_ns(handle, std::ptr::null_mut(), 0) };
        assert_eq!(count, 3);
        let mut times = [0u64; 3];
        assert_eq!(
            unsafe {
                omatrack_video_presentation_times_ns(handle, times.as_mut_ptr(), times.len())
            },
            times.len()
        );
        assert_eq!(times, [101_500_000, 601_500_000, 1_101_500_000]);
        unsafe { omatrack_close(handle) };
    }

    #[test]
    fn motec_format_and_source_laps_are_exposed() {
        let handle = test_handle();
        let format = unsafe { CStr::from_ptr(omatrack_format(handle)) };
        assert_eq!(format.to_bytes(), b"ld");
        assert_eq!(unsafe { omatrack_source_lap_count(handle) }, 2);

        let mut lap = OmatrackSourceLap {
            number: 0,
            start_ns: 0,
            end_ns: 0,
            duration_ns: 0,
            first_video_frame: 0,
            complete: 0,
            has_first_video_frame: 0,
        };
        assert_eq!(unsafe { omatrack_source_lap(handle, 0, &mut lap) }, 1);
        assert_eq!(lap.number, 7);
        assert_eq!(lap.start_ns, 1_000_000_000);
        assert_eq!(lap.end_ns, 91_000_000_000);
        assert_eq!(lap.duration_ns, 90_000_000_000);
        assert_eq!(lap.complete, 1);
        assert_eq!(lap.first_video_frame, 60);
        assert_eq!(lap.has_first_video_frame, 1);
        assert_eq!(unsafe { omatrack_source_lap(handle, 2, &mut lap) }, 0);
        assert_eq!(
            unsafe { omatrack_source_lap(handle, 0, std::ptr::null_mut()) },
            0
        );
        unsafe { omatrack_close(handle) };
    }

    #[test]
    fn index_handle_exposes_laps_derived_from_channels() {
        let samples = (0..45)
            .map(|index| f64::from(index / 15 + 1))
            .collect::<Vec<_>>();
        let channel = Channel {
            id: 0,
            name: "Lap Number".into(),
            unit: String::new(),
            unit_source: UnitSource::Unknown,
            sample_type: SampleType::F64,
            chunks: vec![Chunk {
                sample_period_ns: 1_000_000_000,
                sample_count: samples.len() as u64,
                data_ptr: 0,
                sample_base: 0,
                time_base_ns: 0,
            }],
            sample_count: samples.len() as u64,
            duration_ns: samples.len() as u64 * 1_000_000_000,
        };
        let handle = Box::into_raw(build_handle(Box::new(TestSource {
            channels: vec![channel],
            samples,
            video_presentation_offset_ns: None,
            video_times: Vec::new(),
            videos: Vec::new(),
            source_laps: None,
        }))) as *mut c_void;

        assert_eq!(unsafe { omatrack_source_lap_count(handle) }, 3);
        let mut middle = OmatrackSourceLap {
            number: 0,
            start_ns: 0,
            end_ns: 0,
            duration_ns: 0,
            first_video_frame: 0,
            complete: 0,
            has_first_video_frame: 0,
        };
        assert_eq!(unsafe { omatrack_source_lap(handle, 1, &mut middle) }, 1);
        assert_eq!(middle.number, 2);
        assert_eq!(middle.start_ns, 15_000_000_000);
        assert_eq!(middle.end_ns, 30_000_000_000);
        assert_eq!(middle.complete, 1);
        unsafe { omatrack_close(handle) };
    }

    #[test]
    fn ffi_smoke_open_null_and_free() {
        unsafe { omatrack_close(std::ptr::null_mut()) };
        assert!(as_file(std::ptr::null_mut()).is_none());
    }
}
