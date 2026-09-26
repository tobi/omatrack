//! Video: the primary recording's onboard video.
//!
//! This wave plays the primary video with persisted mute; reference video,
//! compositions and the telemetry HUD build on it.

use gpui_kit::component::{
    ActiveTheme as _, IconName, Selectable as _, Sizable as _, Theme,
    button::{Button, ButtonVariants as _},
    dock::{BasePanel, Panel, PanelEvent, TabGroup},
    h_flex,
    tag::Tag,
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, AppContext as _, Context, Entity, EventEmitter, FocusHandle, Focusable,
    InteractiveElement as _, IntoElement, ParentElement as _, Render, SharedString,
    StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _, WeakEntity,
    Window, div,
};
use mpv_player::VideoView;

use crate::actions::ToggleMute;
use crate::keymap::WORKSPACE_CONTEXT;
use crate::panels::{PanelKind, empty_state, panel_body};
use crate::state::{AppState, VideoAvailability, VideoEvent};

pub struct VideoPanel {
    app: AppState,
    focus_handle: FocusHandle,
    view: Option<Entity<VideoView>>,
    group: Option<WeakEntity<TabGroup>>,
    active: bool,
    _subscriptions: Vec<Subscription>,
}

impl VideoPanel {
    pub fn new(app: AppState, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![
            cx.subscribe_in(&app.video, window, |this, _, event, window, cx| {
                if *event == VideoEvent::SourceChanged {
                    this.sync_view(window, cx);
                }
            }),
            cx.observe(&app.video, |_, _, cx| cx.notify()),
            cx.observe(&app.preferences, |_, _, cx| cx.notify()),
            // The letterbox is painted by the video element from a colour it
            // holds; follow theme changes (Omarchy hot reload) into it.
            cx.observe_global::<Theme>(|this, cx| this.sync_letterbox(cx)),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle().tab_stop(true),
            view: None,
            group: None,
            active: true,
            _subscriptions: subscriptions,
        };
        panel.sync_view(window, cx);
        panel
    }

    /// The tab group holding this panel (for dock zoom).
    pub fn group(&self) -> Option<Entity<TabGroup>> {
        self.group.as_ref().and_then(WeakEntity::upgrade)
    }

    fn sync_view(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let source = self.app.video.read(cx).frame_source();
        match (source, self.view.as_ref()) {
            (Some(source), Some(view)) => {
                view.update(cx, |view, cx| view.set_source(source, window, cx));
            }
            (Some(source), None) => {
                let letterbox = cx.theme().background;
                let active = self.active;
                let view = cx.new(|cx| {
                    let mut view = VideoView::new("primary-video", source, letterbox, window, cx);
                    view.set_visible(active, cx);
                    view
                });
                self.view = Some(view);
            }
            (None, _) => self.view = None,
        }
        cx.notify();
    }

    fn sync_letterbox(&mut self, cx: &mut Context<Self>) {
        let letterbox = cx.theme().background;
        if let Some(view) = &self.view {
            view.update(cx, |view, cx| view.set_letterbox(letterbox, cx));
        }
    }

    /// The video element, when a source is bound.
    pub fn video_view(&self) -> Option<&Entity<VideoView>> {
        self.view.as_ref()
    }

    fn render_bar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let video = self.app.video.read(cx);
        let muted = video.is_muted(cx);
        let layout = video.layout().label();
        let warning = video.identity_warning().cloned();
        h_flex()
            .w_full()
            .gap_2()
            .px_2()
            .py_1()
            .border_b_1()
            .border_color(cx.theme().border)
            .text_xs()
            .child(
                Button::new("video-mute")
                    .ghost()
                    .xsmall()
                    .label(if muted { "Unmute" } else { "Mute" })
                    .selected(muted)
                    .toggled(muted)
                    .tooltip_with_action("Mute audio", &ToggleMute, Some(WORKSPACE_CONTEXT))
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.app.video.update(cx, |video, cx| video.toggle_mute(cx));
                    })),
            )
            .child(
                div()
                    .flex_1()
                    .text_color(cx.theme().muted_foreground)
                    .child(layout),
            )
            .when_some(warning, |this, warning| {
                this.child(
                    div()
                        .id("video-identity-warning")
                        .test_support()
                        .aria_label(warning.clone())
                        .tooltip(move |window, cx| {
                            gpui_kit::component::tooltip::Tooltip::new(warning.clone())
                                .build(window, cx)
                        })
                        .child(Tag::warning().small().child("Video not verified")),
                )
            })
    }

    fn render_placeholder(&self, cx: &App) -> gpui_kit::AnyElement {
        let session = self.app.session.read(cx);
        let (icon, title, description): (IconName, SharedString, SharedString) =
            match self.app.video.read(cx).availability() {
                VideoAvailability::Failed(message) => (
                    IconName::TriangleAlert,
                    "Video unavailable".into(),
                    message.clone(),
                ),
                VideoAvailability::Disabled => (
                    IconName::Frame,
                    "Video playback is off".into(),
                    "This session was started without video playback.".into(),
                ),
                _ if session.primary().is_some() => (
                    IconName::Frame,
                    "No onboard video".into(),
                    "The primary lap’s recording has no video.".into(),
                ),
                _ => (
                    IconName::Frame,
                    "No video".into(),
                    "Select a primary lap with onboard video in the Library.".into(),
                ),
            };
        let label = SharedString::from(format!("{title}. {description}"));
        panel_body(
            "video-empty",
            label,
            empty_state(icon, title, description),
            cx,
        )
        .into_any_element()
    }
}

impl BasePanel for VideoPanel {
    fn panel_name(&self) -> &'static str {
        PanelKind::Video.name()
    }

    fn closable(&self, _: &App) -> bool {
        false
    }

    fn set_active(&mut self, active: bool, _: &mut Window, cx: &mut Context<Self>) {
        self.active = active;
        if let Some(view) = &self.view {
            view.update(cx, |view, cx| view.set_visible(active, cx));
        }
    }

    fn on_added_to(&mut self, group: WeakEntity<TabGroup>, _: &mut Window, _: &mut Context<Self>) {
        self.group = Some(group);
    }
}

impl Panel for VideoPanel {
    fn tab_name(&self, _: &App) -> Option<SharedString> {
        Some(PanelKind::Video.title().into())
    }

    fn title(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        PanelKind::Video.title()
    }
}

impl EventEmitter<PanelEvent> for VideoPanel {}

impl Focusable for VideoPanel {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for VideoPanel {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let content = match &self.view {
            Some(view) => div().size_full().child(view.clone()).into_any_element(),
            None => self.render_placeholder(cx),
        };
        v_flex()
            .id("video-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full()
            .child(self.render_bar(cx))
            .child(div().flex_1().min_h_0().child(content))
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and, when it has them, its own actions and key
/// bindings. Called once from [`crate::panels::init`].
pub fn init(cx: &mut gpui_kit::App) {
    crate::panels::register(PanelKind::Video, cx);
}
