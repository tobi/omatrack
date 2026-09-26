//! UI integration tests for the Preferences screen and the library dialogs
//! (recording metadata, `TRACK.yml`). Every configuration root, library
//! folder and `TRACK.yml` lives in a temporary directory.

mod common;

use std::path::{Path, PathBuf};
use std::time::Duration;

use gpui_kit::component::WindowExt as _;
use gpui_kit::test::TestWindowExt as _;
use gpui_kit::{AppContext as _, Focusable as _, TestAppContext};
use omatrack_app::actions::OpenPreferences;
use omatrack_app::dialogs::{EditFolderMetadata, EditRecordingMetadata};
use omatrack_app::panels::PanelKind;
use omatrack_app::preferences::{PreferencesSection, PreferencesView};
use omatrack_library::location::{DiscoveredFile, LocationId};
use omatrack_library::metadata::MetadataSources;
use omatrack_library::{
    CatalogRecord, Config, FileIdentity, LibrarySnapshot, RecordingSummary, effective_metadata,
    track_yml,
};
use serde_yaml::{Mapping, Value};

/// A sandbox whose library folder is a configured location.
struct Library {
    sandbox: common::Sandbox,
    root: PathBuf,
}

impl Library {
    fn new() -> Self {
        let sandbox = common::Sandbox::new();
        let root = sandbox.dir.path().join("library");
        std::fs::create_dir_all(&root).unwrap();
        sandbox.write_config(&format!(
            "locations:\n  - type: folder\n    target: {}\n",
            root.display()
        ));
        Self { sandbox, root }
    }

    /// Write `TRACK.yml` into `folder` (relative to the library root).
    fn write_track_yml(&self, folder: &str, text: &str) -> PathBuf {
        let directory = self.root.join(folder);
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join(track_yml::FILE_NAME);
        std::fs::write(&path, text).unwrap();
        path
    }
}

fn summary() -> RecordingSummary {
    serde_json::from_value(serde_json::json!({
        "format": "pds",
        "utc_start_ns": -1,
        "timezone": "",
        "duration_ns": 300_000_000_000u64,
        "driver_id": 0.0,
        "has_video": false,
        "channel_count": 12,
        "laps": [
            {"id": 1, "start_time": 0.0, "end_time": 95.0, "time_ms": 95000.0,
             "complete": false, "pit_lap": false, "kind": "out"},
            {"id": 2, "start_time": 95.0, "end_time": 171.5, "time_ms": 76500.0,
             "complete": true, "pit_lap": false, "kind": "flying"},
            {"id": 3, "start_time": 171.5, "end_time": 261.5, "time_ms": 90000.0,
             "complete": false, "pit_lap": false, "kind": "in"},
        ],
    }))
    .unwrap()
}

/// One synthetic recording at `path`, with the metadata a scan would give
/// it (its folder's `TRACK.yml` chain). The file itself need not exist.
fn snapshot_with(path: &Path) -> LibrarySnapshot {
    let summary = summary();
    let folder = track_yml::read_hierarchy(path.parent().unwrap(), true).0;
    let config = Config::default();
    let metadata = effective_metadata(
        path,
        MetadataSources::new(&folder, &config).with_summary(Some(&summary)),
    );
    let file = DiscoveredFile::new(
        LocationId::new("synthetic"),
        path.to_path_buf(),
        FileIdentity::new(1, 1, 1, 0),
    );
    LibrarySnapshot::build(vec![CatalogRecord::new(file, summary, metadata)])
}

/// Start the application with motion reduced, so sheets and dialogs are
/// in place on their first frame (their slide-in runs on wall-clock time).
fn start(cx: &mut TestAppContext, state: omatrack_app::StateOptions) -> common::TestApp {
    cx.update(|cx| cx.set_reduce_motion(true));
    common::start(cx, state)
}

fn install(test: &common::TestApp, snapshot: LibrarySnapshot, cx: &mut TestAppContext) -> String {
    let session = snapshot.sessions().next().unwrap().id.clone();
    let library = test.app.library.clone();
    cx.update(|cx| library.update(cx, |library, cx| library.set_snapshot(snapshot, cx)));
    cx.run_until_parked();
    session
}

/// Let the debounced preferences save run, then read the document.
fn saved_config(
    test: &common::TestApp,
    sandbox: &common::Sandbox,
    cx: &mut TestAppContext,
) -> Config {
    cx.executor().advance_clock(Duration::from_secs(1));
    cx.run_until_parked();
    let preferences = test.app.preferences.clone();
    cx.update(|cx| preferences.update(cx, |preferences, cx| preferences.flush(cx)));
    sandbox.read_config()
}

fn focus_traces(test: &common::TestApp, cx: &mut TestAppContext) -> gpui_kit::FocusHandle {
    cx.update_window(test.window.into(), |_, window, cx| {
        let traces = test
            .workspace
            .read(cx)
            .panels()
            .focus_handle(PanelKind::Traces, cx);
        window.focus(&traces, cx);
        window.render_frame(cx);
        traces
    })
    .unwrap()
}

/// Open Preferences the way the palette and the title bar do: dispatch
/// the action from the focused element.
fn open_preferences(
    test: &common::TestApp,
    cx: &mut TestAppContext,
) -> gpui_kit::Entity<PreferencesView> {
    cx.update_window(test.window.into(), |_, window, cx| {
        window.dispatch_action(Box::new(OpenPreferences), cx);
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update(|cx| test.workspace.read(cx).preferences().cloned())
        .expect("OpenPreferences opens the screen")
}

/// Click `section` in the section list.
fn show(window: &mut gpui_kit::Window, section: PreferencesSection, cx: &mut gpui_kit::App) {
    window.within("preferences-nav").click(section.nav_id(), cx);
    window.render_frame(cx);
}

fn is_open(test: &common::TestApp, cx: &mut TestAppContext) -> bool {
    cx.update(|cx| test.workspace.read(cx).preferences().is_some())
}

/// Press and release `key`: a keyboard click fires on the release, which
/// `press` (key down only) does not send.
fn activate(window: &mut gpui_kit::Window, key: &str, cx: &mut gpui_kit::App) {
    window.press(key, cx);
    let keystroke = gpui_kit::Keystroke::parse(key).unwrap();
    window.dispatch_event(
        gpui_kit::PlatformInput::KeyUp(gpui_kit::KeyUpEvent { keystroke }),
        cx,
    );
    window.render_frame(cx);
}

fn page_visible(
    test: &common::TestApp,
    section: PreferencesSection,
    cx: &mut TestAppContext,
) -> bool {
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window
            .try_find(section.page_id())
            .is_some_and(|page| page.visible())
    })
    .unwrap()
}

#[gpui_kit::test]
fn ctrl_comma_opens_the_preferences_screen_and_escape_returns_focus(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = start(cx, sandbox.options());
    let traces = focus_traces(&test, cx);

    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-,", cx);
        window.render_frame(cx);
        // A screen, not an overlay: it replaces the dock area and status bar.
        assert!(!window.has_active_sheet(cx));
        assert!(window.find("preferences").visible());
        assert!(window.find("preferences-title").visible());
        assert!(window.try_find("workspace-dock").is_none());
        assert!(window.try_find("status-bar").is_none());
        assert!(!traces.is_focused(window), "the screen takes focus");
    })
    .unwrap();
    let nav_focused = cx
        .update_window(test.window.into(), |_, window, cx| {
            let view = test.workspace.read(cx).preferences().unwrap().clone();
            view.read(cx).is_nav_focused(window)
        })
        .unwrap();
    assert!(nav_focused, "the section list has focus");

    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("escape", cx);
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert!(!is_open(&test, cx));
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("workspace-dock").visible());
        assert!(window.try_find("preferences").is_none());
        assert!(traces.is_focused(window), "focus returns to the traces");
    })
    .unwrap();
}

#[gpui_kit::test]
fn done_and_escape_from_a_control_return_to_the_workspace(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = start(cx, sandbox.options());
    let traces = focus_traces(&test, cx);
    let layout_before = cx.update(|cx| test.workspace.read(cx).dock_area().read(cx).dump(cx));

    // Escape from a focused control inside the screen, not only the list.
    open_preferences(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        show(window, PreferencesSection::Video, cx);
        window.click("prefs-video-muted", cx);
        window.press("escape", cx);
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert!(!is_open(&test, cx));
    cx.update_window(test.window.into(), |_, window, _| {
        assert!(traces.is_focused(window), "focus returns to the traces");
    })
    .unwrap();

    // Done in the title bar.
    open_preferences(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        window.click("preferences-done", cx);
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert!(!is_open(&test, cx));
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(traces.is_focused(window), "focus returns to the traces");
    })
    .unwrap();
    let layout_after = cx.update(|cx| test.workspace.read(cx).dock_area().read(cx).dump(cx));
    assert_eq!(
        serde_json::to_value(&layout_before).unwrap(),
        serde_json::to_value(&layout_after).unwrap(),
        "the dock layout is untouched"
    );
}

#[gpui_kit::test]
fn the_palette_command_opens_preferences(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = start(cx, sandbox.options());
    focus_traces(&test, cx);
    step(&test, cx, |window, cx| {
        window.press("ctrl-k", cx);
    });
    step(&test, cx, |window, cx| {
        assert!(window.has_active_dialog(cx));
        window.input("Preferences", cx);
    });
    step(&test, cx, |window, cx| window.press("enter", cx));
    cx.executor().advance_clock(Duration::from_millis(300));
    cx.run_until_parked();
    assert!(is_open(&test, cx));
    let nav_focused = cx
        .update_window(test.window.into(), |_, window, cx| {
            window.render_frame(cx);
            assert!(!window.has_active_dialog(cx), "the palette closes");
            let view = test.workspace.read(cx).preferences().unwrap().clone();
            view.read(cx).is_nav_focused(window)
        })
        .unwrap();
    assert!(nav_focused, "the section list keeps the focus");
}

#[gpui_kit::test]
fn sections_switch_with_the_pointer_and_the_arrow_keys(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = start(cx, sandbox.options());
    let view = open_preferences(&test, cx);
    assert!(page_visible(&test, PreferencesSection::Library, cx));
    assert!(!page_visible(&test, PreferencesSection::Video, cx));

    cx.update_window(test.window.into(), |_, window, cx| {
        show(window, PreferencesSection::Video, cx);
    })
    .unwrap();
    assert!(page_visible(&test, PreferencesSection::Video, cx));
    assert!(!page_visible(&test, PreferencesSection::Library, cx));

    // The list is one Tab stop; Up and Down move through it, no wrap.
    let section = |cx: &mut TestAppContext| {
        cx.update_window(test.window.into(), |_, window, cx| {
            window.render_frame(cx);
            assert!(view.read(cx).is_nav_focused(window));
            view.read(cx).section()
        })
        .unwrap()
    };
    assert_eq!(section(cx), PreferencesSection::Video);
    step(&test, cx, |window, cx| window.press("down", cx));
    assert_eq!(section(cx), PreferencesSection::Drivers);
    step(&test, cx, |window, cx| {
        window.press("down", cx);
    });
    assert!(page_visible(&test, PreferencesSection::Tracks, cx));
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("atlas-attribution").visible());
    })
    .unwrap();
    step(&test, cx, |window, cx| {
        for _ in 0..4 {
            window.press("down", cx);
        }
    });
    assert_eq!(section(cx), PreferencesSection::Appearance);
    step(&test, cx, |window, cx| {
        for _ in 0..8 {
            window.press("up", cx);
        }
    });
    assert_eq!(section(cx), PreferencesSection::Library);
}

#[gpui_kit::test]
fn the_mute_switch_writes_video_muted(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = start(cx, sandbox.options());
    open_preferences(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        show(window, PreferencesSection::Video, cx);
        window.render_frame(cx);
        assert_eq!(window.find("prefs-video-muted").checked(), Some(false));
        window.click("prefs-video-muted", cx);
        window.render_frame(cx);
        assert_eq!(window.find("prefs-video-muted").checked(), Some(true));
    })
    .unwrap();
    // Nothing is written before the debounce.
    assert!(sandbox.read_config().video.muted.is_none());
    let saved = saved_config(&test, &sandbox, cx);
    assert_eq!(saved.video.muted, Some(true));
    let muted = cx.update(|cx| test.app.video.read(cx).is_muted(cx));
    assert!(muted, "the video controller follows");

    // The fit switch on the Traces tab writes trace.fit_channels.
    cx.update_window(test.window.into(), |_, window, cx| {
        show(window, PreferencesSection::Traces, cx);
        window.render_frame(cx);
        window.click("prefs-fit-lanes", cx);
    })
    .unwrap();
    let saved = saved_config(&test, &sandbox, cx);
    assert_eq!(saved.trace.fit_channels, Some(false));
}

#[gpui_kit::test]
fn the_driver_table_writes_driver_mappings(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = start(cx, sandbox.options());
    open_preferences(&test, cx);
    step(&test, cx, |window, cx| {
        show(window, PreferencesSection::Drivers, cx);
        window.render_frame(cx);
        window.click("drivers-add", cx);
        // The new row's id field has the caret.
        window.input("12", cx);
    });
    step(&test, cx, |window, cx| {
        window.click("driver-name-0", cx);
        window.input("Ada", cx);
    });
    step(&test, cx, |window, cx| {
        window.click("drivers-add", cx);
        window.input("x", cx);
    });
    step(&test, cx, |window, _| {
        assert_eq!(
            window.find("driver-error-1").label(),
            Some("Use a positive driver id, or * for every other id.")
        );
    });
    let saved = saved_config(&test, &sandbox, cx);
    assert_eq!(
        saved.driver_mappings.into_iter().collect::<Vec<_>>(),
        vec![("12".to_string(), "Ada".to_string())]
    );
}

#[gpui_kit::test]
fn removing_a_library_folder_asks_first(cx: &mut TestAppContext) {
    let library = Library::new();
    let test = start(cx, library.sandbox.options());
    let id = cx.update(|cx| {
        test.app
            .preferences
            .read(cx)
            .config()
            .folder_locations()
            .next()
            .unwrap()
            .resolved_id()
    });
    let remove = format!("prefs-remove-folder-{id}");
    open_preferences(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.click(remove.clone(), cx);
        window.render_frame(cx);
        assert!(window.has_active_dialog(cx));
        window.within("dialog").click("cancel", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let count = cx.update(|cx| {
        test.app
            .preferences
            .read(cx)
            .config()
            .folder_locations()
            .count()
    });
    assert_eq!(count, 1, "Cancel keeps the folder");

    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.click(remove.clone(), cx);
        window.render_frame(cx);
        window.within("dialog").click("ok", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let saved = saved_config(&test, &library.sandbox, cx);
    assert_eq!(saved.folder_locations().count(), 0);
    assert!(library.root.is_dir(), "the folder itself stays on disk");
}

/// Run one step of UI input, then let GPUI deliver its events (input
/// changes reach their subscribers when the update ends).
fn step(
    test: &common::TestApp,
    cx: &mut TestAppContext,
    f: impl FnOnce(&mut gpui_kit::Window, &mut gpui_kit::App),
) {
    cx.update_window(test.window.into(), |_, window, cx| {
        f(window, cx);
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
}

/// Dispatch a library row command from the focused library, the way its
/// context menu does. (A test cannot open the menu itself: gpui-component
/// 0.6.6's context menu keeps its popup alive through a reference cycle,
/// which the test harness reports as a leaked entity; the menu's entries
/// are covered by the library panel's unit tests.)
fn dispatch_in_library(
    test: &common::TestApp,
    action: impl gpui_kit::Action,
    cx: &mut TestAppContext,
) {
    step(test, cx, |window, cx| {
        window.press("ctrl-6", cx);
        window.dispatch_action(Box::new(action), cx);
    });
}

#[gpui_kit::test]
fn the_recording_metadata_dialog_saves_and_shows_the_effective_value(cx: &mut TestAppContext) {
    let library = Library::new();
    library.write_track_yml("2026-09-02", "driver: {name: Ada}\n");
    let recording = library.root.join("2026-09-02").join("Run1_Q1.pds");
    let test = start(cx, library.sandbox.options());
    let session = install(&test, snapshot_with(&recording), cx);
    let trigger = cx
        .update_window(test.window.into(), |_, window, cx| {
            window.press("ctrl-6", cx);
            window.focused(cx)
        })
        .unwrap();
    assert!(trigger.is_some(), "the library tree has focus");

    dispatch_in_library(
        &test,
        EditRecordingMetadata {
            session: Some(session.into()),
        },
        cx,
    );
    step(&test, cx, |window, cx| {
        assert!(window.has_active_dialog(cx));
        assert_eq!(
            window.find("metadata-driver-status").label(),
            Some("Driver in effect: Ada · TRACK.yml")
        );
        // The driver field has the caret: the draft previews at once.
        assert_eq!(window.find("metadata-driver").focused(), Some(true));
        window.input("Jim", cx);
    });
    step(&test, cx, |window, cx| {
        assert_eq!(
            window.find("metadata-driver-status").label(),
            Some("Driver in effect: Jim · This recording")
        );
        // Single-letter shortcuts stay out of text fields: `m` would mute.
        window.click("metadata-event", cx);
        window.input("mx", cx);
    });
    step(&test, cx, |window, cx| {
        assert_eq!(window.find("metadata-event").value(), Some("mx"));
        // An invalid value blocks saving and says why.
        window.click("metadata-car-number", cx);
        window.input("#52", cx);
    });
    step(&test, cx, |window, cx| {
        assert_eq!(
            window.find("metadata-car-number-status").label(),
            Some("Car number: Use up to 6 letters or digits.")
        );
        window.click("metadata-save", cx);
    });
    let muted = cx.update(|cx| test.app.video.read(cx).is_muted(cx));
    assert!(!muted, "typing m in a dialog field does not mute");
    step(&test, cx, |window, cx| {
        assert!(
            window.has_active_dialog(cx),
            "an invalid draft is not saved"
        );
        window.click("metadata-car-number", cx);
        for _ in 0..3 {
            window.press("backspace", cx);
        }
        window.input("52", cx);
    });
    step(&test, cx, |window, cx| window.click("metadata-save", cx));
    // The dialog returns focus once its closing animation is over.
    cx.executor().advance_clock(Duration::from_millis(300));
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(!window.has_active_dialog(cx), "Save closes the dialog");
        // Back on the tree row the dialog was opened for. (Saving rescans
        // the library folder, which holds no real recording here, so the
        // tree itself is no longer drawn: check the handle, not the path.)
        assert_eq!(window.focused(cx), trigger, "focus returns to the library");
    })
    .unwrap();

    let saved = saved_config(&test, &library.sandbox, cx);
    let key = recording.to_string_lossy().into_owned();
    let expected: Mapping =
        serde_yaml::from_str("driver: {name: Jim}\nevent: mx\ncar: {number: '52'}\n").unwrap();
    assert_eq!(saved.recording_metadata.get(&key), Some(&expected));
    assert!(
        saved.video.muted.is_none(),
        "no shortcut reached the workspace"
    );
}

#[gpui_kit::test]
fn ctrl_i_opens_the_metadata_of_the_selected_recording(cx: &mut TestAppContext) {
    let library = Library::new();
    let recording = library.root.join("2026-09-02").join("Run1_Q1.pds");
    std::fs::create_dir_all(recording.parent().unwrap()).unwrap();
    let test = start(cx, library.sandbox.options());
    install(&test, snapshot_with(&recording), cx);
    step(&test, cx, |window, cx| {
        window.press("ctrl-6", cx);
        window.press("down", cx);
        window.press("down", cx);
        window.press("ctrl-i", cx);
    });
    step(&test, cx, |window, cx| {
        assert!(window.has_active_dialog(cx));
        assert!(window.find("recording-metadata").visible());
        window.press("escape", cx);
    });
    cx.update_window(test.window.into(), |_, window, cx| {
        assert!(!window.has_active_dialog(cx), "Escape closes the dialog");
        let library_panel = test.workspace.read(cx).panels().library.clone();
        assert!(
            library_panel
                .read(cx)
                .focus_handle(cx)
                .contains_focused(window, cx),
            "focus returns to the library"
        );
    })
    .unwrap();
}

#[gpui_kit::test]
fn a_track_yml_edit_keeps_unknown_keys(cx: &mut TestAppContext) {
    let library = Library::new();
    library.write_track_yml("", "event: Petit Le Mans\n");
    let file = library.write_track_yml(
        "CT1",
        "schema: 1\n\
         track: {name: Road Atlanta, slug: road-atlanta}\n\
         driver:\n  name: Ada\n  mappings: {'12': Bob}\n\
         files: [Run1.mp4]\n\
         custom:\n  keep: true\n",
    );
    let recording = library.root.join("CT1").join("Run1.pds");
    let test = start(cx, library.sandbox.options());
    let session = install(&test, snapshot_with(&recording), cx);

    dispatch_in_library(
        &test,
        EditFolderMetadata {
            session: session.into(),
        },
        cx,
    );
    step(&test, cx, |window, cx| {
        assert!(window.find("track-yml").visible());
        assert_eq!(window.find("track-yml-track").value(), Some("Road Atlanta"));
        assert_eq!(window.find("track-yml-driver").value(), Some("Ada"));
        // Event is inherited from the parent folder, not set here.
        assert_eq!(window.find("track-yml-event").value(), Some(""));
        window.click("track-yml-event", cx);
        window.input("Sprint", cx);
        window.click("track-yml-driver", cx);
        for _ in 0..3 {
            window.press("backspace", cx);
        }
        window.input("Grace", cx);
    });
    step(&test, cx, |window, cx| window.click("track-yml-save", cx));
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(
            !window.has_active_dialog(cx),
            "a successful write closes it"
        );
    })
    .unwrap();

    let document = track_yml::read_document(&file).unwrap();
    let expected: Mapping = serde_yaml::from_str(
        "schema: 1\n\
         track: {name: Road Atlanta, slug: road-atlanta}\n\
         driver:\n  name: Grace\n  mappings: {'12': Bob}\n\
         files: [Run1.mp4]\n\
         custom:\n  keep: true\n\
         event: Sprint\n",
    )
    .unwrap();
    assert_eq!(document, expected);
    assert_eq!(
        document.get("custom"),
        Some(&Value::Mapping(serde_yaml::from_str("keep: true").unwrap()))
    );
    // The parent's file is untouched.
    let parent = track_yml::read_document(&library.root.join(track_yml::FILE_NAME)).unwrap();
    assert_eq!(
        parent,
        serde_yaml::from_str::<Mapping>("event: Petit Le Mans").unwrap()
    );
}

#[gpui_kit::test]
fn track_yml_is_refused_outside_library_folders(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let outside = sandbox.dir.path().join("elsewhere").join("Run1.pds");
    std::fs::create_dir_all(outside.parent().unwrap()).unwrap();
    let test = start(cx, sandbox.options());
    let session = install(&test, snapshot_with(&outside), cx);
    dispatch_in_library(
        &test,
        EditFolderMetadata {
            session: session.into(),
        },
        cx,
    );
    cx.update_window(test.window.into(), |_, window, cx| {
        assert!(!window.has_active_dialog(cx));
    })
    .unwrap();
    assert!(
        !outside
            .parent()
            .unwrap()
            .join(track_yml::FILE_NAME)
            .exists()
    );
}

#[gpui_kit::test]
fn single_keys_stay_inside_the_preferences_screen(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = start(cx, sandbox.options());
    let view = open_preferences(&test, cx);
    step(&test, cx, |window, cx| {
        show(window, PreferencesSection::Traces, cx);
        // `m` would mute, `t` would switch the x axis.
        window.press("m", cx);
        window.press("t", cx);
        // Tab leaves the section list for the first control; Space flips
        // that switch instead of playing video.
        window.press("tab", cx);
        activate(window, "space", cx);
    });
    let (muted, axis, section) = cx.update(|cx| {
        (
            test.app.video.read(cx).is_muted(cx),
            test.app.viewport.read(cx).axis(),
            view.read(cx).section(),
        )
    });
    assert!(!muted);
    assert_eq!(axis, omatrack_trace::XAxis::Distance);
    assert_eq!(section, PreferencesSection::Traces);
    let saved = saved_config(&test, &sandbox, cx);
    assert_eq!(
        saved.trace.fit_channels,
        Some(false),
        "Space reached the switch"
    );
    assert!(saved.video.muted.is_none());
}
