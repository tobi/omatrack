//! Application state: one entity per concern, shared through [`AppState`].
//!
//! | Entity | Owns |
//! | --- | --- |
//! | [`Preferences`] | `omatrack.yml`, debounced atomic saves |
//! | [`Library`] | locations, scans, the catalog snapshot, search and facets |
//! | [`Session`] | primary/reference laps and their analysis |
//! | [`Jobs`] | named running work for the status bar |
//! | [`VideoController`] | the primary mpv player |
//! | `ViewportState` / `CursorState` | the shared trace window and cursor (omatrack-trace) |
//!
//! Every parse, scan, unify and analysis runs on the background executor;
//! these entities only apply results.

pub mod jobs;
pub mod library;
pub mod preferences;
pub mod session;
pub mod video;

use std::path::PathBuf;

use gpui_kit::{App, AppContext as _, Entity, Global};
use omatrack_library::Paths;
use omatrack_trace::{CursorState, ViewportState};

pub use jobs::{Job, JobHandle, JobId, Jobs};
pub use library::{Library, LibraryEvent, RecordingSource, ScanStatus};
pub use preferences::{Preferences, PreferencesEvent};
pub use session::{LapInfo, LapRef, RoleSlot, RoleState, Session, SessionEvent};
pub use video::{ComposeLayout, VideoAvailability, VideoController, VideoEvent};

/// How the application state is built: where its files live, whether a
/// fresh library gets a default folder, whether video plays.
#[derive(Debug, Clone)]
pub struct StateOptions {
    paths: Paths,
    default_library: Option<PathBuf>,
    video: bool,
    audio_output: Option<String>,
    scan_on_start: bool,
}

impl StateOptions {
    /// XDG paths from the environment, `~/Documents/Telemetry` as the first
    /// library location, video on, a scan at startup.
    pub fn system() -> Self {
        Self {
            paths: Paths::from_env(),
            default_library: omatrack_library::paths::default_telemetry_dir(),
            video: true,
            audio_output: None,
            scan_on_start: true,
        }
    }

    /// Explicit roots (tests): no default library folder, no video, no
    /// startup scan.
    pub fn isolated(paths: Paths) -> Self {
        Self {
            paths,
            default_library: None,
            video: false,
            audio_output: None,
            scan_on_start: false,
        }
    }

    pub fn paths(mut self, paths: Paths) -> Self {
        self.paths = paths;
        self
    }

    pub fn default_library(mut self, directory: Option<PathBuf>) -> Self {
        self.default_library = directory;
        self
    }

    pub fn video(mut self, enabled: bool) -> Self {
        self.video = enabled;
        self
    }

    /// mpv `ao` (for example `null` in tests).
    pub fn audio_output(mut self, output: Option<String>) -> Self {
        self.audio_output = output;
        self
    }

    pub fn scan_on_start(mut self, scan: bool) -> Self {
        self.scan_on_start = scan;
        self
    }
}

/// Handles on every state entity. Cheap to clone; installed as a global
/// so dock panels rebuilt from a saved layout can reach it.
#[derive(Clone)]
#[non_exhaustive]
pub struct AppState {
    pub preferences: Entity<Preferences>,
    pub library: Entity<Library>,
    pub session: Entity<Session>,
    pub jobs: Entity<Jobs>,
    pub video: Entity<VideoController>,
    pub viewport: Entity<ViewportState>,
    pub cursor: Entity<CursorState>,
}

impl Global for AppState {}

impl AppState {
    /// Build every entity and install the global. With
    /// `scan_on_start` the first library scan starts at once.
    pub fn install(options: StateOptions, cx: &mut App) -> Self {
        let preferences = cx.new(|cx| Preferences::open(options.paths.clone(), cx));
        let jobs = cx.new(Jobs::new);
        let library = cx.new(|cx| {
            Library::new(
                preferences.clone(),
                jobs.clone(),
                options.default_library.clone(),
                cx,
            )
        });
        let session =
            cx.new(|cx| Session::new(library.clone(), preferences.clone(), jobs.clone(), cx));
        let video = cx.new(|cx| {
            VideoController::new(
                preferences.clone(),
                &session,
                options.video,
                options.audio_output.clone(),
                cx,
            )
        });
        let axis = match preferences.read(cx).config().trace.x_axis() {
            omatrack_library::config::XAxis::Distance => omatrack_trace::XAxis::Distance,
            omatrack_library::config::XAxis::Time => omatrack_trace::XAxis::Time,
        };
        let viewport = cx.new(|cx| {
            let mut state = ViewportState::new();
            state.set_axis(axis, cx);
            state
        });
        let cursor = cx.new(|_| CursorState::new());
        // Connected here, not deferred: outside an update a deferred
        // callback runs before the global exists and the video would never
        // drive the cursor.
        video.update(cx, |video, cx| {
            video.connect(cursor.clone(), viewport.clone(), cx)
        });
        let state = Self {
            preferences,
            library,
            session,
            jobs,
            video,
            viewport,
            cursor,
        };
        cx.set_global(state.clone());
        if options.scan_on_start {
            state.library.update(cx, |library, cx| library.rescan(cx));
        }
        state
    }

    pub fn global(cx: &App) -> &Self {
        cx.global::<Self>()
    }

    pub fn try_global(cx: &App) -> Option<&Self> {
        cx.try_global::<Self>()
    }
}
