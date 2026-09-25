//! The main window's content: title bar, dock workspace and status bar.

use gpui_kit::component::{
    ActiveTheme as _, Root, StyledExt as _, Theme, TitleBar,
    dock::{DockArea, DockLayout, DockSkin, panel_handle},
    h_flex,
    status_bar::StatusBar,
    v_flex,
};
use gpui_kit::{
    AppContext as _, Context, Entity, InteractiveElement as _, IntoElement, ParentElement as _,
    Render, Role, StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _,
    Window, div,
};
use omatrack_ui::theme::ThemeStatus;

use crate::welcome::WelcomePanel;

/// Persistence id of the workspace dock area.
pub const DOCK_AREA_ID: &str = "omatrack.workspace";
/// Written into every saved layout; a saved layout with another version is
/// replaced by the default layout.
pub const LAYOUT_VERSION: usize = 1;

/// The window's root view below `Root`.
///
/// Owns the dock area (panels are entities inside it) and re-renders when the
/// theme changes so the status bar names the active theme.
pub struct Workspace {
    dock_area: Entity<DockArea>,
    _subscriptions: Vec<Subscription>,
}

impl Workspace {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let (dock_area, _skin) =
            DockSkin::dock_area(DOCK_AREA_ID, Some(LAYOUT_VERSION), window, cx);
        let welcome = cx.new(WelcomePanel::new);
        let layout = DockLayout::tabs().panel_view(panel_handle(welcome), cx);
        dock_area.update(cx, |area, cx| area.set_center(layout, window, cx));

        let subscriptions = vec![
            cx.observe_global::<Theme>(|_, cx| cx.notify()),
            cx.observe_global::<ThemeStatus>(|_, cx| cx.notify()),
        ];
        Self {
            dock_area,
            _subscriptions: subscriptions,
        }
    }

    /// The dock area holding every panel.
    pub fn dock_area(&self) -> &Entity<DockArea> {
        &self.dock_area
    }

    fn render_title_bar(&self) -> impl IntoElement {
        TitleBar::new().child(
            h_flex()
                .gap_2()
                .child(div().text_sm().font_semibold().child("Omatrack")),
        )
    }

    fn render_status_bar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme_label = ThemeStatus::global(cx)
            .map(ThemeStatus::label)
            .unwrap_or_default();
        StatusBar::new().right(
            div()
                .id("theme-status")
                .role(Role::Status)
                .test_support()
                .aria_label(theme_label.clone())
                .child(theme_label),
        )
    }
}

impl Render for Workspace {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        // gpui-component 0.6.6's `Root` does not mount its overlay layers; the
        // window's first view renders them, above everything else.
        let sheets = Root::render_sheet_layer(window, cx);
        let dialogs = Root::render_dialog_layer(window, cx);
        let notifications = Root::render_notification_layer(window, cx);
        v_flex()
            .relative()
            .size_full()
            .bg(cx.theme().background)
            .text_color(cx.theme().foreground)
            .child(self.render_title_bar())
            .child(
                div()
                    .id("workspace-dock")
                    .test_support()
                    .flex_1()
                    .min_h_0()
                    .child(self.dock_area.clone()),
            )
            .child(self.render_status_bar(cx))
            .children(sheets)
            .children(dialogs)
            .children(notifications)
    }
}
