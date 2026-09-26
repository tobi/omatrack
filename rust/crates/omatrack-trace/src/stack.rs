//! `TraceStack`: the stacked, synchronized lanes of the trace workspace.
//!
//! Composition, top to bottom:
//!
//! - the lanes: a chrome column (name and unit; P/R/Δ readouts in fixed
//!   columns, tabular figures, only while a cursor or hover is active)
//!   beside the plot column. The plot column holds the cached
//!   [`TraceStaticView`] and the overlay element on top of it. Pinned lanes
//!   sit above the scroll region;
//! - the shared x-axis.
//!
//! Corners are labelled once, by the [`crate::CornerRuler`] above the stack;
//! the stack paints their zones in the overlay only.
//!
//! State ownership: the application owns [`ViewportState`] and
//! [`CursorState`] and shares them with every view that follows the lap.
//! The stack owns lane layout, the gesture machine and the scroll offset;
//! it reports user intent as [`TraceEvent`]s. A cursor move re-renders the
//! stack (chrome and overlay) but never the static layer.
//!
//! Lane chrome and lap-edge labels are positioned with
//! `px(...)` from the resolved lane layout and the viewport mapping: that is
//! measured runtime geometry (the Coding Guides' documented exception), not
//! spacing. Everything else uses rem-based helpers and theme tokens. The plot
//! size is measured by the overlay's prepaint; after a resize the chrome
//! follows one frame later.
//!
//! Keyboard: the stack is focusable (`key_context("TraceStack")`); the
//! application binds its actions to the public methods here
//! (`step_cursor`, `zoom_in`, `zoom_out`, `reset_view`, `toggle_axis`).

use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::base::TestSupportExt as _;
use gpui_kit::component::{ActiveTheme as _, StyledExt as _, h_flex, v_flex};
use gpui_kit::{
    AnyElement, AppContext as _, Bounds, Context, ElementId, Entity, EventEmitter, FocusHandle,
    Focusable, Hsla, InteractiveElement as _, IntoElement, ParentElement as _, Pixels, Point,
    Render, Role, SharedString, StatefulInteractiveElement as _, StyleRefinement, Styled,
    Subscription, Window, div, prelude::FluentBuilder as _, px, rems,
};
use omatrack_ui::{
    DeltaSense, DeltaTrend, TypeScale as _, format_delta, format_value, trace_figures,
};

use crate::axis::{AXIS_TICK_SPACING, TraceAxis};
use crate::interaction::{
    CornerSpan, Effect, Effects, GestureCursor, Interaction, InteractionContext, KeyModifiers,
    PointerButton, WheelDelta,
};
use crate::layout::{GAP_LANE_MIN_HEIGHT, LaneLayout, LaneSizing, LayoutMode, layout_lanes};
use crate::overlay::TraceOverlay;
use crate::palette::{APPROXIMATE_DELTA_EMPHASIS, ColorMode, TracePalette};
use crate::scale::{Tick, Viewport, XAxis, axis_ticks};
use crate::scene::{CornerBand, LaneKind, LaneSeries, LaneStyles, Readout, TraceScene};
use crate::state::{CursorState, Selection, ViewportState};
use crate::static_layer::{StaticStats, TraceStaticView};

/// User intent reported by a [`TraceStack`].
#[derive(Clone, Debug, PartialEq)]
pub enum TraceEvent {
    /// The user moved the shared cursor (click or drag), lap fraction.
    CursorMoved(f64),
    /// A range selection was completed.
    RangeSelected(Selection),
    /// The user panned, zoomed or reset the viewport.
    ViewportChanged(Viewport),
    /// A corner zone was dragged in corner editing.
    CornerEdited {
        id: u32,
        start: f64,
        end: f64,
    },
    /// Lane resize draft: heights of the visible lanes, top to bottom, keyed
    /// by each lane's root channel.
    LaneResized {
        keys: Vec<SharedString>,
        heights: Vec<f64>,
    },
    LanePinToggled {
        key: SharedString,
        pinned: bool,
    },
    /// Secondary click in the plot, over `lane` when one is under it.
    ContextMenu {
        lane: Option<SharedString>,
        position: Point<Pixels>,
    },
}

/// Stacked synced lanes with a shared x-axis. See the module docs.
pub struct TraceStack {
    scene: Arc<TraceScene>,
    styles: Arc<LaneStyles>,
    corners: Arc<[CornerBand]>,
    corner_spans: Vec<CornerSpan>,
    mode: LayoutMode,
    scroll: f64,
    resize_draft: Option<Vec<f64>>,
    editing_corners: bool,
    focused_corner: Option<u32>,
    viewport: Entity<ViewportState>,
    cursor: Entity<CursorState>,
    static_view: Entity<TraceStaticView>,
    interaction: Interaction,
    focus_handle: FocusHandle,
    plot_bounds: Option<Bounds<Pixels>>,
    measured: Rc<Cell<Option<Bounds<Pixels>>>>,
    layout: Arc<LaneLayout>,
    lane_bottoms: Vec<f64>,
    lane_heights: Vec<f64>,
    ticks: Vec<Tick>,
    pointer_inside: bool,
    color_mode: ColorMode,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<TraceEvent> for TraceStack {}

impl Focusable for TraceStack {
    fn focus_handle(&self, _: &gpui_kit::App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl TraceStack {
    pub fn new(
        scene: Arc<TraceScene>,
        viewport: Entity<ViewportState>,
        cursor: Entity<CursorState>,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let styles = Arc::new(LaneStyles::new());
        let layout = Arc::new(LaneLayout::default());
        let static_view = cx.new(|cx| {
            TraceStaticView::new(
                scene.clone(),
                styles.clone(),
                layout.clone(),
                viewport.clone(),
                cx,
            )
        });
        let subscriptions = vec![
            cx.observe(&viewport, |this, _, cx| {
                this.refresh_ticks(cx);
                cx.notify();
            }),
            cx.observe(&cursor, |_, _, cx| cx.notify()),
        ];
        let corners: Arc<[CornerBand]> = scene.corners.clone().into();
        let mut stack = Self {
            corner_spans: spans(&corners),
            corners,
            scene,
            styles,
            mode: LayoutMode::default(),
            scroll: 0.0,
            resize_draft: None,
            editing_corners: false,
            focused_corner: None,
            viewport,
            cursor,
            static_view,
            interaction: Interaction::new(),
            focus_handle: cx.focus_handle().tab_stop(true),
            plot_bounds: None,
            measured: Rc::default(),
            layout,
            lane_bottoms: Vec::new(),
            lane_heights: Vec::new(),
            ticks: Vec::new(),
            pointer_inside: false,
            color_mode: ColorMode::default(),
            _subscriptions: subscriptions,
        };
        stack.relayout(cx);
        stack
    }

    // ---- public API ------------------------------------------------------

    pub fn scene(&self) -> &Arc<TraceScene> {
        &self.scene
    }

    /// Replace the drawn data. Corners come with the scene.
    pub fn set_scene(&mut self, scene: Arc<TraceScene>, cx: &mut Context<Self>) {
        self.corners = scene.corners.clone().into();
        self.corner_spans = spans(&self.corners);
        if let Some(id) = self.focused_corner
            && !self.corners.iter().any(|c| c.id == id)
        {
            self.clear_corner_focus(cx);
        }
        self.scene = scene.clone();
        self.interaction.cancel();
        self.static_view
            .update(cx, |view, cx| view.set_scene(scene, cx));
        self.relayout(cx);
        self.refresh_ticks(cx);
        cx.notify();
    }

    /// Replace corner zones without touching the static layer (corner edits).
    pub fn set_corners(&mut self, corners: Vec<CornerBand>, cx: &mut Context<Self>) {
        self.corners = corners.into();
        self.corner_spans = spans(&self.corners);
        cx.notify();
    }

    pub fn lane_styles(&self) -> &LaneStyles {
        &self.styles
    }

    /// Per-channel appearance and sizing (`channels.<key>.*`).
    pub fn set_lane_styles(&mut self, styles: LaneStyles, cx: &mut Context<Self>) {
        if styles == *self.styles {
            return;
        }
        self.styles = Arc::new(styles);
        let styles = self.styles.clone();
        self.static_view
            .update(cx, |view, cx| view.set_styles(styles, cx));
        self.relayout(cx);
        cx.notify();
    }

    /// FIT (weights) or manual (exact percentages, scrolls on overflow).
    pub fn set_fit(&mut self, fit: bool, cx: &mut Context<Self>) {
        if self.mode.fit != fit {
            self.mode = LayoutMode::fit(fit).resizing(self.mode.resizing);
            self.relayout(cx);
            cx.notify();
        }
    }

    pub fn is_fit(&self) -> bool {
        self.mode.fit
    }

    /// Lane resize editing: dividers drag, lanes project into FIT.
    pub fn set_resizing(&mut self, resizing: bool, cx: &mut Context<Self>) {
        if self.mode.resizing != resizing {
            self.mode = self.mode.resizing(resizing);
            self.resize_draft = None;
            self.interaction.cancel();
            self.relayout(cx);
            cx.notify();
        }
    }

    pub fn is_resizing(&self) -> bool {
        self.mode.resizing
    }

    pub fn set_editing_corners(&mut self, editing: bool, cx: &mut Context<Self>) {
        if self.editing_corners != editing {
            self.editing_corners = editing;
            self.interaction.cancel();
            cx.notify();
        }
    }

    pub fn is_editing_corners(&self) -> bool {
        self.editing_corners
    }

    pub fn focused_corner(&self) -> Option<u32> {
        self.focused_corner
    }

    /// Focus a corner: ease the viewport so the zone sits in the left half
    /// (140 ms, OutCubic) and dim the traces outside it.
    pub fn focus_corner(&mut self, id: u32, animate: bool, cx: &mut Context<Self>) {
        let Some(corner) = self.corners.iter().find(|c| c.id == id).cloned() else {
            return;
        };
        self.focused_corner = Some(id);
        self.viewport
            .update(cx, |v, cx| v.focus(corner.start, corner.end, animate, cx));
        self.cursor.update(cx, |c, cx| {
            c.set_focus(Some(Selection::new(corner.start, corner.end)), cx)
        });
        cx.notify();
    }

    pub fn clear_corner_focus(&mut self, cx: &mut Context<Self>) {
        if self.focused_corner.take().is_some() {
            self.cursor.update(cx, |c, cx| c.set_focus(None, cx));
            cx.notify();
        }
    }

    /// Pin or unpin a lane (by its root channel key).
    pub fn toggle_lane_pinned(&mut self, key: &str, cx: &mut Context<Self>) {
        let mut styles = (*self.styles).clone();
        let style = styles.get_mut(key);
        style.sizing.pinned = !style.sizing.pinned;
        let pinned = style.sizing.pinned;
        self.set_lane_styles(styles, cx);
        cx.emit(TraceEvent::LanePinToggled {
            key: SharedString::from(key.to_string()),
            pinned,
        });
    }

    /// Move the cursor by whole samples of the primary lap.
    pub fn step_cursor(&mut self, steps: i64, cx: &mut Context<Self>) {
        let samples = self.primary_samples();
        self.cursor.update(cx, |c, cx| c.step(steps, samples, cx));
        if let Some(fraction) = self.cursor.read(cx).fraction() {
            cx.emit(TraceEvent::CursorMoved(fraction));
        }
    }

    pub fn zoom_in(&mut self, cx: &mut Context<Self>) {
        let anchor = self.cursor.read(cx).fraction();
        self.viewport.update(cx, |v, cx| v.zoom_in(anchor, cx));
        self.emit_viewport(cx);
    }

    pub fn zoom_out(&mut self, cx: &mut Context<Self>) {
        let anchor = self.cursor.read(cx).fraction();
        self.viewport.update(cx, |v, cx| v.zoom_out(anchor, cx));
        self.emit_viewport(cx);
    }

    pub fn reset_view(&mut self, cx: &mut Context<Self>) {
        self.viewport.update(cx, |v, cx| v.reset(cx));
        self.clear_corner_focus(cx);
        self.emit_viewport(cx);
    }

    pub fn toggle_axis(&mut self, cx: &mut Context<Self>) {
        self.viewport.update(cx, |v, cx| v.toggle_axis(cx));
    }

    /// Times the static layer has rendered. A cursor move must not change it.
    pub fn static_rebuilds(&self, cx: &gpui_kit::App) -> usize {
        self.static_view.read(cx).stats().renders
    }

    /// Static layer counters (renders, geometry rebuilds, last build time).
    pub fn static_stats(&self, cx: &gpui_kit::App) -> StaticStats {
        self.static_view.read(cx).stats()
    }

    /// The resolved lane geometry (for tests and the app's lane menus).
    pub fn layout(&self) -> &LaneLayout {
        &self.layout
    }

    pub fn scroll_offset(&self) -> f64 {
        self.layout.scroll
    }

    // ---- layout -----------------------------------------------------------

    fn primary_samples(&self) -> usize {
        self.scene
            .lanes
            .iter()
            .map(|lane| lane.primary.len())
            .max()
            .unwrap_or(0)
    }

    fn sizing(&self) -> Vec<LaneSizing> {
        self.scene
            .lanes
            .iter()
            .map(|lane| {
                let mut sizing = self.styles.get(&lane.key).sizing;
                sizing.visible = sizing.visible && lane.primary.len() >= 2;
                if lane.kind == LaneKind::Delta {
                    sizing.min_height = sizing.min_height.max(GAP_LANE_MIN_HEIGHT);
                }
                sizing
            })
            .collect()
    }

    pub(crate) fn set_plot_bounds(&mut self, bounds: Bounds<Pixels>, cx: &mut Context<Self>) {
        if self.plot_bounds != Some(bounds) {
            let resized = self.plot_bounds.map(|b| b.size) != Some(bounds.size);
            self.plot_bounds = Some(bounds);
            if resized {
                self.relayout(cx);
                self.refresh_ticks(cx);
                cx.notify();
            }
        }
    }

    fn relayout(&mut self, cx: &mut Context<Self>) {
        let height = self
            .plot_bounds
            .map_or(0.0, |b| b.size.height.as_f32() as f64);
        let mut layout = layout_lanes(&self.sizing(), self.mode, height, self.scroll);
        if self.mode.resizing
            && let Some(draft) = &self.resize_draft
            && draft.len() == layout.slots.len()
        {
            let mut y = 0.0;
            for (slot, height) in layout.slots.iter_mut().zip(draft) {
                slot.y = y;
                slot.height = *height;
                y += height;
            }
        }
        self.scroll = layout.scroll;
        self.lane_bottoms = layout.slots.iter().map(|s| s.y + s.height).collect();
        self.lane_heights = layout.slots.iter().map(|s| s.height).collect();
        if *self.layout != layout {
            self.layout = Arc::new(layout);
            let layout = self.layout.clone();
            self.static_view
                .update(cx, |view, cx| view.set_layout(layout, cx));
        }
    }

    fn refresh_ticks(&mut self, cx: &mut Context<Self>) {
        let width = self
            .plot_bounds
            .map_or(0.0, |b| b.size.width.as_f32() as f64);
        let state = self.viewport.read(cx);
        let (viewport, axis) = (state.viewport(), state.axis());
        let values = match axis {
            XAxis::Distance => &self.scene.distance_m,
            XAxis::Time => &self.scene.time_s,
        };
        axis_ticks(
            axis,
            &viewport,
            values,
            width,
            AXIS_TICK_SPACING,
            &mut self.ticks,
        );
    }

    // ---- input ------------------------------------------------------------

    fn context<'a>(
        viewport: Viewport,
        width: f64,
        mode: LayoutMode,
        layout: &LaneLayout,
        has_data: bool,
        editing: bool,
        corners: &'a [CornerSpan],
        focused: Option<usize>,
        bottoms: &'a [f64],
        heights: &'a [f64],
    ) -> InteractionContext<'a> {
        InteractionContext {
            viewport,
            plot_left: 0.0,
            plot_width: width,
            lanes_overflow: layout.overflows(),
            // FIT that overflows at the readable minimum scrolls like manual
            // mode (the wheel scrolls lanes instead of zooming).
            fit: (mode.fit || mode.resizing) && !layout.overflows(),
            has_data,
            editing_corners: editing,
            corners,
            focused_corner: focused,
            resizing: mode.resizing,
            lane_bottoms: bottoms,
            lane_heights: heights,
        }
    }

    fn with_context<R>(
        &mut self,
        cx: &mut Context<Self>,
        f: impl FnOnce(&mut Interaction, &InteractionContext) -> R,
    ) -> R {
        let viewport = self.viewport.read(cx).viewport();
        let width = self
            .plot_bounds
            .map_or(1.0, |b| b.size.width.as_f32() as f64);
        let focused = self
            .focused_corner
            .and_then(|id| self.corners.iter().position(|c| c.id == id));
        let has_data = !self.scene.is_empty();
        let ctx = Self::context(
            viewport,
            width,
            self.mode,
            &self.layout,
            has_data,
            self.editing_corners,
            &self.corner_spans,
            focused,
            &self.lane_bottoms,
            &self.lane_heights,
        );
        f(&mut self.interaction, &ctx)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn pointer_down(
        &mut self,
        x: f64,
        y: f64,
        button: PointerButton,
        click_count: usize,
        position: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        window.focus(&self.focus_handle, cx);
        let effects = self.with_context(cx, |machine, ctx| {
            machine.pointer_down(x, y, button, click_count, ctx)
        });
        self.apply(effects, position, cx);
        cx.notify();
    }

    pub(crate) fn pointer_move(&mut self, x: f64, y: f64, inside: bool, cx: &mut Context<Self>) {
        let before = self.interaction.cursor();
        let effects = if self.interaction.is_dragging() || inside {
            self.pointer_inside = inside;
            self.with_context(cx, |machine, ctx| machine.pointer_move(x, y, ctx))
        } else if self.pointer_inside {
            self.pointer_inside = false;
            self.interaction.pointer_leave()
        } else {
            return;
        };
        self.apply(effects, Point::default(), cx);
        if self.interaction.cursor() != before {
            cx.notify();
        }
    }

    pub(crate) fn pointer_up(&mut self, x: f64, button: PointerButton, cx: &mut Context<Self>) {
        if !self.interaction.is_dragging() {
            return;
        }
        let effects = self.with_context(cx, |machine, ctx| machine.pointer_up(x, button, ctx));
        self.apply(effects, Point::default(), cx);
        cx.notify();
    }

    pub(crate) fn wheel(
        &mut self,
        x: f64,
        delta: WheelDelta,
        modifiers: KeyModifiers,
        cx: &mut Context<Self>,
    ) {
        let effects = self.with_context(cx, |machine, ctx| machine.wheel(x, delta, modifiers, ctx));
        self.apply(effects, Point::default(), cx);
    }

    fn emit_viewport(&mut self, cx: &mut Context<Self>) {
        let viewport = self.viewport.read(cx).viewport();
        cx.emit(TraceEvent::ViewportChanged(viewport));
    }

    fn apply(&mut self, effects: Effects, position: Point<Pixels>, cx: &mut Context<Self>) {
        for effect in effects {
            match effect {
                Effect::MoveCursor(fraction) => {
                    self.cursor
                        .update(cx, |c, cx| c.set_fraction(Some(fraction), cx));
                    cx.emit(TraceEvent::CursorMoved(fraction));
                }
                Effect::Hover(hover) => {
                    self.cursor.update(cx, |c, cx| c.set_hover(hover, cx));
                }
                Effect::Select {
                    start,
                    end,
                    finished,
                } => {
                    let selection = Selection::new(start, end);
                    self.cursor
                        .update(cx, |c, cx| c.set_selection(Some(selection), cx));
                    if finished {
                        cx.emit(TraceEvent::RangeSelected(selection));
                    }
                }
                Effect::ClearSelection => {
                    self.cursor.update(cx, |c, cx| c.set_selection(None, cx));
                }
                Effect::SetViewport(viewport) => {
                    self.viewport
                        .update(cx, |v, cx| v.set_viewport(viewport, cx));
                    cx.emit(TraceEvent::ViewportChanged(viewport));
                }
                Effect::ResetViewport => self.reset_view(cx),
                Effect::ScrollLanes(delta) => {
                    let before = self.scroll;
                    self.scroll = (self.scroll + delta).max(0.0);
                    self.relayout(cx);
                    if self.scroll != before {
                        cx.notify();
                    }
                }
                Effect::EditCorner { index, start, end } => {
                    let mut corners: Vec<CornerBand> = self.corners.to_vec();
                    if let Some(corner) = corners.get_mut(index) {
                        corner.start = start;
                        corner.end = end;
                        let id = corner.id;
                        self.set_corners(corners, cx);
                        cx.emit(TraceEvent::CornerEdited { id, start, end });
                    }
                }
                Effect::ResizeLanes { heights } => {
                    let keys = self
                        .layout
                        .slots
                        .iter()
                        .filter_map(|s| self.scene.lanes.get(s.root).map(|l| l.key.clone()))
                        .collect();
                    self.resize_draft = Some(heights.clone());
                    self.relayout(cx);
                    cx.emit(TraceEvent::LaneResized { keys, heights });
                    cx.notify();
                }
                Effect::ContextMenu { y, .. } => {
                    let lane = self
                        .layout
                        .slot_at(y)
                        .and_then(|ix| self.layout.slots.get(ix))
                        .and_then(|slot| self.scene.lanes.get(slot.root))
                        .map(|lane| lane.key.clone());
                    cx.emit(TraceEvent::ContextMenu { lane, position });
                }
            }
        }
    }

    // ---- rendering ----------------------------------------------------------

    fn render_chrome(
        &self,
        readout_at: Option<f64>,
        palette: &TracePalette,
        rem: f32,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let theme = cx.theme();
        let mono = theme.mono_font_family.clone();
        let pinned_height = self.layout.pinned_height as f32;
        let map = self.scene.map.as_deref();
        let muted = theme.muted_foreground;
        let foreground = theme.foreground;
        let border = theme.border;
        let viewport = self.viewport.read(cx).viewport();
        let approximate = self.scene.approximate_delta;
        let lap_label: SharedString = self
            .scene
            .reference_label()
            .cloned()
            .unwrap_or_else(|| "R".into());
        let tall_min = rem * TALL_LEGEND_REMS;
        let cells = self
            .layout
            .slots
            .iter()
            .enumerate()
            .filter_map(|(ix, slot)| {
                let root = self.scene.lanes.get(slot.root)?;
                let (top, height) = (slot.y as f32, slot.height as f32);
                if !slot.pinned && top + height <= pinned_height {
                    return None;
                }
                let channels: Vec<&LaneSeries> = slot
                    .channels()
                    .filter_map(|i| self.scene.lanes.get(i))
                    .collect();
                let title = channels
                    .iter()
                    .map(|l| l.title.as_ref())
                    .collect::<Vec<_>>()
                    .join(" / ");
                let mut units: Vec<&str> = Vec::new();
                for unit in channels.iter().map(|l| l.unit.as_ref()) {
                    if !unit.is_empty() && !units.contains(&unit) {
                        units.push(unit);
                    }
                }
                let unit: SharedString = units.join(" / ").into();
                let shared = channels.len() > 1;
                let mut label = if unit.is_empty() {
                    title.clone()
                } else {
                    format!("{title}, {unit}")
                };
                // A shared lane names each channel in the hue of its primary
                // line, so the overlaid line is findable before any cursor.
                let names: Vec<_> = channels
                    .iter()
                    .enumerate()
                    .map(|(position, lane)| {
                        let color = if position == 0 || !shared {
                            foreground
                        } else {
                            let style = self.styles.get(&lane.key);
                            palette.channel_colors(&lane.key, false, &style).0
                        };
                        h_flex()
                            .gap_1()
                            .min_w_0()
                            .when(position > 0, |el| {
                                el.flex_shrink_0().child(div().text_color(muted).child("/"))
                            })
                            .child(div().text_color(color).truncate().child(lane.title.clone()))
                    })
                    .collect();
                let mut rows: Vec<AnyElement> = Vec::new();
                let is_gap = root.kind == LaneKind::Delta;
                let tall = !shared && height >= tall_min;
                if is_gap {
                    // The gap lane's legend is one big figure: the gap at the
                    // cursor (idle: the change across the view), then the
                    // corner under the cursor and its Δt, or where the gap
                    // ends (the lap, or the view when zoomed).
                    if self.scene.time_share_delta {
                        label.push_str(", share of lap time, not station aligned");
                    }
                    let zoomed = viewport != Viewport::FULL;
                    let change = root.change_in(viewport);
                    let (view_text, _) = delta_seconds(change, approximate);
                    label.push_str(&format!(", in view {view_text}"));
                    let (column, text, trend, context) = match readout_at {
                        Some(fraction) => {
                            let readout = root.readout(fraction, map);
                            let (text, trend) = delta_seconds(readout.primary, approximate);
                            label.push_str(&format!(", at cursor {text}"));
                            let context = match self.corner_at(fraction) {
                                Some(corner) => {
                                    let delta =
                                        corner.delta.map(|dt| delta_seconds(dt, approximate));
                                    if let Some((dt, _)) = &delta {
                                        label.push_str(&format!(", {} {dt}", corner.label));
                                    }
                                    GapContext::Corner(corner.label.clone(), delta)
                                }
                                None if zoomed => {
                                    GapContext::Plain("in view", Some(view_text.clone()))
                                }
                                None => {
                                    let (end, _) =
                                        delta_seconds(root.change_in(Viewport::FULL), approximate);
                                    GapContext::Plain("ends", Some(end))
                                }
                            };
                            ("cursor", text, trend, context)
                        }
                        None => {
                            let (text, trend) = delta_seconds(change, approximate);
                            let context = if zoomed { "in view" } else { "over the lap" };
                            ("view", text, trend, GapContext::Plain(context, None))
                        }
                    };
                    rows.push(
                        gap_figure(
                            &root.key,
                            (column, text, trend, context),
                            approximate,
                            palette,
                            muted,
                            &mono,
                        )
                        .into_any_element(),
                    );
                } else if let Some(fraction) = readout_at {
                    for lane in &channels {
                        let readout = lane.readout(fraction, map);
                        let style = self.styles.get(&lane.key);
                        // Values carry the lap colour of the mode: the
                        // primary role (or the channel's hue), the
                        // reference plainly beside its lap label.
                        let primary = palette.channel_colors(&lane.key, true, &style).0;
                        let reference = palette.reference_value(&lane.key, true, &style);
                        let swatch =
                            shared.then(|| palette.channel_colors(&lane.key, false, &style).0);
                        let text = ReadoutText::new(lane, &readout, palette, muted);
                        label.push_str(&format!(", {}", text.spoken(lane)));
                        rows.push(
                            readout_rows(
                                lane,
                                text,
                                tall,
                                swatch,
                                (primary, reference, muted),
                                &lap_label,
                                &mono,
                            )
                            .into_any_element(),
                        );
                    }
                }
                let id = ElementId::Name(format!("lane-{}", root.key).into());
                // Scroll lanes are placed inside the scroll region, which
                // clips them where they pass under the pinned lanes.
                let top = if slot.pinned {
                    top
                } else {
                    top - pinned_height
                };
                let roomy = is_gap || tall;
                let cell = div()
                    .id(id)
                    .role(Role::Group)
                    .aria_label(SharedString::from(label))
                    .test_support()
                    .absolute()
                    .left_0()
                    .right_0()
                    .top(px(top))
                    .h(px(height))
                    .overflow_hidden()
                    .px_3()
                    .when(roomy, |el| el.pt_1p5())
                    .when(!roomy, |el| el.pt_px())
                    .text_label()
                    .line_height(rems(LEGEND_LINE_REMS))
                    .when(ix > 0, |el| el.border_t_1().border_color(border))
                    .child(
                        h_flex()
                            .gap_1()
                            .min_w_0()
                            .items_baseline()
                            .child(h_flex().gap_1().min_w_0().font_medium().children(names))
                            .when(!unit.is_empty(), |el| {
                                el.child(
                                    div()
                                        .text_caption()
                                        .font_family(mono.clone())
                                        .text_color(muted)
                                        .flex_shrink_0()
                                        .child(unit),
                                )
                            }),
                    )
                    .child(v_flex().children(rows));
                Some((slot.pinned, cell.into_any_element()))
            });
        let (pinned, scrolled): (Vec<_>, Vec<_>) = cells.partition(|(pinned, _)| *pinned);
        div()
            .relative()
            .size_full()
            .overflow_hidden()
            .border_r_1()
            .border_color(border)
            .child(
                div()
                    .absolute()
                    .top_0()
                    .left_0()
                    .right_0()
                    .h(px(pinned_height))
                    .children(pinned.into_iter().map(|(_, cell)| cell)),
            )
            .child(
                div()
                    .absolute()
                    .top(px(pinned_height))
                    .bottom_0()
                    .left_0()
                    .right_0()
                    .overflow_hidden()
                    .children(scrolled.into_iter().map(|(_, cell)| cell)),
            )
    }

    /// The corner zone under a primary lap fraction.
    fn corner_at(&self, fraction: f64) -> Option<&CornerBand> {
        self.corners
            .iter()
            .find(|c| (c.start..=c.end).contains(&fraction))
    }

    /// Repaint the lanes in `mode` (lap or channel colours). Geometry never
    /// keys on colour: the static layer repaints from its cached paths.
    pub fn set_color_mode(&mut self, mode: ColorMode, cx: &mut Context<Self>) {
        if self.color_mode != mode {
            self.color_mode = mode;
            self.static_view
                .update(cx, |view, cx| view.set_color_mode(mode, cx));
            cx.notify();
        }
    }

    pub fn color_mode(&self) -> ColorMode {
        self.color_mode
    }
}

/// Line height of a lane legend's text rows, rems: dense (1.17 of the
/// label step), so a shared lane's title and two readout rows fit a
/// [`MIN_LANE_HEIGHT`](crate::layout::MIN_LANE_HEIGHT) lane.
const LEGEND_LINE_REMS: f32 = 0.875;
/// Width of the lane legend column, rems: a title and unit, the primary
/// value in the display step and `L12 −180 −12` in label-size numerals,
/// with the cell's inset, so legends never clip.
pub const LEGEND_REMS: f32 = 10.5;
/// Width of the value-axis gutter between the legends and the plot, rems:
/// `+1.0` or `250` in caption numerals, right-aligned against the plot.
pub const GUTTER_REMS: f32 = 3.0;
/// Width of the lane chrome column: the legends and the value-axis gutter,
/// rems. Rows above the lanes (the corner ruler's, the damper strip) inset
/// their plots by it.
pub const CHROME_REMS: f32 = LEGEND_REMS + GUTTER_REMS;
/// Lanes at least this tall, rems, show the primary value large with the
/// reference beneath; shorter or shared lanes put both on one row.
const TALL_LEGEND_REMS: f32 = 4.0;
/// Line height of a legend's large figures (the display step), rems.
const FIGURE_LINE_REMS: f32 = 1.375;
/// Width of a shared lane's line swatch, rems.
const SWATCH_REMS: f32 = 0.75;

/// Which direction of a channel's difference is better, where one is.
/// Only channels with a sense carry a Δ in the legend.
fn delta_sense(key: &str) -> Option<DeltaSense> {
    match key {
        "speed" => Some(DeltaSense::HigherIsBetter),
        _ => None,
    }
}

/// A signed legend value (steering: `+53°`, `0°` at centre).
fn signed_value(value: f64, decimals: usize) -> SharedString {
    let (text, trend) = format_delta(Some(value), decimals, DeltaSense::default());
    if trend == DeltaTrend::Even {
        format_value(Some(0.0), decimals)
    } else {
        text
    }
}

/// Formatted values of one channel at the cursor.
struct ReadoutText {
    primary: SharedString,
    reference: Option<SharedString>,
    /// The difference and its colour (gain/loss), for channels with a sense.
    delta: Option<(SharedString, Hsla)>,
}

impl ReadoutText {
    fn new(lane: &LaneSeries, readout: &Readout, palette: &TracePalette, muted: Hsla) -> Self {
        let scale = lane.display_scale();
        let span = lane.y_range.span() * scale;
        let decimals = if lane.kind == LaneKind::Step || span >= 100.0 {
            0
        } else if span >= 10.0 {
            1
        } else {
            2
        };
        let steering = lane.key.as_ref() == "steering";
        let value = |v: f64| {
            if steering && v.is_finite() {
                let text = signed_value(v * scale, decimals);
                SharedString::from(format!("{text}°"))
            } else {
                format_value(Some(v * scale), decimals)
            }
        };
        let primary = value(readout.primary);
        let reference = lane
            .reference
            .is_some()
            .then(|| value(readout.reference))
            // A held value equal on both laps (gear) says it once.
            .filter(|reference| lane.kind != LaneKind::Step || *reference != primary);
        let delta = reference.as_ref().and_then(|_| {
            let sense = delta_sense(&lane.key)?;
            let (text, trend) = format_delta(Some(readout.delta * scale), decimals, sense);
            let color = match trend {
                DeltaTrend::Gain => palette.gain,
                DeltaTrend::Loss => palette.loss,
                DeltaTrend::Even => muted,
            };
            Some((text, color))
        });
        Self {
            primary,
            reference,
            delta,
        }
    }

    fn spoken(&self, lane: &LaneSeries) -> String {
        let mut text = format!("{} primary {}", lane.title, self.primary);
        if let Some(reference) = &self.reference {
            text.push_str(&format!(" reference {reference}"));
        }
        if let Some((delta, _)) = &self.delta {
            text.push_str(&format!(" delta {delta}"));
        }
        text
    }
}

/// A channel's values at the cursor, in the trace numerals: the primary
/// (large on a tall lane, else leading the row), then the reference lap's
/// label, its value and the difference (`L6 82 −3`).
fn readout_rows(
    lane: &LaneSeries,
    text: ReadoutText,
    tall: bool,
    // A shared lane's row starts with its channel's line.
    swatch: Option<Hsla>,
    (primary, reference, muted): (Hsla, Hsla, Hsla),
    lap_label: &SharedString,
    mono: &SharedString,
) -> impl IntoElement {
    let id = |column: &str| ElementId::Name(format!("readout-{}-{column}", lane.key).into());
    let cell = |column: &str, value: SharedString, color: Hsla| {
        div()
            .id(id(column))
            .test_support()
            .flex_shrink_0()
            .text_color(color)
            .child(value)
    };
    let reference_row = text.reference.map(|value| {
        h_flex()
            .gap_1p5()
            .min_w_0()
            .child(cell("lap", lap_label.clone(), muted))
            .child(cell("r", value, reference))
            .when_some(text.delta, |el, (delta, color)| {
                el.child(cell("d", delta, color))
            })
    });
    let figure = cell("p", text.primary, primary);
    let body = if tall {
        v_flex()
            .child(figure.text_display().line_height(rems(FIGURE_LINE_REMS)))
            .children(reference_row)
    } else {
        v_flex().child(
            h_flex()
                .gap_2()
                .min_w_0()
                .when_some(swatch, |el, hue| {
                    el.child(
                        div()
                            .w(rems(SWATCH_REMS))
                            .h(px(2.))
                            .rounded_sm()
                            .flex_shrink_0()
                            .bg(hue),
                    )
                })
                .child(figure)
                .children(reference_row),
        )
    };
    numerals(body.whitespace_nowrap(), mono)
}

/// What the gap lane says under its figure.
enum GapContext {
    /// Words and a figure: `ends +2.440`, `in view −0.310` (at the
    /// cursor), `over the lap`, `in view` (idle).
    Plain(&'static str, Option<SharedString>),
    /// The corner under the cursor and its Δt, where the map places time
    /// loss.
    Corner(SharedString, Option<(SharedString, Option<bool>)>),
}

/// The gap lane's figure: the gap in seconds as the legend's one large
/// number, gain or loss coloured (at reduced emphasis when approximate),
/// over a caption saying what it measures: the corner under the cursor and
/// its Δt, or where the gap ends.
fn gap_figure(
    key: &str,
    (column, value, trend, context): (&str, SharedString, Option<bool>, GapContext),
    approximate: bool,
    palette: &TracePalette,
    muted: Hsla,
    mono: &SharedString,
) -> impl IntoElement {
    let emphasis = if approximate {
        APPROXIMATE_DELTA_EMPHASIS
    } else {
        1.0
    };
    let tone = |trend: Option<bool>| match trend {
        Some(true) => palette.gain.opacity(emphasis),
        Some(false) => palette.loss.opacity(emphasis),
        None => muted,
    };
    let caption = h_flex()
        .id(ElementId::Name(format!("readout-{key}-context").into()))
        .test_support()
        .gap_1()
        .min_w_0()
        .text_caption()
        .text_color(muted);
    let caption = match context {
        GapContext::Plain(words, figure) => caption
            .child(div().flex_shrink_0().child(words))
            .when_some(figure, |el, figure| {
                el.child(numerals(div().flex_shrink_0().child(figure), mono))
            }),
        GapContext::Corner(name, delta) => caption
            .child(div().flex_shrink_0().child("at"))
            .child(div().min_w_0().truncate().child(name))
            .when_some(delta, |el, (dt, trend)| {
                el.child(numerals(
                    div().flex_shrink_0().text_color(tone(trend)).child(dt),
                    mono,
                ))
            }),
    };
    v_flex()
        .min_w_0()
        .whitespace_nowrap()
        .child(numerals(
            div()
                .id(ElementId::Name(format!("readout-{key}-{column}").into()))
                .test_support()
                .text_display()
                .line_height(rems(FIGURE_LINE_REMS))
                .text_color(tone(trend))
                .child(value),
            mono,
        ))
        .child(caption)
}

/// Text in the trace numerals: the monospace family with
/// [`trace_figures`] (the legend's words stay in the interface family).
fn numerals<E: Styled>(element: E, mono: &SharedString) -> E {
    element
        .font_family(mono.clone())
        .font_features(trace_figures())
}

/// A time delta for the gap lane: signed seconds (`≈` and two decimals under
/// a LOW-confidence alignment) and its trend, `Some(true)` for a gain.
/// Below the display resolution it is neither.
fn delta_seconds(value: f64, approximate: bool) -> (SharedString, Option<bool>) {
    if !value.is_finite() {
        return ("—".into(), None);
    }
    let decimals = if approximate { 2 } else { 3 };
    let text = signed_seconds(value, decimals);
    let text = if approximate {
        format!("≈{text}")
    } else {
        text
    };
    // Positive Δt: the primary lap is slower (loss).
    let trend = (value.abs() >= 0.5 * 10f64.powi(-(decimals as i32))).then_some(value < 0.0);
    (text.into(), trend)
}

/// Signed seconds at `decimals`; a value that rounds to zero reads
/// `±0.000` (neither gain nor loss).
fn signed_seconds(value: f64, decimals: usize) -> String {
    format_delta(Some(value), decimals, DeltaSense::LowerIsBetter)
        .0
        .to_string()
}

fn spans(corners: &[CornerBand]) -> Vec<CornerSpan> {
    corners
        .iter()
        .map(|c| CornerSpan {
            start: c.start,
            end: c.end,
        })
        .collect()
}

impl Render for TraceStack {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let palette = TracePalette::from_theme(theme).with_mode(self.color_mode);
        let rem = window.rem_size().as_f32();
        let (border, muted) = (theme.border, theme.muted_foreground);
        // Keyboard focus shows on the axis row's upper edge: a hairline in
        // the ring colour, which no ancestor clip can hide.
        let focus_edge = if self.focus_handle.is_focused(window) {
            theme.ring
        } else {
            border
        };
        let state = self.viewport.read(cx);
        let viewport = state.viewport();
        let cursor = self.cursor.read(cx);
        let (fraction, hover, selection, focus) = (
            cursor.fraction(),
            cursor.hover(),
            cursor.selection(),
            cursor.focus(),
        );
        let readout_at = cursor.readout_fraction();
        let width = self.plot_bounds.map_or(0.0, |b| b.size.width.as_f32());
        let empty = self.scene.is_empty();

        let overlay = TraceOverlay {
            stack: cx.entity().downgrade(),
            scene: self.scene.clone(),
            layout: self.layout.clone(),
            styles: self.styles.clone(),
            corners: self.corners.clone(),
            viewport,
            cursor: fraction,
            hover,
            selection,
            focus,
            editing_corners: self.editing_corners,
            palette,
            gesture: if empty {
                GestureCursor::Default
            } else {
                self.interaction.cursor()
            },
            measured: self.measured.clone(),
        };

        let previous_label = (viewport.start < 0.0)
            .then(|| self.scene.previous_label.clone())
            .flatten();
        let next_label = (viewport.end > 1.0)
            .then(|| self.scene.next_label.clone())
            .flatten();
        let lap_start = viewport.x_for_fraction(0.0, 0.0, width as f64) as f32;
        let lap_end = viewport.x_for_fraction(1.0, 0.0, width as f64) as f32;

        let plot = div()
            .id("trace-plot")
            .role(Role::Figure)
            .aria_label("Traces")
            .test_support()
            .absolute()
            .top_0()
            .bottom_0()
            .right_0()
            .left(rems(GUTTER_REMS))
            .overflow_hidden()
            .child(overlay)
            .when_some(previous_label, |el, label| {
                el.child(
                    div()
                        .absolute()
                        .top_1()
                        .right(px((width - lap_start).max(0.0)))
                        .pr_2()
                        .text_caption()
                        .text_color(muted)
                        .child(SharedString::from(format!("« {label}"))),
                )
            })
            .when_some(next_label, |el, label| {
                el.child(
                    div()
                        .absolute()
                        .top_1()
                        .left(px(lap_end.max(0.0)))
                        .pl_2()
                        .text_caption()
                        .text_color(muted)
                        .child(SharedString::from(format!("{label} »"))),
                )
            })
            .when(empty, |el| {
                el.child(
                    div()
                        .absolute()
                        .inset_0()
                        .flex()
                        .items_center()
                        .justify_center()
                        .text_body()
                        .text_color(muted)
                        .child("Select a lap to see its traces"),
                )
            });
        // The static layer spans the value-axis gutter and the plot; the
        // overlay and the pointer surface cover the plot only.
        let lanes = div()
            .relative()
            .flex_1()
            .min_w_0()
            .h_full()
            .overflow_hidden()
            .child(
                self.static_view
                    .clone()
                    .cached(StyleRefinement::default().size_full()),
            )
            .child(plot);

        v_flex()
            .id("trace-stack")
            .key_context("TraceStack")
            .track_focus(&self.focus_handle)
            .size_full()
            .min_h_0()
            .child(
                h_flex()
                    .flex_1()
                    .min_h_0()
                    .items_stretch()
                    .child(
                        div()
                            .w(rems(LEGEND_REMS))
                            .flex_shrink_0()
                            .child(self.render_chrome(readout_at, &palette, rem, cx)),
                    )
                    .child(lanes),
            )
            .child(
                h_flex()
                    .h_6()
                    .flex_shrink_0()
                    .items_stretch()
                    .border_t_1()
                    .border_color(focus_edge)
                    .child(
                        div()
                            .w(rems(LEGEND_REMS))
                            .flex_shrink_0()
                            .border_r_1()
                            .border_color(border),
                    )
                    .child(div().w(rems(GUTTER_REMS)).flex_shrink_0())
                    .child(div().flex_1().min_w_0().child(TraceAxis::new(
                        &self.ticks,
                        &viewport,
                        width,
                    ))),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui_kit::TestAppContext;
    use gpui_kit::component::Theme;

    fn lane(key: &str, kind: LaneKind, unit: &str, primary: f64, reference: f64) -> LaneSeries {
        LaneSeries::new(key, key, kind, vec![primary; 4].into())
            .with_unit(unit)
            .with_reference(Some(vec![reference; 4].into()))
            .with_y_range(crate::scene::YRange::new(-200.0, 300.0))
    }

    #[gpui_kit::test]
    fn legend_values_follow_each_channel(cx: &mut TestAppContext) {
        cx.update(gpui_kit::init);
        let palette = cx.update(|cx| TracePalette::from_theme(Theme::global(cx)));
        let muted = palette.label;
        let text =
            |lane: &LaneSeries| ReadoutText::new(lane, &lane.readout(0.5, None), &palette, muted);
        // Speed: value, reference and a Δ in the loss colour when slower.
        let speed = text(&lane("speed", LaneKind::Line, "km/h", 79.0, 82.0));
        assert_eq!(speed.primary.as_ref(), "79");
        assert_eq!(speed.reference.as_deref(), Some("82"));
        let (delta, color) = speed.delta.unwrap();
        assert_eq!(delta.as_ref(), "\u{2212}3");
        assert_eq!(color, palette.loss);
        // Steering is signed, in degrees, with no Δ.
        let steering = text(&lane("steering", LaneKind::Line, "°", 53.0, 53.0));
        assert_eq!(steering.primary.as_ref(), "+53°");
        assert!(steering.delta.is_none());
        let centre = text(&lane("steering", LaneKind::Line, "°", 0.2, -0.2));
        assert_eq!(centre.primary.as_ref(), "0°");
        // Gear says an equal reference once.
        let gear = text(&lane("gear", LaneKind::Step, "", 2.0, 2.0));
        assert_eq!(gear.primary.as_ref(), "2");
        assert!(gear.reference.is_none());
        let shifted = text(&lane("gear", LaneKind::Step, "", 2.0, 3.0));
        assert_eq!(shifted.reference.as_deref(), Some("3"));
    }
}
