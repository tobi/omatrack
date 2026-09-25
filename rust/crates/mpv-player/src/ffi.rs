//! Thin safe wrappers over `libmpv2-sys` (libmpv client API 2.5).
//!
//! This is the only module of the crate that calls into C. (The other
//! `unsafe` code is `render`'s pixel-copy loop, raw-pointer word moves over
//! Rust-owned buffers.) Every other module talks to mpv through the types
//! here:
//!
//! - [`MpvHandle`] owns one `mpv_handle` (a core plus its client). The client
//!   API is documented as fully thread-safe, so the handle is `Send + Sync`
//!   and shared through an `Arc`. The one exception, `mpv_wait_event`, is
//!   serialized by an internal mutex.
//! - [`SwRenderContext`] owns the software render context. The render API may
//!   be driven from any thread but only one call at a time, so the context is
//!   `Send` and not `Sync`: exactly one render thread owns it.
//! - [`AlignedBuffer`] is the 64-byte aligned render target mpv's SIMD paths
//!   want (`MPV_RENDER_PARAM_SW_POINTER` and `_STRIDE` "should be a multiple
//!   of 64").

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::fmt;
use std::ptr::NonNull;
use std::sync::{Arc, Mutex};

use libmpv2_sys as sys;

/// An mpv error: the numeric `mpv_error` code plus its message, or a local
/// validation failure (code 0).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MpvError {
    code: i32,
    message: String,
}

impl MpvError {
    pub(crate) fn from_code(code: c_int) -> Self {
        Self {
            code,
            message: error_string(code),
        }
    }

    pub(crate) fn local(message: impl Into<String>) -> Self {
        Self {
            code: 0,
            message: message.into(),
        }
    }

    /// The libmpv error code (`MPV_ERROR_*`, negative), or 0 for errors raised
    /// by this crate before reaching mpv.
    pub fn code(&self) -> i32 {
        self.code
    }

    /// Human-readable description.
    pub fn message(&self) -> &str {
        &self.message
    }
}

impl fmt::Display for MpvError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.code == 0 {
            f.write_str(&self.message)
        } else {
            write!(f, "{} (mpv error {})", self.message, self.code)
        }
    }
}

impl std::error::Error for MpvError {}

fn check(code: c_int) -> Result<(), MpvError> {
    if code < 0 {
        Err(MpvError::from_code(code))
    } else {
        Ok(())
    }
}

fn c_string(value: &str) -> Result<CString, MpvError> {
    CString::new(value).map_err(|_| MpvError::local(format!("interior NUL in {value:?}")))
}

/// `mpv_error_string` for a code.
pub(crate) fn error_string(code: c_int) -> String {
    // SAFETY: mpv_error_string has no handle parameter, accepts any integer
    // and returns a pointer to a static, NUL-terminated string (never NULL).
    let raw = unsafe { sys::mpv_error_string(code) };
    if raw.is_null() {
        return format!("mpv error {code}");
    }
    // SAFETY: `raw` is non-null and points to a static C string owned by mpv.
    unsafe { CStr::from_ptr(raw) }
        .to_string_lossy()
        .into_owned()
}

/// Converts a borrowed C string from an mpv event into an owned `String`.
///
/// # Safety
/// `raw` must be NULL or a valid NUL-terminated string that lives for the
/// duration of the call.
unsafe fn owned(raw: *const c_char) -> String {
    if raw.is_null() {
        String::new()
    } else {
        // SAFETY: guaranteed by the caller.
        unsafe { CStr::from_ptr(raw) }
            .to_string_lossy()
            .into_owned()
    }
}

/// The value formats this crate observes properties in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PropertyFormat {
    Flag,
    Double,
    Int64,
}

impl PropertyFormat {
    fn raw(self) -> sys::mpv_format {
        match self {
            Self::Flag => sys::mpv_format_MPV_FORMAT_FLAG,
            Self::Double => sys::mpv_format_MPV_FORMAT_DOUBLE,
            Self::Int64 => sys::mpv_format_MPV_FORMAT_INT64,
        }
    }
}

/// A property value copied out of an mpv event.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum PropertyValue {
    /// The property is unavailable (for example `time-pos` with no file).
    Unavailable,
    Flag(bool),
    Double(f64),
    Int64(i64),
}

/// Why a file stopped playing (`mpv_end_file_reason`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EndReason {
    /// The end of the file was reached.
    Eof,
    /// Playback was stopped by a command (for example a new `loadfile`).
    Stop,
    /// The player is quitting.
    Quit,
    /// Playback failed; see the accompanying error.
    Error,
    /// The file was a playlist or redirect.
    Redirect,
    /// A reason newer than this crate.
    Unknown,
}

/// An mpv event with every borrowed pointer copied into owned data, so it
/// outlives the next `mpv_wait_event` call.
#[derive(Clone, Debug, PartialEq)]
pub(crate) enum RawEvent {
    /// The wait timed out or was interrupted by [`MpvHandle::wakeup`].
    None,
    Shutdown,
    StartFile,
    FileLoaded,
    EndFile {
        reason: EndReason,
        error: Option<MpvError>,
    },
    Seek,
    PlaybackRestart,
    VideoReconfig,
    Property {
        id: u64,
        value: PropertyValue,
    },
    /// Reply to an asynchronous command or property write.
    Reply {
        userdata: u64,
        error: Option<MpvError>,
    },
    Log {
        level: u32,
        prefix: String,
        text: String,
    },
    Other(u32),
}

/// Owner of one mpv core and its client handle.
pub(crate) struct MpvHandle {
    raw: NonNull<sys::mpv_handle>,
    /// `mpv_wait_event` must not be called concurrently on one handle.
    wait_lock: Mutex<()>,
}

// SAFETY: client.h: "The client API is generally fully thread-safe, unless
// otherwise noted." The only noted exception on a shared handle is
// mpv_wait_event, which `wait_lock` serializes. Destruction happens once, in
// Drop, when the last `Arc` goes away.
unsafe impl Send for MpvHandle {}
// SAFETY: see `Send` above.
unsafe impl Sync for MpvHandle {}

impl MpvHandle {
    /// `mpv_create`. The core is not initialized yet: set options first, then
    /// call [`MpvHandle::initialize`].
    pub(crate) fn create() -> Result<Self, MpvError> {
        // SAFETY: mpv_create takes no arguments; it returns NULL on failure.
        let raw = unsafe { sys::mpv_create() };
        let raw = NonNull::new(raw).ok_or_else(|| MpvError::local("mpv_create returned NULL"))?;
        Ok(Self {
            raw,
            wait_lock: Mutex::new(()),
        })
    }

    fn ptr(&self) -> *mut sys::mpv_handle {
        self.raw.as_ptr()
    }

    pub(crate) fn set_option_string(&self, name: &str, value: &str) -> Result<(), MpvError> {
        let name = c_string(name)?;
        let value = c_string(value)?;
        // SAFETY: valid handle; both strings are NUL-terminated and outlive the
        // call (mpv copies them).
        check(unsafe { sys::mpv_set_option_string(self.ptr(), name.as_ptr(), value.as_ptr()) })
    }

    pub(crate) fn initialize(&self) -> Result<(), MpvError> {
        // SAFETY: valid, not yet initialized handle.
        check(unsafe { sys::mpv_initialize(self.ptr()) })
    }

    pub(crate) fn request_log_messages(&self, min_level: &str) -> Result<(), MpvError> {
        let level = c_string(min_level)?;
        // SAFETY: valid handle and NUL-terminated level string.
        check(unsafe { sys::mpv_request_log_messages(self.ptr(), level.as_ptr()) })
    }

    /// `mpv_command_async`; the reply arrives as [`RawEvent::Reply`].
    pub(crate) fn command_async(&self, userdata: u64, args: &[&str]) -> Result<(), MpvError> {
        let owned = args
            .iter()
            .map(|arg| c_string(arg))
            .collect::<Result<Vec<_>, _>>()?;
        let mut pointers: Vec<*const c_char> = owned.iter().map(|arg| arg.as_ptr()).collect();
        pointers.push(std::ptr::null());
        // SAFETY: valid handle; `pointers` is a NULL-terminated array of
        // NUL-terminated strings that stay alive (in `owned`) for the call.
        // mpv copies the arguments before returning.
        check(unsafe { sys::mpv_command_async(self.ptr(), userdata, pointers.as_mut_ptr()) })
    }

    pub(crate) fn set_flag_async(
        &self,
        userdata: u64,
        name: &str,
        value: bool,
    ) -> Result<(), MpvError> {
        let name = c_string(name)?;
        let mut flag: c_int = value.into();
        // SAFETY: valid handle; FLAG data is an `int*`; mpv copies the value
        // before returning.
        check(unsafe {
            sys::mpv_set_property_async(
                self.ptr(),
                userdata,
                name.as_ptr(),
                sys::mpv_format_MPV_FORMAT_FLAG,
                (&mut flag as *mut c_int).cast::<c_void>(),
            )
        })
    }

    pub(crate) fn set_double_async(
        &self,
        userdata: u64,
        name: &str,
        value: f64,
    ) -> Result<(), MpvError> {
        let name = c_string(name)?;
        let mut number = value;
        // SAFETY: valid handle; DOUBLE data is a `double*`; mpv copies the
        // value before returning.
        check(unsafe {
            sys::mpv_set_property_async(
                self.ptr(),
                userdata,
                name.as_ptr(),
                sys::mpv_format_MPV_FORMAT_DOUBLE,
                (&mut number as *mut f64).cast::<c_void>(),
            )
        })
    }

    /// Synchronous `mpv_get_property` as a double.
    pub(crate) fn get_double(&self, name: &str) -> Result<f64, MpvError> {
        let name = c_string(name)?;
        let mut value = 0.0_f64;
        // SAFETY: valid handle; DOUBLE output is written to a `double*` we own.
        check(unsafe {
            sys::mpv_get_property(
                self.ptr(),
                name.as_ptr(),
                sys::mpv_format_MPV_FORMAT_DOUBLE,
                (&mut value as *mut f64).cast::<c_void>(),
            )
        })?;
        Ok(value)
    }

    /// Synchronous `mpv_get_property` as a 64-bit integer.
    pub(crate) fn get_int64(&self, name: &str) -> Result<i64, MpvError> {
        let name = c_string(name)?;
        let mut value = 0_i64;
        // SAFETY: valid handle; INT64 output is written to an `int64_t*` we own.
        check(unsafe {
            sys::mpv_get_property(
                self.ptr(),
                name.as_ptr(),
                sys::mpv_format_MPV_FORMAT_INT64,
                (&mut value as *mut i64).cast::<c_void>(),
            )
        })?;
        Ok(value)
    }

    pub(crate) fn observe_property(
        &self,
        id: u64,
        name: &str,
        format: PropertyFormat,
    ) -> Result<(), MpvError> {
        let name = c_string(name)?;
        // SAFETY: valid handle and NUL-terminated name (copied by mpv).
        check(unsafe { sys::mpv_observe_property(self.ptr(), id, name.as_ptr(), format.raw()) })
    }

    /// Interrupts a blocked [`MpvHandle::wait_event`], which then returns
    /// [`RawEvent::None`].
    pub(crate) fn wakeup(&self) {
        // SAFETY: valid handle; mpv_wakeup is thread-safe.
        unsafe { sys::mpv_wakeup(self.ptr()) }
    }

    /// `mpv_wait_event`, copying the event out before the next call can
    /// invalidate it. A negative timeout waits forever.
    pub(crate) fn wait_event(&self, timeout_secs: f64) -> RawEvent {
        let _serialized = self
            .wait_lock
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        // SAFETY: valid handle; calls are serialized by `wait_lock`. The
        // returned pointer is never NULL and stays valid until the next
        // mpv_wait_event on this handle, which cannot happen before this
        // function has copied everything it needs (the lock is still held).
        let event = unsafe { &*sys::mpv_wait_event(self.ptr(), timeout_secs) };
        // SAFETY: `event` is valid per the above; `convert_event` only reads
        // the documented payload for each event id.
        unsafe { convert_event(event) }
    }
}

/// # Safety
/// `event` must be a live event returned by `mpv_wait_event`.
unsafe fn convert_event(event: &sys::mpv_event) -> RawEvent {
    let reply_error = (event.error < 0).then(|| MpvError::from_code(event.error));
    match event.event_id {
        sys::mpv_event_id_MPV_EVENT_NONE => RawEvent::None,
        sys::mpv_event_id_MPV_EVENT_SHUTDOWN => RawEvent::Shutdown,
        sys::mpv_event_id_MPV_EVENT_START_FILE => RawEvent::StartFile,
        sys::mpv_event_id_MPV_EVENT_FILE_LOADED => RawEvent::FileLoaded,
        sys::mpv_event_id_MPV_EVENT_SEEK => RawEvent::Seek,
        sys::mpv_event_id_MPV_EVENT_PLAYBACK_RESTART => RawEvent::PlaybackRestart,
        sys::mpv_event_id_MPV_EVENT_VIDEO_RECONFIG => RawEvent::VideoReconfig,
        sys::mpv_event_id_MPV_EVENT_COMMAND_REPLY
        | sys::mpv_event_id_MPV_EVENT_SET_PROPERTY_REPLY => RawEvent::Reply {
            userdata: event.reply_userdata,
            error: reply_error,
        },
        sys::mpv_event_id_MPV_EVENT_END_FILE => {
            let data = event.data.cast::<sys::mpv_event_end_file>();
            if data.is_null() {
                return RawEvent::EndFile {
                    reason: EndReason::Unknown,
                    error: None,
                };
            }
            // SAFETY: END_FILE carries a `mpv_event_end_file*` in `data`.
            let end = unsafe { &*data };
            let reason = match end.reason {
                sys::mpv_end_file_reason_MPV_END_FILE_REASON_EOF => EndReason::Eof,
                sys::mpv_end_file_reason_MPV_END_FILE_REASON_STOP => EndReason::Stop,
                sys::mpv_end_file_reason_MPV_END_FILE_REASON_QUIT => EndReason::Quit,
                sys::mpv_end_file_reason_MPV_END_FILE_REASON_ERROR => EndReason::Error,
                sys::mpv_end_file_reason_MPV_END_FILE_REASON_REDIRECT => EndReason::Redirect,
                _ => EndReason::Unknown,
            };
            RawEvent::EndFile {
                reason,
                error: (end.error < 0).then(|| MpvError::from_code(end.error)),
            }
        }
        sys::mpv_event_id_MPV_EVENT_PROPERTY_CHANGE => {
            let data = event.data.cast::<sys::mpv_event_property>();
            if data.is_null() {
                return RawEvent::Other(event.event_id);
            }
            // SAFETY: PROPERTY_CHANGE carries a `mpv_event_property*`.
            let property = unsafe { &*data };
            let value = if property.data.is_null() {
                PropertyValue::Unavailable
            } else {
                match property.format {
                    // SAFETY: FLAG data points to an `int`.
                    sys::mpv_format_MPV_FORMAT_FLAG => {
                        PropertyValue::Flag(unsafe { *property.data.cast::<c_int>() } != 0)
                    }
                    // SAFETY: DOUBLE data points to a `double`.
                    sys::mpv_format_MPV_FORMAT_DOUBLE => {
                        PropertyValue::Double(unsafe { *property.data.cast::<f64>() })
                    }
                    // SAFETY: INT64 data points to an `int64_t`.
                    sys::mpv_format_MPV_FORMAT_INT64 => {
                        PropertyValue::Int64(unsafe { *property.data.cast::<i64>() })
                    }
                    _ => PropertyValue::Unavailable,
                }
            };
            RawEvent::Property {
                id: event.reply_userdata,
                value,
            }
        }
        sys::mpv_event_id_MPV_EVENT_LOG_MESSAGE => {
            let data = event.data.cast::<sys::mpv_event_log_message>();
            if data.is_null() {
                return RawEvent::Other(event.event_id);
            }
            // SAFETY: LOG_MESSAGE carries a `mpv_event_log_message*` whose
            // strings live until the next mpv_wait_event.
            let message = unsafe { &*data };
            RawEvent::Log {
                level: message.log_level,
                // SAFETY: see above.
                prefix: unsafe { owned(message.prefix) },
                // SAFETY: see above.
                text: unsafe { owned(message.text) }.trim_end().to_owned(),
            }
        }
        other => RawEvent::Other(other),
    }
}

impl Drop for MpvHandle {
    fn drop(&mut self) {
        // SAFETY: the handle is valid and this is the only place it is
        // destroyed. Every render context holds an `Arc<MpvHandle>`, so all of
        // them have been freed already, as render.h requires.
        unsafe { sys::mpv_terminate_destroy(self.ptr()) }
    }
}

/// The `mpv_log_level` value of `MPV_LOG_LEVEL_WARN`.
pub(crate) const LOG_LEVEL_WARN: u32 = sys::mpv_log_level_MPV_LOG_LEVEL_WARN;

type UpdateCallback = Box<dyn Fn() + Send + Sync>;

unsafe extern "C" fn update_trampoline(context: *mut c_void) {
    if context.is_null() {
        return;
    }
    // SAFETY: `context` is the `*const UpdateCallback` registered in
    // `set_update_callback`, kept alive (boxed in the context) until the
    // callback is cleared in Drop. The callback never calls into mpv.
    let callback = unsafe { &*context.cast_const().cast::<UpdateCallback>() };
    callback();
}

/// Software (`MPV_RENDER_API_TYPE_SW`) render context.
pub(crate) struct SwRenderContext {
    raw: NonNull<sys::mpv_render_context>,
    callback: Option<Box<UpdateCallback>>,
    /// Keeps the core alive: render.h requires the context to be freed before
    /// the core is destroyed.
    _handle: Arc<MpvHandle>,
}

// SAFETY: render.h allows the mpv_render_* functions to be called from any
// thread as long as only one is called at a time. `SwRenderContext` is not
// `Sync` and every method takes `&mut self`, so one owner thread serializes
// them.
unsafe impl Send for SwRenderContext {}

/// A software render target in device pixels.
pub(crate) struct SwTarget<'a> {
    pub width: u32,
    pub height: u32,
    pub stride: usize,
    pub buffer: &'a mut AlignedBuffer,
}

impl SwRenderContext {
    /// `mpv_render_context_create` with the SW API. Must happen before a file
    /// is loaded, otherwise video initialization fails.
    pub(crate) fn create(handle: Arc<MpvHandle>) -> Result<Self, MpvError> {
        let mut api = sys::MPV_RENDER_API_TYPE_SW.to_vec();
        let mut params = [
            sys::mpv_render_param {
                type_: sys::mpv_render_param_type_MPV_RENDER_PARAM_API_TYPE,
                data: api.as_mut_ptr().cast::<c_void>(),
            },
            sys::mpv_render_param {
                type_: 0,
                data: std::ptr::null_mut(),
            },
        ];
        let mut raw: *mut sys::mpv_render_context = std::ptr::null_mut();
        // SAFETY: valid handle; `params` is a 0-terminated array whose API
        // type points to the NUL-terminated "sw" string, alive for the call.
        check(unsafe {
            sys::mpv_render_context_create(&mut raw, handle.ptr(), params.as_mut_ptr())
        })?;
        let raw = NonNull::new(raw)
            .ok_or_else(|| MpvError::local("mpv_render_context_create returned NULL"))?;
        Ok(Self {
            raw,
            callback: None,
            _handle: handle,
        })
    }

    /// Registers the update callback. It runs on mpv's threads and must only
    /// signal (never call mpv).
    pub(crate) fn set_update_callback(&mut self, callback: impl Fn() + Send + Sync + 'static) {
        let boxed: Box<UpdateCallback> = Box::new(Box::new(callback));
        let context = (&*boxed as *const UpdateCallback)
            .cast_mut()
            .cast::<c_void>();
        // SAFETY: valid context; the trampoline matches `mpv_render_update_fn`
        // and `context` points to a heap allocation stored in `self.callback`
        // below, which outlives the registration (cleared in Drop first).
        unsafe {
            sys::mpv_render_context_set_update_callback(
                self.raw.as_ptr(),
                Some(update_trampoline),
                context,
            )
        };
        // Replacing an older callback is safe: mpv has switched to the new
        // context pointer before this assignment drops the old box.
        self.callback = Some(boxed);
    }

    /// `mpv_render_context_update`: true when a new frame should be rendered.
    pub(crate) fn update(&mut self) -> bool {
        // SAFETY: valid context, called from its single owner thread.
        let flags = unsafe { sys::mpv_render_context_update(self.raw.as_ptr()) };
        flags & u64::from(sys::mpv_render_update_flag_MPV_RENDER_UPDATE_FRAME) != 0
    }

    /// Renders the current frame into `target` in `bgr0` (B, G, R, X byte
    /// order; X is undefined).
    pub(crate) fn render(&mut self, target: SwTarget<'_>) -> Result<(), MpvError> {
        let SwTarget {
            width,
            height,
            stride,
            buffer,
        } = target;
        if width == 0 || height == 0 {
            return Err(MpvError::local("empty render target"));
        }
        let row = width as usize * 4;
        if stride < row || stride % 4 != 0 {
            return Err(MpvError::local(format!(
                "stride {stride} too small for width {width}"
            )));
        }
        let needed = stride * height as usize;
        if buffer.len() < needed {
            return Err(MpvError::local(format!(
                "buffer of {} bytes, need {needed}",
                buffer.len()
            )));
        }
        let mut size: [c_int; 2] = [width as c_int, height as c_int];
        let mut format = *b"bgr0\0";
        let mut stride_value: usize = stride;
        let mut params = [
            sys::mpv_render_param {
                type_: sys::mpv_render_param_type_MPV_RENDER_PARAM_SW_SIZE,
                data: size.as_mut_ptr().cast::<c_void>(),
            },
            sys::mpv_render_param {
                type_: sys::mpv_render_param_type_MPV_RENDER_PARAM_SW_FORMAT,
                data: format.as_mut_ptr().cast::<c_void>(),
            },
            sys::mpv_render_param {
                type_: sys::mpv_render_param_type_MPV_RENDER_PARAM_SW_STRIDE,
                data: (&mut stride_value as *mut usize).cast::<c_void>(),
            },
            sys::mpv_render_param {
                type_: sys::mpv_render_param_type_MPV_RENDER_PARAM_SW_POINTER,
                data: buffer.as_mut_ptr().cast::<c_void>(),
            },
            sys::mpv_render_param {
                type_: 0,
                data: std::ptr::null_mut(),
            },
        ];
        // SAFETY: valid context on its owner thread. SW_SIZE points to int[2],
        // SW_FORMAT to a NUL-terminated string, SW_STRIDE to a size_t and
        // SW_POINTER to a writable, 64-byte aligned buffer of at least
        // stride * height bytes (checked above); all outlive the call.
        check(unsafe { sys::mpv_render_context_render(self.raw.as_ptr(), params.as_mut_ptr()) })
    }

    /// Consumes the pending frame without drawing it
    /// (`MPV_RENDER_PARAM_SKIP_RENDERING`), so the core never waits on a
    /// frame that is rate-capped or has no target size. The target size is
    /// `None` only when no view reported one: a view laid out at zero size,
    /// hidden with `VideoView::set_visible(false)`, given another source, or
    /// released. A view that merely stops being painted keeps its last size.
    pub(crate) fn skip(&mut self) -> Result<(), MpvError> {
        let mut skip: c_int = 1;
        let mut params = [
            sys::mpv_render_param {
                type_: sys::mpv_render_param_type_MPV_RENDER_PARAM_SKIP_RENDERING,
                data: (&mut skip as *mut c_int).cast::<c_void>(),
            },
            sys::mpv_render_param {
                type_: 0,
                data: std::ptr::null_mut(),
            },
        ];
        // SAFETY: valid context on its owner thread; SKIP_RENDERING points to
        // an int; with it set no target surface is required.
        check(unsafe { sys::mpv_render_context_render(self.raw.as_ptr(), params.as_mut_ptr()) })
    }
}

impl Drop for SwRenderContext {
    fn drop(&mut self) {
        // SAFETY: valid context on its owner thread. Clearing the callback
        // first guarantees mpv no longer holds `self.callback`'s pointer when
        // the box is dropped; the context is freed before `_handle` (and with
        // it possibly the core) is released.
        unsafe {
            sys::mpv_render_context_set_update_callback(
                self.raw.as_ptr(),
                None,
                std::ptr::null_mut(),
            );
            sys::mpv_render_context_free(self.raw.as_ptr());
        }
        self.callback = None;
    }
}

/// One 64-byte, 64-byte-aligned block of pixel memory.
#[derive(Clone, Copy)]
#[repr(C, align(64))]
struct Block([u8; 64]);

/// A 64-byte aligned, zero-initialized byte buffer whose length is a multiple
/// of 64. Reused across frames by the render thread's pool.
pub(crate) struct AlignedBuffer {
    blocks: Vec<Block>,
}

impl AlignedBuffer {
    pub(crate) fn new(len: usize) -> Self {
        Self {
            blocks: vec![Block([0; 64]); len.div_ceil(64)],
        }
    }

    pub(crate) fn len(&self) -> usize {
        self.blocks.len() * 64
    }

    pub(crate) fn as_bytes(&self) -> &[u8] {
        // SAFETY: `Block` is `repr(C)` around `[u8; 64]` with size 64 and no
        // padding, so `blocks` is `len()` contiguous, initialized bytes.
        unsafe { std::slice::from_raw_parts(self.blocks.as_ptr().cast::<u8>(), self.len()) }
    }

    pub(crate) fn as_mut_ptr(&mut self) -> *mut u8 {
        self.blocks.as_mut_ptr().cast::<u8>()
    }
}

/// The libmpv client API version the linked library reports, as
/// `(major, minor)`.
pub fn client_api_version() -> (u32, u32) {
    // SAFETY: mpv_client_api_version takes no arguments and has no
    // preconditions.
    let version = unsafe { sys::mpv_client_api_version() } as u64;
    ((version >> 16) as u32, (version & 0xffff) as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn aligned_buffer_is_64_byte_aligned_and_rounded_up() {
        for len in [1, 63, 64, 65, 960 * 4 * 540, 1920 * 4 * 1080] {
            let mut buffer = AlignedBuffer::new(len);
            assert_eq!(buffer.as_mut_ptr() as usize % 64, 0, "len {len}");
            assert!(buffer.len() >= len);
            assert_eq!(buffer.len() % 64, 0);
            assert!(buffer.as_bytes().iter().all(|byte| *byte == 0));
        }
    }

    #[test]
    fn linked_libmpv_is_at_least_2_5() {
        let (major, minor) = client_api_version();
        assert!(
            (major, minor) >= (2, 5),
            "libmpv client API {major}.{minor}"
        );
    }

    #[test]
    fn error_strings_are_readable() {
        let error = MpvError::from_code(-5);
        assert!(!error.message().is_empty());
        assert_eq!(error.code(), -5);
    }
}
