//! Omatrack UI on gpui-kit components: the Omarchy theme bridge and the small
//! first-party domain components that gpui-component does not provide.
//!
//! Generic grids, palettes, title and status bars, notifications and dialogs
//! come from `gpui_kit::component`; this crate only adds what carries
//! telemetry meaning (lap roles, signed deltas, readouts). Every color is a
//! theme token; [`theme`] is the only module that knows literal colors.

mod readout;
mod role_chip;
mod swatch;
pub mod theme;

pub use readout::{
    DeltaSense, DeltaText, DeltaTrend, MISSING_VALUE, Readout, format_delta, format_value,
};
pub use role_chip::{LapRole, RoleChip};
pub use swatch::Swatch;
