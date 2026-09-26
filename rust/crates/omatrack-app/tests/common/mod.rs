//! Shared setup for the application's UI integration tests: an isolated
//! application (temporary XDG roots, built-in theme, no video, no startup
//! scan) and a small synthetic library.

#![allow(dead_code)]

use std::path::{Path, PathBuf};

use gpui_kit::component::Root;
use gpui_kit::{Entity, TestAppContext, WindowHandle};
use omatrack_app::{AppOptions, AppState, StateOptions, Workspace};
use omatrack_library::location::{DiscoveredFile, LocationId};
use omatrack_library::metadata::MetadataSources;
use omatrack_library::{
    CatalogRecord, Config, FileIdentity, LibrarySnapshot, Paths, RecordingSummary,
    effective_metadata,
};

/// A temporary home for one test's configuration, cache and state.
pub struct Sandbox {
    pub dir: tempfile::TempDir,
}

impl Sandbox {
    pub fn new() -> Self {
        Self {
            dir: tempfile::tempdir().expect("temporary directory"),
        }
    }

    pub fn paths(&self) -> Paths {
        Paths::with_roots(
            self.dir.path().join("config"),
            self.dir.path().join("cache"),
            self.dir.path().join("state"),
        )
    }

    /// Write `omatrack.yml` before the application starts.
    pub fn write_config(&self, yaml: &str) {
        let path = self.paths().config_file();
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, yaml).unwrap();
    }

    pub fn read_config(&self) -> Config {
        Config::load(&self.paths().config_file()).expect("omatrack.yml parses")
    }

    pub fn options(&self) -> StateOptions {
        StateOptions::isolated(self.paths())
    }
}

/// The running test application.
pub struct TestApp {
    pub window: WindowHandle<Root>,
    pub workspace: Entity<Workspace>,
    pub app: AppState,
}

/// Initialize the application with `state` and open the main window.
pub fn start(cx: &mut TestAppContext, state: StateOptions) -> TestApp {
    cx.update(|cx| omatrack_app::init_with(AppOptions::isolated(state), cx));
    open(cx)
}

/// Open (another) main window on the already initialized application.
pub fn open(cx: &mut TestAppContext) -> TestApp {
    let window = cx
        .update(omatrack_app::open_main_window)
        .expect("main window opens");
    let workspace = cx
        .update(|cx| {
            window
                .read(cx)
                .map(|root| root.view().clone().downcast::<Workspace>().ok())
        })
        .unwrap()
        .expect("Root wraps the Workspace");
    let app = cx.update(|cx| AppState::global(cx).clone());
    cx.run_until_parked();
    TestApp {
        window,
        workspace,
        app,
    }
}

fn summary(laps: &[(i32, f64, bool, &str)], numbered: bool) -> RecordingSummary {
    let mut start = 0.0;
    let laps: Vec<serde_json::Value> = laps
        .iter()
        .map(|(id, seconds, complete, kind)| {
            let mut lap = serde_json::json!({
                "id": id,
                "start_time": start,
                "end_time": start + seconds,
                "time_ms": seconds * 1000.0,
                "complete": complete,
                "pit_lap": false,
                "kind": kind,
            });
            if numbered {
                lap["source_number"] = serde_json::json!(id);
            }
            start += seconds;
            lap
        })
        .collect();
    serde_json::from_value(serde_json::json!({
        "format": "pds",
        "utc_start_ns": -1,
        "timezone": "",
        "duration_ns": (start * 1e9) as u64,
        "driver_id": 0.0,
        "has_video": false,
        "channel_count": 12,
        "laps": laps,
    }))
    .expect("synthetic summary")
}

fn record(
    path: &str,
    driver: &str,
    laps: &[(i32, f64, bool, &str)],
    numbered: bool,
) -> CatalogRecord {
    let path = PathBuf::from(path);
    let summary = summary(laps, numbered);
    let folder: serde_yaml::Mapping = serde_yaml::from_str(&format!(
        "track: {{name: Test Circuit}}\nsession: Q1\ndriver: {{name: {driver}}}\n"
    ))
    .unwrap();
    let config = Config::default();
    let metadata = effective_metadata(
        Path::new(&path),
        MetadataSources::new(&folder, &config).with_summary(Some(&summary)),
    );
    let file = DiscoveredFile::new(
        LocationId::new("synthetic"),
        path,
        FileIdentity::new(1, 1, 1, 0),
    );
    CatalogRecord::new(file, summary, metadata)
}

/// One track, one day, two recordings of five laps each (out, three
/// flying laps, in). The files do not exist: loading a lap fails, which is
/// enough to observe which lap a role asked for.
pub fn synthetic_snapshot() -> LibrarySnapshot {
    synthetic_snapshot_numbered(false)
}

/// [`synthetic_snapshot`]; with `numbered`, laps carry the recording's own
/// lap numbers (their ids), the way most loggers report them.
pub fn synthetic_snapshot_numbered(numbered: bool) -> LibrarySnapshot {
    let laps_a = [
        (1, 95.0, false, "out"),
        (2, 76.5, true, "flying"),
        (3, 75.25, true, "flying"),
        (4, 75.75, true, "flying"),
        (5, 90.0, false, "in"),
    ];
    let laps_b = [
        (1, 96.0, false, "out"),
        (2, 77.0, true, "flying"),
        (3, 76.0, true, "flying"),
        (4, 76.5, true, "flying"),
        (5, 91.0, false, "in"),
    ];
    LibrarySnapshot::build(vec![
        record(
            "/synthetic/2026-09-02/Run1_Q1.pds",
            "Ada",
            &laps_a,
            numbered,
        ),
        record(
            "/synthetic/2026-09-02/Run2_Q1.pds",
            "Grace",
            &laps_b,
            numbered,
        ),
    ])
}

/// Install [`synthetic_snapshot`] into the running application.
pub fn load_synthetic_library(test: &TestApp, cx: &mut TestAppContext) -> LibrarySnapshot {
    install_snapshot(test, cx, synthetic_snapshot())
}

/// Install `snapshot` into the running application.
pub fn install_snapshot(
    test: &TestApp,
    cx: &mut TestAppContext,
    snapshot: LibrarySnapshot,
) -> LibrarySnapshot {
    let library = test.app.library.clone();
    let installed = snapshot.clone();
    cx.update(|cx| library.update(cx, |library, cx| library.set_snapshot(installed, cx)));
    cx.run_until_parked();
    snapshot
}
