//! The static trace layer: the value-axis gutter, dotted grids, lane
//! separators, masks, every lane's paths and the speed lane's apex
//! callouts, drawn by one custom element inside [`TraceStaticView`].
//!
//! The element spans the value-axis gutter ([`crate::GUTTER_REMS`], tick
//! labels right-aligned in the trace numerals) and the plot to its right;
//! every plot coordinate is relative to the plot's left edge.
//!
//! The owning [`crate::TraceStack`] embeds this view through
//! `Entity::cached`, so GPUI replays the previous frame's primitives until
//! the view is notified. It is notified only on scene, viewport, layout,
//! style or colour-mode change; it never reads the cursor, so a cursor move
//! cannot rebuild it. Theme and colour-mode changes repaint this layer, but
//! the per-channel geometry cache never keys on colour, so they only
//! recolour.

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::Arc;
use std::time::Instant;

use gpui_kit::component::ActiveTheme as _;
use gpui_kit::{
    App, BorderStyle, Bounds, ContentMask, Context, Edges, Element, ElementId, Entity,
    GlobalElementId, Hsla, InspectorElementId, IntoElement, LayoutId, Pixels, Render, SharedString,
    Style, Subscription, Window, fill, point, px, quad, relative, size, transparent_black,
};

use crate::label;
use crate::lanes::{BuildInput, ChannelColors, ChannelGeometry, Scratch, SpreadGeometry};
use crate::layout::LaneLayout;
use crate::palette::{APPROXIMATE_DELTA_EMPHASIS, ColorMode, TracePalette};
use crate::scale::{Viewport, nice_step, value_at_fraction};
use crate::scene::{
    EventMarkKind, LaneKind, LaneSeries, LaneStyles, TraceLayers, TraceScene, YRange,
};
use crate::stack::GUTTER_REMS;
use crate::state::ViewportState;
use omatrack_ui::{DeltaSense, DeltaTrend, TypeStep, format_delta, format_value};

/// The channel whose lane carries the apex callouts.
const SPEED_KEY: &str = "speed";
/// Lanes shorter than this carry no value axis (two captions would crowd
/// the trace), logical pixels.
const VALUE_AXIS_MIN_HEIGHT: f32 = 56.0;
/// Least spacing of value ticks, logical pixels.
const VALUE_TICK_SPACING: f32 = 30.0;
/// Most value ticks in one lane.
const VALUE_TICKS_MAX: usize = 5;
/// Gap between a tick label and the plot's left edge, logical pixels.
const TICK_LABEL_GAP: f32 = 6.0;
/// Radius of an apex callout's dot, logical pixels.
const APEX_DOT_RADIUS: f32 = 2.5;
/// Gap between an apex dot and its label, logical pixels.
const APEX_LABEL_GAP: f32 = 3.0;

/// Decimals of a value-axis figure: as many as its tick `step` needs.
fn axis_decimals(kind: LaneKind, step: f64) -> usize {
    if kind == LaneKind::Step || step >= 1.0 {
        0
    } else if step >= 0.1 {
        1
    } else {
        2
    }
}

/// Where a value-axis label sits.
#[derive(Clone, Debug, PartialEq)]
pub(crate) enum TickPlace {
    /// At a value (raw units), with a dotted gridline.
    Value(f64),
    /// Pinned to the lane's top or bottom edge, no gridline (steering's
    /// `L` / `R`).
    Top,
    Bottom,
}

/// The value ticks of a lane `height` px tall showing `range`: round values
/// in display units (Δ signed with its decimals, `0` bare; gear whole
/// gears), or, for steering, the direction letters at the lane's edges.
/// Nothing below [`VALUE_AXIS_MIN_HEIGHT`]. `out` is reused.
pub(crate) fn value_ticks(
    series: &LaneSeries,
    range: YRange,
    height: f32,
    out: &mut Vec<(TickPlace, SharedString)>,
) {
    out.clear();
    if height < VALUE_AXIS_MIN_HEIGHT || range.span().is_nan() || range.span() <= 0.0 {
        return;
    }
    if series.key.as_ref() == "steering" {
        // Positive steering is to the left: up the lane.
        out.push((TickPlace::Top, "L".into()));
        out.push((TickPlace::Bottom, "R".into()));
        return;
    }
    let scale = series.display_scale();
    let span = range.span() * scale;
    let count = ((height / VALUE_TICK_SPACING).floor() as usize).clamp(2, VALUE_TICKS_MAX);
    let mut step = nice_step(span / count as f64);
    if series.kind == LaneKind::Step {
        step = step.max(1.0).round();
    }
    let decimals = axis_decimals(series.kind, step);
    let first = (range.min * scale / step).ceil() as i64;
    let last = (range.max * scale / step).floor() as i64;
    for n in first..=last.min(first + VALUE_TICKS_MAX as i64) {
        let value = n as f64 * step;
        let label = if n == 0 {
            SharedString::from("0")
        } else if series.kind == LaneKind::Delta {
            format_delta(Some(value), decimals.max(1), DeltaSense::LowerIsBetter).0
        } else {
            format_value(Some(value), decimals)
        };
        out.push((TickPlace::Value(value / scale), label));
    }
}

/// Counters of the static layer, for tests, benchmarks and the frame HUD.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct StaticStats {
    /// Times the static view rendered (a cached replay does not count).
    pub renders: usize,
    /// Channel geometries rebuilt (cache misses) since creation.
    pub geometry_builds: usize,
    /// Wall time of the last prepaint that rebuilt geometry, in ms.
    pub last_build_ms: f64,
    /// Vertices and paths submitted by the last paint.
    pub vertices: usize,
    pub paths: usize,
    /// Apex callouts (dots) painted by the last paint.
    pub apexes: usize,
}

/// Geometry caches and scratch buffers, shared between the view and its
/// element (the element is rebuilt every render; the cache is not).
#[derive(Default)]
pub struct StaticCache {
    channels: HashMap<SharedString, ChannelGeometry>,
    spreads: HashMap<SharedString, SpreadGeometry>,
    scratch: Scratch,
    value_ticks: Vec<(TickPlace, SharedString)>,
    stats: StaticStats,
}

impl StaticCache {
    pub fn stats(&self) -> StaticStats {
        self.stats
    }
}

/// Entity owning the static layer's inputs. Create it once per stack.
pub struct TraceStaticView {
    scene: Arc<TraceScene>,
    styles: Arc<LaneStyles>,
    layout: Arc<LaneLayout>,
    viewport: Entity<ViewportState>,
    color_mode: ColorMode,
    layers: TraceLayers,
    cache: Rc<RefCell<StaticCache>>,
    _viewport_subscription: Subscription,
}

impl TraceStaticView {
    pub fn new(
        scene: Arc<TraceScene>,
        styles: Arc<LaneStyles>,
        layout: Arc<LaneLayout>,
        viewport: Entity<ViewportState>,
        cx: &mut Context<Self>,
    ) -> Self {
        let subscription = cx.observe(&viewport, |_, _, cx| cx.notify());
        Self {
            scene,
            styles,
            layout,
            viewport,
            color_mode: ColorMode::default(),
            layers: TraceLayers::LAP,
            cache: Rc::default(),
            _viewport_subscription: subscription,
        }
    }

    pub fn set_scene(&mut self, scene: Arc<TraceScene>, cx: &mut Context<Self>) {
        if !Arc::ptr_eq(&scene, &self.scene) {
            self.scene = scene;
            cx.notify();
        }
    }

    pub fn set_styles(&mut self, styles: Arc<LaneStyles>, cx: &mut Context<Self>) {
        if *styles != *self.styles {
            self.styles = styles;
            cx.notify();
        }
    }

    pub fn set_layout(&mut self, layout: Arc<LaneLayout>, cx: &mut Context<Self>) {
        if *layout != *self.layout {
            self.layout = layout;
            cx.notify();
        }
    }

    /// Repaint in `mode`. Geometry never keys on colour, so this repaints
    /// from the cached paths without rebuilding any.
    pub fn set_color_mode(&mut self, mode: ColorMode, cx: &mut Context<Self>) {
        if self.color_mode != mode {
            self.color_mode = mode;
            cx.notify();
        }
    }

    pub fn color_mode(&self) -> ColorMode {
        self.color_mode
    }

    /// The optional layers (the trace view mode). Lane geometry is kept;
    /// spread geometry is built on first use and cached like a lane's.
    pub fn set_layers(&mut self, layers: TraceLayers, cx: &mut Context<Self>) {
        if layers != self.layers {
            self.layers = layers;
            cx.notify();
        }
    }

    pub fn layers(&self) -> TraceLayers {
        self.layers
    }

    pub fn stats(&self) -> StaticStats {
        self.cache.borrow().stats()
    }
}

impl Render for TraceStaticView {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.cache.borrow_mut().stats.renders += 1;
        let state = self.viewport.read(cx);
        StaticLayerElement {
            scene: self.scene.clone(),
            styles: self.styles.clone(),
            layout: self.layout.clone(),
            viewport: state.viewport(),
            palette: TracePalette::from_theme(cx.theme()).with_mode(self.color_mode),
            numerals: cx.theme().mono_font_family.clone(),
            layers: self.layers,
            cache: self.cache.clone(),
        }
    }
}

struct StaticLayerElement {
    scene: Arc<TraceScene>,
    styles: Arc<LaneStyles>,
    layout: Arc<LaneLayout>,
    viewport: Viewport,
    palette: TracePalette,
    /// The trace numerals' family (the theme's monospace family).
    numerals: SharedString,
    layers: TraceLayers,
    cache: Rc<RefCell<StaticCache>>,
}

impl IntoElement for StaticLayerElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

/// Alpha of the session envelope band over the lane background.
pub const SPREAD_BAND_ALPHA: f32 = 0.12;
/// Emphasis of a session lap line (the label tone over the background).
pub const SPREAD_LINE_ALPHA: f32 = 0.35;
/// Event tick: flag size and line alpha, logical pixels. The reference's
/// ticks are quieter than the primary's.
pub const EVENT_FLAG_SIZE: f32 = 5.0;
const EVENT_LINE_ALPHA: f32 = 0.55;
const EVENT_REFERENCE_EMPHASIS: f32 = 0.55;

/// The colour of an event tick: the lap's role, the reference quieter,
/// a corner note in the label tone.
pub(crate) fn event_color(palette: &TracePalette, kind: EventMarkKind, reference: bool) -> Hsla {
    match (kind, reference) {
        (EventMarkKind::Note, _) => palette.foreground,
        (_, false) => palette.primary,
        (_, true) => palette
            .background
            .blend(palette.reference.opacity(EVENT_REFERENCE_EMPHASIS)),
    }
}

/// Ticks of the events on lane `key`: a thin line through the lane and a
/// small flag at its top (a downshift's at its foot, where gear falls).
#[allow(clippy::too_many_arguments)]
fn paint_event_ticks(
    scene: &TraceScene,
    key: &str,
    viewport: &Viewport,
    origin: gpui_kit::Point<Pixels>,
    width: f32,
    height: f32,
    palette: &TracePalette,
    window: &mut Window,
) {
    let events = scene.events();
    let from = events.partition_point(|mark| mark.fraction < viewport.start);
    let flag = EVENT_FLAG_SIZE.min(height * 0.25);
    for mark in &events[from..] {
        if mark.fraction > viewport.end {
            break;
        }
        if mark.channel.as_ref() != key {
            continue;
        }
        let x = viewport
            .x_for_fraction(mark.fraction, 0.0, width as f64)
            .round() as f32;
        if x < 0.0 || x > width {
            continue;
        }
        let color = event_color(palette, mark.kind, mark.reference);
        let emphasis = if mark.reference {
            EVENT_REFERENCE_EMPHASIS
        } else {
            1.0
        };
        window.paint_quad(fill(
            Bounds::new(
                origin + point(px(x), px(1.)),
                size(px(1.), px((height - 2.0).max(0.0))),
            ),
            palette
                .background
                .blend(color.opacity(EVENT_LINE_ALPHA * emphasis)),
        ));
        let top = if mark.kind == EventMarkKind::Downshift {
            height - 1.0 - flag
        } else {
            1.0
        };
        window.paint_quad(fill(
            Bounds::new(
                origin + point(px(x - flag * 0.5 + 0.5), px(top)),
                size(px(flag), px(flag)),
            ),
            color,
        ));
    }
}

/// The plot inside the element's bounds: right of the value-axis gutter.
fn plot_of(bounds: Bounds<Pixels>, window: &Window) -> Bounds<Pixels> {
    let gutter = (window.rem_size() * GUTTER_REMS).min(bounds.size.width);
    Bounds::new(
        bounds.origin + point(gutter, px(0.)),
        size(bounds.size.width - gutter, bounds.size.height),
    )
}

/// The vertical extent of the pinned (`true`) or scrolling region.
fn region_of(layout: &LaneLayout, pinned: bool, height: f32) -> (f32, f32) {
    if pinned {
        (0.0, layout.pinned_height as f32)
    } else {
        (layout.pinned_height as f32, height)
    }
}

impl Element for StaticLayerElement {
    type RequestLayoutState = ();
    type PrepaintState = ();

    fn id(&self) -> Option<ElementId> {
        Some("trace-static-layer".into())
    }

    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }

    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        let mut style = Style::default();
        style.size.width = relative(1.).into();
        style.size.height = relative(1.).into();
        (window.request_layout(style, [], cx), ())
    }

    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        _: &mut App,
    ) {
        let started = Instant::now();
        let mut cache = self.cache.borrow_mut();
        let cache = &mut *cache;
        let dpr = window.scale_factor();
        let plot = plot_of(bounds, window);
        let width = plot.size.width.as_f32();
        let height = plot.size.height.as_f32();
        let mut rebuilt = 0;
        for slot in &self.layout.slots {
            let (top, bottom) = region_of(&self.layout, slot.pinned, height);
            if slot.y as f32 + slot.height as f32 <= top || slot.y as f32 >= bottom {
                continue;
            }
            for index in slot.channels() {
                let Some(series) = self.scene.lanes.get(index) else {
                    continue;
                };
                let style = self.styles.get(&series.key);
                let input = BuildInput::new(
                    &self.scene,
                    series,
                    self.viewport,
                    width,
                    slot.height as f32,
                )
                .with_dpr(dpr)
                .with_stroke_width(style.stroke_width)
                .with_fill(style.fill_for(series.kind, &series.key) > 0.0);
                let geometry = cache.channels.entry(series.key.clone()).or_default();
                if geometry.prepare(&input, &mut cache.scratch) {
                    rebuilt += 1;
                }
                if self.layers.consistency && series.spread.is_some() {
                    let spread = cache.spreads.entry(series.key.clone()).or_default();
                    if spread.prepare(&input, &mut cache.scratch) {
                        rebuilt += 1;
                    }
                }
            }
        }
        if rebuilt > 0 {
            cache.stats.geometry_builds += rebuilt;
            cache.stats.last_build_ms = started.elapsed().as_secs_f64() * 1e3;
        }
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        let mut cache = self.cache.borrow_mut();
        let cache = &mut *cache;
        let tick_size = TypeStep::Caption.size(window);
        let tick_height = tick_size * 1.2;
        let palette = &self.palette;
        let plot = plot_of(bounds, window);
        let gutter = (plot.origin.x - bounds.origin.x).as_f32();
        let origin = plot.origin;
        let width = plot.size.width.as_f32();
        let height = plot.size.height.as_f32();
        let x_for = |fraction: f64| -> f32 {
            self.viewport.x_for_fraction(fraction, 0.0, width as f64) as f32
        };
        // Horizontal rules in plot coordinates (`left` may reach into the
        // gutter: a negative x).
        let hline = |window: &mut Window, y: f32, left: f32, right: f32, color: Hsla| {
            window.paint_quad(fill(
                Bounds::new(
                    origin + point(px(left), px(y)),
                    size(px((right - left).max(0.0)), px(1.)),
                ),
                color,
            ));
        };
        // One dashed-border quad per gridline: a single primitive, so the
        // cached layer's replay stays as cheap as a solid grid. The quad is
        // two pixels tall with only its top edge drawn: the quad shader picks
        // the bottom edge for a pixel centred on a 1 px quad's midline.
        let dotted = |window: &mut Window, y: f32, color: Hsla| {
            window.paint_quad(quad(
                Bounds::new(origin + point(px(0.), px(y)), size(px(width), px(2.))),
                px(0.),
                transparent_black(),
                Edges {
                    top: px(1.),
                    ..Edges::default()
                },
                color,
                BorderStyle::Dashed,
            ));
        };

        let mut vertices = 0;
        let mut paths = 0;
        let mut apexes = 0;
        for pinned in [true, false] {
            let (top, bottom) = region_of(&self.layout, pinned, height);
            if bottom <= top {
                continue;
            }
            // The region spans the gutter too: tick labels clip with it.
            let mask = ContentMask {
                bounds: Bounds::new(
                    bounds.origin + point(px(0.), px(top)),
                    size(bounds.size.width, px(bottom - top)),
                ),
            };
            window.with_content_mask(Some(mask), |window| {
                for (slot_ix, slot) in self.layout.slots.iter().enumerate() {
                    if slot.pinned != pinned {
                        continue;
                    }
                    let y = slot.y as f32;
                    let h = slot.height as f32;
                    if y + h <= top || y >= bottom {
                        continue;
                    }
                    let Some(root) = self.scene.lanes.get(slot.root) else {
                        continue;
                    };
                    let range = root.range_in(self.viewport);
                    let y_of = |value: f64| -> f32 {
                        y + 1.0 + (h - 2.0) * ((range.max - value) / range.span()) as f32
                    };
                    // The value axis: dotted gridlines at round values,
                    // their labels right-aligned in the gutter.
                    value_ticks(root, range, h, &mut cache.value_ticks);
                    let lh = tick_height.as_f32();
                    let mut zero_drawn = false;
                    for (place, text) in &cache.value_ticks {
                        let (label_top, grid) = match place {
                            TickPlace::Value(value) => {
                                let ty = y_of(*value).round();
                                (
                                    (ty - lh * 0.5).clamp(y + 1.0, (y + h - 1.0 - lh).max(y + 1.0)),
                                    Some((ty, *value == 0.0)),
                                )
                            }
                            TickPlace::Top => (y + 2.0, None),
                            TickPlace::Bottom => (y + h - 2.0 - lh, None),
                        };
                        if let Some((ty, zero)) = grid {
                            zero_drawn |= zero;
                            // Δ's zero is the reference lap: solid.
                            if zero && root.kind == LaneKind::Delta {
                                hline(window, ty, 0.0, width, palette.zero);
                            } else {
                                dotted(window, ty, palette.grid_strong);
                            }
                        }
                        let line = label::shape_numerals(
                            text.clone(),
                            &self.numerals,
                            tick_size,
                            &[(text.len(), palette.label)],
                            window,
                        );
                        let x = -TICK_LABEL_GAP - line.width.as_f32();
                        label::paint(
                            &line,
                            origin + point(px(x), px(label_top)),
                            tick_height,
                            window,
                            cx,
                        );
                    }
                    // Zero where the range crosses it without a tick: solid
                    // for Δ (level with the reference lap), dotted for a
                    // channel (steering's centre).
                    if !zero_drawn && range.min < 0.0 && range.max > 0.0 {
                        let zy = y_of(0.0).round();
                        if root.kind == LaneKind::Delta {
                            hline(window, zy, 0.0, width, palette.zero);
                        } else {
                            dotted(window, zy, palette.grid_strong);
                        }
                    }
                    for (position, index) in slot.channels().enumerate() {
                        let Some(series) = self.scene.lanes.get(index) else {
                            continue;
                        };
                        let Some(geometry) = cache.channels.get(&series.key) else {
                            continue;
                        };
                        let style = self.styles.get(&series.key);
                        let (primary, reference) =
                            palette.channel_colors(&series.key, position == 0, &style);
                        // The session behind the primary: its envelope as a
                        // low-alpha band, each lap a thin quiet line.
                        if self.layers.consistency
                            && series.spread.is_some()
                            && let Some(spread) = cache.spreads.get(&series.key)
                        {
                            spread.paint(
                                origin + point(px(0.), px(y)),
                                primary.opacity(SPREAD_BAND_ALPHA),
                                palette
                                    .background
                                    .blend(palette.label.opacity(SPREAD_LINE_ALPHA)),
                                window,
                            );
                            vertices += spread.vertex_count();
                            paths += spread.path_count();
                        }
                        let mut colors = ChannelColors::new(primary, reference);
                        colors.fill_alpha = style.fill_for(series.kind, &series.key);
                        colors.neighbour = palette.background.blend(primary.opacity(0.5));
                        if series.kind == LaneKind::Delta {
                            // An approximate Δ keeps its gain/loss reading
                            // at reduced emphasis.
                            let emphasis = if self.scene.approximate_delta {
                                APPROXIMATE_DELTA_EMPHASIS
                            } else {
                                1.0
                            };
                            let loss = palette.loss.opacity(emphasis);
                            let gain = palette.gain.opacity(emphasis);
                            // The Δ line in its sign: loss above zero, gain
                            // below it.
                            colors.primary = palette.background.blend(loss);
                            colors.primary_below = Some(palette.background.blend(gain));
                            colors.fill_above = loss;
                            colors.fill_below = gain;
                        }
                        geometry.paint(origin + point(px(0.), px(y)), &colors, window);
                        vertices += geometry.vertex_count();
                        paths += geometry.path_count();
                        if series.key.as_ref() == SPEED_KEY && self.layers.apexes {
                            apexes +=
                                self.paint_apexes(series, range, (y, h), primary, plot, window, cx);
                        }
                    }
                    if self.layers.events {
                        for index in slot.channels() {
                            if let Some(series) = self.scene.lanes.get(index) {
                                paint_event_ticks(
                                    &self.scene,
                                    &series.key,
                                    &self.viewport,
                                    origin + point(px(0.), px(y)),
                                    width,
                                    h,
                                    palette,
                                    window,
                                );
                            }
                        }
                    }
                    // Lane separator (owned by the lower lane), across the
                    // gutter and the plot.
                    if slot_ix > 0 {
                        hline(window, y.round(), -gutter, width, palette.grid_strong);
                    }
                }
            });
        }

        // Past the lap edges: the neighbouring lap, masked, and a lap edge.
        let lap_start = x_for(0.0);
        let lap_end = x_for(1.0);
        if lap_start > 0.0 {
            let right = lap_start.min(width);
            window.paint_quad(fill(
                Bounds::new(origin, size(px(right), px(height))),
                palette.mask,
            ));
            if lap_start < width {
                window.paint_quad(fill(
                    Bounds::new(
                        origin + point(px(lap_start - 1.0), px(0.)),
                        size(px(2.), px(height)),
                    ),
                    palette.label,
                ));
            }
        }
        if lap_end < width {
            let left = lap_end.max(0.0);
            window.paint_quad(fill(
                Bounds::new(
                    origin + point(px(left), px(0.)),
                    size(px(width - left), px(height)),
                ),
                palette.mask,
            ));
            if lap_end > 0.0 {
                window.paint_quad(fill(
                    Bounds::new(
                        origin + point(px(lap_end - 1.0), px(0.)),
                        size(px(2.), px(height)),
                    ),
                    palette.label,
                ));
            }
        }
        cache.stats.vertices = vertices;
        cache.stats.paths = paths;
        cache.stats.apexes = apexes;
    }
}

impl StaticLayerElement {
    /// The speed lane's apex callouts: a dot on the primary's curve at each
    /// corner's minimum and, beneath it, the minimum and its difference to
    /// the reference's (`66 −1`, gain/loss coloured). A label that would
    /// run into its left neighbour's is left out; its dot stays. Returns
    /// the dots painted.
    #[allow(clippy::too_many_arguments)]
    fn paint_apexes(
        &self,
        series: &LaneSeries,
        range: YRange,
        (y, h): (f32, f32),
        color: Hsla,
        plot: Bounds<Pixels>,
        window: &mut Window,
        cx: &mut App,
    ) -> usize {
        let palette = &self.palette;
        let width = plot.size.width.as_f32();
        let size_px = TypeStep::Caption.size(window);
        let line_height = size_px * 1.2;
        let lh = line_height.as_f32();
        let mut previous_right = f32::NEG_INFINITY;
        let mut painted = 0;
        for apex in &self.scene.apexes {
            let x = self
                .viewport
                .x_for_fraction(apex.fraction, 0.0, width as f64) as f32;
            if !(0.0..=width).contains(&x) {
                continue;
            }
            let value = value_at_fraction(&series.primary, apex.fraction);
            if !value.is_finite() || range.span() <= 0.0 {
                continue;
            }
            let t = ((value - range.min) / range.span()).clamp(0.0, 1.0) as f32;
            let dot_y = y + 1.0 + (h - 2.0) * (1.0 - t);
            let r = APEX_DOT_RADIUS;
            window.paint_quad(
                fill(
                    Bounds::new(
                        plot.origin + point(px(x - r), px(dot_y - r)),
                        size(px(r * 2.0), px(r * 2.0)),
                    ),
                    color,
                )
                .corner_radii(px(r))
                .border_widths(px(1.))
                .border_color(palette.background),
            );
            painted += 1;
            let speed = format_value(Some(apex.speed), 0);
            let (text, split, tone) = match apex.reference_speed {
                Some(reference) => {
                    let (delta, trend) = format_delta(
                        Some(apex.speed.round() - reference.round()),
                        0,
                        DeltaSense::HigherIsBetter,
                    );
                    let tone = match trend {
                        DeltaTrend::Gain => palette.gain,
                        DeltaTrend::Loss => palette.loss,
                        DeltaTrend::Even => palette.label,
                    };
                    let split = speed.len() + 1;
                    (SharedString::from(format!("{speed} {delta}")), split, tone)
                }
                None => {
                    let split = speed.len();
                    (speed, split, palette.foreground)
                }
            };
            let line = label::shape_numerals(
                text,
                &self.numerals,
                size_px,
                &[(split, palette.foreground), (0, tone)],
                window,
            );
            let label_width = line.width.as_f32();
            let left = (x - label_width * 0.5).clamp(0.0, (width - label_width).max(0.0));
            if left < previous_right + APEX_LABEL_GAP {
                continue;
            }
            // Beneath the dot, or above it where the lane ends first.
            let below = dot_y + r + APEX_LABEL_GAP;
            let label_top = if below + lh <= y + h - 1.0 {
                below
            } else {
                (dot_y - r - APEX_LABEL_GAP - lh).max(y + 1.0)
            };
            label::paint(
                &line,
                plot.origin + point(px(left), px(label_top)),
                line_height,
                window,
                cx,
            );
            previous_right = left + label_width;
        }
        painted
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn series(key: &str, kind: LaneKind, unit: &str, values: &[f64]) -> LaneSeries {
        LaneSeries::new(key, key, kind, values.to_vec().into()).with_unit(unit)
    }

    fn labels(series: &LaneSeries, range: YRange, height: f32) -> Vec<String> {
        let mut out = Vec::new();
        value_ticks(series, range, height, &mut out);
        out.into_iter().map(|(_, text)| text.to_string()).collect()
    }

    #[test]
    fn value_ticks_read_round_values_per_lane() {
        let gear = series("gear", LaneKind::Step, "", &[1.0, 6.0]);
        assert_eq!(labels(&gear, YRange::new(0.5, 6.5), 90.0), ["2", "4", "6"]);
        let delta = series("delta", LaneKind::Delta, "s", &[0.0, 1.0]);
        assert_eq!(
            labels(&delta, YRange::new(-0.1, 1.1), 90.0),
            ["0", "+0.5", "+1.0"]
        );
        let steering = series("steering", LaneKind::Line, "°", &[-90.0, 90.0]);
        assert_eq!(
            labels(&steering, YRange::new(-90.0, 90.0), 90.0),
            ["L", "R"]
        );
        // Short lanes carry no axis.
        assert!(labels(&gear, YRange::new(0.5, 6.5), 40.0).is_empty());
    }
}
