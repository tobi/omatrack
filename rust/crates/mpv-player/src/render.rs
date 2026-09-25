//! The software render thread.
//!
//! One thread per player owns the mpv SW render context. mpv's update
//! callback only flips a flag and signals a condvar; the thread then calls
//! `mpv_render_context_update` and, when a frame is due, renders it in `bgr0`
//! into a pooled 64-byte-aligned buffer at the view's device size, converts it
//! to a tightly packed opaque BGRA image (GPUI's `RenderImage` byte order),
//! and publishes it into the player's latest-wins [`FrameSlot`].
//!
//! Per render.h the thread calls nothing but `mpv_render_*` functions, and it
//! never holds its own lock while inside mpv.
//!
//! [`FrameSlot`]: crate::FrameSlot

use std::sync::atomic::Ordering;
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use gpui_kit::{DevicePixels, RenderImage, Size};
use smallvec::smallvec;

use crate::ffi::{AlignedBuffer, MpvError, SwRenderContext, SwTarget};
use crate::source::VideoFrame;
use crate::state::Shared;

/// Frames are never rendered wider than this many device pixels; GPUI scales
/// the image the rest of the way at no CPU cost.
pub const MAX_RENDER_WIDTH: u32 = 2560;

/// Render widths are multiples of this, so a `bgr0` row (width × 4 bytes) is
/// a multiple of 64 bytes and every row starts 64-byte aligned.
pub const WIDTH_ALIGNMENT: u32 = 16;

/// Chooses the software render size for a view of `requested` device pixels.
///
/// With a known `video` size the result keeps the video's aspect ratio
/// (fitted inside `requested`, never upscaled past the native size, so the
/// CPU never scales up what the GPU can scale for free). The width is capped
/// at `max_width` and rounded down to a multiple of [`WIDTH_ALIGNMENT`]; the
/// height follows the aspect ratio. Returns `None` for an empty request.
pub fn render_size(
    requested: Size<DevicePixels>,
    video: Option<(u32, u32)>,
    max_width: u32,
) -> Option<(u32, u32)> {
    let requested_width = u32::try_from(requested.width.0)
        .ok()
        .filter(|width| *width > 0)?;
    let requested_height = u32::try_from(requested.height.0)
        .ok()
        .filter(|height| *height > 0)?;
    let max_width = max_width.max(WIDTH_ALIGNMENT);

    let (mut width, mut height) = match video {
        Some((video_width, video_height)) if video_width > 0 && video_height > 0 => {
            let scale = (f64::from(requested_width) / f64::from(video_width))
                .min(f64::from(requested_height) / f64::from(video_height))
                .min(1.0);
            (
                f64::from(video_width) * scale,
                f64::from(video_height) * scale,
            )
        }
        _ => (f64::from(requested_width), f64::from(requested_height)),
    };
    if width > f64::from(max_width) {
        height *= f64::from(max_width) / width;
        width = f64::from(max_width);
    }

    let aligned_width =
        ((width.floor() as u32) / WIDTH_ALIGNMENT * WIDTH_ALIGNMENT).max(WIDTH_ALIGNMENT);
    let aligned_height = match video {
        Some((video_width, video_height)) if video_width > 0 && video_height > 0 => {
            (f64::from(aligned_width) * f64::from(video_height) / f64::from(video_width)).round()
        }
        _ => height.round(),
    };
    Some((aligned_width, (aligned_height as u32).max(1)))
}

/// Copies a `bgr0` surface into a tightly packed BGRA buffer, forcing every
/// alpha byte to 0xFF (mpv leaves the `0` byte undefined).
///
/// This runs once per video frame over up to ~15 MB, so it is written to be
/// fast even in unoptimized builds: one fused copy-and-mask pass over 8-byte
/// words instead of a per-pixel iterator chain.
///
/// # Panics
/// If `stride < width * 4` or `source` is shorter than the surface.
pub fn copy_opaque(source: &[u8], width: u32, height: u32, stride: usize) -> Vec<u8> {
    let row = width as usize * 4;
    let rows = height as usize;
    let len = row * rows;
    assert!(
        stride >= row,
        "stride {stride} shorter than a row of {row} bytes"
    );
    if rows > 0 {
        assert!(
            source.len() >= stride * (rows - 1) + row,
            "source of {} bytes too short for {width}x{height} at stride {stride}",
            source.len()
        );
    }
    let mut pixels = Vec::<u8>::with_capacity(len);
    let destination = pixels.as_mut_ptr();
    for y in 0..rows {
        // SAFETY: row `y` of the source spans `y * stride .. y * stride + row`,
        // in bounds by the assertion above; the destination row
        // `y * row .. (y + 1) * row` lies within the `len` bytes reserved.
        // Source and destination are distinct allocations.
        unsafe {
            opaque_copy(
                source.as_ptr().add(y * stride),
                destination.add(y * row),
                row,
            )
        };
    }
    // SAFETY: every byte in `0..len` was written by `opaque_copy` above.
    unsafe { pixels.set_len(len) };
    pixels
}

/// Sets every 4th byte (alpha in BGRA) to 0xFF, in place.
///
/// # Panics
/// If the length is not a multiple of 4.
pub fn force_opaque(pixels: &mut [u8]) {
    assert!(
        pixels.len().is_multiple_of(4),
        "BGRA data must be whole pixels"
    );
    let pointer = pixels.as_mut_ptr();
    // SAFETY: source and destination are the same valid, writable region of
    // `pixels.len()` bytes; `opaque_copy` reads each word before writing it.
    unsafe { opaque_copy(pointer, pointer, pixels.len()) };
}

/// Copies `len` bytes (a multiple of 4) from `source` to `destination`,
/// setting the 4th byte of every pixel to 0xFF.
///
/// # Safety
/// `source` must be readable and `destination` writable for `len` bytes, and
/// the two regions must be either identical or non-overlapping.
unsafe fn opaque_copy(source: *const u8, destination: *mut u8, len: usize) {
    // Alpha is the byte at offset 3 of each pixel; a native-endian mask built
    // from bytes is correct on any endianness.
    const ALPHA: u64 = u64::from_ne_bytes([0, 0, 0, 0xFF, 0, 0, 0, 0xFF]);
    // 64-byte blocks: few, large moves keep per-iteration overhead (and the
    // debug-build checks on it) small.
    let blocks = len / 64;
    let source_blocks = source.cast::<[u64; 8]>();
    let destination_blocks = destination.cast::<[u64; 8]>();
    for index in 0..blocks {
        // SAFETY: `index < len / 64`, so the 64 bytes at `index * 64` are in
        // bounds for both regions (caller contract); unaligned accesses are
        // used because a `Vec<u8>` has no alignment guarantee.
        unsafe {
            let [a, b, c, d, e, f, g, h] = source_blocks.add(index).read_unaligned();
            destination_blocks.add(index).write_unaligned([
                a | ALPHA,
                b | ALPHA,
                c | ALPHA,
                d | ALPHA,
                e | ALPHA,
                f | ALPHA,
                g | ALPHA,
                h | ALPHA,
            ]);
        }
    }
    let words = len / 8;
    let source_words = source.cast::<u64>();
    let destination_words = destination.cast::<u64>();
    for index in blocks * 8..words {
        // SAFETY: `index < len / 8`: the 8 bytes at `index * 8` are in bounds.
        unsafe {
            let word = source_words.add(index).read_unaligned();
            destination_words.add(index).write_unaligned(word | ALPHA);
        }
    }
    if len % 8 == 4 {
        let tail = words * 8;
        // SAFETY: the last pixel `tail .. tail + 4` is in bounds (len % 8 == 4).
        unsafe {
            let pixel = source.add(tail).cast::<[u8; 4]>().read_unaligned();
            destination
                .add(tail)
                .cast::<[u8; 4]>()
                .write_unaligned([pixel[0], pixel[1], pixel[2], 0xFF]);
        }
    }
}

/// Software render timings, for diagnostics and benchmarks.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct RenderStats {
    /// Frames rendered and published.
    pub frames: u64,
    /// Frames consumed without drawing (no view, rate cap).
    pub skipped: u64,
    /// Total time inside `mpv_render_context_render`.
    pub render_time: Duration,
    /// Total time converting to opaque BGRA and wrapping the image.
    pub convert_time: Duration,
    /// Slowest single `mpv_render_context_render`.
    pub max_render_time: Duration,
    /// Size of the last rendered frame in device pixels.
    pub last_size: (u32, u32),
}

impl RenderStats {
    /// Average milliseconds per frame inside mpv's software renderer.
    pub fn average_render_ms(&self) -> f64 {
        if self.frames == 0 {
            0.0
        } else {
            self.render_time.as_secs_f64() * 1000.0 / self.frames as f64
        }
    }

    /// Average milliseconds per frame spent in the BGRA conversion.
    pub fn average_convert_ms(&self) -> f64 {
        if self.frames == 0 {
            0.0
        } else {
            self.convert_time.as_secs_f64() * 1000.0 / self.frames as f64
        }
    }
}

#[derive(Default)]
struct RenderRequest {
    /// mpv's update callback fired.
    update_pending: bool,
    /// Size or video geometry changed: redraw the current frame.
    redraw: bool,
    target: Option<Size<DevicePixels>>,
    max_fps: Option<f64>,
    stop: bool,
}

/// The render thread's control block, shared with the update callback, the
/// event thread and the UI.
pub(crate) struct RenderControl {
    request: Mutex<RenderRequest>,
    wake: Condvar,
}

impl RenderControl {
    pub(crate) fn new(max_fps: Option<f64>) -> Self {
        Self {
            request: Mutex::new(RenderRequest {
                max_fps: sanitize_fps(max_fps),
                ..RenderRequest::default()
            }),
            wake: Condvar::new(),
        }
    }

    fn lock(&self) -> MutexGuard<'_, RenderRequest> {
        self.request
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    /// Called from mpv's threads: only records and signals.
    fn frame_ready(&self) {
        self.lock().update_pending = true;
        self.wake.notify_one();
    }

    pub(crate) fn set_target_size(&self, size: Option<Size<DevicePixels>>) {
        let mut request = self.lock();
        if request.target != size {
            request.target = size;
            request.redraw = true;
            drop(request);
            self.wake.notify_one();
        }
    }

    pub(crate) fn set_max_fps(&self, max_fps: Option<f64>) {
        self.lock().max_fps = sanitize_fps(max_fps);
    }

    pub(crate) fn request_redraw(&self) {
        self.lock().redraw = true;
        self.wake.notify_one();
    }

    fn stop(&self) {
        self.lock().stop = true;
        self.wake.notify_one();
    }
}

fn sanitize_fps(max_fps: Option<f64>) -> Option<f64> {
    max_fps.filter(|fps| fps.is_finite() && *fps > 0.0)
}

/// Owns the render thread; stopping joins it, which frees the render context
/// on that thread.
pub(crate) struct RenderThread {
    control: Arc<RenderControl>,
    thread: Option<JoinHandle<()>>,
}

impl RenderThread {
    pub(crate) fn start(
        mut context: SwRenderContext,
        control: Arc<RenderControl>,
        shared: Arc<Shared>,
    ) -> std::io::Result<Self> {
        let callback_control = Arc::clone(&control);
        context.set_update_callback(move || callback_control.frame_ready());
        let thread_control = Arc::clone(&control);
        let thread = std::thread::Builder::new()
            .name("mpv-sw-render".into())
            .spawn(move || render_loop(context, &thread_control, &shared))?;
        Ok(Self {
            control,
            thread: Some(thread),
        })
    }

    pub(crate) fn stop(&mut self) {
        self.control.stop();
        if let Some(thread) = self.thread.take()
            && thread.join().is_err()
        {
            log::error!("mpv render thread panicked");
        }
    }
}

impl Drop for RenderThread {
    fn drop(&mut self) {
        self.stop();
    }
}

/// A single reusable render target, reallocated only when the size changes.
#[derive(Default)]
struct BufferPool {
    buffer: Option<AlignedBuffer>,
}

impl BufferPool {
    fn get(&mut self, len: usize) -> &mut AlignedBuffer {
        if self
            .buffer
            .as_ref()
            .is_none_or(|buffer| buffer.len() < len || buffer.len() > len * 2)
        {
            self.buffer = Some(AlignedBuffer::new(len));
        }
        self.buffer.as_mut().expect("allocated above")
    }
}

fn render_loop(mut context: SwRenderContext, control: &RenderControl, shared: &Shared) {
    let mut pool = BufferPool::default();
    let mut pacer = FramePacer::default();
    loop {
        let (update, redraw, target, max_fps) = {
            let mut request = control.lock();
            while !request.stop && !request.update_pending && !request.redraw {
                request = control
                    .wake
                    .wait(request)
                    .unwrap_or_else(|poison| poison.into_inner());
            }
            if request.stop {
                break;
            }
            let update = std::mem::take(&mut request.update_pending);
            let redraw = std::mem::take(&mut request.redraw);
            (update, redraw, request.target, request.max_fps)
        };

        let frame_due = update && context.update();
        if !frame_due && !redraw {
            continue;
        }

        let video = shared.video_size();
        let size = target.and_then(|target| render_size(target, video, MAX_RENDER_WIDTH));
        let now = Instant::now();
        let capped = frame_due && !redraw && !pacer.admits(now, max_fps);
        let (Some((width, height)), Some(_)) = (size, video) else {
            skip(&mut context, frame_due, shared);
            continue;
        };
        if capped {
            skip(&mut context, frame_due, shared);
            continue;
        }

        let epoch = shared.load_epoch.load(Ordering::Acquire);
        match render_frame(&mut context, &mut pool, width, height, shared) {
            Ok(frame) => {
                pacer.rendered(now, max_fps);
                // A `load` raced this render: the frame shows the old file.
                if shared.load_epoch.load(Ordering::Acquire) == epoch {
                    shared.frames.publish(Some(frame));
                }
            }
            Err(error) => log::warn!("mpv software render failed: {error}"),
        }
    }
    // `context` drops here, on the thread that rendered with it.
}

/// Frame-rate cap bookkeeping: admits a frame once its slot is (nearly) due,
/// on a fixed cadence rather than "time since the last render", so a 30 fps
/// cap on 60 fps video renders every second frame instead of every third.
#[derive(Default)]
struct FramePacer {
    next_due: Option<Instant>,
}

impl FramePacer {
    fn admits(&self, now: Instant, max_fps: Option<f64>) -> bool {
        match (max_fps, self.next_due) {
            (Some(fps), Some(next_due)) => {
                // A quarter interval of slack absorbs callback jitter.
                now + Duration::from_secs_f64(0.25 / fps) >= next_due
            }
            _ => true,
        }
    }

    fn rendered(&mut self, now: Instant, max_fps: Option<f64>) {
        self.next_due = max_fps.map(|fps| {
            let interval = Duration::from_secs_f64(1.0 / fps);
            match self.next_due {
                // Keep the cadence, but never bank more than one slot.
                Some(next_due) if next_due + interval > now => next_due + interval,
                _ => now + interval,
            }
        });
    }
}

fn skip(context: &mut SwRenderContext, frame_due: bool, shared: &Shared) {
    if !frame_due {
        return;
    }
    if let Err(error) = context.skip() {
        log::debug!("mpv skip render failed: {error}");
    }
    shared
        .stats
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .skipped += 1;
}

fn render_frame(
    context: &mut SwRenderContext,
    pool: &mut BufferPool,
    width: u32,
    height: u32,
    shared: &Shared,
) -> Result<VideoFrame, MpvError> {
    let stride = width as usize * 4;
    let buffer = pool.get(stride * height as usize);
    let started = Instant::now();
    context.render(SwTarget {
        width,
        height,
        stride,
        buffer,
    })?;
    let rendered = Instant::now();
    let media_time = shared.time_pos.load();

    let pixels = copy_opaque(buffer.as_bytes(), width, height, stride);
    let image = image::RgbaImage::from_raw(width, height, pixels)
        .ok_or_else(|| MpvError::local("frame buffer size mismatch"))?;
    let render_image = Arc::new(RenderImage::new(smallvec![image::Frame::new(image)]));
    let converted = Instant::now();

    let render_time = rendered - started;
    {
        let mut stats = shared
            .stats
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        stats.frames += 1;
        stats.render_time += render_time;
        stats.convert_time += converted - rendered;
        stats.max_render_time = stats.max_render_time.max(render_time);
        stats.last_size = (width, height);
    }
    Ok(VideoFrame::new(
        render_image,
        media_time,
        converted - started,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn device(width: i32, height: i32) -> Size<DevicePixels> {
        Size::new(DevicePixels(width), DevicePixels(height))
    }

    #[test]
    fn render_size_keeps_the_video_aspect_and_aligns_the_width() {
        let hd = Some((1920, 1080));
        assert_eq!(
            render_size(device(960, 540), hd, MAX_RENDER_WIDTH),
            Some((960, 540))
        );
        // Letterboxed view: fit by width, round to 16, height from aspect.
        assert_eq!(
            render_size(device(1000, 1000), hd, MAX_RENDER_WIDTH),
            Some((992, 558))
        );
        // Pillarboxed view: fit by height.
        assert_eq!(
            render_size(device(3000, 540), hd, MAX_RENDER_WIDTH),
            Some((960, 540))
        );
        for (width, height) in [(333, 211), (1001, 999), (17, 9), (2559, 1439)] {
            let (w, _) = render_size(device(width, height), hd, MAX_RENDER_WIDTH).unwrap();
            assert_eq!(w % WIDTH_ALIGNMENT, 0, "{width}x{height}");
            assert_eq!((w as usize * 4) % 64, 0);
        }
    }

    #[test]
    fn render_size_never_upscales_and_caps_the_width() {
        assert_eq!(
            render_size(device(3840, 2160), Some((1920, 1080)), MAX_RENDER_WIDTH),
            Some((1920, 1080))
        );
        assert_eq!(
            render_size(device(5120, 2880), Some((3840, 2160)), MAX_RENDER_WIDTH),
            Some((2560, 1440))
        );
        assert_eq!(
            render_size(device(5000, 1000), None, MAX_RENDER_WIDTH),
            Some((2560, 512))
        );
    }

    #[test]
    fn render_size_without_video_uses_the_request() {
        assert_eq!(
            render_size(device(1001, 500), None, MAX_RENDER_WIDTH),
            Some((992, 500))
        );
        assert_eq!(
            render_size(device(5, 5), None, MAX_RENDER_WIDTH),
            Some((16, 5))
        );
        assert_eq!(render_size(device(0, 500), None, MAX_RENDER_WIDTH), None);
        assert_eq!(render_size(device(500, -1), None, MAX_RENDER_WIDTH), None);
    }

    #[test]
    fn copy_opaque_forces_alpha_and_drops_row_padding() {
        let width = 3;
        let height = 2;
        let stride = 16;
        let mut source = vec![0u8; stride * height];
        for (index, byte) in source.iter_mut().enumerate() {
            *byte = index as u8;
        }
        let pixels = copy_opaque(&source, width, height as u32, stride);
        assert_eq!(pixels.len(), 3 * 4 * 2);
        for (y, row) in pixels.chunks_exact(12).enumerate() {
            for (x, pixel) in row.chunks_exact(4).enumerate() {
                let base = y * stride + x * 4;
                assert_eq!(&pixel[..3], &source[base..base + 3]);
                assert_eq!(pixel[3], 0xFF);
            }
        }
    }

    #[test]
    #[ignore = "timing probe"]
    fn convert_timing_probe() {
        for (width, height) in [(960u32, 540u32), (1920, 1080)] {
            let source = vec![0x40u8; width as usize * 4 * height as usize];
            let started = std::time::Instant::now();
            let rounds = 20;
            for _ in 0..rounds {
                std::hint::black_box(copy_opaque(&source, width, height, width as usize * 4));
            }
            let ms = started.elapsed().as_secs_f64() * 1000.0 / f64::from(rounds);
            eprintln!("copy_opaque {width}x{height}: {ms:.2} ms");
        }
    }

    #[test]
    fn frame_pacer_halves_60_fps_at_a_30_fps_cap() {
        let mut pacer = FramePacer::default();
        let start = Instant::now();
        let cap = Some(30.0);
        let mut rendered = 0;
        for frame in 0..120u32 {
            // 60 fps arrivals with ±2 ms jitter.
            let jitter = if frame % 2 == 0 { 2.0 } else { -2.0 };
            let now = start
                + Duration::from_secs_f64(
                    (f64::from(frame) * 1000.0 / 60.0 + 5.0 + jitter) / 1000.0,
                );
            if pacer.admits(now, cap) {
                pacer.rendered(now, cap);
                rendered += 1;
            }
        }
        assert!(
            (58..=62).contains(&rendered),
            "{rendered} of 120 frames at a 30 fps cap"
        );

        let mut uncapped = FramePacer::default();
        uncapped.rendered(start, None);
        assert!(uncapped.admits(start, None));
    }

    #[test]
    fn force_opaque_touches_only_alpha() {
        let mut pixels: Vec<u8> = (0..(64 * 3 + 8)).map(|value| value as u8).collect();
        let original = pixels.clone();
        force_opaque(&mut pixels);
        for (index, (after, before)) in pixels.iter().zip(&original).enumerate() {
            if index % 4 == 3 {
                assert_eq!(*after, 0xFF);
            } else {
                assert_eq!(after, before);
            }
        }
    }
}
