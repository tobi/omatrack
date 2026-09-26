//! Video: the primary and reference onboard videos, composed, synced to the
//! telemetry cursor, with the telemetry HUD over them.
//!
//! Behaviour lives in [`VideoController`](crate::state::VideoController);
//! this panel owns presentation: the transport bar, the composed video panes
//! with their role captions, the HUD (inside the large pane, never across
//! the seam between two videos) and the countdown ([`overlay`]), and the
//! display-frame pull that drives the cursor while the primary plays.
//!
//! Docked, the panel has no dock title bar; the pictures lead and one
//! control row sits under them, serving the video and the traces below it
//! (see [`VideoPanel::render_bar`]): round play/pause, the `0.25×` toggle,
//! `Per lap | Continuous`, `Distance | Time`, then where the cursor is
//! (`Cursor at 368 m, Turn 1`), chips that need attention (a degraded
//! reference sync, identity checks, a missing reference video), and small
//! icon buttons: mute, composition (layouts 1-5 plus reference pacing),
//! fullscreen and the traces' tools menu.
//!
//! `F` (or the bar's fullscreen button) puts this panel on the fullscreen
//! stage: the workspace renders it alone over the whole window (no title
//! bar, docks or status bar; the dock layout is untouched behind it) and
//! calls [`VideoPanel::set_stage`]. On the stage the videos are composed on
//! black per [`stage`], the lap filmstrip (the workspace's one entity)
//! runs in a lane at the bottom, the broadcast telemetry band and the live
//! delta bar ride over the pictures ([`overlay`]), and the transport
//! controls float at the bottom, hiding [`CONTROLS_HIDE_AFTER`] after the
//! last pointer motion or key press. Escape leaves (the workspace routes
//! it). Nothing enters fullscreen on its own.

mod clock;
mod icons;
pub mod overlay;
pub mod stage;

use omatrack_ui::TypeScale as _;
use std::time::{Duration, Instant};

use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Icon, IconName, Selectable as _, Sizable as _,
    StyledExt as _, Theme, WindowExt as _,
    button::{Button, ButtonCustomVariant, ButtonGroup, ButtonRounded, ButtonVariants as _},
    dock::{BasePanel, Panel, PanelEvent, TabGroup},
    h_flex,
    menu::DropdownMenu as _,
    notification::Notification,
    separator::Separator,
    tag::Tag,
    tooltip::Tooltip,
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    Action, AnyElement, App, AppContext as _, Context, CursorHideMode, Entity, EventEmitter,
    FocusHandle, Focusable, InteractiveElement as _, IntoElement, ParentElement as _, Render,
    Role as AccessRole, SharedString, StatefulInteractiveElement as _, Styled as _, Subscription,
    Task, TestSupportExt as _, WeakEntity, Window, div, px, relative,
};
use mpv_player::{VideoView, VideoViewEvent};
use omatrack_core::playback::{ReferencePlayback, SyncState};
use omatrack_ui::LapRole;

use crate::actions::{
    ComposeLayout1, ComposeLayout2, ComposeLayout3, ComposeLayout4, ComposeLayout5, ExitFullscreen,
    ResizeLanes, Role, SeekBack, SeekForward, ToggleContinuous, ToggleCornerEdit, ToggleFit,
    ToggleMute, TogglePlay, ToggleSlowMotion, ToggleVideoFullscreen, ToggleXAxis, ZoomIn, ZoomOut,
    ZoomReset,
};
use crate::commands::{self, CommandCategory, CommandSpec};
use crate::keymap::WORKSPACE_CONTEXT;
use crate::panels::{PanelKind, empty_state, panel_body};
use crate::state::{AppState, ComposeLayout, RoleState, VideoAvailability, VideoEvent};
use crate::workspace::Filmstrip;
use omatrack_trace::XAxis;

use clock::LapClock;
use icons::VideoIcons;
use overlay::{StageOverlay, VideoOverlay, lap_caption};

/// The fullscreen controls hide this long after the last pointer motion or
/// key press (the Qt stage's 1.8 s, rounded up).
pub const CONTROLS_HIDE_AFTER: Duration = Duration::from_millis(2000);
/// Height of the fullscreen controls, rem.
const CONTROLS_REMS: f32 = 2.75;
/// Height of the fullscreen delta lane over the pictures, rem.
const DELTA_LANE_REMS: f32 = 4.25;

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

/// The short name of a composition on the transport bar.
pub fn layout_short_label(layout: ComposeLayout) -> &'static str {
    match layout {
        ComposeLayout::Split => "Split",
        ComposeLayout::PrimaryWithReferenceInset => "Primary + inset",
        ComposeLayout::ReferenceWithPrimaryInset => "Reference + inset",
        ComposeLayout::PrimaryOnly => "Primary",
        ComposeLayout::ReferenceOnly => "Reference",
    }
}

/// The action that selects `layout` (keys 1-5).
fn layout_action(layout: ComposeLayout) -> Box<dyn Action> {
    match layout {
        ComposeLayout::Split => Box::new(ComposeLayout1),
        ComposeLayout::PrimaryWithReferenceInset => Box::new(ComposeLayout2),
        ComposeLayout::ReferenceWithPrimaryInset => Box::new(ComposeLayout3),
        ComposeLayout::PrimaryOnly => Box::new(ComposeLayout4),
        ComposeLayout::ReferenceOnly => Box::new(ComposeLayout5),
    }
}

/// Where a pane's picture frame sits.
#[derive(Clone, Copy, PartialEq, Eq)]
enum PaneAlign {
    Start,
    Center,
    End,
}

/// A pane's role in the composition.
#[derive(Clone, Copy, PartialEq, Eq)]
enum PanePlace {
    /// A pane filling its part of the stage.
    Main { align: PaneAlign },
    /// The small picture-in-picture pane.
    Inset,
}

pub struct VideoPanel {
    app: AppState,
    focus_handle: FocusHandle,
    primary_view: Option<Entity<VideoView>>,
    reference_view: Option<Entity<VideoView>>,
    overlay: Entity<VideoOverlay>,
    icons: VideoIcons,
    group: Option<WeakEntity<TabGroup>>,
    active: bool,
    /// A display-frame pull is scheduled.
    pumping: bool,
    /// Presented on the fullscreen stage.
    stage: Option<Stage>,
    clock: Entity<LapClock>,
    _subscriptions: Vec<Subscription>,
}

/// The fullscreen stage's presentation state.
struct Stage {
    /// The workspace's filmstrip, shown in the bottom lane.
    filmstrip: Entity<Filmstrip>,
    /// The floating controls are shown.
    controls: bool,
    /// The pointer is over the controls (they stay while it is).
    hovered: bool,
    /// Hides the controls after [`CONTROLS_HIDE_AFTER`]; replaced (so
    /// cancelled) by every reveal.
    hide: Option<Task<()>>,
    /// The telemetry band is shown.
    hud: bool,
    /// The cursor-hide policy to restore on leaving.
    cursor_mode: CursorHideMode,
    _keys: Subscription,
}

impl VideoPanel {
    pub fn new(app: AppState, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let overlay = cx.new(|cx| VideoOverlay::new(app.clone(), cx));
        let clock = cx.new(|cx| LapClock::new(app.clone(), cx));
        let subscriptions = vec![
            cx.subscribe_in(&app.video, window, |this, _, event, window, cx| {
                this.on_video_event(event, window, cx);
            }),
            // Structural changes only: the controller does not notify per
            // frame. Playing starts the display-frame pull.
            cx.observe_in(&app.video, window, |this, _, window, cx| {
                this.sync_visibility(cx);
                this.ensure_pull(window, cx);
                cx.notify();
            }),
            cx.observe(&app.preferences, |_, _, cx| cx.notify()),
            cx.observe(&app.session, |_, _, cx| cx.notify()),
            // The letterbox is painted by the video element from a colour it
            // holds; follow theme changes (Omarchy hot reload) into it.
            cx.observe_global::<Theme>(|this, cx| this.sync_letterbox(cx)),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle().tab_stop(true),
            primary_view: None,
            reference_view: None,
            overlay,
            icons: VideoIcons::new(),
            group: None,
            active: true,
            pumping: false,
            stage: None,
            clock,
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

    /// The HUD layer.
    pub fn overlay(&self) -> &Entity<VideoOverlay> {
        &self.overlay
    }

    /// Whether a display-frame pull is scheduled.
    pub fn is_pulling(&self) -> bool {
        self.pumping
    }

    /// Present the panel on the fullscreen stage with `filmstrip` in its
    /// bottom lane (`Some`), or back in its dock (`None`). The workspace
    /// calls this; it owns the switch and the focus.
    pub fn set_stage(
        &mut self,
        filmstrip: Option<Entity<Filmstrip>>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match filmstrip {
            Some(filmstrip) => {
                if self.stage.is_some() {
                    return;
                }
                filmstrip.update(cx, |strip, cx| strip.set_on_stage(true, cx));
                // Keys (Space, arrows, 1-5) hide the pointer until it moves.
                let cursor_mode = cx.cursor_hide_mode();
                cx.set_cursor_hide_mode(CursorHideMode::OnTypingAndAction);
                self.stage = Some(Stage {
                    filmstrip,
                    controls: true,
                    hovered: false,
                    hide: None,
                    hud: true,
                    cursor_mode,
                    // Any keystroke (after its action ran) brings hidden
                    // controls back; key-down listeners never see bound keys.
                    _keys: cx.observe_keystrokes(|this, _, _, cx| this.reveal_controls(cx)),
                });
                self.reveal_controls(cx);
            }
            None => {
                let Some(stage) = self.stage.take() else {
                    return;
                };
                stage
                    .filmstrip
                    .update(cx, |strip, cx| strip.set_on_stage(false, cx));
                cx.set_cursor_hide_mode(stage.cursor_mode);
                self.overlay
                    .update(cx, |overlay, cx| overlay.set_stage(None, cx));
            }
        }
        self.sync_letterbox(cx);
        self.ensure_pull(window, cx);
        cx.notify();
    }

    /// Whether the panel is on the fullscreen stage.
    pub fn is_on_stage(&self) -> bool {
        self.stage.is_some()
    }

    /// Whether the stage's floating controls are shown (false off stage).
    pub fn controls_visible(&self) -> bool {
        self.stage.as_ref().is_some_and(|stage| stage.controls)
    }

    /// Whether the stage shows the telemetry band.
    pub fn is_hud_shown(&self) -> bool {
        self.stage.as_ref().is_some_and(|stage| stage.hud)
    }

    /// Show or hide the stage's telemetry band.
    pub fn toggle_hud(&mut self, cx: &mut Context<Self>) {
        if let Some(stage) = self.stage.as_mut() {
            stage.hud = !stage.hud;
            cx.notify();
        }
    }

    /// Show the stage controls and restart their hide timer.
    pub fn reveal_controls(&mut self, cx: &mut Context<Self>) {
        let Some(stage) = self.stage.as_mut() else {
            return;
        };
        if !stage.controls {
            stage.controls = true;
            cx.notify();
        }
        stage.hide = Some(cx.spawn(async move |this, cx| {
            cx.background_executor().timer(CONTROLS_HIDE_AFTER).await;
            let _ = this.update(cx, |this, cx| {
                if let Some(stage) = this.stage.as_mut()
                    && stage.controls
                    && !stage.hovered
                {
                    stage.controls = false;
                    stage.hide = None;
                    cx.notify();
                }
            });
        }));
    }

    fn set_controls_hovered(&mut self, hovered: bool, cx: &mut Context<Self>) {
        let Some(stage) = self.stage.as_mut() else {
            return;
        };
        stage.hovered = hovered;
        if !hovered {
            // Leaving the controls starts the countdown to hiding them.
            self.reveal_controls(cx);
        }
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
        let letterbox = if self.stage.is_some() {
            gpui_kit::black()
        } else {
            cx.theme().background
        };
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
                    let view = cx.new(|cx| VideoView::new(id, source, letterbox, window, cx));
                    // The picture frame follows the video's aspect, known
                    // from the first frame of each source.
                    cx.subscribe(&view, |_, _, _: &VideoViewEvent, cx| cx.notify())
                        .detach();
                    *slot = Some(view);
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

    fn sync_letterbox(&mut self, cx: &mut Context<Self>) {
        let letterbox = if self.stage.is_some() {
            gpui_kit::black()
        } else {
            cx.theme().background
        };
        for view in [&self.primary_view, &self.reference_view]
            .into_iter()
            .flatten()
        {
            view.update(cx, |view, cx| view.set_letterbox(letterbox, cx));
        }
    }

    // ── transport bar ───────────────────────────────────────────────

    /// Play/pause (cancels a running countdown).
    fn render_play(&self, cx: &mut Context<Self>) -> Button {
        let video = self.app.video.read(cx);
        let playing = video.is_playing();
        let counting = video.countdown().is_some();
        let can_play = video.clock(Role::Primary).is_some();
        let play_label = if counting {
            "Cancel the countdown"
        } else if playing {
            "Pause"
        } else {
            "Play"
        };
        Button::new("video-play")
            .ghost()
            .xsmall()
            .icon(if playing {
                IconName::Pause
            } else {
                IconName::Play
            })
            .accessibility_label(play_label)
            .disabled(!can_play)
            .tooltip_with_action(play_label, &TogglePlay, Some(WORKSPACE_CONTEXT))
            .on_click(cx.listener(|this, _, _, cx| {
                this.app.video.update(cx, |video, cx| video.toggle_play(cx));
            }))
    }

    fn render_mute(&self, cx: &mut Context<Self>) -> Button {
        let muted = self.app.video.read(cx).is_muted(cx);
        let mute_label = if muted { "Unmute" } else { "Mute" };
        Button::new("video-mute")
            .ghost()
            .xsmall()
            .icon(if muted {
                self.icons.muted.clone()
            } else {
                self.icons.volume.clone()
            })
            .accessibility_label(mute_label)
            .tooltip_with_action(mute_label, &ToggleMute, Some(WORKSPACE_CONTEXT))
            .on_click(cx.listener(|this, _, _, cx| {
                this.app.video.update(cx, |video, cx| video.toggle_mute(cx));
            }))
    }

    /// The clock rate: the selected segment is the one playing.
    fn render_rate(&self, cx: &mut Context<Self>) -> ButtonGroup {
        let slow = self.app.video.read(cx).is_slow_motion();
        ButtonGroup::new("video-rate")
            .xsmall()
            .outline()
            .child(
                Button::new("video-rate-normal")
                    .label("1×")
                    .selected(!slow)
                    .when(!slow, |button| button.primary())
                    .accessibility_label("Normal speed")
                    .tooltip_with_action(
                        "Normal speed",
                        &ToggleSlowMotion,
                        Some(WORKSPACE_CONTEXT),
                    ),
            )
            .child(
                Button::new("video-slow-motion")
                    .label("0.25×")
                    .selected(slow)
                    .when(slow, |button| button.primary())
                    .accessibility_label("Slow motion")
                    .tooltip_with_action(
                        "Slow motion, quarter speed",
                        &ToggleSlowMotion,
                        Some(WORKSPACE_CONTEXT),
                    ),
            )
            .on_click(cx.listener(move |this, selected: &Vec<usize>, _, cx| {
                let wants_slow = selected.first() == Some(&1);
                if wants_slow != slow {
                    this.app
                        .video
                        .update(cx, |video, cx| video.toggle_slow_motion(cx));
                }
            }))
    }

    /// Lap-end behaviour: pause and count in, or play through.
    fn render_mode(&self, cx: &mut Context<Self>) -> ButtonGroup {
        let continuous = self.app.video.read(cx).is_continuous(cx);
        // Selection is neutral, like `Distance | Time`: colour is for lap
        // roles and Δ.
        ButtonGroup::new("video-playback-mode")
            .xsmall()
            .outline()
            .child(
                Button::new("video-per-lap")
                    .label("Per lap")
                    .selected(!continuous)
                    .tooltip_with_action(
                        "At the lap end, pause and count into the next lap",
                        &ToggleContinuous,
                        Some(WORKSPACE_CONTEXT),
                    ),
            )
            .child(
                Button::new("video-continuous")
                    .label("Continuous")
                    .selected(continuous)
                    .tooltip_with_action(
                        "Play through lap ends into the next lap",
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
            }))
    }

    /// Enter (docked) or leave (on stage) the fullscreen stage.
    fn render_fullscreen_button(&self) -> Button {
        let on_stage = self.stage.is_some();
        let (id, label, icon) = if on_stage {
            (
                "video-exit-fullscreen",
                "Leave fullscreen",
                self.icons.exit_fullscreen.clone(),
            )
        } else {
            (
                "video-enter-fullscreen",
                "Fullscreen",
                self.icons.fullscreen.clone(),
            )
        };
        let button = Button::new(id)
            .ghost()
            .xsmall()
            .icon(icon)
            .accessibility_label(label);
        if on_stage {
            button
                .label("Exit")
                .tooltip_with_action(label, &ExitFullscreen, Some(WORKSPACE_CONTEXT))
                .on_click(|_, window, cx| window.dispatch_action(Box::new(ExitFullscreen), cx))
        } else {
            button
                .tooltip_with_action(label, &ToggleVideoFullscreen, Some(WORKSPACE_CONTEXT))
                .on_click(|_, window, cx| {
                    window.dispatch_action(Box::new(ToggleVideoFullscreen), cx)
                })
        }
    }

    /// The control row under the pictures, left to right: the round
    /// play/pause, the `0.25×` toggle, `Per lap | Continuous`, the traces'
    /// `Distance | Time` axis, then where the cursor is, the chips that need
    /// attention (a degraded sync, identity checks, a missing reference
    /// video) and small icon buttons: mute, composition, fullscreen and the
    /// traces' overflow menu (fit, lane sizing, corner editing, zoom). One
    /// row serves the video and the traces, so the centre carries no
    /// toolbars of its own. Every control dispatches the action of its key
    /// and names that key in its tooltip.
    fn render_bar(&self, window: &Window, cx: &mut Context<Self>) -> impl IntoElement {
        let dual = self.app.video.read(cx).is_dual();
        let layout = self.app.video.read(cx).layout().effective(dual);
        let play = self.render_play(cx);
        let slow = self.render_slow_toggle(cx);
        let mode = self.render_mode(cx).small();
        let axis = self.render_axis(cx);
        let mute = self.render_mute(cx);
        let traces = self.render_traces_menu(cx);
        let theme = cx.theme();
        // The row's one strong control: a filled disc in the foreground.
        let play = play
            .small()
            .rounded(ButtonRounded::Size(window.rem_size() * 0.75))
            .custom(
                ButtonCustomVariant::new(cx)
                    .color(theme.foreground)
                    .foreground(theme.background)
                    .hover(theme.foreground.opacity(0.85))
                    .active(theme.foreground.opacity(0.7)),
            );

        let layout_icon: Icon = match layout {
            ComposeLayout::Split => self.icons.split.clone(),
            ComposeLayout::PrimaryWithReferenceInset | ComposeLayout::ReferenceWithPrimaryInset => {
                self.icons.inset.clone()
            }
            ComposeLayout::PrimaryOnly | ComposeLayout::ReferenceOnly => self.icons.single.clone(),
        };
        let focus = self.focus_handle.clone();
        let controller = self.app.video.clone();
        let compose_label = SharedString::from(format!("Video layout: {}", layout.label()));
        let compose = Button::new("video-layout")
            .ghost()
            .small()
            .icon(layout_icon)
            .accessibility_label(compose_label.clone())
            .disabled(!dual)
            .when(!dual, |this| {
                this.tooltip("Layouts need a reference video; the primary is shown alone")
            })
            .when(dual, |this| this.tooltip(compose_label))
            .dropdown_menu(move |menu, _, cx| {
                let (current, pacing) = {
                    let video = controller.read(cx);
                    (video.layout(), video.reference_playback(cx))
                };
                let mut menu = menu.action_context(focus.clone()).label("Layout");
                for layout in ComposeLayout::ALL {
                    menu = menu.menu_with_check(
                        layout.label(),
                        layout == current,
                        layout_action(layout),
                    );
                }
                menu.separator()
                    .label("Reference pacing")
                    .menu_with_check(
                        "Hold 1× through corners",
                        pacing == ReferencePlayback::Corners,
                        Box::new(PaceReferenceByCorners),
                    )
                    .menu_with_check(
                        "Follow the GPS map",
                        pacing == ReferencePlayback::Gps,
                        Box::new(PaceReferenceByGps),
                    )
                    .menu_with_check(
                        "Recording speed",
                        pacing == ReferencePlayback::Recording,
                        Box::new(PaceReferenceByRecording),
                    )
            });

        h_flex()
            .id("video-bar")
            .role(AccessRole::Toolbar)
            .aria_label("Playback and traces")
            .test_support()
            .w_full()
            .flex_shrink_0()
            .items_center()
            .gap_2()
            .px_2()
            .py_1p5()
            .overflow_hidden()
            .text_label()
            .child(div().flex_shrink_0().child(play))
            .child(div().flex_shrink_0().child(slow))
            .child(div().flex_shrink_0().child(mode))
            .child(div().flex_shrink_0().child(axis))
            // Where the cursor is: right-aligned, and the first thing to
            // give way on a narrow window.
            .child(
                h_flex()
                    .flex_1()
                    .min_w_0()
                    .justify_end()
                    .overflow_hidden()
                    .child(self.overlay.clone()),
            )
            .child(self.render_status(cx))
            .child(
                h_flex()
                    .flex_shrink_0()
                    .gap_0p5()
                    .child(mute)
                    .child(compose)
                    .child(self.render_fullscreen_button().small())
                    .child(traces),
            )
    }

    /// Quarter speed on or off (the docked row's one rate control; the
    /// fullscreen stage keeps `1× | 0.25×`).
    fn render_slow_toggle(&self, cx: &mut Context<Self>) -> Button {
        let slow = self.app.video.read(cx).is_slow_motion();
        Button::new("video-slow-motion")
            .outline()
            .small()
            .label("0.25×")
            .selected(slow)
            .toggled(slow)
            .accessibility_label("Slow motion, quarter speed")
            .tooltip_with_action(
                "Slow motion, quarter speed",
                &ToggleSlowMotion,
                Some(WORKSPACE_CONTEXT),
            )
            .on_click(cx.listener(|this, _, _, cx| {
                this.app
                    .video
                    .update(cx, |video, cx| video.toggle_slow_motion(cx));
            }))
    }

    /// The traces' x-axis (`t`): distance or time.
    fn render_axis(&self, cx: &mut Context<Self>) -> ButtonGroup {
        let axis = self.app.viewport.read(cx).axis();
        let has_data = self.app.session.read(cx).analysis().is_some();
        let keys = self.focus_handle.clone();
        ButtonGroup::new("trace-axis")
            .small()
            .outline()
            .disabled(!has_data)
            .child(
                Button::new("trace-axis-distance")
                    .label("Distance")
                    .selected(axis == XAxis::Distance)
                    .tooltip_with_action(
                        "Traces by distance",
                        &ToggleXAxis,
                        Some(WORKSPACE_CONTEXT),
                    ),
            )
            .child(
                Button::new("trace-axis-time")
                    .label("Time")
                    .selected(axis == XAxis::Time)
                    .tooltip_with_action("Traces by time", &ToggleXAxis, Some(WORKSPACE_CONTEXT)),
            )
            .on_click(move |clicked: &Vec<usize>, window, cx| {
                let wanted = if clicked.contains(&0) {
                    XAxis::Distance
                } else {
                    XAxis::Time
                };
                if wanted != axis {
                    keys.dispatch_action(&ToggleXAxis, window, cx);
                }
            })
    }

    /// The traces' less frequent tools, behind one icon: fit, lane sizing,
    /// corner editing and zoom (each also on its key and in the palette).
    fn render_traces_menu(&self, cx: &mut Context<Self>) -> impl IntoElement + use<> {
        let has_data = self.app.session.read(cx).analysis().is_some();
        let focus = self.focus_handle.clone();
        let preferences = self.app.preferences.clone();
        Button::new("trace-tools")
            .ghost()
            .small()
            .icon(IconName::Ellipsis)
            .accessibility_label("Trace tools")
            .tooltip("Trace tools")
            .disabled(!has_data)
            .dropdown_menu(move |menu, _, cx| {
                let fit = preferences.read(cx).config().trace.is_fitting_channels();
                menu.action_context(focus.clone())
                    .menu_with_check("Fit lanes to the height", fit, Box::new(ToggleFit))
                    .menu("Resize lanes…", Box::new(ResizeLanes))
                    .menu("Edit corners…", Box::new(ToggleCornerEdit))
                    .separator()
                    .menu("Zoom in", Box::new(ZoomIn))
                    .menu("Zoom out", Box::new(ZoomOut))
                    .menu("Whole lap", Box::new(ZoomReset))
            })
    }

    /// The chips that need attention: a degraded reference sync (a healthy
    /// one is silent; the title bar states the alignment), identity checks
    /// and a missing reference video.
    fn render_status(&self, cx: &App) -> impl IntoElement {
        let video = self.app.video.read(cx);
        let session = self.app.session.read(cx);
        let dual = video.is_dual();
        let sync = dual
            .then(|| (video.sync_state(), video.reference_playback(cx)))
            .filter(|(state, _)| matches!(state, SyncState::NoMap | SyncState::Best));
        let warnings: Vec<(Role, SharedString)> = [Role::Primary, Role::Reference]
            .into_iter()
            .filter_map(|role| video.identity(role).warning().map(|w| (role, w.clone())))
            .collect();
        let checking = [Role::Primary, Role::Reference]
            .into_iter()
            .any(|role| video.identity(role).is_checking());
        // A loaded reference lap whose recording has no (or a failed) video.
        let reference_missing = (!dual
            && video.clock(Role::Primary).is_some()
            && session
                .reference()
                .is_some_and(|slot| matches!(slot.state(), RoleState::Loaded(_))))
        .then(|| match video.reference_availability() {
            VideoAvailability::Failed(message) => (
                "Reference video failed",
                SharedString::from(format!("The reference video could not play: {message}")),
                true,
            ),
            VideoAvailability::Starting => (
                "Reference video starting",
                "The reference video opens in a moment.".into(),
                false,
            ),
            _ => (
                "No reference video",
                "The reference lap’s recording has no onboard video; the primary is shown alone."
                    .into(),
                false,
            ),
        });

        h_flex()
            .flex_shrink_0()
            .gap_1()
            .when_some(sync, |this, (state, pacing)| {
                let label = SharedString::from(format!("Reference {}", sync_label(state)));
                let explain = SharedString::from(format!(
                    "{} Pacing: {}.",
                    sync_description(state),
                    pacing_label(pacing)
                ));
                this.child(
                    div()
                        .id("video-sync-state")
                        .role(AccessRole::Status)
                        .aria_label(label.clone())
                        .test_support()
                        .tooltip(move |window, cx| Tooltip::new(explain.clone()).build(window, cx))
                        .child(Tag::warning().small().child(label)),
                )
            })
            .when_some(reference_missing, |this, (text, explain, failed)| {
                this.child(
                    div()
                        .id("video-reference-missing")
                        .role(AccessRole::Status)
                        .aria_label(text)
                        .test_support()
                        .tooltip(move |window, cx| Tooltip::new(explain.clone()).build(window, cx))
                        .child(
                            if failed {
                                Tag::warning()
                            } else {
                                Tag::secondary()
                            }
                            .small()
                            .child(text),
                        ),
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

    // ── stage ───────────────────────────────────────────────────────

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

    /// The caption of `role`'s pane: a role badge (`P` / `R` on the role
    /// colour), driver, lap, lap time and `3/12`; `compact` (an inset) keeps
    /// the badge and lap only.
    fn render_caption(&self, role: Role, compact: bool, cx: &App) -> Option<AnyElement> {
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
        let (color, on_color) = match lap_role {
            LapRole::Primary => (theme.primary, theme.primary_foreground),
            LapRole::Reference => (theme.warning, theme.warning_foreground),
        };
        let spoken = SharedString::from(match &info.driver {
            Some(driver) => format!("{} {driver} {caption}", lap_role.label()),
            None => format!("{} {caption}", lap_role.label()),
        });
        let place = lap.and_then(|lap| {
            let ix = lap.laps().iter().position(|l| l.id == lap.lap_id())?;
            Some(SharedString::from(format!(
                "{}/{}",
                ix + 1,
                lap.laps().len()
            )))
        });
        Some(
            h_flex()
                .id(id)
                .role(AccessRole::Label)
                .aria_label(spoken)
                .test_support()
                .absolute()
                .top_1p5()
                .left_1p5()
                .max_w(relative(0.9))
                .overflow_hidden()
                .gap_1p5()
                .p_0p5()
                .pr_2()
                .rounded(theme.radius)
                .border_1()
                .border_color(color.opacity(0.6))
                .bg(theme.background.opacity(0.88))
                .text_color(theme.foreground)
                .text_xs()
                .whitespace_nowrap()
                .shadow_sm()
                .child(
                    div()
                        .flex_shrink_0()
                        .px_1()
                        .rounded(theme.radius_tokens().sm)
                        .bg(color)
                        .text_color(on_color)
                        .numeric()
                        .font_semibold()
                        .child(lap_role.marker()),
                )
                .when_some(info.driver.clone().filter(|_| !compact), |this, driver| {
                    this.child(div().min_w_0().truncate().font_medium().child(driver))
                })
                .child(
                    div()
                        .flex_shrink_0()
                        .numeric()
                        .font_semibold()
                        .child(info.label.clone()),
                )
                .when(!compact && !info.time.is_empty(), |this| {
                    this.child(div().flex_shrink_0().numeric().child(info.time.clone()))
                })
                .when_some(place.filter(|_| !compact), |this, place| {
                    this.child(
                        div()
                            .flex_shrink_0()
                            .numeric()
                            .text_color(theme.muted_foreground)
                            .child(place),
                    )
                })
                .into_any_element(),
        )
    }

    /// The width / height of `role`'s picture: the latest frame's (the
    /// backend renders at the video's aspect), else 16:9 until one arrives.
    fn picture_aspect(&self, role: Role, cx: &App) -> f32 {
        let view = match role {
            Role::Primary => self.primary_view.as_ref(),
            Role::Reference => self.reference_view.as_ref(),
        };
        view.and_then(|view| view.read(cx).image())
            .map(|image| image.size(0))
            .filter(|size| size.width.0 > 0 && size.height.0 > 0)
            .map_or(16. / 9., |size| size.width.0 as f32 / size.height.0 as f32)
    }

    /// One video pane. A main pane holds a frame as tall as the pane and as
    /// wide as the picture (clamped to the pane), aligned per `place`, with
    /// the video, its caption, the inset (`inset`) and, for the large pane,
    /// the HUD layer: so they sit on the picture's corners, never out in a
    /// pillarbox, while a letterbox above or below the picture stays inside
    /// the frame and takes them first. An inset pane is the picture alone,
    /// sized by the frame it sits in, with a compact caption.
    fn render_pane(
        &self,
        role: Role,
        place: PanePlace,
        inset: Option<AnyElement>,
        cx: &App,
    ) -> AnyElement {
        let view = match role {
            Role::Primary => self.primary_view.clone(),
            Role::Reference => self.reference_view.clone(),
        };
        let (id, frame_id) = match role {
            Role::Primary => ("primary-video-pane", "primary-video-frame"),
            Role::Reference => ("reference-video-pane", "reference-video-frame"),
        };
        let theme = cx.theme();
        let aspect = self.picture_aspect(role, cx);
        let align = match place {
            PanePlace::Main { align } => align,
            PanePlace::Inset => {
                return div()
                    .id(id)
                    .test_support()
                    .absolute()
                    .bottom_2()
                    .right_2()
                    .h(relative(0.42))
                    .max_w(relative(0.32))
                    .aspect_ratio(aspect)
                    .overflow_hidden()
                    .bg(theme.background)
                    .border_1()
                    .border_color(theme.border)
                    .rounded(theme.radius)
                    .shadow_md()
                    .when_some(view, |this, view| this.child(view))
                    .children(self.render_caption(role, true, cx))
                    .into_any_element();
            }
        };
        let frame = div()
            .id(frame_id)
            .test_support()
            .relative()
            .flex_shrink_0()
            .h_full()
            .max_w_full()
            .aspect_ratio(aspect)
            .when_some(view, |this, view| this.child(view))
            .children(self.render_caption(role, false, cx))
            .children(inset);
        div()
            .id(id)
            .test_support()
            .flex()
            .flex_row()
            .size_full()
            .overflow_hidden()
            .bg(theme.background)
            .map(|this| match align {
                PaneAlign::End => this.justify_end(),
                PaneAlign::Start => this.justify_start(),
                PaneAlign::Center => this.justify_center(),
            })
            .child(frame)
            .into_any_element()
    }

    /// The composed video panes. Split pictures meet at the seam; an inset
    /// sits on the large picture's bottom-right corner, clear of the HUD's
    /// default bottom-left place.
    fn render_stage(&self, window: &Window, cx: &App) -> AnyElement {
        let video = self.app.video.read(cx);
        let layout = video.layout().effective(video.is_dual());
        let theme = cx.theme();
        let single = |role: Role, inset: Option<Role>| {
            let inset = inset.map(|inset| {
                self.render_pane(inset, PanePlace::Inset, None, cx)
                    .into_any_element()
            });
            let place = PanePlace::Main {
                align: PaneAlign::Center,
            };
            div()
                .size_full()
                .child(self.render_pane(role, place, inset, cx))
        };
        let split_half = |role: Role, align: PaneAlign| {
            let place = PanePlace::Main { align };
            div()
                .flex_1()
                .min_w_0()
                .child(self.render_pane(role, place, None, cx))
        };
        let stage = match layout {
            ComposeLayout::Split => h_flex()
                .items_stretch()
                .size_full()
                .gap_px()
                .bg(theme.border)
                .child(split_half(Role::Primary, PaneAlign::End))
                .child(split_half(Role::Reference, PaneAlign::Start)),
            ComposeLayout::PrimaryWithReferenceInset => {
                single(Role::Primary, Some(Role::Reference))
            }
            ComposeLayout::ReferenceWithPrimaryInset => {
                single(Role::Reference, Some(Role::Primary))
            }
            ComposeLayout::PrimaryOnly => single(Role::Primary, None),
            ComposeLayout::ReferenceOnly => single(Role::Reference, None),
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
            .children(overlay::countdown(&self.app, window, cx))
            .into_any_element()
    }
}

impl VideoPanel {
    // ── fullscreen stage ────────────────────────────────────────────

    /// The whole-window stage: pictures composed on black, the telemetry
    /// layer, the countdown, the filmstrip lane and the floating controls.
    fn render_fullscreen(&mut self, window: &mut Window, cx: &mut Context<Self>) -> AnyElement {
        let Some(stage) = self.stage.as_ref() else {
            return div().into_any_element();
        };
        let viewport = window.viewport_size();
        let (width, height) = (viewport.width.as_f32(), viewport.height.as_f32());
        let rem = window.rem_size().as_f32();
        let filmstrip = stage.filmstrip.clone();
        let (controls, hud) = (stage.controls, stage.hud);
        let strip = filmstrip.read(cx).height_rems() * rem;
        let controls_height = CONTROLS_REMS * rem;
        let layout = {
            let video = self.app.video.read(cx);
            video.layout().effective(video.is_dual())
        };
        let aspects = (
            self.picture_aspect(Role::Primary, cx),
            self.picture_aspect(Role::Reference, cx),
        );
        let aspect = |role: Role| match role {
            Role::Primary => aspects.0,
            Role::Reference => aspects.1,
        };
        let plan = stage::plan(
            layout,
            width,
            height,
            aspect,
            stage::Lanes {
                strip,
                delta: DELTA_LANE_REMS * rem,
                controls: controls_height,
                band: hud,
            },
        );
        let overlay_stage = StageOverlay {
            delta: plan.delta,
            band: plan.band,
            controls: controls_height,
            hud,
            primary: plan.pane(Role::Primary),
            reference: plan.pane(Role::Reference),
        };
        let strip_lane = plan.strip;
        let panes = plan.panes;
        self.overlay
            .update(cx, |overlay, cx| overlay.set_stage(Some(overlay_stage), cx));

        let label = SharedString::from(format!("Fullscreen video, {}", layout.label()));
        let panes = panes
            .into_iter()
            .map(|pane| self.render_stage_pane(pane, cx))
            .collect::<Vec<_>>();
        let controls_bar = controls.then(|| self.render_stage_controls(cx));
        div()
            .id("video-fullscreen")
            .role(AccessRole::Group)
            .aria_label(label)
            .test_support()
            .track_focus(&self.focus_handle)
            .relative()
            .size_full()
            .overflow_hidden()
            .bg(gpui_kit::black())
            .on_mouse_move(cx.listener(|this, _, _, cx| this.reveal_controls(cx)))
            .children(panes)
            .child(self.overlay.clone())
            .children(overlay::countdown(&self.app, window, cx))
            .when(strip > 0., |this| {
                this.child(
                    div()
                        .id("video-filmstrip-lane")
                        .test_support()
                        .absolute()
                        .left(px(strip_lane.x))
                        .top(px(strip_lane.y))
                        .w(px(strip_lane.w))
                        .child(filmstrip),
                )
            })
            .children(controls_bar)
            .into_any_element()
    }

    /// One picture on the stage, at its computed place; an inset is framed.
    fn render_stage_pane(&self, pane: stage::Pane, cx: &App) -> AnyElement {
        let view = match pane.role {
            Role::Primary => self.primary_view.clone(),
            Role::Reference => self.reference_view.clone(),
        };
        let id = match pane.role {
            Role::Primary => "primary-video-pane",
            Role::Reference => "reference-video-pane",
        };
        let theme = cx.theme();
        let rect = pane.rect;
        div()
            .id(id)
            .test_support()
            .absolute()
            .left(px(rect.x))
            .top(px(rect.y))
            .w(px(rect.w))
            .h(px(rect.h))
            .overflow_hidden()
            .when(pane.inset, |this| {
                this.border_1()
                    .border_color(theme.border)
                    .rounded(theme.radius)
                    .shadow_lg()
            })
            .when_some(view, |this, view| this.child(view))
            .into_any_element()
    }

    /// The floating transport controls: play, ±2 s, layouts 1-5, rate,
    /// lap-end mode, HUD and mute, the lap clock and Exit.
    fn render_stage_controls(&mut self, cx: &mut Context<Self>) -> AnyElement {
        let dual = self.app.video.read(cx).is_dual();
        let current = self.app.video.read(cx).layout().effective(dual);
        let hud = self.is_hud_shown();
        let play = self.render_play(cx);
        let mute = self.render_mute(cx);
        let rate = self.render_rate(cx);
        let mode = self.render_mode(cx);
        let exit = self.render_fullscreen_button();
        let seek = |id: &'static str, label: &'static str, spoken: &'static str, back: bool| {
            let button = Button::new(id)
                .ghost()
                .xsmall()
                .label(label)
                .accessibility_label(spoken);
            if back {
                button
                    .tooltip_with_action(spoken, &SeekBack, Some(WORKSPACE_CONTEXT))
                    .on_click(|_, window, cx| window.dispatch_action(Box::new(SeekBack), cx))
            } else {
                button
                    .tooltip_with_action(spoken, &SeekForward, Some(WORKSPACE_CONTEXT))
                    .on_click(|_, window, cx| window.dispatch_action(Box::new(SeekForward), cx))
            }
        };
        let layouts = dual.then(|| {
            let mut group = ButtonGroup::new("video-stage-layouts").xsmall().outline();
            for layout in ComposeLayout::ALL {
                let selected = layout == current;
                let spoken = SharedString::from(format!("{} ({})", layout.label(), layout.key()));
                group = group.child(
                    Button::new(SharedString::from(format!(
                        "video-stage-layout-{}",
                        layout.key()
                    )))
                    .label(layout.key())
                    .selected(selected)
                    .when(selected, |button| button.primary())
                    .accessibility_label(spoken.clone())
                    .tooltip_with_action(
                        spoken,
                        layout_action(layout).as_ref(),
                        Some(WORKSPACE_CONTEXT),
                    ),
                );
            }
            group.on_click(cx.listener(|this, selected: &Vec<usize>, _, cx| {
                if let Some(layout) = selected.first().and_then(|ix| ComposeLayout::ALL.get(*ix)) {
                    let layout = *layout;
                    this.app
                        .video
                        .update(cx, |video, cx| video.set_layout(layout, cx));
                }
            }))
        });
        let hud_label = if hud {
            "Hide the telemetry overlay"
        } else {
            "Show the telemetry overlay"
        };
        let hud_button = Button::new("video-stage-hud")
            .ghost()
            .xsmall()
            .icon(self.icons.hud.clone())
            .label("HUD")
            .selected(hud)
            .accessibility_label(hud_label)
            .tooltip(hud_label)
            .on_click(cx.listener(|this, _, _, cx| this.toggle_hud(cx)));
        let theme = cx.theme();
        let divider = || Separator::vertical().h_4().mx_1();
        h_flex()
            .id("video-stage-controls")
            .role(AccessRole::Toolbar)
            .aria_label("Playback")
            .test_support()
            .absolute()
            .left_0()
            .right_0()
            .bottom_0()
            .h(gpui_kit::rems(CONTROLS_REMS))
            .items_center()
            .gap_1()
            .px_3()
            .border_t_1()
            .border_color(theme.border.opacity(0.6))
            .bg(gpui_kit::black().opacity(0.72))
            .text_color(theme.foreground)
            .text_label()
            .on_hover(
                cx.listener(|this, hovered: &bool, _, cx| this.set_controls_hovered(*hovered, cx)),
            )
            .child(play)
            .child(seek("video-seek-back", "−2 s", "Back 2 seconds", true))
            .child(seek(
                "video-seek-forward",
                "+2 s",
                "Forward 2 seconds",
                false,
            ))
            .child(divider())
            .children(layouts.map(|group| div().flex_shrink_0().child(group)))
            .when(dual, |this| this.child(divider()))
            .child(rate)
            .child(mode)
            .child(divider())
            .child(hud_button)
            .child(mute)
            .child(div().flex_1().min_w_2())
            .child(self.clock.clone())
            .child(divider())
            .child(exit)
            .into_any_element()
    }
}

/// The reference sync indicator, sentence case except established acronyms.
fn sync_label(state: SyncState) -> &'static str {
    match state {
        SyncState::Wait => "waiting",
        SyncState::NoMap => "not aligned",
        SyncState::Aligning => "aligning",
        SyncState::Locked => "in sync",
        SyncState::Best => "nearest frame",
        SyncState::RealTime => "1×",
        SyncState::Gps => "GPS",
        SyncState::Corner => "corner",
        SyncState::Hold => "holding",
        SyncState::Straight => "straight",
    }
}

/// What a sync state means, for the chip's tooltip.
fn sync_description(state: SyncState) -> &'static str {
    match state {
        SyncState::Wait => "Waiting for the reference video to open.",
        SyncState::NoMap => {
            "This pair has no alignment map, so the reference video does not follow the primary."
        }
        SyncState::Aligning => "Seeking the reference video to the primary’s place on the map.",
        SyncState::Locked => {
            "The reference video shows the same place on track as the primary, through the shared alignment map."
        }
        SyncState::Best => {
            "The reference video could not land exactly on the mapped place; it shows the nearest frame."
        }
        SyncState::RealTime => {
            "The reference plays at recording speed and re-syncs at lap start, jumps and pause."
        }
        SyncState::Gps => "The reference follows the map’s local slope to close a small gap.",
        SyncState::Corner => "In a corner: the reference holds 1× on the mapped place.",
        SyncState::Hold => {
            "In a corner the reference holds 1×; it catches up on the next straight."
        }
        SyncState::Straight => {
            "On the straight the reference catches up to arrive at the next turn-in together."
        }
    }
}

fn pacing_label(pacing: ReferencePlayback) -> &'static str {
    match pacing {
        ReferencePlayback::Corners => "hold 1× through corners",
        ReferencePlayback::Gps => "follow the GPS map",
        ReferencePlayback::Recording => "recording speed",
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

    fn title_bar(&self, _: &App) -> bool {
        PanelKind::Video.has_title_bar()
    }
}

impl EventEmitter<PanelEvent> for VideoPanel {}

impl Focusable for VideoPanel {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for VideoPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if self.stage.is_some() {
            return self.render_fullscreen(window, cx);
        }
        let shows_video = {
            let video = self.app.video.read(cx);
            self.primary_view.is_some() || video.clock(Role::Primary).is_some()
        };
        let content = if shows_video {
            self.render_stage(window, cx)
        } else {
            self.render_placeholder(cx)
        };
        v_flex()
            .id("video-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full()
            .child(div().flex_1().min_h_0().child(content))
            .child(self.render_bar(window, cx))
            .into_any_element()
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
