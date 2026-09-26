//! UI integration tests for the dock workspace: the default layout, dock
//! toggles and layout persistence through `omatrack.yml`.

mod common;

use gpui_kit::AppContext as _;
use gpui_kit::TestAppContext;
use gpui_kit::component::dock::DockPlacement;
use gpui_kit::test::TestWindowExt as _;
use omatrack_app::LayoutOrigin;
use omatrack_app::panels::PanelKind;

/// Panels opened on demand (palette, Ctrl+4, "Open in detail"), not
/// placed by the default layout.
const ON_DEMAND: [PanelKind; 4] = [
    PanelKind::Corners,
    PanelKind::Channels,
    PanelKind::Map,
    PanelKind::Inspector,
];

fn holds_every_panel(test: &common::TestApp, cx: &mut TestAppContext) {
    cx.update(|cx| {
        let workspace = test.workspace.read(cx);
        let area = workspace.dock_area().read(cx);
        for kind in PanelKind::ALL {
            let id = workspace.panels().handle(kind).panel_id(cx);
            assert_eq!(
                area.panel(id).is_some(),
                !ON_DEMAND.contains(&kind),
                "{kind:?} in the default dock"
            );
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
        // Active tabs are on screen: video over traces, the laps sidebar left
        // (the library is its second tab), Where the time goes right (the
        // other right panels are its tabs).
        let video = window.find("video-panel");
        let traces = window.find("traces-panel");
        assert!(video.bounds().bottom() <= traces.bounds().top());
        assert!(traces.bounds().size.height > video.bounds().size.height);
        let laps = window.find("laps-panel");
        let time_goes = window.find("time-goes-panel");
        assert!(laps.bounds().right() <= video.bounds().left());
        assert!(time_goes.bounds().left() >= traces.bounds().right());
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
                .try_find("laps-panel")
                .is_none_or(|laps| !laps.visible())
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
    // Version 2 hid the map behind the inspector; its saved layouts reset
    // to the default and say so.
    assert_eq!(omatrack_app::LAYOUT_VERSION, 7);
    let sandbox = common::Sandbox::new();
    sandbox.write_config(
        "workspace:\n  layout:\n    version: 2\n    center: {panel_name: StackPanel, children: [], info: {stack: {sizes: [], axis: 0}}}\n",
    );
    let test = common::start(cx, sandbox.options());
    cx.update(|cx| {
        assert!(matches!(
            test.workspace.read(cx).layout_origin(),
            LayoutOrigin::Replaced(reason) if reason.contains("another version")
        ));
    });
    holds_every_panel(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(
            window.find("notification").visible(),
            "the reset is announced"
        );
        assert!(window.find("time-goes-panel").visible());
    })
    .unwrap();
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

/// The on-demand panels join the right dock beside Time lost when opened.
#[gpui_kit::test]
fn on_demand_panels_open_into_the_right_dock(cx: &mut TestAppContext) {
    use omatrack_app::actions::{ShowChannels, ShowInspector, ShowMap};

    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    cx.update_window(test.window.into(), |_, window, cx| {
        window.dispatch_action(Box::new(ShowMap), cx);
        window.dispatch_action(Box::new(ShowChannels), cx);
        window.dispatch_action(Box::new(ShowInspector), cx);
        window.press("ctrl-4", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        let workspace = test.workspace.read(cx);
        let area = workspace.dock_area().read(cx);
        let right = area.layout(DockPlacement::Right).expect("a right dock");
        for kind in ON_DEMAND {
            let id = workspace.panels().handle(kind).panel_id(cx);
            assert!(right.find_panel_node(id).is_some(), "{kind:?} on the right");
        }
        assert!(area.is_dock_open(DockPlacement::Right));
    });
}

/// Ctrl+4 reveals the dock the Corners panel is in now, not the one it
/// starts in.
#[gpui_kit::test]
fn focusing_a_moved_panel_opens_its_current_dock(cx: &mut TestAppContext) {
    use gpui_kit::component::dock::InsertTarget;

    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let area = cx.update(|cx| test.workspace.read(cx).dock_area().clone());
    // Corners is opened on demand; Ctrl+4 places it.
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-4", cx);
    })
    .unwrap();
    cx.run_until_parked();
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

    // The moved panel took focus into the closed dock; the user is back
    // in the traces when they press Ctrl+4.
    let traces = cx.update(|cx| {
        test.workspace
            .read(cx)
            .panels()
            .focus_handle(PanelKind::Traces, cx)
    });
    cx.update_window(test.window.into(), |_, window, cx| {
        window.focus(&traces, cx);
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

#[gpui_kit::test]
fn a_narrow_window_closes_the_right_dock_and_keeps_laps(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let size = gpui_kit::size(gpui_kit::px(1280.), gpui_kit::px(800.));
    cx.simulate_window_resize(test.window.into(), size);
    cx.run_until_parked();
    cx.update(|cx| {
        let area = test.workspace.read(cx).dock_area().read(cx);
        assert!(area.is_dock_open(DockPlacement::Left), "Laps open");
        assert!(
            !area.is_dock_open(DockPlacement::Right),
            "right dock closed"
        );
    });
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        let traces = window.find("traces-panel");
        assert!(
            traces.bounds().size.width >= gpui_kit::px(640.),
            "traces take at least half of 1280 px: {:?}",
            traces.bounds().size.width
        );
        assert!(
            window.find("laps-panel").bounds().size.width >= gpui_kit::px(240.),
            "the Laps sidebar keeps 15 rem"
        );
    })
    .unwrap();

    // Back to a wide window: the default layout opens it again.
    let size = gpui_kit::size(gpui_kit::px(1920.), gpui_kit::px(1080.));
    cx.simulate_window_resize(test.window.into(), size);
    cx.run_until_parked();
    cx.update(|cx| {
        let area = test.workspace.read(cx).dock_area().read(cx);
        assert!(area.is_dock_open(DockPlacement::Right));
    });

    // Once the user toggles a dock the layout is theirs.
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-b", cx);
        window.press("ctrl-b", cx);
    })
    .unwrap();
    let size = gpui_kit::size(gpui_kit::px(1280.), gpui_kit::px(800.));
    cx.simulate_window_resize(test.window.into(), size);
    cx.run_until_parked();
    cx.update(|cx| {
        let area = test.workspace.read(cx).dock_area().read(cx);
        assert!(
            area.is_dock_open(DockPlacement::Right),
            "the user's layout stays"
        );
    });
}
