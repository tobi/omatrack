//! Omatrack UI on gpui-kit components: the Omarchy theme bridge and the small
//! first-party domain components that gpui-component does not provide.
//!
//! Generic grids, palettes, title and status bars, notifications and dialogs
//! come from `gpui_kit::component`; this crate only adds what carries
//! telemetry meaning: lap roles ([`RoleChip`]), signed deltas and readouts
//! ([`DeltaText`], [`Readout`]), a session's laps ([`LapStrip`]) and the
//! telemetry HUD over the video ([`VideoHud`]). Every color is a theme token;
//! [`theme`] is the only module that knows literal colors.

mod lap_strip;
mod readout;
mod role_chip;
mod swatch;
pub mod theme;
mod video_hud;

pub use lap_strip::{
    CellSpan, LapSelect, LapStrip, LapStripItem, MAX_GAP, MIN_CELL, PIT_STOP_CELL, StripCells,
    lap_strip_layout, strip_cells,
};
pub use readout::{
    DeltaSense, DeltaText, DeltaTrend, MISSING_VALUE, Readout, format_delta, format_value,
};
pub use role_chip::{LapRole, RoleChip};
pub use swatch::Swatch;
pub use video_hud::{
    GAP_RANGE_M, HudPosition, HudVariant, VideoHud, format_gap, format_gear, gap_position,
};
