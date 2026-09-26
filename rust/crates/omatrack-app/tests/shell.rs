//! UI integration tests for the application shell: the real main window in
//! a headless test platform.

mod common;

use gpui_kit::AppContext as _;
use gpui_kit::component::{
    ActiveTheme as _, ThemeMode, WindowExt as _, notification::Notification,
};
use gpui_kit::test::TestWindowExt as _;
use gpui_kit::{TestAppContext, px};
use omatrack_app::panels::PanelKind;
use omatrack_app::{Workspace, main_window_options};

#[gpui_kit::test]
fn main_window_shows_title_dock_and_status_with_the_built_in_theme(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());

    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);

        let status = window.find("theme-status");
        assert!(status.visible());
        // The theme status names the palette and the interface fonts.
        assert_eq!(status.label(), Some("Built-in dark · Inter · Geist Mono"));

        // The dock area fills the space between title bar and status bar.
        let dock = window.find("workspace-dock");
        assert!(dock.visible());
        assert!(dock.bounds().size.height > px(0.));
        assert!(status.bounds().top() >= dock.bounds().bottom());
        let track = window.find("header-track");
        assert!(track.bounds().bottom() <= dock.bounds().top());
        assert_eq!(track.label(), Some("Omatrack"));

        // Without a lap every analysis surface says what to do next.
        // The filmstrip takes no space until a lap is chosen.
        assert!(window.try_find("filmstrip-primary").is_none());
        assert!(window.try_find("filmstrip-swap").is_none());
        assert_eq!(window.find("status-cursor").label(), Some("No lap loaded"));
        let library = window.find("library-panel");
        assert!(library.visible());
        assert!(library.bounds().right() <= window.find("traces-panel").bounds().left());

        // The right dock holds two surfaces: the tables over the map, with
        // the inspector a tab behind the map (the trace gutter reads the
        // cursor), so neither clips.
        let corners = window.find("corners-panel");
        let map = window.find("map-panel");
        assert!(corners.visible() && map.visible());
        assert!(window.try_find("inspector-panel").is_none());
        assert!(map.bounds().top() >= corners.bounds().bottom());
        assert!(map.bounds().size.height > px(200.));
        assert!(corners.bounds().size.height > px(200.));
    })
    .unwrap();

    cx.update(|cx| {
        assert_eq!(cx.theme().mode, ThemeMode::Dark);
        let area = test.workspace.read(cx).dock_area().read(cx);
        assert_eq!(area.id().as_ref(), omatrack_app::DOCK_AREA_ID);
        assert_eq!(area.version(), Some(omatrack_app::LAYOUT_VERSION));
    });
}

#[gpui_kit::test]
fn notifications_reach_the_screen(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.push_notification(
            Notification::new()
                .id::<Workspace>()
                .message("Library scan finished"),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        let notification = window.find("notification");
        assert!(notification.visible(), "the notification layer is mounted");
    })
    .unwrap();
}

#[gpui_kit::test]
fn preferences_open_as_a_screen_and_escape_closes_it_and_restores_focus(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let traces = cx.update(|cx| {
        test.workspace
            .read(cx)
            .panels()
            .focus_handle(PanelKind::Traces, cx)
    });
    cx.update_window(test.window.into(), |_, window, cx| {
        window.focus(&traces, cx);
        window.render_frame(cx);
        assert!(traces.is_focused(window));
        window.press("ctrl-,", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("preferences").visible());
        assert!(
            window.try_find("workspace-dock").is_none(),
            "it replaces the docks"
        );
        assert!(!traces.is_focused(window), "the screen takes focus");
        window.press("escape", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find("preferences").is_none());
        assert!(traces.is_focused(window), "focus returns to the traces");
    })
    .unwrap();
}

#[gpui_kit::test]
fn the_main_window_has_a_minimum_and_default_size(cx: &mut TestAppContext) {
    let options = cx.update(|cx| {
        gpui_kit::init(cx);
        main_window_options(cx)
    });
    let min = options.window_min_size.expect("minimum size");
    assert_eq!((min.width, min.height), (px(1024.), px(640.)));
    let bounds = options.window_bounds.expect("default bounds").get_bounds();
    assert_eq!(
        (bounds.size.width, bounds.size.height),
        (px(1600.), px(1000.))
    );
    assert!(options.titlebar.is_some(), "the title bar is client-drawn");
}
