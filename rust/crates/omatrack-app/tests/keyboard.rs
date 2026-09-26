//! UI integration tests for the keymap: single keys work from the trace
//! workspace and never fire while a text field has focus.

mod common;

use gpui_kit::AppContext as _;
use gpui_kit::TestAppContext;
use gpui_kit::test::TestWindowExt as _;
use omatrack_app::panels::PanelKind;
use omatrack_app::state::LapRef;
use omatrack_trace::Viewport;

/// Select laps from the synthetic library for both roles and return
/// their references (primary, reference).
fn select_pair(test: &common::TestApp, cx: &mut TestAppContext) -> (LapRef, LapRef) {
    let snapshot = common::load_synthetic_library(test, cx);
    let sessions: Vec<_> = snapshot.sessions().collect();
    let primary = LapRef::new(sessions[0].id.clone(), 3);
    let reference = LapRef::new(sessions[1].id.clone(), 3);
    let session = test.app.session.clone();
    let (p, r) = (primary.clone(), reference.clone());
    cx.update(|cx| {
        session.update(cx, |session, cx| {
            session.set_primary(p.session().clone(), p.lap(), cx);
            session.set_reference(r.session().clone(), r.lap(), cx);
        })
    });
    cx.run_until_parked();
    (primary, reference)
}

fn roles(test: &common::TestApp, cx: &mut TestAppContext) -> (Option<LapRef>, Option<LapRef>) {
    cx.update(|cx| {
        let session = test.app.session.read(cx);
        (
            session.primary().map(|slot| slot.lap_ref().clone()),
            session.reference().map(|slot| slot.lap_ref().clone()),
        )
    })
}

fn viewport(test: &common::TestApp, cx: &mut TestAppContext) -> Viewport {
    cx.update(|cx| test.app.viewport.read(cx).viewport())
}

#[gpui_kit::test]
fn x_and_equals_work_from_the_traces_panel(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let (primary, reference) = select_pair(&test, cx);

    cx.update_window(test.window.into(), |_, window, cx| {
        let traces = test
            .workspace
            .read(cx)
            .panels()
            .focus_handle(PanelKind::Traces, cx);
        window.focus(&traces, cx);
        window.render_frame(cx);
        assert!(traces.is_focused(window), "traces start focused");
        window.press("x", cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(
        roles(&test, cx),
        (Some(reference.clone()), Some(primary.clone()))
    );

    assert_eq!(viewport(&test, cx), Viewport::FULL);
    cx.update_window(test.window.into(), |_, window, cx| window.press("=", cx))
        .unwrap();
    let zoomed = viewport(&test, cx);
    assert!(zoomed.span() < Viewport::FULL.span(), "{zoomed:?}");
    cx.update_window(test.window.into(), |_, window, cx| window.press("-", cx))
        .unwrap();
    assert!(viewport(&test, cx).span() > zoomed.span());
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-0", cx)
    })
    .unwrap();
    assert_eq!(viewport(&test, cx), Viewport::FULL);
}

#[gpui_kit::test]
fn single_keys_type_into_the_library_search(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let (primary, reference) = select_pair(&test, cx);

    // The Library is the tab behind the Laps sidebar: bring it forward.
    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-1", cx)
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.click("library-search", cx);
        window.input("x=", cx);
        assert_eq!(window.find("library-search").value(), Some("x="));
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(roles(&test, cx), (Some(primary), Some(reference)));
    assert_eq!(viewport(&test, cx), Viewport::FULL);
    let query = cx.update(|cx| test.app.library.read(cx).query().to_string());
    assert_eq!(query, "x=");
}

#[gpui_kit::test]
fn single_keys_type_into_the_palette(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let (primary, reference) = select_pair(&test, cx);

    cx.update_window(test.window.into(), |_, window, cx| {
        window.press("ctrl-k", cx)
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("command-palette").visible());
        window.input("x=", cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(roles(&test, cx), (Some(primary), Some(reference)));
    assert_eq!(viewport(&test, cx), Viewport::FULL);
}

/// Hover `id` long enough for its tooltip to open, then draw the frame
/// that renders the tooltip (and resolves its shortcut).
fn hover_until_tooltip(test: &common::TestApp, id: &'static str, cx: &mut TestAppContext) {
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        window.hover(id, cx);
    })
    .unwrap();
    cx.executor()
        .advance_clock(std::time::Duration::from_millis(800));
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| window.render_frame(cx))
        .unwrap();
    cx.run_until_parked();
}

/// Regression: the tooltips of single-key commands looked their shortcut
/// up with the `Workspace && !Input` predicate, which `KeyContext::parse`
/// cannot parse (it recursed until the stack overflowed and aborted).
#[gpui_kit::test]
fn single_key_tooltips_open_and_show_their_key(cx: &mut TestAppContext) {
    use gpui_kit::component::kbd::Kbd;
    use omatrack_app::WORKSPACE_CONTEXT;
    use omatrack_app::actions::{SwapRoles, ToggleMute};

    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    select_pair(&test, cx);

    hover_until_tooltip(&test, "filmstrip-swap", cx);
    hover_until_tooltip(&test, "video-mute", cx);

    // The context the tooltips use resolves the predicate bindings.
    cx.update_window(test.window.into(), |_, window, _| {
        let swap = Kbd::binding_for_action(&SwapRoles, Some(WORKSPACE_CONTEXT), window);
        let mute = Kbd::binding_for_action(&ToggleMute, Some(WORKSPACE_CONTEXT), window);
        assert!(swap.is_some(), "x resolves in the Workspace context");
        assert!(mute.is_some(), "m resolves in the Workspace context");
    })
    .unwrap();
}
