//! UI integration tests for the command palette.

mod common;

use gpui_kit::AppContext as _;
use gpui_kit::TestAppContext;
use gpui_kit::component::WindowExt as _;
use gpui_kit::component::dock::DockPlacement;
use gpui_kit::test::TestWindowExt as _;
use omatrack_app::panels::PanelKind;

#[gpui_kit::test]
fn ctrl_k_opens_typing_filters_and_enter_runs_the_command(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-k", cx)
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("command-palette").visible());
        window.input("toggle library", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.press("enter", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(
            !window.has_active_dialog(cx),
            "confirming closes the palette"
        );
    })
    .unwrap();
    cx.update(|cx| {
        let area = test.workspace.read(cx).dock_area().read(cx);
        assert!(
            !area.is_dock_open(DockPlacement::Left),
            "Toggle library ran"
        );
    });
}

#[gpui_kit::test]
fn escape_closes_the_palette_and_restores_focus(cx: &mut TestAppContext) {
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
        window.press("ctrl-k", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.has_active_dialog(cx));
        assert!(!traces.is_focused(window), "the palette takes focus");
        window.press("escape", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(!window.has_active_dialog(cx));
        assert!(traces.is_focused(window), "focus returns to the traces");
    })
    .unwrap();
}

#[gpui_kit::test]
fn a_focus_command_keeps_the_focus_it_moved(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-k", cx)
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.input("focus video", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.press("enter", cx);
    })
    .unwrap();
    cx.run_until_parked();
    let video = cx.update(|cx| {
        test.workspace
            .read(cx)
            .panels()
            .focus_handle(PanelKind::Video, cx)
    });
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(!window.has_active_dialog(cx));
        assert!(video.is_focused(window));
    })
    .unwrap();
}
