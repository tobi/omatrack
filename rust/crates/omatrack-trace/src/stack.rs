//! `TraceStack`: the stacked, synchronized lanes of the trace workspace.
//!
//! Composition, top to bottom:
//!
//! - the lanes: a chrome column (name and unit; mono P/R/Δ readouts only
//!   while a cursor or hover is active) beside the plot column. The plot column holds the cached
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
    AppContext as _, Bounds, Context, ElementId, Entity, EventEmitter, FocusHandle, Focusable,
    Hsla, InteractiveElement as _, IntoElement, ParentElement as _, Pixels, Point, Render, Role,
    SharedString, StatefulInteractiveElement as _, StyleRefinement, Styled as _, Subscription,
    Window, div, prelude::FluentBuilder as _, px,
};

use crate::axis::TraceAxis;
use crate::interaction::{
    CornerSpan, Effect, Effects, GestureCursor, Interaction, InteractionContext, KeyModifiers,
    PointerButton, WheelDelta,
};
use crate::layout::{LaneLayout, LaneSizing, LayoutMode, layout_lanes};
use crate::overlay::TraceOverlay;
use crate::palette::TracePalette;
use crate::scale::{Tick, Viewport, XAxis, axis_ticks};
use crate::scene::{CornerBand, LaneKind, LaneSeries, LaneStyles, Readout, TraceScene};
use crate::state::{CursorState, Selection, ViewportState};
use crate::static_layer::{StaticStats, TICK_SPACING, TraceStaticView};

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
            TICK_SPACING,
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
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let theme = cx.theme();
        let pinned_height = self.layout.pinned_height as f32;
        let map = self.scene.map.as_deref();
        let mono = theme.mono_font_family.clone();
        let muted = theme.muted_foreground;
        let foreground = theme.foreground;
        let border = theme.border;
        let viewport = self.viewport.read(cx).viewport();
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
                let combined = channels.len() > 1;
                let mut label = if unit.is_empty() {
                    title.clone()
                } else {
                    format!("{title}, {unit}")
                };
                // A shared lane names each channel in its own hue, so the
                // overlaid line is findable before any cursor exists.
                let names: Vec<_> = channels
                    .iter()
                    .enumerate()
                    .map(|(position, lane)| {
                        let color = if position == 0 {
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
                // Δ states the time gained or lost across the view (its
                // range follows the view): the lane's one number that
                // matters before a cursor exists.
                let approximate = self.scene.approximate_delta;
                let scale = (root.kind == LaneKind::Delta).then(|| {
                    let change = root.change_in(viewport);
                    SharedString::from(if !change.is_finite() {
                        "in view —".to_string()
                    } else if approximate {
                        format!("in view ≈{}", signed_seconds(change, 2))
                    } else {
                        format!("in view {}", signed_seconds(change, 3))
                    })
                });
                let rows: Vec<_> = readout_at
                    .map(|fraction| {
                        channels
                            .iter()
                            .enumerate()
                            .map(|(position, lane)| {
                                let readout = lane.readout(fraction, map);
                                let style = self.styles.get(&lane.key);
                                let (primary, reference) =
                                    palette.channel_colors(&lane.key, position == 0, &style);
                                let text = ReadoutText::new(lane, &readout, approximate);
                                label.push_str(&format!(", {}", text.spoken(lane)));
                                readout_row(
                                    lane, text, combined, primary, reference, palette, muted,
                                )
                            })
                            .collect()
                    })
                    .unwrap_or_default();
                let id = ElementId::Name(format!("lane-{}", root.key).into());
                // Scroll lanes are placed inside the scroll region, which
                // clips them where they pass under the pinned lanes.
                let top = if slot.pinned {
                    top
                } else {
                    top - pinned_height
                };
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
                    .px_2()
                    .pt_0p5()
                    .when(ix > 0, |el| el.border_t_1().border_color(border))
                    .child(
                        h_flex()
                            .gap_1()
                            .min_w_0()
                            .text_xs()
                            .child(h_flex().gap_1().min_w_0().font_medium().children(names))
                            .when(!unit.is_empty(), |el| {
                                el.child(div().text_color(muted).flex_shrink_0().child(unit))
                            })
                            .when_some(scale, |el, scale| {
                                el.child(
                                    div()
                                        .ml_auto()
                                        .flex_shrink_0()
                                        .font_family(mono.clone())
                                        .text_color(muted)
                                        .child(scale),
                                )
                            }),
                    )
                    .child(v_flex().font_family(mono.clone()).text_xs().children(rows));
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
}

/// Formatted P/R/Δ values of one channel at the cursor.
struct ReadoutText {
    primary: SharedString,
    reference: Option<SharedString>,
    delta: Option<SharedString>,
    delta_color: Option<bool>,
}

impl ReadoutText {
    fn new(lane: &LaneSeries, readout: &Readout, approximate: bool) -> Self {
        let scale = lane.display_scale();
        let span = lane.y_range.span() * scale;
        let decimals = if lane.kind == LaneKind::Step {
            0
        } else if lane.kind == LaneKind::Delta {
            3
        } else if span >= 100.0 {
            0
        } else if span >= 10.0 {
            1
        } else {
            2
        };
        let value = |v: f64| -> SharedString {
            if v.is_finite() {
                format!("{:.*}", decimals, v * scale).into()
            } else {
                "—".into()
            }
        };
        if lane.kind == LaneKind::Delta {
            let v = readout.primary;
            let decimals = if approximate { 2 } else { 3 };
            let text = if !v.is_finite() {
                "—".to_string()
            } else if approximate {
                format!("≈{}", signed_seconds(v, decimals))
            } else {
                signed_seconds(v, decimals)
            };
            return Self {
                primary: text.into(),
                reference: None,
                delta: None,
                // Positive Δt: the primary lap is slower (loss). Below the
                // display resolution, or under LOW confidence, neither.
                delta_color: (v.is_finite()
                    && !approximate
                    && v.abs() >= 0.5 * 10f64.powi(-(decimals as i32)))
                .then_some(v < 0.0),
            };
        }
        let has_reference = lane.reference.is_some();
        Self {
            primary: value(readout.primary),
            reference: has_reference.then(|| value(readout.reference)),
            delta: has_reference.then(|| {
                if readout.delta.is_finite() {
                    format!("{:+.*}", decimals, readout.delta * scale).into()
                } else {
                    "—".into()
                }
            }),
            delta_color: None,
        }
    }

    fn spoken(&self, lane: &LaneSeries) -> String {
        let mut text = format!("{} primary {}", lane.title, self.primary);
        if let Some(reference) = &self.reference {
            text.push_str(&format!(" reference {reference}"));
        }
        if let Some(delta) = &self.delta {
            text.push_str(&format!(" delta {delta}"));
        }
        text
    }
}

fn readout_row(
    lane: &LaneSeries,
    text: ReadoutText,
    combined: bool,
    primary: Hsla,
    reference: Hsla,
    palette: &TracePalette,
    muted: Hsla,
) -> impl IntoElement {
    let value_color = match text.delta_color {
        Some(true) => palette.gain,
        Some(false) => palette.loss,
        None if lane.kind == LaneKind::Delta => muted,
        None => primary,
    };
    h_flex()
        .gap_2()
        .min_w_0()
        .whitespace_nowrap()
        .when(combined, |el| {
            el.child(
                div()
                    .text_color(muted)
                    .flex_shrink_0()
                    .child(SharedString::from(
                        lane.title.chars().take(3).collect::<String>(),
                    )),
            )
        })
        .when(lane.kind == LaneKind::Delta, |el| {
            el.child(div().text_color(muted).child("Δ"))
        })
        .when(lane.kind != LaneKind::Delta, |el| {
            el.child(div().text_color(muted).child("P"))
        })
        .child(div().text_color(value_color).child(text.primary))
        .when_some(text.reference, |el, value| {
            el.child(div().text_color(muted).child("R"))
                .child(div().text_color(reference).child(value))
        })
        .when_some(text.delta, |el, value| {
            el.child(div().text_color(muted).child("Δ"))
                .child(div().text_color(muted).child(value))
        })
}

/// Signed seconds at `decimals`; a value that rounds to zero reads
/// `±0.000` (neither gain nor loss).
fn signed_seconds(value: f64, decimals: usize) -> String {
    if value.abs() < 0.5 * 10f64.powi(-(decimals as i32)) {
        format!("±{:.*}", decimals, 0.0)
    } else {
        format!("{value:+.*}", decimals)
    }
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
        let palette = TracePalette::from_theme(theme);
        let (background, border, muted) = (theme.background, theme.border, theme.muted_foreground);
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
            .child(overlay)
            .when_some(previous_label, |el, label| {
                el.child(
                    div()
                        .absolute()
                        .top_1()
                        .right(px((width - lap_start).max(0.0)))
                        .pr_2()
                        .text_xs()
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
                        .text_xs()
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
                        .text_sm()
                        .text_color(muted)
                        .child("Select a lap to see its traces"),
                )
            });

        v_flex()
            .id("trace-stack")
            .key_context("TraceStack")
            .track_focus(&self.focus_handle)
            .size_full()
            .min_h_0()
            .bg(background)
            .child(
                h_flex()
                    .flex_1()
                    .min_h_0()
                    .items_stretch()
                    .child(
                        div()
                            .w_40()
                            .flex_shrink_0()
                            .child(self.render_chrome(readout_at, &palette, cx)),
                    )
                    .child(plot),
            )
            .child(
                h_flex()
                    .h_5()
                    .flex_shrink_0()
                    .items_stretch()
                    .border_t_1()
                    .border_color(focus_edge)
                    .child(
                        div()
                            .w_40()
                            .flex_shrink_0()
                            .border_r_1()
                            .border_color(border),
                    )
                    .child(div().flex_1().min_w_0().child(TraceAxis::new(
                        &self.ticks,
                        &viewport,
                        width,
                    ))),
            )
    }
}
