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

use motorsport_telemetry_core::TelemetrySource;
use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

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
}

/// Stable C representation of one authoritative source-provided lap.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OmatrackSourceLap {
    pub number: i64,
    pub start_ns: u64,
    pub end_ns: u64,
    pub duration_ns: u64,
    pub complete: u8,
}

fn as_file<'a>(handle: *mut c_void) -> Option<&'a BridgeFile> {
    if handle.is_null() {
        None
    } else {
        Some(unsafe { &*(handle as *const BridgeFile) })
    }
}

fn parse_path(open_path: &Path) -> Result<Box<dyn TelemetrySource>, String> {
    let ext = open_path
        .extension()
        .and_then(|value| value.to_str())
        .map(|value| value.to_ascii_lowercase())
        .unwrap_or_default();
    match ext.as_str() {
        "mp4" => aim_telemetry::AimFile::open(open_path)
            .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
            .map_err(|error| error.to_string()),
        "pds" => cosworth_telemetry::CosworthFile::open(open_path)
            .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
            .map_err(|error| error.to_string()),
        "ld" => motec_telemetry::MotecFile::open(open_path)
            .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
            .map_err(|error| error.to_string()),
        "vbo" => racelogic_telemetry::RacelogicFile::open(open_path)
            .map(|source| Box::new(source) as Box<dyn TelemetrySource>)
            .map_err(|error| error.to_string()),
        other => Err(format!("unsupported telemetry format: {other:?}")),
    }
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
    let source_laps = src
        .source_lap_metadata()
        .map(|metadata| {
            metadata
                .laps
                .into_iter()
                .map(|lap| OmatrackSourceLap {
                    number: lap.number,
                    start_ns: lap.start_ns,
                    end_ns: lap.end_ns,
                    duration_ns: lap.duration_ns,
                    complete: u8::from(lap.complete),
                })
                .collect()
        })
        .unwrap_or_default();
    Box::new(BridgeFile {
        src,
        names,
        units,
        source_laps,
    })
}

// ── handling ─────────────────────────────────────────────────────────

/// Open a telemetry file by path (format dispatched from extension).
/// Returns a handle, or NULL on error (see `omatrack_last_error`).
///
/// # Safety
/// `path` must be NULL or a valid NUL-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn omatrack_open(path: *const c_char) -> *mut c_void {
    ffi_guard(stringify!(omatrack_open), std::ptr::null_mut(), || {
        if path.is_null() {
            set_error("omatrack_open: null path");
            return std::ptr::null_mut();
        }
        let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(s) => s,
            Err(_) => {
                set_error("omatrack_open: path is not valid UTF-8");
                return std::ptr::null_mut();
            }
        };
        match parse_path(Path::new(path_str)) {
            Ok(src) => Box::into_raw(build_handle(src)) as *mut c_void,
            Err(e) => {
                set_error(format!("omatrack_open: {e}"));
                std::ptr::null_mut()
            }
        }
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
                let fmt = f.src.format();
                let bytes: &[u8] = match fmt {
                    "aimd" => AIMD,
                    "pds" => PDS,
                    "ld" | "motec" => LD,
                    "vbo" => VBO,
                    _ => b"unknown\0",
                };
                bytes.as_ptr() as *const c_char
            }
            None => std::ptr::null(),
        },
    )
}

/// Return the number of authoritative source-provided laps.
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

/// Copy one authoritative source-provided lap into `out`.
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

#[cfg(test)]
mod tests {
    use super::*;
    use motorsport_telemetry_core::{
        Channel, Chunk, LapMetadata, SampleType, SourceLapMetadata, UnitSource,
    };

    struct TestSource {
        channels: Vec<Channel>,
        video_presentation_offset_ns: Option<i128>,
        source_laps: Vec<LapMetadata>,
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
            local_index as f64
        }

        fn video_presentation_offset_ns(&self) -> Option<i128> {
            self.video_presentation_offset_ns
        }

        fn source_lap_metadata(&self) -> Option<SourceLapMetadata> {
            Some(SourceLapMetadata {
                laps: self.source_laps.clone(),
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
            video_presentation_offset_ns: Some(101_500_000),
            source_laps: vec![
                LapMetadata {
                    number: 7,
                    start_ns: 1_000_000_000,
                    end_ns: 91_000_000_000,
                    duration_ns: 90_000_000_000,
                    complete: true,
                },
                LapMetadata {
                    number: 8,
                    start_ns: 91_000_000_000,
                    end_ns: 100_000_000_000,
                    duration_ns: 9_000_000_000,
                    complete: false,
                },
            ],
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
            complete: 0,
        };
        assert_eq!(unsafe { omatrack_source_lap(handle, 0, &mut lap) }, 1);
        assert_eq!(lap.number, 7);
        assert_eq!(lap.start_ns, 1_000_000_000);
        assert_eq!(lap.end_ns, 91_000_000_000);
        assert_eq!(lap.duration_ns, 90_000_000_000);
        assert_eq!(lap.complete, 1);
        assert_eq!(unsafe { omatrack_source_lap(handle, 2, &mut lap) }, 0);
        assert_eq!(
            unsafe { omatrack_source_lap(handle, 0, std::ptr::null_mut()) },
            0
        );
        unsafe { omatrack_close(handle) };
    }

    #[test]
    fn ffi_smoke_open_null_and_free() {
        unsafe { omatrack_close(std::ptr::null_mut()) };
        assert!(as_file(std::ptr::null_mut()).is_none());
    }
}
