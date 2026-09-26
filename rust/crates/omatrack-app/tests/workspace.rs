//! UI integration tests for the dock workspace: the default layout, dock
//! toggles and layout persistence through `omatrack.yml`.

mod common;

use gpui_kit::AppContext as _;
use gpui_kit::TestAppContext;
use gpui_kit::component::dock::DockPlacement;
use gpui_kit::test::TestWindowExt as _;
use omatrack_app::LayoutOrigin;
use omatrack_app::panels::PanelKind;

fn holds_every_panel(test: &common::TestApp, cx: &mut TestAppContext) {
    cx.update(|cx| {
        let workspace = test.workspace.read(cx);
        let area = workspace.dock_area().read(cx);
        for kind in PanelKind::ALL {
            let id = workspace.panels().handle(kind).panel_id(cx);
            assert!(area.panel(id).is_some(), "{kind:?} is in the dock");
        }
    });
}

#[gpui_kit::test]
fn the_default_dock_holds_every_panel(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    holds_every_panel(&test, cx);
    cx.update(|cx| {
        let workspace = test.workspace.read(cx);
        assert_eq!(*workspace.layout_origin(), LayoutOrigin::Default);
        let area = workspace.dock_area().read(cx);
        assert!(area.is_dock_open(DockPlacement::Left));
        assert!(area.is_dock_open(DockPlacement::Right));
    });
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        // Active tabs are on screen: video over traces, library left,
        // corners and inspector right.
        let video = window.find("video-panel");
        let traces = window.find("traces-panel");
        assert!(video.bounds().bottom() <= traces.bounds().top());
        assert!(traces.bounds().size.height > video.bounds().size.height);
        let library = window.find("library-panel");
        let corners = window.find("corners-panel");
        let inspector = window.find("inspector-panel");
        assert!(library.bounds().right() <= video.bounds().left());
        assert!(corners.bounds().left() >= traces.bounds().right());
        assert!(corners.bounds().bottom() <= inspector.bounds().top());
    })
    .unwrap();
}

#[gpui_kit::test]
fn ctrl_b_and_ctrl_j_toggle_the_docks(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let open = |placement, cx: &mut TestAppContext| {
        cx.update(|cx| {
            test.workspace
                .read(cx)
                .dock_area()
                .read(cx)
                .is_dock_open(placement)
        })
    };
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-b", cx)
    })
    .unwrap();
    assert!(!open(DockPlacement::Left, cx));
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(
            window
                .try_find("library-panel")
                .is_none_or(|library| !library.visible())
        );
        window.press("ctrl-b", cx);
    })
    .unwrap();
    assert!(open(DockPlacement::Left, cx));
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-j", cx)
    })
    .unwrap();
    assert!(!open(DockPlacement::Right, cx));
}

#[gpui_kit::test]
fn the_layout_round_trips_through_preferences(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-b", cx)
    })
    .unwrap();
    // The save is debounced; let it run, then write the document.
    cx.executor()
        .advance_clock(std::time::Duration::from_secs(1));
    cx.run_until_parked();
    let preferences = test.app.preferences.clone();
    cx.update(|cx| preferences.update(cx, |preferences, cx| preferences.flush(cx)));

    let saved = sandbox.read_config();
    let layout = saved.workspace.layout.expect("workspace.layout is saved");
    assert_eq!(layout["version"], omatrack_app::LAYOUT_VERSION);
    assert_eq!(layout["left_dock"]["open"], false);

    // A new window restores it.
    let second = common::open(cx);
    cx.update(|cx| {
        let workspace = second.workspace.read(cx);
        assert_eq!(*workspace.layout_origin(), LayoutOrigin::Restored);
        let area = workspace.dock_area().read(cx);
        assert!(!area.is_dock_open(DockPlacement::Left));
        assert!(area.is_dock_open(DockPlacement::Right));
    });
    holds_every_panel(&second, cx);
}

#[gpui_kit::test]
fn a_corrupt_layout_falls_back_to_the_default(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    sandbox.write_config("workspace:\n  layout: {version: 2, center: 12}\n");
    let test = common::start(cx, sandbox.options());
    cx.update(|cx| {
        let workspace = test.workspace.read(cx);
        assert!(matches!(
            workspace.layout_origin(),
            LayoutOrigin::Replaced(_)
        ));
        assert!(
            workspace
                .dock_area()
                .read(cx)
                .is_dock_open(DockPlacement::Left)
        );
    });
    holds_every_panel(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(
            window.find("notification").visible(),
            "the fallback is announced"
        );
    })
    .unwrap();
}

#[gpui_kit::test]
fn a_layout_from_another_version_is_replaced(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    sandbox.write_config(
        "workspace:\n  layout:\n    version: 1\n    center: {panel_name: StackPanel, children: [], info: {stack: {sizes: [], axis: 0}}}\n",
    );
    let test = common::start(cx, sandbox.options());
    cx.update(|cx| {
        assert!(matches!(
            test.workspace.read(cx).layout_origin(),
            LayoutOrigin::Replaced(_)
        ));
    });
    holds_every_panel(&test, cx);
}

#[gpui_kit::test]
fn reset_layout_restores_the_default(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-b", cx);
        window.press("ctrl-j", cx);
        window.dispatch_action(Box::new(omatrack_app::actions::ResetLayout), cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        let area = test.workspace.read(cx).dock_area().read(cx);
        assert!(area.is_dock_open(DockPlacement::Left));
        assert!(area.is_dock_open(DockPlacement::Right));
    });
    holds_every_panel(&test, cx);
}

/// Ctrl+4 reveals the dock the Corners panel is in now, not the one it
/// starts in.
#[gpui_kit::test]
fn focusing_a_moved_panel_opens_its_current_dock(cx: &mut TestAppContext) {
    use gpui_kit::component::dock::InsertTarget;

    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let area = cx.update(|cx| test.workspace.read(cx).dock_area().clone());
    let (library, corners) = cx.update(|cx| {
        let panels = test.workspace.read(cx).panels().clone();
        (
            panels.handle(PanelKind::Library).panel_id(cx),
            panels.handle(PanelKind::Corners).panel_id(cx),
        )
    });
    cx.update_window(test.window.into(), |_, window, cx| {
        area.update(cx, |area, cx| {
            let node = area
                .layout(DockPlacement::Left)
                .and_then(|tree| tree.find_panel_node(library))
                .expect("the library's tab group");
            area.move_panel(
                corners,
                InsertTarget::Tabs {
                    node,
                    ix: None,
                    activate: true,
                },
                window,
                cx,
            );
            area.toggle_dock(DockPlacement::Left, window, cx);
        });
    })
    .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        let area = area.read(cx);
        assert!(
            area.layout(DockPlacement::Left)
                .is_some_and(|tree| tree.find_panel_node(corners).is_some()),
            "Corners moved to the left dock"
        );
        assert!(!area.is_dock_open(DockPlacement::Left));
    });

    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-4", cx)
    })
    .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        let area = area.read(cx);
        assert!(area.is_dock_open(DockPlacement::Left), "its dock opened");
        assert!(
            area.layout(DockPlacement::Left)
                .is_some_and(|tree| tree.find_panel_node(corners).is_some()),
            "and it stayed where the user put it"
        );
    });
}
