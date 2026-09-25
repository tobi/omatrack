//! The panel shown before any session is loaded.

use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName,
    dock::{BasePanel, Panel, PanelControl, PanelEvent},
    empty::{
        Empty, EmptyContent, EmptyDescription, EmptyHeader, EmptyMedia, EmptyMediaVariant,
        EmptyTitle,
    },
    h_flex,
    kbd::Kbd,
    v_flex,
};
use gpui_kit::{
    Action, App, AsKeystroke as _, Context, EventEmitter, FocusHandle, Focusable,
    InteractiveElement as _, IntoElement, ParentElement as _, Render, SharedString,
    StatefulInteractiveElement as _, Styled as _, TestSupportExt as _, Window, div,
};

use crate::actions::Quit;

/// The empty state of the workspace: what Omatrack is for, and the key
/// commands that work right now. Only commands with a key binding are
/// listed, each with the binding the keymap actually holds.
pub(crate) struct WelcomePanel {
    focus_handle: FocusHandle,
}

impl WelcomePanel {
    pub(crate) fn new(cx: &mut Context<Self>) -> Self {
        Self {
            focus_handle: cx.focus_handle(),
        }
    }

    /// The commands the empty state teaches, in reading order, with the
    /// stable element id of each row.
    fn shortcuts() -> [(&'static str, &'static str, Box<dyn Action>); 1] {
        [("shortcut-quit", "Quit", Box::new(Quit))]
    }

    fn render_shortcuts(&self, window: &Window, cx: &App) -> impl IntoElement {
        let rows = Self::shortcuts()
            .into_iter()
            .filter_map(|(id, label, action)| {
                let binding = window.highest_precedence_binding_for_action(action.as_ref())?;
                let keystroke = binding.keystrokes().first()?.as_keystroke().clone();
                let spoken = SharedString::from(format!("{label}: {}", Kbd::format(&keystroke)));
                Some(
                    h_flex()
                        .id(id)
                        .test_support()
                        .aria_label(spoken)
                        .justify_between()
                        .gap_4()
                        .text_sm()
                        .child(div().text_color(cx.theme().muted_foreground).child(label))
                        .child(Kbd::new(keystroke)),
                )
            });
        v_flex().gap_1().w_full().children(rows)
    }
}

impl BasePanel for WelcomePanel {
    fn panel_name(&self) -> &'static str {
        "omatrack.welcome"
    }

    fn closable(&self, _: &App) -> bool {
        false
    }

    fn zoomable(&self, _: &App) -> bool {
        false
    }
}

impl Panel for WelcomePanel {
    fn tab_name(&self, _: &App) -> Option<SharedString> {
        Some("Start".into())
    }

    fn title(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        "Start"
    }

    fn zoom_control(&self, _: &App) -> Option<PanelControl> {
        None
    }

    fn title_bar(&self, _: &App) -> bool {
        false
    }
}

impl EventEmitter<PanelEvent> for WelcomePanel {}

impl Focusable for WelcomePanel {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for WelcomePanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .id("welcome")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full()
            .flex()
            .items_center()
            .justify_center()
            .p_8()
            .child(
                Empty::new()
                    .header(
                        EmptyHeader::new()
                            .media(
                                EmptyMedia::new()
                                    .with_variant(EmptyMediaVariant::Icon)
                                    .child(Icon::new(IconName::FolderOpen)),
                            )
                            .title(EmptyTitle::new().child("No session loaded"))
                            .description(EmptyDescription::new().child(
                                "Recordings from AiM, MoTeC, Cosworth and RaceLogic loggers open here for lap-by-lap comparison.",
                            )),
                    )
                    .content(EmptyContent::new().child(self.render_shortcuts(window, cx))),
            )
    }
}
