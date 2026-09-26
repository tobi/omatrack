//! UI integration tests for the library panel: keyboard navigation of the
//! session tree sets the primary and the reference lap.

mod common;

use gpui_kit::AppContext as _;
use gpui_kit::TestAppContext;
use gpui_kit::test::TestWindowExt as _;
use omatrack_app::state::LapRef;

/// The Library is the tab behind the Laps sidebar: Ctrl+1 brings it
/// forward (and focuses its tree).
fn show_library(test: &common::TestApp, cx: &mut TestAppContext) {
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-1", cx)
    })
    .unwrap();
    cx.run_until_parked();
}

#[gpui_kit::test]
fn arrows_and_enter_set_the_primary_and_alt_enter_the_reference(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    show_library(&test, cx);
    let snapshot = common::load_synthetic_library(&test, cx);
    let first = snapshot.sessions().next().unwrap().id.clone();

    // Rows: 0 track, 1 day, 2 first recording (closed), its laps once open.
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.press("ctrl-1", cx);
        for key in ["down", "down", "right", "down", "down"] {
            window.press(key, cx);
        }
        window.press("enter", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let primary = cx.update(|cx| {
        test.app
            .session
            .read(cx)
            .primary()
            .map(|slot| slot.lap_ref().clone())
    });
    assert_eq!(primary, Some(LapRef::new(first.clone(), 2)));

    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("down", cx);
        window.press("alt-enter", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let (primary, reference) = cx.update(|cx| {
        let session = test.app.session.read(cx);
        (
            session.primary().map(|slot| slot.lap_ref().clone()),
            session.reference().map(|slot| slot.lap_ref().clone()),
        )
    });
    assert_eq!(primary, Some(LapRef::new(first.clone(), 2)));
    assert_eq!(reference, Some(LapRef::new(first, 3)));
}

#[gpui_kit::test]
fn an_empty_library_offers_to_add_a_folder(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    show_library(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        let empty = window.find("library-empty");
        assert!(empty.visible());
        assert_eq!(
            empty.label(),
            Some(
                "No library folders. Add a folder of AiM, MoTeC, Cosworth or RaceLogic recordings."
            )
        );
        assert!(window.find("library-empty-add-folder").visible());
    })
    .unwrap();
}

#[gpui_kit::test]
fn search_filters_the_tree(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    show_library(&test, cx);
    common::load_synthetic_library(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.click("library-search", cx);
        window.input("grace", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let count = cx.update(|cx| test.app.library.read(cx).filtered().recording_count());
    assert_eq!(count, 1);
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.input("zzz", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("library-empty").visible());
        window.click("library-clear-filters", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let (count, query) = cx.update(|cx| {
        let library = test.app.library.read(cx);
        (
            library.filtered().recording_count(),
            library.query().to_string(),
        )
    });
    assert_eq!((count, query.as_str()), (2, ""));
}

#[gpui_kit::test]
fn double_clicking_a_lap_sets_the_primary(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    show_library(&test, cx);
    let snapshot = common::load_synthetic_library(&test, cx);
    let second = snapshot.sessions().nth(1).unwrap().id.clone();
    let row = LapRef::new(second.clone(), 4).row_id();

    // Open the second recording (row 3) with the keyboard, then
    // double-click one of its laps.
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.press("ctrl-1", cx);
        for key in ["down", "down", "down", "right"] {
            window.press(key, cx);
        }
        window.render_frame(cx);
        window.double_click(row.clone(), cx);
    })
    .unwrap();
    cx.run_until_parked();
    let primary = cx.update(|cx| {
        test.app
            .session
            .read(cx)
            .primary()
            .map(|slot| slot.lap_ref().clone())
    });
    assert_eq!(primary, Some(LapRef::new(second, 4)));
}

/// The spoken label of a tree row, revealing it first.
fn row_label(test: &common::TestApp, cx: &mut TestAppContext, id: &str) -> String {
    let panel = cx.update(|cx| test.workspace.read(cx).panels().library.clone());
    let id = gpui_kit::SharedString::from(id.to_string());
    cx.update(|cx| panel.update(cx, |panel, cx| panel.reveal(&id, cx)));
    cx.run_until_parked();
    cx.update(|cx| {
        let tree = panel.read(cx).tree().read(cx);
        let ix = tree.index_of(&id).expect("row is revealed");
        tree.entry(ix).unwrap().item().label.to_string()
    })
}

#[gpui_kit::test]
fn laps_read_as_number_kind_time_and_gap_to_best(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    show_library(&test, cx);
    // The recording numbers its laps: out and in laps keep their number.
    let snapshot = common::install_snapshot(&test, cx, common::synthetic_snapshot_numbered(true));
    let first = snapshot.sessions().next().unwrap().id.clone();
    let lap = |lap| LapRef::new(first.clone(), lap).row_id().to_string();
    assert_eq!(row_label(&test, cx, &lap(1)), "L1 Out, 1:35.000");
    assert_eq!(row_label(&test, cx, &lap(3)), "L3, 1:15.250, best lap");
    assert_eq!(
        row_label(&test, cx, &lap(2)),
        "L2, 1:16.500, +1.250 to best"
    );
    assert_eq!(row_label(&test, cx, &lap(5)), "L5 In, 1:30.000");
    // A recording counts its complete laps, not out and in laps.
    assert_eq!(
        row_label(&test, cx, &first),
        "Q1, Ada, 3 laps, best 1:15.250"
    );

    // Laps numbered in sequence (L1 is the first flying lap): the out lap
    // is only "Out", never a second "L1".
    let snapshot = common::load_synthetic_library(&test, cx);
    let first = snapshot.sessions().next().unwrap().id.clone();
    let lap = |lap| LapRef::new(first.clone(), lap).row_id().to_string();
    assert_eq!(row_label(&test, cx, &lap(1)), "Out, 1:35.000");
    assert_eq!(
        row_label(&test, cx, &lap(2)),
        "L1, 1:16.500, +1.250 to best"
    );
    assert_eq!(row_label(&test, cx, &lap(5)), "In, 1:30.000");
}

#[gpui_kit::test]
fn the_footer_loads_the_selected_row_and_is_disabled_without_one(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    show_library(&test, cx);
    let snapshot = common::load_synthetic_library(&test, cx);
    let first = snapshot.sessions().next().unwrap().id.clone();
    let roles = |cx: &mut TestAppContext| {
        cx.update(|cx| {
            let session = test.app.session.read(cx);
            (
                session.primary().map(|slot| slot.lap_ref().clone()),
                session.reference().map(|slot| slot.lap_ref().clone()),
            )
        })
    };

    // Nothing selected: both commands are inert.
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("library-footer").visible());
        window.click("library-set-primary", cx);
        window.click("library-set-reference", cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(roles(cx), (None, None));

    // A recording row: "Set primary" loads its best lap.
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-1", cx);
        // Rows: 0 track, 1 day, 2 first recording.
        for key in ["down", "down"] {
            window.press(key, cx);
        }
        window.render_frame(cx);
        window.click("library-set-primary", cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(roles(cx).0, Some(LapRef::new(first.clone(), 3)));

    // A lap row: "Set reference" loads that lap.
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-1", cx);
        for key in ["right", "down", "down"] {
            window.press(key, cx);
        }
        window.render_frame(cx);
        window.click("library-set-reference", cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(roles(cx).1, Some(LapRef::new(first, 2)));
}

#[gpui_kit::test]
fn a_filter_says_what_it_hides_and_clears_in_one_click(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    show_library(&test, cx);
    common::load_synthetic_library(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("library-filter-status").is_none());
        window.click("library-search", cx);
        window.input("grace", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        let status = window.find("library-filter-status");
        assert!(status.visible());
        assert_eq!(status.label(), Some("1 of 2 recordings"));
        window.click("library-status-clear", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let (count, query) = cx.update(|cx| {
        let library = test.app.library.read(cx);
        (
            library.filtered().recording_count(),
            library.query().to_string(),
        )
    });
    assert_eq!((count, query.as_str()), (2, ""));
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("library-filter-status").is_none());
    })
    .unwrap();
}
