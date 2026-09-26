//! Omatrack 2.0: the GPUI telemetry workstation.
//!
//! The application composes the feature crates into one window: a
//! client-drawn title bar, a dock workspace of panels and a status bar, all
//! from `gpui_kit::component`. Analysis lives in `omatrack-core`, the
//! library model in `omatrack-library`, trace drawing in `omatrack-trace`,
//! video in `mpv-player`, and the theme bridge plus domain components in
//! `omatrack-ui`.
//!
//! - [`state`]: the entities (preferences, library, session, jobs, video)
//!   shared through [`AppState`].
//! - [`workspace`]: the root view, the dock layout and its persistence, the
//!   title and status bars, and action routing.
//! - [`panels`]: the dock panels.
//! - [`actions`] and `keymap`: every command and its default keys.
//! - [`commands`]: the command registry and palette.
//! - [`sync`]: video sync rules (identity, pacing, lap end, timelines).
//! - [`preferences`] and [`dialogs`]: the Preferences screen and the
//!   recording metadata / `TRACK.yml` dialogs.

pub mod actions;
mod app;
pub mod commands;
pub mod dialogs;
mod keymap;
pub mod panels;
pub mod preferences;
pub mod state;
pub mod sync;
pub mod workspace;

pub use app::{AppOptions, init, init_with, main_window_options, open_main_window, run};
pub use keymap::{
    LIBRARY_CONTEXT, PREFERENCES_CONTEXT, PREFERENCES_NAV_CONTEXT, SINGLE_KEY_PREDICATE,
    TRACE_EDIT_CONTEXT, WORKSPACE_CONTEXT,
};
pub use state::{AppState, StateOptions};
pub use workspace::{DOCK_AREA_ID, LAYOUT_VERSION, LayoutOrigin, Workspace};
