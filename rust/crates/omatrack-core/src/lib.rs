//! Omatrack analysis core.
//!
//! One normalized analytical truth for every telemetry format: recordings
//! are opened through the pinned `motorsport-telemetry-rs` parser crates,
//! mapped onto standard channels, split into classified laps and unified
//! into the canonical 50 Hz [`UnifiedLap`]. Alignment, delta and corner
//! analysis build on that one model so traces, readouts and video agree.
//!
//! No GPUI, no I/O beyond opening the recording: the CLI, the tests and the
//! app all run exactly this code.

pub mod alignment;
pub mod atlas_spatial;
pub mod cfmt;
pub mod comparison;
pub mod consistency;
pub mod corners;
pub mod laps;
pub mod mapping;
pub mod meta;
pub mod monotonic;
pub mod num;
pub mod overlay;
pub mod playback;
pub mod recording;
pub mod report;
pub mod session;
mod stopped;
pub mod track;
pub mod unify;
pub mod video_clock;

pub use comparison::Comparison;
pub use laps::{Lap, LapKind, classify_laps, fastest_lap_index, format_lap_time};
pub use mapping::{ChannelMapping, ChannelOverrides};
pub use recording::{OpenError, RawChannel, Recording};
pub use session::{Analysis, LoadOptions, LoadedLap, SessionError, load_lap};
pub use unify::{DistanceSource, UnifiedLap};
pub use video_clock::{VideoClock, VideoFileReference};

/// Identity of the converter whose normalization this build trusts:
/// `{native format version}-{pinned upstream rev, 12 hex}`. Caches of
/// normalized telemetry are keyed by it, so advancing the parser pin
/// regenerates them.
pub fn converter_generation() -> &'static str {
    static GENERATION: std::sync::OnceLock<String> = std::sync::OnceLock::new();
    GENERATION.get_or_init(|| {
        let rev = env!("OMATRACK_UPSTREAM_REV");
        let short = &rev[..rev.len().min(12)];
        format!("{}-{}", telemetry_format::FORMAT_VERSION, short)
    })
}
