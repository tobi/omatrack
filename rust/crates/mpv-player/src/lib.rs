//! mpv-player: libmpv playback rendered into GPUI.
//!
//! A reusable crate with no application dependency:
//!
//! - [`Player`] wraps one libmpv (client API ≥ 2.5) core: load, play/pause,
//!   exact and relative seeks, speed, mute, volume, a [`PlayerState`]
//!   snapshot and a [`PlayerEvent`] stream. An event thread mirrors mpv's
//!   properties; `time-pos` feeds only the lock-free [`PlaybackClock`].
//! - A render thread drives mpv's **software** render API and publishes
//!   opaque BGRA [`RenderImage`](gpui_kit::RenderImage)s at the display size
//!   into a latest-wins [`FrameSlot`], behind the [`FrameSource`] trait so a
//!   GPU-interop backend can replace it later.
//! - [`VideoView`] paints a source aspect-fit on a caller-supplied letterbox
//!   color, drops every replaced image from the sprite atlas, shows themed
//!   loading and error states, and requests animation frames while playing.
//!   The backend draws only while a view reports a size: hide a view that
//!   stays alive off screen with [`VideoView::set_visible`].
//! - [`Follower`] holds generic sync mechanics: given a target position and
//!   rate it answers "nothing", "set speed" or "hard seek".
//!
//! ```no_run
//! use mpv_player::{Player, PlayerOptions, VideoView};
//! # fn open(window: &mut gpui_kit::Window, cx: &mut gpui_kit::App) -> Result<(), mpv_player::MpvError> {
//! use gpui_kit::AppContext as _;
//! use gpui_kit::component::ActiveTheme as _;
//! let player = Player::new(PlayerOptions::default().muted(true))?;
//! player.load(std::path::Path::new("/path/to/video.mp4"))?;
//! let letterbox = cx.theme().background;
//! let view = cx.new(|cx| VideoView::new("primary-video", player.frame_source(), letterbox, window, cx));
//! # let _ = view; Ok(()) }
//! ```

#[expect(
    unsafe_code,
    reason = "All libmpv FFI and allocation operations are contained in this audited module."
)]
mod ffi;

mod clock;
mod events;
mod player;
#[expect(
    unsafe_code,
    reason = "The software renderer contains the documented unaligned pixel-copy kernel."
)]
mod render;
mod source;
mod state;
mod sync;
mod view;

pub use clock::{ClockSample, PlaybackClock};
pub use ffi::{EndReason, MpvError, client_api_version};
pub use player::{Player, PlayerEvent, PlayerOptions, PlayerState};
pub use render::{
    MAX_RENDER_WIDTH, RenderStats, WIDTH_ALIGNMENT, copy_opaque, force_opaque, render_size,
};
pub use source::{FrameSlot, FrameSource, MediaStatus, VideoFrame};
pub use sync::{FollowAction, FollowPolicy, Follower};
pub use view::{VideoSurface, VideoView, VideoViewEvent, aspect_fit};
