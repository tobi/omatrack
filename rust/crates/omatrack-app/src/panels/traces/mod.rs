//! Traces: the synchronized channel lanes of the primary and reference laps.
//!
//! Top to bottom, with no title bar or toolbar of its own: the corner ruler
//! (two staggered label rows, the cursor's corner as a chip, complex
//! brackets), the damper strip while manual damper alignment is in effect,
//! then the [`TraceStack`] (pinned gap lane, scrollable channel lanes,
//! shared x-axis). The range statistics of a selection float at the top
//! right of the lanes. The axis, FIT, lane sizing, corner editing and zoom
//! controls live in the control row under the video, on their keys and in
//! the palette.
//!
//! State ownership:
//! - the application owns the analysis (`Session`), the viewport and the
//!   cursor (`AppState`); corner focus, zoom, axis and FIT are workspace
//!   actions, so every entry point (keys, control row, palette, clicks) runs the
//!   same handler in `workspace/mod.rs`;
//! - this panel owns the scene built from the analysis (on the background
//!   executor, latest request wins), the lane styles from `channels.<key>`,
//!   this session's pinned lanes, and the drafts of its two modal editors
//!   (lane resize, corner zones), which reach `omatrack.yml` or the session
//!   only on Save.
//!
//! A cursor move notifies `CursorState`; this panel re-renders its readouts
//! (the corner focus, the stats chip) but never the cached static
//! trace layer (see [`TraceStack::static_rebuilds`]).
//!
//! Video seeking follows `CursorState` in the video controller; this panel
//! only moves the shared cursor.

mod edit;
pub(crate) mod scene_build;
mod stats;
mod toolbar;

use std::collections::HashMap;
use std::sync::Arc;

use gpui_kit::component::{
    ActiveTheme as _, IconName,
    menu::{PopupMenu, PopupMenuItem},
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, AppContext as _, Context, DismissEvent, Entity, FocusHandle, Focusable,
    InteractiveElement as _, IntoElement, ParentElement as _, Pixels, Point, Render, SharedString,
    Styled as _, Subscription, Task, TestSupportExt as _, Window, anchored, deferred, div,
};
use omatrack_core::alignment::Strategy;
use omatrack_trace::{
    CornerBand, CornerRuler, CornerRulerEvent, DamperStrip, DamperStripData, DamperStripEvent,
    Selection, TraceEvent, TraceScene, TraceStack,
};

use crate::actions::{CancelEdit, FocusCorner, ResizeLanes, SaveEdit};
use crate::commands::{self, CommandCategory, CommandSpec};
use crate::keymap::TRACE_EDIT_CONTEXT;
use crate::panels::{PanelKind, analysis_body, simple_panel};
use crate::state::{AppState, SessionEvent};

pub use edit::{CornerDraft, ResizeDraft};
pub use scene_build::{DELTA_KEY, DELTA_TITLE};
pub use stats::RangeStats;

use scene_build::{BuiltScene, CornerLink};

/// Show or hide one lane (`channels.<key>.visible`). Dispatched by the
/// palette and the lane menu; handled app-wide so it works from any focus.
#[derive(Debug, Clone, PartialEq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct ToggleLane {
    pub key: SharedString,
}

/// The modal editors of the trace workspace. They are mutually exclusive;
/// Ctrl+S saves and Escape cancels.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum TraceMode {
    #[default]
    Browse,
    ResizingLanes,
    EditingCorners,
}

impl TraceMode {
    fn label(self) -> &'static str {
        match self {
            Self::Browse => "",
            Self::ResizingLanes => "Resize lanes",
            Self::EditingCorners => "Edit corners",
        }
    }
}

/// Where a corner zone drag happens.
#[derive(Clone, Copy)]
enum EditSource {
    Ruler,
    Lanes,
}

/// The lane menu opened by a secondary click in the plot.
struct LaneMenu {
    menu: Entity<PopupMenu>,
    position: Point<Pixels>,
    _dismiss: Subscription,
}

pub struct TracesPanel {
    app: AppState,
    focus_handle: FocusHandle,
    mode: TraceMode,
    /// Built on the first render: the stack needs a window.
    stack: Option<Entity<TraceStack>>,
    ruler: Entity<CornerRuler>,
    damper: Entity<DamperStrip>,
    scene: Arc<TraceScene>,
    built: Option<BuiltScene>,
    show_damper: bool,
    scene_request: u64,
    scene_task: Option<Task<()>>,
    /// Pin choices of this session, by lane key (Δ is pinned by default).
    pinned: HashMap<SharedString, bool>,
    resize: Option<ResizeDraft>,
    corners: Option<CornerDraft>,
    range: Option<RangeStats>,
    focused_band: Option<u32>,
    /// Lane palette commands as registered (key, visible), to register again
    /// only on a change.
    lane_commands: Vec<(SharedString, bool)>,
    menu: Option<LaneMenu>,
    _subscriptions: Vec<Subscription>,
    _view_subscriptions: Vec<Subscription>,
}

impl TracesPanel {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let ruler = cx.new(|cx| CornerRuler::new(app.viewport.clone(), cx));
        let damper = cx.new(|cx| DamperStrip::new(app.cursor.clone(), cx));
        let subscriptions = vec![
            cx.subscribe(&app.session, |this, _, event, cx| {
                if let SessionEvent::AnalysisReady = event {
                    this.request_scene(cx);
                }
            }),
            // Loading and failure states, the primary's track.
            cx.observe(&app.session, |_, _, cx| cx.notify()),
            cx.observe(&app.preferences, |this, _, cx| this.restyle(cx)),
            cx.observe(&app.cursor, |this, _, cx| this.on_cursor(cx)),
            cx.subscribe(&damper, |this, _, event, cx| {
                if let DamperStripEvent::OffsetChanged { fraction, .. } = event {
                    let fraction = *fraction;
                    this.app
                        .session
                        .update(cx, |session, cx| session.set_manual_offset(fraction, cx));
                }
            }),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle().tab_stop(true),
            mode: TraceMode::default(),
            stack: None,
            ruler,
            damper,
            scene: Arc::new(TraceScene::default()),
            built: None,
            show_damper: false,
            scene_request: 0,
            scene_task: None,
            pinned: HashMap::new(),
            resize: None,
            corners: None,
            range: None,
            focused_band: None,
            lane_commands: Vec::new(),
            menu: None,
            _subscriptions: subscriptions,
            _view_subscriptions: Vec::new(),
        };
        panel.request_scene(cx);
        panel
    }

    // ---- readers --------------------------------------------------------

    pub fn mode(&self) -> TraceMode {
        self.mode
    }

    /// The trace stack, once the panel has rendered.
    pub fn stack(&self) -> Option<&Entity<TraceStack>> {
        self.stack.as_ref()
    }

    pub fn ruler(&self) -> &Entity<CornerRuler> {
        &self.ruler
    }

    pub fn damper(&self) -> &Entity<DamperStrip> {
        &self.damper
    }

    /// The scene on screen (empty until an analysis arrives).
    pub fn scene(&self) -> &Arc<TraceScene> {
        &self.scene
    }

    /// Whether the damper strip is shown (manual damper alignment).
    pub fn is_showing_damper(&self) -> bool {
        self.show_damper
    }

    /// Statistics of the current range selection.
    pub fn range_stats(&self) -> Option<&RangeStats> {
        self.range.as_ref()
    }

    /// The lane resize in progress.
    pub fn resize_draft(&self) -> Option<&ResizeDraft> {
        self.resize.as_ref()
    }

    /// The corner edit in progress.
    pub fn corner_draft(&self) -> Option<&CornerDraft> {
        self.corners.as_ref()
    }

    /// Whether the lane menu is open.
    pub fn is_lane_menu_open(&self) -> bool {
        self.menu.is_some()
    }

    /// The analysis zone id of the focused corner.
    pub fn focused_corner(&self) -> Option<&SharedString> {
        let band = self.focused_band?;
        self.corner_links()
            .iter()
            .find(|link| link.band == band)
            .map(|link| &link.zone)
    }

    fn corner_links(&self) -> &[CornerLink] {
        self.built
            .as_ref()
            .map_or(&[][..], |built| built.corners.as_slice())
    }

    fn analysis(&self) -> Option<&Arc<omatrack_core::session::Analysis>> {
        self.built.as_ref().map(|built| &built.analysis)
    }

    // ---- scene ----------------------------------------------------------

    /// Build the scene of the session's analysis on the background executor.
    /// The current scene stays on screen until the new one lands; a newer
    /// request supersedes (and drops) an older one.
    fn request_scene(&mut self, cx: &mut Context<Self>) {
        let analysis = self.app.session.read(cx).analysis().cloned();
        let Some(analysis) = analysis else {
            self.scene_request += 1;
            self.scene_task = None;
            self.apply_scene(None, cx);
            return;
        };
        if self
            .built
            .as_ref()
            .is_some_and(|built| Arc::ptr_eq(&built.analysis, &analysis))
        {
            return;
        }
        self.scene_request += 1;
        let request = self.scene_request;
        let neighbours = self.built.as_ref().and_then(|b| b.neighbours.clone());
        let work = cx.background_spawn(async move { scene_build::build(analysis, neighbours) });
        self.scene_task = Some(cx.spawn(async move |this, cx| {
            let built = work.await;
            let _ = this.update(cx, |this, cx| {
                if this.scene_request == request {
                    this.scene_task = None;
                    this.apply_scene(Some(built), cx);
                }
            });
        }));
    }

    fn apply_scene(&mut self, built: Option<BuiltScene>, cx: &mut Context<Self>) {
        // New zones replace whatever was being dragged; without lanes there
        // is nothing left to resize either.
        if self.mode == TraceMode::EditingCorners
            || built.as_ref().is_none_or(|built| built.scene.is_empty())
        {
            self.leave_mode(cx);
        }
        self.scene = built
            .as_ref()
            .map_or_else(|| Arc::new(TraceScene::default()), |b| b.scene.clone());
        let damper = built
            .as_ref()
            .and_then(|built| damper_data(&built.analysis));
        self.show_damper = damper.is_some();
        self.built = built;
        let scene = self.scene.clone();
        if let Some(stack) = &self.stack {
            stack.update(cx, |stack, cx| stack.set_scene(scene.clone(), cx));
        }
        self.ruler.update(cx, |ruler, cx| {
            ruler.set_corners(scene.corners().to_vec(), scene.complexes().to_vec(), cx)
        });
        self.damper
            .update(cx, |strip, cx| strip.set_data(damper.map(Arc::new), cx));
        self.restyle(cx);
        // Statistics and the focused band belong to the old scene.
        self.range = None;
        self.focused_band = None;
        self.on_cursor(cx);
        cx.notify();
    }

    /// Lane styles from `channels.<key>`, this session's pins and a resize
    /// draft; FIT from `trace.fit_channels`. Also keeps the lane commands of
    /// the palette in step.
    fn restyle(&mut self, cx: &mut Context<Self>) {
        let preferences = self.app.preferences.read(cx);
        let config = preferences.config();
        let styles = scene_build::lane_styles(
            config,
            &self.scene,
            &self.pinned,
            self.resize.as_ref().map(ResizeDraft::weights),
        );
        let fit = config.trace.is_fitting_channels();
        let color_mode = scene_build::color_mode(config);
        let lanes: Vec<(SharedString, SharedString, bool)> = self
            .scene
            .lanes()
            .iter()
            .map(|lane| {
                (
                    lane.key.clone(),
                    lane.title.clone(),
                    scene_build::is_lane_visible(config, &lane.key),
                )
            })
            .collect();
        if let Some(stack) = &self.stack {
            stack.update(cx, |stack, cx| {
                stack.set_lane_styles(styles, cx);
                stack.set_fit(fit, cx);
                stack.set_color_mode(color_mode, cx);
            });
        }
        self.register_lane_commands(&lanes, cx);
        cx.notify();
    }

    fn register_lane_commands(
        &mut self,
        lanes: &[(SharedString, SharedString, bool)],
        cx: &mut Context<Self>,
    ) {
        let current: Vec<(SharedString, bool)> = lanes
            .iter()
            .map(|(key, _, visible)| (key.clone(), *visible))
            .collect();
        if current == self.lane_commands {
            return;
        }
        for (key, title, visible) in lanes {
            let verb = if *visible { "Hide" } else { "Show" };
            commands::register(
                cx,
                CommandSpec::new(
                    format!("lane-{key}"),
                    format!("{verb} {title} lane"),
                    CommandCategory::Commands,
                    ToggleLane { key: key.clone() },
                )
                .keywords(["lane", "channel", "trace"]),
            );
        }
        self.lane_commands = current;
    }

    // ---- cursor -----------------------------------------------------------

    /// Follow the shared cursor: the focused corner (whoever focused it),
    /// and the selection statistics.
    fn on_cursor(&mut self, cx: &mut Context<Self>) {
        let cursor = self.app.cursor.read(cx);
        let (selection, focus) = (cursor.selection(), cursor.focus());
        let under_cursor = cursor
            .fraction()
            .and_then(|fraction| corner_at(self.scene.corners(), fraction));
        let mut changed = false;

        // The ruler's chip follows the cursor; it repaints only when the
        // corner under the cursor changes.
        self.ruler
            .update(cx, |ruler, cx| ruler.set_cursor_corner(under_cursor, cx));

        let band = focus.and_then(|focus| band_for(self.scene.corners(), focus));
        if band != self.focused_band {
            self.focused_band = band;
            changed = true;
            self.ruler
                .update(cx, |ruler, cx| ruler.set_focused_corner(band, cx));
            if let Some(stack) = self.stack.clone() {
                let current = stack.read(cx).focused_corner();
                match band {
                    // Focused elsewhere (keys, palette): the stack marks it.
                    // The viewport is already easing there; this only
                    // retargets it to the same place.
                    Some(band) if current != Some(band) => {
                        stack.update(cx, |stack, cx| stack.focus_corner(band, true, cx))
                    }
                    None if current.is_some() => {
                        stack.update(cx, |stack, cx| stack.clear_corner_focus(cx))
                    }
                    _ => {}
                }
            }
        }

        if self.range.map(|range| range.selection) != selection {
            self.range = selection.map(|selection| RangeStats::of(&self.scene, selection));
            changed = true;
        }
        if changed {
            cx.notify();
        }
    }

    fn clear_selection(&mut self, cx: &mut Context<Self>) {
        self.app
            .cursor
            .update(cx, |cursor, cx| cursor.set_selection(None, cx));
    }

    // ---- modal editors ------------------------------------------------------

    /// Enter `mode`, or leave it (cancelling) when it is the current one.
    /// Without an analysis there is nothing to edit and nothing happens.
    pub fn toggle_mode(&mut self, mode: TraceMode, cx: &mut Context<Self>) {
        if self.mode == mode {
            self.leave_mode(cx);
            return;
        }
        if mode == TraceMode::Browse || self.scene.is_empty() {
            return;
        }
        self.leave_mode(cx);
        self.mode = mode;
        match mode {
            TraceMode::ResizingLanes => {
                self.resize = Some(ResizeDraft::default());
                self.with_stack(cx, |stack, cx| stack.set_resizing(true, cx));
            }
            TraceMode::EditingCorners => {
                self.corners = Some(CornerDraft::new(self.scene.corners().to_vec()));
                self.with_stack(cx, |stack, cx| stack.set_editing_corners(true, cx));
                self.ruler
                    .update(cx, |ruler, cx| ruler.set_editing(true, cx));
            }
            TraceMode::Browse => {}
        }
        cx.notify();
    }

    /// Leave the current editor without saving: the lanes and zones return
    /// to what `omatrack.yml` and the analysis say.
    fn leave_mode(&mut self, cx: &mut Context<Self>) {
        match std::mem::take(&mut self.mode) {
            TraceMode::ResizingLanes => {
                self.resize = None;
                self.with_stack(cx, |stack, cx| stack.set_resizing(false, cx));
                self.restyle(cx);
            }
            TraceMode::EditingCorners => {
                self.corners = None;
                self.show_corners(self.scene.corners().to_vec(), cx);
                self.with_stack(cx, |stack, cx| stack.set_editing_corners(false, cx));
                self.ruler
                    .update(cx, |ruler, cx| ruler.set_editing(false, cx));
            }
            TraceMode::Browse => {}
        }
        cx.notify();
    }

    fn cancel(&mut self, _: &CancelEdit, _: &mut Window, cx: &mut Context<Self>) {
        self.leave_mode(cx);
    }

    fn save(&mut self, _: &SaveEdit, _: &mut Window, cx: &mut Context<Self>) {
        match self.mode {
            TraceMode::ResizingLanes => self.save_resize(cx),
            TraceMode::EditingCorners => self.save_corners(cx),
            TraceMode::Browse => {}
        }
    }

    /// Write the drafted weights to `channels.<key>.weight` and select FIT
    /// (Save keeps the proportions as a fitted layout).
    fn save_resize(&mut self, cx: &mut Context<Self>) {
        let draft = self.resize.take().unwrap_or_default();
        self.mode = TraceMode::Browse;
        self.with_stack(cx, |stack, cx| stack.set_resizing(false, cx));
        self.app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| {
                for (key, weight) in draft.weights() {
                    config.channels.entry(key.to_string()).or_default().weight = Some(*weight);
                }
                config.trace.fit_channels = Some(true);
            });
        });
        self.restyle(cx);
    }

    /// Preview every lane at weight 1 (the drag draft is dropped).
    fn reset_heights(&mut self, cx: &mut Context<Self>) {
        let Some(draft) = self.resize.as_mut() else {
            return;
        };
        draft.reset(self.scene.lanes().iter().map(|lane| lane.key.clone()));
        // Restart the stack's resize so its dragged heights give way to the
        // previewed weights.
        self.with_stack(cx, |stack, cx| {
            stack.set_resizing(false, cx);
            stack.set_resizing(true, cx);
        });
        self.restyle(cx);
    }

    /// Store the edited zones as the track's override and rebuild the
    /// analysis with them.
    fn save_corners(&mut self, cx: &mut Context<Self>) {
        if !self.can_save_corners(cx) {
            return;
        }
        let Some(draft) = self.corners.take() else {
            return;
        };
        let zones = match self.analysis() {
            Some(analysis) => draft.zones(analysis.corners(), self.corner_links()),
            None => return,
        };
        self.mode = TraceMode::Browse;
        // Keep the edited zones on screen until the new analysis lands.
        self.show_corners(draft.bands().to_vec(), cx);
        self.with_stack(cx, |stack, cx| stack.set_editing_corners(false, cx));
        self.ruler
            .update(cx, |ruler, cx| ruler.set_editing(false, cx));
        self.app.session.update(cx, |session, cx| {
            session.set_corner_override(Some(zones), cx)
        });
        cx.notify();
    }

    /// Drop the track's corner override and return to Track Atlas zones.
    fn use_atlas_corners(&mut self, cx: &mut Context<Self>) {
        self.leave_mode(cx);
        self.app
            .session
            .update(cx, |session, cx| session.set_corner_override(None, cx));
    }

    /// Saving corner zones needs a moved zone and a named track to store
    /// them under.
    fn can_save_corners(&self, cx: &App) -> bool {
        self.corners.as_ref().is_some_and(CornerDraft::is_edited) && self.has_track(cx)
    }

    fn has_track(&self, cx: &App) -> bool {
        self.app
            .session
            .read(cx)
            .primary()
            .is_some_and(|slot| slot.track_key().is_some())
    }

    /// Adopt a zone drag from `source` and show it in the other view. The
    /// dragging view already shows it, and must not be handed the zones
    /// back: replacing a view's zones cancels the drag in progress.
    fn edit_corner(
        &mut self,
        source: EditSource,
        band: u32,
        start: f64,
        end: f64,
        cx: &mut Context<Self>,
    ) {
        let Some(draft) = self.corners.as_mut() else {
            return;
        };
        if !draft.edit(band, start, end) {
            return;
        }
        let bands = draft.bands().to_vec();
        match source {
            EditSource::Ruler => self.with_stack(cx, |stack, cx| stack.set_corners(bands, cx)),
            EditSource::Lanes => {
                let complexes = self.scene.complexes().to_vec();
                self.ruler
                    .update(cx, |ruler, cx| ruler.set_corners(bands, complexes, cx));
            }
        }
        cx.notify();
    }

    fn show_corners(&mut self, bands: Vec<CornerBand>, cx: &mut Context<Self>) {
        let complexes = self.scene.complexes().to_vec();
        let for_stack = bands.clone();
        self.with_stack(cx, |stack, cx| stack.set_corners(for_stack, cx));
        self.ruler
            .update(cx, |ruler, cx| ruler.set_corners(bands, complexes, cx));
    }

    fn with_stack(
        &self,
        cx: &mut Context<Self>,
        f: impl FnOnce(&mut TraceStack, &mut Context<TraceStack>),
    ) {
        if let Some(stack) = &self.stack {
            stack.update(cx, f);
        }
    }

    // ---- lanes -----------------------------------------------------------

    fn is_pinned(&self, key: &str, cx: &App) -> bool {
        self.pinned.get(key).copied().unwrap_or_else(|| {
            scene_build::lane_style(self.app.preferences.read(cx).config(), key, None)
                .sizing
                .pinned
        })
    }

    fn toggle_pin(&mut self, key: SharedString, cx: &mut Context<Self>) {
        let pinned = !self.is_pinned(&key, cx);
        self.pinned.insert(key, pinned);
        self.restyle(cx);
    }

    // ---- child views and their events ---------------------------------------

    /// The stack needs a window; the panel is created without one, so the
    /// stack and every window-bound subscription start with the first frame.
    fn ensure_views(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.stack.is_some() {
            return;
        }
        let scene = self.scene.clone();
        let (viewport, cursor) = (self.app.viewport.clone(), self.app.cursor.clone());
        let stack = cx.new(|cx| TraceStack::new(scene, viewport, cursor, window, cx));
        self._view_subscriptions = vec![
            cx.subscribe_in(&stack, window, Self::on_trace_event),
            cx.subscribe_in(
                &self.ruler,
                window,
                |this, _, event, window, cx| match event {
                    CornerRulerEvent::CornerClicked(band) => this.request_focus(*band, window, cx),
                    CornerRulerEvent::CornerEdited { id, start, end } => {
                        this.edit_corner(EditSource::Ruler, *id, *start, *end, cx)
                    }
                    _ => {}
                },
            ),
        ];
        self.stack = Some(stack);
        self.restyle(cx);
    }

    fn on_trace_event(
        &mut self,
        stack: &Entity<TraceStack>,
        event: &TraceEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match event {
            TraceEvent::CornerEdited { id, start, end } => {
                self.edit_corner(EditSource::Lanes, *id, *start, *end, cx)
            }
            TraceEvent::LaneResized { keys, heights } => {
                let config = self.app.preferences.read(cx).config().clone();
                let pinned = self.pinned.clone();
                if let Some(draft) = self.resize.as_mut() {
                    let share = |key: &str| {
                        scene_build::lane_style(&config, key, pinned.get(key).copied())
                            .sizing
                            .height_percent
                            / 100.0
                    };
                    draft.apply_heights(keys, heights, share);
                    cx.notify();
                }
            }
            TraceEvent::LanePinToggled { key, pinned } => {
                self.pinned.insert(key.clone(), *pinned);
                self.restyle(cx);
            }
            TraceEvent::ContextMenu { lane, position } => {
                if let Some(key) = lane {
                    let focus = stack.read(cx).focus_handle(cx);
                    self.open_lane_menu(key.clone(), *position, focus, window, cx);
                }
            }
            TraceEvent::CursorMoved(_)
            | TraceEvent::RangeSelected(_)
            | TraceEvent::ViewportChanged(_) => {}
        }
    }

    /// Focus a corner through the workspace's `FocusCorner` (it owns the
    /// focus and the pre-focus viewport Escape returns to).
    fn request_focus(&mut self, band: u32, window: &mut Window, cx: &mut Context<Self>) {
        let Some(zone) = self
            .corner_links()
            .iter()
            .find(|link| link.band == band)
            .map(|link| link.zone.clone())
        else {
            return;
        };
        if !self.focus_handle.contains_focused(window, cx) {
            window.focus(&self.focus_handle, cx);
        }
        window.dispatch_action(Box::new(FocusCorner { id: zone }), cx);
    }

    fn open_lane_menu(
        &mut self,
        key: SharedString,
        position: Point<Pixels>,
        focus: FocusHandle,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let pinned = self.is_pinned(&key, cx);
        let panel = cx.entity().downgrade();
        let pin_key = key.clone();
        let menu = PopupMenu::build(window, cx, move |menu, _, _| {
            menu.action_context(focus)
                .item(
                    PopupMenuItem::new(if pinned { "Unpin lane" } else { "Pin lane" }).on_click(
                        move |_, _, cx| {
                            let key = pin_key.clone();
                            let _ = panel.update(cx, |panel, cx| panel.toggle_pin(key, cx));
                        },
                    ),
                )
                .item(
                    PopupMenuItem::new("Hide lane")
                        .icon(IconName::EyeOff)
                        .action(Box::new(ToggleLane { key })),
                )
                .separator()
                .item(PopupMenuItem::new("Resize lanes…").action(Box::new(ResizeLanes)))
        });
        let dismiss = cx.subscribe_in(&menu, window, |this, _, _: &DismissEvent, _, cx| {
            this.menu = None;
            cx.notify();
        });
        window.focus(&menu.focus_handle(cx), cx);
        self.menu = Some(LaneMenu {
            menu,
            position,
            _dismiss: dismiss,
        });
        cx.notify();
    }

    // ---- rendering ----------------------------------------------------------

    fn render_body(&mut self, cx: &mut Context<Self>) -> gpui_kit::AnyElement {
        if self.built.is_none() || self.scene.is_empty() {
            let preparing = self.app.session.read(cx).analysis().is_some();
            if preparing {
                let body = crate::panels::empty_state(
                    IconName::LoaderCircle,
                    "Preparing traces",
                    "Building the lanes of this lap.",
                );
                return crate::panels::panel_body("traces-summary", "Preparing traces", body, cx)
                    .into_any_element();
            }
            return analysis_body("traces-summary", &self.app, cx, |_| SharedString::default());
        }
        let stack = self.stack.clone();
        let theme = cx.theme();
        let (border, radius) = (theme.border, theme.radius_lg);
        // The lanes sit in one bordered card: ruler row, damper strip,
        // lanes and the distance axis.
        let card = v_flex()
            .id("trace-card")
            .test_support()
            .size_full()
            .min_h_0()
            .border_1()
            .border_color(border)
            .rounded(radius)
            .overflow_hidden()
            .child(self.render_ruler_row(cx))
            .when(self.show_damper, |el| el.child(self.damper.clone()))
            .child(
                div()
                    .id("trace-lanes")
                    .relative()
                    .flex_1()
                    .min_h_0()
                    .children(stack)
                    .children(self.render_range_stats(cx)),
            );
        div()
            .size_full()
            .min_h_0()
            .p_2()
            .child(card)
            .into_any_element()
    }
}

/// The band whose zone is `focus` (the workspace focuses by zone bounds).
fn band_for(corners: &[CornerBand], focus: Selection) -> Option<u32> {
    const EPSILON: f64 = 1e-9;
    corners
        .iter()
        .find(|corner| {
            (corner.start - focus.start).abs() < EPSILON && (corner.end - focus.end).abs() < EPSILON
        })
        .map(|corner| corner.id)
}

/// The corner whose zone holds `fraction`.
fn corner_at(corners: &[CornerBand], fraction: f64) -> Option<u32> {
    corners
        .iter()
        .find(|corner| (corner.start..=corner.end).contains(&fraction))
        .map(|corner| corner.id)
}

/// The damper strip's data while manual damper alignment is in effect.
fn damper_data(analysis: &omatrack_core::session::Analysis) -> Option<DamperStripData> {
    if analysis.strategy() != Some(Strategy::ManualDampers) {
        return None;
    }
    let comparison = analysis.comparison()?;
    let reference = analysis.reference()?;
    let primary = analysis.primary().unified();
    let map: Arc<dyn omatrack_trace::FractionMap> = comparison.clone();
    Some(
        DamperStripData::new(
            Arc::from(primary.damper_fl.as_slice()),
            Arc::from(reference.unified().damper_fl.as_slice()),
            primary.duration(),
        )
        .with_map(Some(map), comparison.manual_offset()),
    )
}

simple_panel!(TracesPanel, PanelKind::Traces);

impl Render for TracesPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.ensure_views(window, cx);
        let editing = self.mode != TraceMode::Browse;
        let menu = self.menu.as_ref().map(|menu| {
            deferred(
                anchored()
                    .position(menu.position)
                    .snap_to_window_with_margin(window.rem_size() * 0.5)
                    .child(menu.menu.clone()),
            )
            .with_priority(gpui_kit::base::POPUP_PRIORITY)
        });
        v_flex()
            .id("traces-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .when(editing, |this| {
                this.key_context(TRACE_EDIT_CONTEXT)
                    .on_action(cx.listener(Self::save))
                    .on_action(cx.listener(Self::cancel))
            })
            .size_full()
            .bg(cx.theme().background)
            .when(editing, |this| this.child(self.render_mode_bar(cx)))
            .child(div().flex_1().min_h_0().child(self.render_body(cx)))
            .children(menu)
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name), the lane visibility command, and the palette
/// entries of its toolbar commands. Called once from
/// [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::Traces, cx);
    cx.on_action(|action: &ToggleLane, cx| toggle_lane(&action.key, cx));
    use crate::actions::{ToggleCornerEdit, ToggleXAxis, ZoomReset};
    for spec in [
        CommandSpec::new(
            "x-axis",
            "Toggle x-axis",
            CommandCategory::Commands,
            ToggleXAxis,
        )
        .keywords(["distance", "time", "traces"]),
        CommandSpec::new(
            "zoom-reset",
            "Reset zoom",
            CommandCategory::Commands,
            ZoomReset,
        )
        .keywords(["traces", "whole lap"]),
        CommandSpec::new(
            "resize-lanes",
            "Resize lanes…",
            CommandCategory::Commands,
            ResizeLanes,
        )
        .keywords(["traces", "height"]),
        CommandSpec::new(
            "edit-corners",
            "Edit corners…",
            CommandCategory::Commands,
            ToggleCornerEdit,
        )
        .keywords(["traces", "zones", "track"]),
    ] {
        commands::register(cx, spec);
    }
}

/// Flip `channels.<key>.visible` (explicitly, so the choice survives a
/// change of defaults).
fn toggle_lane(key: &str, cx: &mut App) {
    let Some(state) = AppState::try_global(cx) else {
        return;
    };
    let preferences = state.preferences.clone();
    preferences.update(cx, |preferences, cx| {
        preferences.update(cx, |config| {
            let visible = scene_build::is_lane_visible(config, key);
            config.channels.entry(key.to_string()).or_default().visible = Some(!visible);
        });
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_focus_maps_to_the_band_with_its_bounds() {
        let corners = [
            CornerBand::new(1, "T1", 0.1, 0.2),
            CornerBand::new(2, "T2", 0.4, 0.5),
        ];
        assert_eq!(band_for(&corners, Selection::new(0.4, 0.5)), Some(2));
        assert_eq!(band_for(&corners, Selection::new(0.4, 0.51)), None);
    }
}
