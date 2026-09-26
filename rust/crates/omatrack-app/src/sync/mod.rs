//! Telemetry <-> video sync: the GPUI-free half of the video controller.
//!
//! One analytical truth (AGENTS.md invariants 5 and 6): the cursor, the
//! readouts, the traces and both videos derive from the primary lap's
//! timeline and the pair's one `Comparison`.
//!
//! - [`timeline`]: one lap's map between video, telemetry and lap fraction.
//! - [`pacing`]: the reference follower (`omatrack_core::playback` rules
//!   through `mpv_player::Follower`).
//! - [`lap_end`]: the per-lap countdown and continuous adoption.
//! - [`identity`]: whether a video's clock may be trusted (BLAKE3).
//!
//! The primary recording is the clock: 1x (0.25x in slow motion), never
//! rate-corrected. Once per display frame the video panel pulls its
//! `PlaybackClock::estimate` and the controller maps it to a lap fraction
//! for `CursorState` alone.

pub mod identity;
pub mod lap_end;
pub mod pacing;
pub mod timeline;

pub use identity::IdentityStatus;
pub use lap_end::{LapAdvance, LapEnd};
pub use pacing::{PacerCommand, PacerInput, PairMap, ReferencePacer};
pub use timeline::{LapPosition, LapTimeline, VideoMap};
