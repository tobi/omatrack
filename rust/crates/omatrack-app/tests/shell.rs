//! UI integration tests for the application shell: the real main window in a
//! headless test platform.

use gpui_kit::component::{
    ActiveTheme as _, ThemeMode, WindowExt as _, notification::Notification,
};
use gpui_kit::test::TestWindowExt as _;
use gpui_kit::{AppContext as _, TestAppContext, px};
use omatrack_app::{Workspace, main_window_options, open_main_window};
use omatrack_ui::theme::ThemeSource;

#[gpui_kit::test]
fn main_window_shows_the_workspace_with_the_built_in_theme(cx: &mut TestAppContext) {
    cx.update(|cx| omatrack_app::init_with_theme(ThemeSource::none(), cx));
    let handle = cx.update(open_main_window).expect("main window opens");

    cx.update_window(handle.into(), |_, window, cx| {
        window.render_frame(cx);

        let status = window.find("theme-status");
        assert!(status.visible());
        assert_eq!(status.label(), Some("Built-in dark"));

        // The dock area fills the space between title bar and status bar,
        // and holds the welcome panel.
        let dock = window.find("workspace-dock");
        assert!(dock.visible());
        assert!(dock.bounds().size.height > px(0.));
        let welcome = window.find("welcome");
        assert!(welcome.visible());
        assert!(welcome.bounds().top() >= dock.bounds().top());
        assert!(status.bounds().top() >= dock.bounds().bottom());
    })
    .unwrap();

    cx.update(|cx| assert_eq!(cx.theme().mode, ThemeMode::Dark));
    let workspace = cx
        .update(|cx| {
            handle
                .read(cx)
                .map(|root| root.view().clone().downcast::<Workspace>().ok())
        })
        .unwrap()
        .expect("Root wraps the Workspace");
    cx.update(|cx| {
        let area = workspace.read(cx).dock_area().read(cx);
        assert_eq!(area.id().as_ref(), omatrack_app::DOCK_AREA_ID);
        assert_eq!(area.version(), Some(omatrack_app::LAYOUT_VERSION));
    });
}

#[gpui_kit::test]
fn notifications_reach_the_screen(cx: &mut TestAppContext) {
    cx.update(|cx| omatrack_app::init_with_theme(ThemeSource::none(), cx));
    let handle = cx.update(open_main_window).expect("main window opens");
    cx.update_window(handle.into(), |_, window, cx| {
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
    cx.update_window(handle.into(), |_, window, cx| {
        window.render_frame(cx);
        let notification = window.find("notification");
        assert!(notification.visible(), "the notification layer is mounted");
    })
    .unwrap();
}

#[gpui_kit::test]
fn the_welcome_panel_lists_bound_shortcuts(cx: &mut TestAppContext) {
    cx.update(|cx| omatrack_app::init_with_theme(ThemeSource::none(), cx));
    let handle = cx.update(open_main_window).expect("main window opens");
    cx.update_window(handle.into(), |_, window, cx| {
        window.render_frame(cx);
        // Bindings resolve against the previous frame; draw twice.
        window.render_frame(cx);
        assert!(window.find("welcome").visible());
        let quit = window.find("shortcut-quit");
        assert!(quit.visible());
        assert_eq!(quit.label(), Some("Quit: Ctrl+Q"));
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
