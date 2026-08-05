//! C ABI bridge exposing the vendored duckdb_motorsport_telemetry parsers
//! (AiM aimd MP4, Cosworth/Pi PDS, MoTeC LD, VBO) to racecraft-qt.
//!
//! Design notes (bulk-first):
//! - `rc_decode_range` pulls a contiguous run of decoded samples across one
//!   chunk in a single FFI call; the C++ side caches whole channel arrays.
//! - `rc_sample_at` exists only for cheap per-cursor readouts.
//! - Strings returned by `rc_channel_name`/`rc_channel_unit` are owned by the
//!   handle and valid until `rc_close`.
//! - Errors are reported via `rc_last_error` (thread-local) and non-zero/handle
//!   return codes; no panics cross the FFI boundary.

use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::path::Path;

use motorsport_telemetry_core::TelemetrySource;

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::new("").unwrap());
}

fn set_error(msg: impl Into<String>) {
    LAST_ERROR.with(|cell| {
        *cell.borrow_mut() = CString::new(msg.into()).unwrap_or_default();
    });
}

/// Opaque handle wrapping a parsed telemetry source plus owned C-string tables
/// for channel names/units.
pub struct BridgeFile {
    src: Box<dyn TelemetrySource>,
    names: Vec<CString>,
    units: Vec<CString>,
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
        .and_then(|e| e.to_str())
        .map(|e| e.to_ascii_lowercase())
        .unwrap_or_default();
    let path = open_path.to_string_lossy().into_owned();
    match ext.as_str() {
        "mp4" => aim_telemetry::AimFile::open(&path)
            .map(|f| Box::new(f) as Box<dyn TelemetrySource>)
            .map_err(|e| e.to_string()),
        "pds" => cosworth_telemetry::CosworthFile::open(&path)
            .map(|f| Box::new(f) as Box<dyn TelemetrySource>)
            .map_err(|e| e.to_string()),
        "ld" => motec_telemetry::MotecFile::open(&path)
            .map(|f| Box::new(f) as Box<dyn TelemetrySource>)
            .map_err(|e| e.to_string()),
        "vbo" => vbo_telemetry::VboFile::open(&path)
            .map(|f| Box::new(f) as Box<dyn TelemetrySource>)
            .map_err(|e| e.to_string()),
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
    Box::new(BridgeFile { src, names, units })
}

// ── handling ─────────────────────────────────────────────────────────

/// Open a telemetry file by path (format dispatched from extension).
/// Returns a handle, or NULL on error (see `rc_last_error`).
#[no_mangle]
pub extern "C" fn rc_open(path: *const c_char) -> *mut c_void {
    if path.is_null() {
        set_error("rc_open: null path");
        return std::ptr::null_mut();
    }
    let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
        Ok(s) => s,
        Err(_) => {
            set_error("rc_open: path is not valid UTF-8");
            return std::ptr::null_mut();
        }
    };
    match parse_path(Path::new(path_str)) {
        Ok(src) => Box::into_raw(build_handle(src)) as *mut c_void,
        Err(e) => {
            set_error(format!("rc_open: {e}"));
            std::ptr::null_mut()
        }
    }
}

/// Free a handle returned by `rc_open`. Safe to call with NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_close(handle: *mut c_void) {
    if !handle.is_null() {
        drop(Box::from_raw(handle as *mut BridgeFile));
    }
}

/// Thread-local error string from the last failing call.
#[no_mangle]
pub extern "C" fn rc_last_error() -> *const c_char {
    LAST_ERROR.with(|cell| cell.borrow().as_ptr())
}

// ── metadata ─────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn rc_format(handle: *mut c_void) -> *const c_char {
    match as_file(handle) {
        Some(f) => {
            static AIMD: &[u8] = b"aimd\0";
            static PDS: &[u8] = b"pds\0";
            static LD: &[u8] = b"ld\0";
            static VBO: &[u8] = b"vbo\0";
            let fmt = f.src.format();
            let bytes: &[u8] = match fmt {
                "aimd" => AIMD,
                "pds" => PDS,
                "ld" => LD,
                "vbo" => VBO,
                _ => b"unknown\0",
            };
            bytes.as_ptr() as *const c_char
        }
        None => std::ptr::null(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn rc_media_time_offset_ns(
    handle: *mut c_void,
    out: *mut i64,
) -> std::os::raw::c_int {
    if out.is_null() {
        return 0;
    }
    match as_file(handle).and_then(|file| file.src.media_time_offset_ns()) {
        Some(value) => {
            *out = value;
            1
        }
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn rc_channel_count(handle: *mut c_void) -> usize {
    as_file(handle).map(|f| f.src.channels().len()).unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn rc_channel_name(handle: *mut c_void, index: usize) -> *const c_char {
    match as_file(handle) {
        Some(f) => f
            .names
            .get(index)
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        None => std::ptr::null(),
    }
}

#[no_mangle]
pub extern "C" fn rc_channel_unit(handle: *mut c_void, index: usize) -> *const c_char {
    match as_file(handle) {
        Some(f) => f
            .units
            .get(index)
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        None => std::ptr::null(),
    }
}

#[no_mangle]
pub extern "C" fn rc_channel_type_code(handle: *mut c_void, index: usize) -> u32 {
    match as_file(handle) {
        Some(f) => f
            .src
            .channels()
            .get(index)
            .map(|c| c.sample_type.code())
            .unwrap_or(0),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn rc_channel_duration_ns(handle: *mut c_void, index: usize) -> u64 {
    match as_file(handle) {
        Some(f) => f
            .src
            .channels()
            .get(index)
            .map(|c| c.duration_ns)
            .unwrap_or(0),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn rc_channel_sample_count(handle: *mut c_void, index: usize) -> u64 {
    match as_file(handle) {
        Some(f) => f
            .src
            .channels()
            .get(index)
            .map(|c| c.sample_count)
            .unwrap_or(0),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn rc_channel_chunk_count(handle: *mut c_void, index: usize) -> usize {
    match as_file(handle) {
        Some(f) => f
            .src
            .channels()
            .get(index)
            .map(|c| c.chunks.len())
            .unwrap_or(0),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn rc_chunk_period_ns(handle: *mut c_void, index: usize, chunk: usize) -> u64 {
    match as_file(handle) {
        Some(f) => f
            .src
            .channels()
            .get(index)
            .and_then(|c| c.chunks.get(chunk))
            .map(|c| c.sample_period_ns)
            .unwrap_or(0),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn rc_chunk_time_base_ns(handle: *mut c_void, index: usize, chunk: usize) -> u64 {
    match as_file(handle) {
        Some(f) => f
            .src
            .channels()
            .get(index)
            .and_then(|c| c.chunks.get(chunk))
            .map(|c| c.time_base_ns)
            .unwrap_or(0),
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn rc_chunk_sample_count(handle: *mut c_void, index: usize, chunk: usize) -> u64 {
    match as_file(handle) {
        Some(f) => f
            .src
            .channels()
            .get(index)
            .and_then(|c| c.chunks.get(chunk))
            .map(|c| c.sample_count)
            .unwrap_or(0),
        None => 0,
    }
}

// ── bulk sample access ───────────────────────────────────────────────

/// Decode `[start, start+n)` samples of one chunk into `out`.
/// Returns the number of samples actually written (may be less than `n` when
/// the chunk ends early). `out` must have room for `n` doubles.
#[no_mangle]
pub unsafe extern "C" fn rc_decode_range(
    handle: *mut c_void,
    index: usize,
    chunk: usize,
    start: u64,
    n: usize,
    out: *mut f64,
) -> usize {
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
}

/// Decode one logical multichunk channel into a single contiguous buffer.
/// Samples are emitted in chunk order (which is also time order). Returns the
/// total number of samples written; `out` must have room for
/// `rc_channel_sample_count` doubles.
#[no_mangle]
pub unsafe extern "C" fn rc_channel_decode_all(
    handle: *mut c_void,
    index: usize,
    out: *mut f64,
    capacity: usize,
) -> usize {
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
}

/// Sample a channel at an absolute time in ns with linear interpolation.
/// Writes the value to `out` and returns 1 on success, 0 when out of range.
#[no_mangle]
pub unsafe extern "C" fn rc_sample_at(
    handle: *mut c_void,
    index: usize,
    time_ns: u64,
    linear: bool,
    out: *mut f64,
) -> std::os::raw::c_int {
    let Some(file) = as_file(handle) else {
        return 0;
    };
    if out.is_null() {
        return 0;
    }
    match file.src.sample_at(index, time_ns, linear) {
        Some(v) => {
            *out = v;
            1
        }
        None => 0,
    }
}

#[no_mangle]
pub extern "C" fn rc_sample_time_ns(
    handle: *mut c_void,
    index: usize,
    chunk: usize,
    local: u64,
) -> u64 {
    match as_file(handle) {
        Some(f) => f.src.sample_time_ns(index, chunk, local),
        None => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ffi_smoke_open_null_and_free() {
        // rc_close(NULL) must not crash.
        unsafe { rc_close(std::ptr::null_mut()) };
        assert!(as_file(std::ptr::null_mut()).is_none());
    }
}
