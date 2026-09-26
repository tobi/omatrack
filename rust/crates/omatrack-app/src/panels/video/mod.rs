//! Video: the primary and reference onboard videos, composed, synced to the
//! telemetry cursor, with the telemetry HUD over them.
//!
//! Behaviour lives in [`VideoController`](crate::state::VideoController);
//! this panel owns presentation: the top bar (mute, play, composition,
//! per-lap or continuous playback, 0.25x, sync and identity status), the
//! composed video panes with their role captions, the HUD and countdown
//! layer ([`overlay`]), and the display-frame pull that drives the cursor
//! while the primary plays.
//!
//! `F` zooms this panel's dock group and makes the window fullscreen;
//! Escape restores both (the workspace routes those). Nothing enters
//! fullscreen on its own.

pub mod overlay;

use std::time::Instant;

use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, IconName, IndexPath, Selectable as _, Sizable as _,
    StyledExt as _, Theme, WindowExt as _,
    button::{Button, ButtonGroup, ButtonVariants as _},
    dock::{BasePanel, Panel, PanelEvent, TabGroup},
    h_flex,
    notification::Notification,
    searchable_list::SearchableListItem,
    select::{Select, SelectEvent, SelectState},
    tag::Tag,
    tooltip::Tooltip,
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, AppContext as _, Context, Entity, EventEmitter, FocusHandle, Focusable,
    InteractiveElement as _, IntoElement, ParentElement as _, Render, Role as AccessRole,
    SharedString, StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _,
    WeakEntity, Window, div, relative, rems,
};
use mpv_player::VideoView;
use omatrack_core::playback::{ReferencePlayback, SyncState};
use omatrack_ui::{LapRole, Swatch};

use crate::actions::{Role, ToggleContinuous, ToggleMute, TogglePlay, ToggleSlowMotion};
use crate::commands::{self, CommandCategory, CommandSpec};
use crate::keymap::WORKSPACE_CONTEXT;
use crate::panels::{PanelKind, empty_state, panel_body};
use crate::state::{AppState, ComposeLayout, RoleState, VideoAvailability, VideoEvent};

use overlay::{VideoOverlay, lap_caption};

gpui_kit::actions!(
    omatrack,
    [
        /// Check every bound video's identity again (BLAKE3 on a worker).
        VerifyVideoIdentity,
        /// Reference pacing: hold 1x through corners, catch up on straights.
        PaceReferenceByCorners,
        /// Reference pacing: follow the map's local slope.
        PaceReferenceByGps,
        /// Reference pacing: both at recording speed.
        PaceReferenceByRecording,
    ]
);

/// One entry of the composition select.
#[derive(Debug, Clone, PartialEq)]
pub struct LayoutOption {
    layout: ComposeLayout,
    title: SharedString,
}

impl LayoutOption {
    fn new(layout: ComposeLayout) -> Self {
        Self {
            layout,
            title: layout.label().into(),
        }
    }
}

impl SearchableListItem for LayoutOption {
    type Value = ComposeLayout;

    fn title(&self) -> SharedString {
        self.title.clone()
    }

    fn value(&self) -> &Self::Value {
        &self.layout
    }
}

fn layout_index(layout: ComposeLayout) -> IndexPath {
    let row = ComposeLayout::ALL
        .iter()
        .position(|candidate| *candidate == layout)
        .unwrap_or(0);
    IndexPath::default().row(row)
}

pub struct VideoPanel {
    app: AppState,
    focus_handle: FocusHandle,
    primary_view: Option<Entity<VideoView>>,
    reference_view: Option<Entity<VideoView>>,
    overlay: Entity<VideoOverlay>,
    layout_select: Entity<SelectState<Vec<LayoutOption>>>,
    group: Option<WeakEntity<TabGroup>>,
    active: bool,
    /// A display-frame pull is scheduled.
    pumping: bool,
    _subscriptions: Vec<Subscription>,
}

impl VideoPanel {
    pub fn new(app: AppState, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let layout = app.video.read(cx).layout();
        let layout_select = cx.new(|cx| {
            SelectState::new(
                ComposeLayout::ALL.map(LayoutOption::new).to_vec(),
                Some(layout_index(layout)),
                window,
                cx,
            )
        });
        let overlay = cx.new(|cx| VideoOverlay::new(app.clone(), cx));
        let subscriptions = vec![
            cx.subscribe_in(&app.video, window, |this, _, event, window, cx| {
                this.on_video_event(event, window, cx);
            }),
            // Structural changes only: the controller does not notify per
            // frame. Playing starts the display-frame pull.
            cx.observe_in(&app.video, window, |this, _, window, cx| {
                this.sync_layout_select(window, cx);
                this.sync_visibility(cx);
                this.ensure_pull(window, cx);
                cx.notify();
            }),
            cx.observe(&app.preferences, |_, _, cx| cx.notify()),
            cx.observe(&app.session, |_, _, cx| cx.notify()),
            // The letterbox is painted by the video element from a colour it
            // holds; follow theme changes (Omarchy hot reload) into it.
            cx.observe_global::<Theme>(|this, cx| this.sync_letterbox(cx)),
            cx.subscribe(
                &layout_select,
                |this, _, event: &SelectEvent<Vec<LayoutOption>>, cx| {
                    let SelectEvent::Confirm(Some(layout)) = event else {
                        return;
                    };
                    let layout = *layout;
                    this.app
                        .video
                        .update(cx, |video, cx| video.set_layout(layout, cx));
                },
            ),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle().tab_stop(true),
            primary_view: None,
            reference_view: None,
            overlay,
            layout_select,
            group: None,
            active: true,
            pumping: false,
            _subscriptions: subscriptions,
        };
        panel.sync_views(window, cx);
        panel.ensure_pull(window, cx);
        panel
    }

    /// The tab group holding this panel (for dock zoom).
    pub fn group(&self) -> Option<Entity<TabGroup>> {
        self.group.as_ref().and_then(WeakEntity::upgrade)
    }

    /// The primary video element, when a source is bound.
    pub fn video_view(&self) -> Option<&Entity<VideoView>> {
        self.primary_view.as_ref()
    }

    /// The reference video element, when a source is bound.
    pub fn reference_view(&self) -> Option<&Entity<VideoView>> {
        self.reference_view.as_ref()
    }

    /// The HUD and countdown layer.
    pub fn overlay(&self) -> &Entity<VideoOverlay> {
        &self.overlay
    }

    /// Whether a display-frame pull is scheduled.
    pub fn is_pulling(&self) -> bool {
        self.pumping
    }

    fn on_video_event(&mut self, event: &VideoEvent, window: &mut Window, cx: &mut Context<Self>) {
        match event {
            VideoEvent::SourceChanged => self.sync_views(window, cx),
            VideoEvent::IdentityChecked {
                role,
                summary,
                trusted,
            } => {
                let title = match role {
                    Role::Primary => "Primary video",
                    Role::Reference => "Reference video",
                };
                let notification = if *trusted {
                    Notification::success(summary.clone())
                } else {
                    Notification::warning(summary.clone()).autohide(false)
                };
                window.push_notification(notification.title(title), cx);
            }
            VideoEvent::Failed(_) => {}
        }
    }

    /// Schedule the next display-frame pull while the primary plays. Each
    /// pull maps the primary clock to the cursor (the controller notifies
    /// only `CursorState`) and schedules the next.
    fn ensure_pull(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.pumping || !self.app.video.read(cx).is_playing() {
            return;
        }
        self.pumping = true;
        cx.on_next_frame(window, |this, window, cx| {
            this.pumping = false;
            this.app
                .video
                .update(cx, |video, cx| video.sync_frame(Instant::now(), cx));
            this.ensure_pull(window, cx);
        });
    }

    fn sync_views(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let (primary, reference) = {
            let video = self.app.video.read(cx);
            (video.frame_source(), video.reference_frame_source())
        };
        let letterbox = cx.theme().background;
        for (role, source) in [(Role::Primary, primary), (Role::Reference, reference)] {
            let id = match role {
                Role::Primary => "primary-video",
                Role::Reference => "reference-video",
            };
            let slot = match role {
                Role::Primary => &mut self.primary_view,
                Role::Reference => &mut self.reference_view,
            };
            match (source, slot.as_ref()) {
                (Some(source), Some(view)) => {
                    if !std::sync::Arc::ptr_eq(view.read(cx).source(), &source) {
                        view.update(cx, |view, cx| view.set_source(source, window, cx));
                    }
                }
                (Some(source), None) => {
                    *slot = Some(cx.new(|cx| VideoView::new(id, source, letterbox, window, cx)));
                }
                (None, _) => *slot = None,
            }
        }
        self.sync_visibility(cx);
        cx.notify();
    }

    /// Only views on screen report a size, so a hidden player stops drawing.
    fn sync_visibility(&mut self, cx: &mut Context<Self>) {
        let video = self.app.video.read(cx);
        let layout = video.layout().effective(video.is_dual());
        for (role, view) in [
            (Role::Primary, self.primary_view.clone()),
            (Role::Reference, self.reference_view.clone()),
        ] {
            if let Some(view) = view {
                let visible = self.active && layout.shows(role);
                if view.read(cx).is_visible() != visible {
                    view.update(cx, |view, cx| view.set_visible(visible, cx));
                }
            }
        }
    }

    fn sync_layout_select(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let layout = self.app.video.read(cx).layout();
        if self.layout_select.read(cx).selected_value() != Some(&layout) {
            self.layout_select.update(cx, |select, cx| {
                select.set_selected_value(&layout, window, cx)
            });
        }
    }

    fn sync_letterbox(&mut self, cx: &mut Context<Self>) {
        let letterbox = cx.theme().background;
        for view in [&self.primary_view, &self.reference_view]
            .into_iter()
            .flatten()
        {
            view.update(cx, |view, cx| view.set_letterbox(letterbox, cx));
        }
    }

    fn render_bar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let video = self.app.video.read(cx);
        let muted = video.is_muted(cx);
        let playing = video.is_playing();
        let slow = video.is_slow_motion();
        let continuous = video.is_continuous(cx);
        let can_play = video.clock(Role::Primary).is_some();
        let dual = video.is_dual();
        let sync = dual.then(|| video.sync_state());
        let warnings: Vec<(Role, SharedString)> = [Role::Primary, Role::Reference]
            .into_iter()
            .filter_map(|role| video.identity(role).warning().map(|w| (role, w.clone())))
            .collect();
        let checking = [Role::Primary, Role::Reference]
            .into_iter()
            .any(|role| video.identity(role).is_checking());
        let theme = cx.theme();
        h_flex()
            .id("video-bar")
            .w_full()
            .flex_shrink_0()
            .gap_2()
            .px_2()
            .py_1()
            .border_b_1()
            .border_color(theme.border)
            .text_xs()
            .child(
                Button::new("video-mute")
                    .ghost()
                    .xsmall()
                    .label(if muted { "Unmute" } else { "Mute" })
                    .selected(muted)
                    .toggled(muted)
                    .tooltip_with_action(
                        if muted { "Unmute audio" } else { "Mute audio" },
                        &ToggleMute,
                        Some(WORKSPACE_CONTEXT),
                    )
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.app.video.update(cx, |video, cx| video.toggle_mute(cx));
                    })),
            )
            .child(
                Button::new("video-play")
                    .ghost()
                    .xsmall()
                    .icon(if playing {
                        IconName::Pause
                    } else {
                        IconName::Play
                    })
                    .accessibility_label(if playing { "Pause" } else { "Play" })
                    .disabled(!can_play)
                    .tooltip_with_action(
                        if playing { "Pause" } else { "Play" },
                        &TogglePlay,
                        Some(WORKSPACE_CONTEXT),
                    )
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.app.video.update(cx, |video, cx| video.toggle_play(cx));
                    })),
            )
            .child(
                Select::new(&self.layout_select)
                    .id("video-layout")
                    .xsmall()
                    .w(rems(14.))
                    .accessibility_label("Video layout"),
            )
            .child(
                ButtonGroup::new("video-playback-mode")
                    .xsmall()
                    .outline()
                    .child(
                        Button::new("video-per-lap")
                            .label("Per lap")
                            .selected(!continuous)
                            .tooltip("Pause at the lap end and count into the next lap"),
                    )
                    .child(
                        Button::new("video-continuous")
                            .label("Continuous")
                            .selected(continuous)
                            .tooltip_with_action(
                                "Play through lap ends",
                                &ToggleContinuous,
                                Some(WORKSPACE_CONTEXT),
                            ),
                    )
                    .on_click(cx.listener(move |this, selected: &Vec<usize>, _, cx| {
                        let wants_continuous = selected.first() == Some(&1);
                        if wants_continuous != continuous {
                            this.app
                                .video
                                .update(cx, |video, cx| video.toggle_continuous(cx));
                        }
                    })),
            )
            .child(
                Button::new("video-slow-motion")
                    .ghost()
                    .xsmall()
                    .label("0.25×")
                    .selected(slow)
                    .toggled(slow)
                    .tooltip_with_action("Slow motion", &ToggleSlowMotion, Some(WORKSPACE_CONTEXT))
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.app
                            .video
                            .update(cx, |video, cx| video.toggle_slow_motion(cx));
                    })),
            )
            .child(div().flex_1())
            .when_some(sync, |this, state| {
                let label = SharedString::from(format!("Reference {}", sync_label(state)));
                this.child(
                    div()
                        .id("video-sync-state")
                        .role(AccessRole::Status)
                        .aria_label(label.clone())
                        .test_support()
                        .text_color(theme.muted_foreground)
                        .font_family(theme.mono_font_family.clone())
                        .child(label),
                )
            })
            .when(checking, |this| {
                this.child(
                    div()
                        .id("video-identity-checking")
                        .role(AccessRole::Status)
                        .aria_label("Verifying video")
                        .test_support()
                        .child(Tag::secondary().small().child("Verifying video…")),
                )
            })
            .children(warnings.into_iter().map(|(role, warning)| {
                let (id, text) = match role {
                    Role::Primary => ("video-identity-warning", "Video not verified"),
                    Role::Reference => ("reference-identity-warning", "Reference not verified"),
                };
                let tooltip = warning.clone();
                div()
                    .id(id)
                    .test_support()
                    .aria_label(warning)
                    .tooltip(move |window, cx| Tooltip::new(tooltip.clone()).build(window, cx))
                    .child(Tag::warning().small().child(text))
            }))
    }

    fn render_placeholder(&self, cx: &App) -> AnyElement {
        let session = self.app.session.read(cx);
        let loading = session.primary().filter(|slot| slot.is_loading());
        let (icon, title, description): (IconName, SharedString, SharedString) =
            match (self.app.video.read(cx).availability(), loading) {
                (VideoAvailability::Disabled, _) => (
                    IconName::Frame,
                    "Video playback is off".into(),
                    "This session was started without video playback.".into(),
                ),
                // Whether the new lap has a video is known once it loads.
                (_, Some(slot)) => (
                    IconName::LoaderCircle,
                    format!("Loading {}", slot.info().label).into(),
                    slot.info().title.clone(),
                ),
                (VideoAvailability::Starting, None) => (
                    IconName::LoaderCircle,
                    "Starting video".into(),
                    "The onboard video opens in a moment.".into(),
                ),
                (VideoAvailability::Failed(message), None) => (
                    IconName::TriangleAlert,
                    "Video unavailable".into(),
                    message.clone(),
                ),
                _ if session
                    .primary()
                    .is_some_and(|slot| matches!(slot.state(), RoleState::Failed(_))) =>
                {
                    (
                        IconName::TriangleAlert,
                        "No video".into(),
                        "The primary lap couldn’t be loaded.".into(),
                    )
                }
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

    /// A caption for `role`'s pane: role swatch, driver and `L8 · 3/12`.
    fn render_caption(&self, role: Role, cx: &App) -> Option<AnyElement> {
        let session = self.app.session.read(cx);
        let slot = session.slot(role)?;
        let info = slot.info();
        let lap = self.app.video.read(cx).lap(role);
        let caption = lap_caption(&info.label, lap);
        let (lap_role, id) = match role {
            Role::Primary => (LapRole::Primary, "primary-video-caption"),
            Role::Reference => (LapRole::Reference, "reference-video-caption"),
        };
        let theme = cx.theme();
        let spoken = SharedString::from(match &info.driver {
            Some(driver) => format!("{} {driver} {caption}", lap_role.label()),
            None => format!("{} {caption}", lap_role.label()),
        });
        Some(
            h_flex()
                .id(id)
                .role(AccessRole::Label)
                .aria_label(spoken)
                .test_support()
                .absolute()
                .top_2()
                .left_2()
                .gap_1p5()
                .px_2()
                .py_0p5()
                .rounded(theme.radius)
                .bg(theme.popover.opacity(0.85))
                .text_color(theme.popover_foreground)
                .text_xs()
                .child(Swatch::new(lap_role.color(theme)))
                .when_some(info.driver.clone(), |this, driver| {
                    this.child(div().font_semibold().child(driver))
                })
                .child(
                    div()
                        .font_family(theme.mono_font_family.clone())
                        .child(caption),
                )
                .into_any_element(),
        )
    }

    fn render_pane(&self, role: Role, cx: &App) -> gpui_kit::Div {
        let view = match role {
            Role::Primary => self.primary_view.clone(),
            Role::Reference => self.reference_view.clone(),
        };
        div()
            .relative()
            .size_full()
            .overflow_hidden()
            .bg(cx.theme().background)
            .when_some(view, |this, view| this.child(view))
            .children(self.render_caption(role, cx))
    }

    /// The composed video panes.
    fn render_stage(&self, cx: &App) -> AnyElement {
        let video = self.app.video.read(cx);
        let layout = video.layout().effective(video.is_dual());
        let theme = cx.theme();
        let inset = |role: Role, left: bool| {
            self.render_pane(role, cx)
                .absolute()
                .bottom_3()
                .when(left, |this| this.left_3())
                .when(!left, |this| this.right_3())
                .w(relative(0.3))
                .h(relative(0.3))
                .border_1()
                .border_color(theme.border)
                .rounded(theme.radius)
                .shadow_md()
        };
        let stage = match layout {
            ComposeLayout::Split => h_flex()
                .items_stretch()
                .size_full()
                .gap_px()
                .bg(theme.border)
                .child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .child(self.render_pane(Role::Primary, cx)),
                )
                .child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .child(self.render_pane(Role::Reference, cx)),
                ),
            ComposeLayout::PrimaryWithReferenceInset => div()
                .relative()
                .size_full()
                .child(self.render_pane(Role::Primary, cx))
                .child(inset(Role::Reference, false)),
            ComposeLayout::ReferenceWithPrimaryInset => div()
                .relative()
                .size_full()
                .child(self.render_pane(Role::Reference, cx))
                .child(inset(Role::Primary, true)),
            ComposeLayout::PrimaryOnly => {
                div().size_full().child(self.render_pane(Role::Primary, cx))
            }
            ComposeLayout::ReferenceOnly => div()
                .size_full()
                .child(self.render_pane(Role::Reference, cx)),
        };
        let label = SharedString::from(format!("Video, {}", layout.label()));
        div()
            .id("video-stage")
            .role(AccessRole::Group)
            .aria_label(label)
            .test_support()
            .relative()
            .size_full()
            .child(stage)
            .child(self.overlay.clone())
            .into_any_element()
    }
}

/// The reference sync indicator, sentence case except established acronyms.
fn sync_label(state: SyncState) -> &'static str {
    match state {
        SyncState::Wait => "waiting",
        SyncState::NoMap => "not aligned",
        SyncState::Aligning => "aligning",
        SyncState::Locked => "locked",
        SyncState::Best => "best effort",
        SyncState::RealTime => "1×",
        SyncState::Gps => "GPS",
        SyncState::Corner => "corner",
        SyncState::Hold => "holding",
        SyncState::Straight => "straight",
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
        self.sync_visibility(cx);
    }

    fn on_added_to(&mut self, group: WeakEntity<TabGroup>, _: &mut Window, _: &mut Context<Self>) {
        self.group = Some(group);
    }

    /// Zoomed (`F`), the other panels leave the screen: keep keyboard focus
    /// on the video so Space, the arrows and Escape keep working.
    fn set_zoomed(&mut self, zoomed: bool, window: &mut Window, cx: &mut Context<Self>) {
        if zoomed && !self.focus_handle.contains_focused(window, cx) {
            window.focus(&self.focus_handle, cx);
        }
        cx.notify();
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
        let shows_video = {
            let video = self.app.video.read(cx);
            self.primary_view.is_some() || video.clock(Role::Primary).is_some()
        };
        let content = if shows_video {
            self.render_stage(cx)
        } else {
            self.render_placeholder(cx)
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
/// rebuild it by name), its palette commands and their handlers. Called
/// once from [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::Video, cx);
    use CommandCategory::Commands;
    for spec in [
        CommandSpec::new(
            "verify-video",
            "Verify video identity",
            Commands,
            VerifyVideoIdentity,
        )
        .keywords(["blake3", "hash", "video", "sync"]),
        CommandSpec::new(
            "pace-corners",
            "Reference pacing: corners",
            Commands,
            PaceReferenceByCorners,
        )
        .keywords(["video", "sync", "playback"]),
        CommandSpec::new(
            "pace-gps",
            "Reference pacing: GPS",
            Commands,
            PaceReferenceByGps,
        )
        .keywords(["video", "sync", "playback"]),
        CommandSpec::new(
            "pace-recording",
            "Reference pacing: recording speed",
            Commands,
            PaceReferenceByRecording,
        )
        .keywords(["video", "sync", "playback", "real time"]),
    ] {
        commands::register(cx, spec);
    }
    // Palette entries dispatch from the dialog, outside any panel: handle
    // them at the application level.
    cx.on_action(|_: &VerifyVideoIdentity, cx| {
        with_video(cx, |video, cx| video.verify_identity(cx))
    });
    cx.on_action(|_: &PaceReferenceByCorners, cx| {
        with_video(cx, |video, cx| {
            video.set_reference_playback(ReferencePlayback::Corners, cx)
        })
    });
    cx.on_action(|_: &PaceReferenceByGps, cx| {
        with_video(cx, |video, cx| {
            video.set_reference_playback(ReferencePlayback::Gps, cx)
        })
    });
    cx.on_action(|_: &PaceReferenceByRecording, cx| {
        with_video(cx, |video, cx| {
            video.set_reference_playback(ReferencePlayback::Recording, cx)
        })
    });
}

fn with_video(
    cx: &mut App,
    f: impl FnOnce(&mut crate::state::VideoController, &mut Context<crate::state::VideoController>),
) {
    if let Some(video) = AppState::try_global(cx).map(|state| state.video.clone()) {
        video.update(cx, f);
    }
}
