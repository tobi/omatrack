//! The seam between a video backend and the view that paints it.
//!
//! A backend (today the libmpv software renderer, later possibly an EGL/PBO
//! path) publishes decoded frames as GPUI [`RenderImage`]s into a
//! latest-wins [`FrameSlot`] and implements [`FrameSource`]. The
//! [`VideoView`](crate::VideoView) only ever talks to that trait, so swapping
//! the backend, or driving the view from a test double, needs no view change.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use gpui_kit::{DevicePixels, RenderImage, Size};
use smallvec::smallvec;

/// One decoded, displayable video frame.
///
/// The image is BGRA with opaque alpha, as GPUI's sprite atlas expects.
/// Cloning is cheap (the pixels are shared).
#[derive(Clone, Debug)]
pub struct VideoFrame {
    image: Arc<RenderImage>,
    media_time: f64,
    render_time: Duration,
}

impl VideoFrame {
    /// Wraps an already-built image.
    pub fn new(image: Arc<RenderImage>, media_time: f64, render_time: Duration) -> Self {
        Self {
            image,
            media_time,
            render_time,
        }
    }

    /// Builds a frame from tightly packed BGRA bytes (`width * height * 4`).
    /// Returns `None` when the byte count does not match the dimensions.
    pub fn from_bgra(width: u32, height: u32, bytes: Vec<u8>, media_time: f64) -> Option<Self> {
        let buffer = image::RgbaImage::from_raw(width, height, bytes)?;
        let render_image = RenderImage::new(smallvec![image::Frame::new(buffer)]);
        Some(Self::new(
            Arc::new(render_image),
            media_time,
            Duration::ZERO,
        ))
    }

    /// The GPUI image to paint.
    pub fn image(&self) -> &Arc<RenderImage> {
        &self.image
    }

    /// The frame's size in device pixels.
    pub fn size(&self) -> Size<DevicePixels> {
        self.image.size(0)
    }

    /// Media time (seconds) the player reported when the frame was rendered.
    pub fn media_time(&self) -> f64 {
        self.media_time
    }

    /// Wall time the backend spent producing this frame.
    pub fn render_time(&self) -> Duration {
        self.render_time
    }
}

/// What a source is currently doing, for the view's loading and error states.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum MediaStatus {
    /// Nothing is loaded.
    #[default]
    Idle,
    /// A file is opening; no frame yet.
    Loading,
    /// A file is open.
    Ready,
    /// Opening or playing failed; the message is user-presentable.
    Failed(String),
}

/// A producer of video frames for [`VideoView`](crate::VideoView).
///
/// Implementations are shared between their producer thread(s) and the UI,
/// so every method takes `&self` and must be cheap and non-blocking: the view
/// calls them from render and prepaint.
pub trait FrameSource: Send + Sync + 'static {
    /// The most recently published frame, if any.
    fn latest_frame(&self) -> Option<VideoFrame>;

    /// A counter bumped on every publish (including publishing "no frame").
    /// The view compares it to decide whether its image is stale.
    fn frame_generation(&self) -> u64;

    /// A latest-wins wake-up: receives `()` after a publish or a status
    /// change. Intended for a single consumer (the view showing this source).
    fn frame_signal(&self) -> async_channel::Receiver<()>;

    /// The device-pixel size the view paints into, or `None` when it is not
    /// visible. Backends render at (at most) this size.
    fn set_target_size(&self, size: Option<Size<DevicePixels>>);

    /// Current loading/error state.
    fn status(&self) -> MediaStatus;

    /// Whether the media clock is advancing. The view requests an animation
    /// frame per display frame while this is true.
    fn is_playing(&self) -> bool;
}

/// A latest-wins frame slot with a wake-up channel, the building block for
/// [`FrameSource`] implementations.
///
/// Publishing replaces the previous frame; frames nobody painted are simply
/// dropped (they never reached the GPU atlas).
pub struct FrameSlot {
    frame: Mutex<Option<VideoFrame>>,
    generation: AtomicU64,
    signal_tx: async_channel::Sender<()>,
    signal_rx: async_channel::Receiver<()>,
}

impl Default for FrameSlot {
    fn default() -> Self {
        Self::new()
    }
}

impl FrameSlot {
    /// An empty slot.
    pub fn new() -> Self {
        let (signal_tx, signal_rx) = async_channel::bounded(1);
        Self {
            frame: Mutex::new(None),
            generation: AtomicU64::new(0),
            signal_tx,
            signal_rx,
        }
    }

    /// Replaces the current frame (or clears it with `None`) and wakes the
    /// consumer.
    #[expect(
        clippy::significant_drop_tightening,
        reason = "Publish the value and its generation/count in the same critical section."
    )]
    pub fn publish(&self, frame: Option<VideoFrame>) {
        let previous = {
            let mut slot = self
                .frame
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner);
            let previous = std::mem::replace(&mut *slot, frame);
            self.generation.fetch_add(1, Ordering::AcqRel);
            previous
        };
        // Drop the replaced frame's pixels outside the lock.
        drop(previous);
        self.signal();
    }

    /// Wakes the consumer without changing the frame (for status changes).
    pub fn signal(&self) {
        // Capacity 1: a full channel already carries a pending wake-up.
        #[expect(
            clippy::let_underscore_must_use,
            reason = "Bounded wakeups coalesce when full; a closed receiver means the consumer has gone away."
        )]
        let _ = self.signal_tx.try_send(());
    }

    /// The current frame.
    pub fn latest(&self) -> Option<VideoFrame> {
        self.frame
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone()
    }

    /// The publish counter.
    pub fn generation(&self) -> u64 {
        self.generation.load(Ordering::Acquire)
    }

    /// The wake-up receiver.
    pub fn receiver(&self) -> async_channel::Receiver<()> {
        self.signal_rx.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn from_bgra_checks_the_byte_count() {
        assert!(VideoFrame::from_bgra(2, 2, vec![0; 16], 0.0).is_some());
        assert!(VideoFrame::from_bgra(2, 2, vec![0; 15], 0.0).is_none());
        let frame = VideoFrame::from_bgra(4, 2, vec![0; 32], 1.5).unwrap();
        assert_eq!(frame.size(), Size::new(DevicePixels(4), DevicePixels(2)));
        assert_eq!(frame.media_time(), 1.5);
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn slot_is_latest_wins_and_coalesces_signals() {
        let slot = FrameSlot::new();
        let receiver = slot.receiver();
        assert_eq!(slot.generation(), 0);
        slot.publish(VideoFrame::from_bgra(1, 1, vec![0; 4], 1.0));
        slot.publish(VideoFrame::from_bgra(1, 1, vec![0; 4], 2.0));
        assert_eq!(slot.generation(), 2);
        assert_eq!(slot.latest().unwrap().media_time(), 2.0);
        assert!(receiver.try_recv().is_ok());
        assert!(receiver.try_recv().is_err(), "signals coalesce to one");
        slot.publish(None);
        assert!(slot.latest().is_none());
        assert_eq!(slot.generation(), 3);
    }
}
