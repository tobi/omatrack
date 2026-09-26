//! UI integration tests for the library panel: keyboard navigation of the
//! session tree sets the primary and the reference lap.

mod common;

use gpui_kit::AppContext as _;
use gpui_kit::TestAppContext;
use gpui_kit::test::TestWindowExt as _;
use omatrack_app::state::LapRef;

#[gpui_kit::test]
fn arrows_and_enter_set_the_primary_and_alt_enter_the_reference(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
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
