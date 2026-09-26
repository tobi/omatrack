//! The static trace layer: grids, lane separators, masks and every lane's
//! paths, drawn by one custom element inside [`TraceStaticView`].
//!
//! The owning [`crate::TraceStack`] embeds this view through
//! `Entity::cached`, so GPUI replays the previous frame's primitives until
//! the view is notified. It is notified only on scene, viewport, layout or
//! style change; it never reads the cursor, so a cursor move cannot rebuild
//! it. Theme changes refresh the window (and so repaint this layer), but the
//! per-channel geometry cache never keys on colour, so a theme change only
//! recolours.

use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::Arc;
use std::time::Instant;

use gpui_kit::component::ActiveTheme as _;
use gpui_kit::{
    App, Bounds, ContentMask, Context, Element, ElementId, Entity, FontWeight, GlobalElementId,
    Hsla, InspectorElementId, IntoElement, LayoutId, Pixels, Render, SharedString, Style,
    Subscription, Window, fill, point, px, relative, size,
};

use crate::label;
use crate::lanes::{BuildInput, ChannelColors, ChannelGeometry, Scratch};
use crate::layout::LaneLayout;
use crate::palette::TracePalette;
use crate::scale::{Tick, Viewport, XAxis, axis_ticks, nice_step};
use crate::scene::{LaneKind, LaneStyles, TraceScene};
use crate::state::ViewportState;
use omatrack_ui::{TypeStep, format_value};

/// Lanes shorter than this carry no value axis (two captions would crowd
/// the trace), logical pixels.
const VALUE_AXIS_MIN_HEIGHT: f32 = 56.0;

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

/// Minimum spacing of axis ticks (and vertical grid lines), logical pixels.
pub const TICK_SPACING: f64 = 96.0;

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
}

/// Geometry caches and scratch buffers, shared between the view and its
/// element (the element is rebuilt every render; the cache is not).
#[derive(Default)]
pub struct StaticCache {
    channels: HashMap<SharedString, ChannelGeometry>,
    scratch: Scratch,
    ticks: Vec<Tick>,
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
            axis: state.axis(),
            palette: TracePalette::from_theme(cx.theme()),
            cache: self.cache.clone(),
        }
    }
}

struct StaticLayerElement {
    scene: Arc<TraceScene>,
    styles: Arc<LaneStyles>,
    layout: Arc<LaneLayout>,
    viewport: Viewport,
    axis: XAxis,
    palette: TracePalette,
    cache: Rc<RefCell<StaticCache>>,
}

impl IntoElement for StaticLayerElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

/// Whether a lane rectangle is visible inside its region.
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
        let width = bounds.size.width.as_f32();
        let height = bounds.size.height.as_f32();
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
            }
        }
        axis_ticks(
            self.axis,
            &self.viewport,
            match self.axis {
                XAxis::Distance => &self.scene.distance_m,
                XAxis::Time => &self.scene.time_s,
            },
            width as f64,
            TICK_SPACING,
            &mut cache.ticks,
        );
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
        let cache = self.cache.borrow();
        let tick_size = TypeStep::Caption.size(window);
        let tick_height = tick_size * 1.2;
        let palette = &self.palette;
        let width = bounds.size.width.as_f32();
        let height = bounds.size.height.as_f32();
        let x_for = |fraction: f64| -> f32 {
            self.viewport.x_for_fraction(fraction, 0.0, width as f64) as f32
        };
        let hline = |window: &mut Window, y: f32, left: f32, right: f32, color: Hsla| {
            window.paint_quad(fill(
                Bounds::new(
                    bounds.origin + point(px(left), px(y)),
                    size(px((right - left).max(0.0)), px(1.)),
                ),
                color,
            ));
        };

        // Vertical grid at the axis ticks, under everything.
        for tick in &cache.ticks {
            let x = x_for(tick.fraction).round();
            if x <= 0.0 || x >= width {
                continue;
            }
            window.paint_quad(fill(
                Bounds::new(
                    bounds.origin + point(px(x), px(0.)),
                    size(px(1.), px(height)),
                ),
                palette.grid.opacity(0.5),
            ));
        }

        let mut vertices = 0;
        let mut paths = 0;
        for pinned in [true, false] {
            let (top, bottom) = region_of(&self.layout, pinned, height);
            if bottom <= top {
                continue;
            }
            let mask = ContentMask {
                bounds: Bounds::new(
                    bounds.origin + point(px(0.), px(top)),
                    size(px(width), px(bottom - top)),
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
                    // Quarter grid of the root channel.
                    for quarter in 1..4 {
                        let gy = (y + 1.0 + (h - 2.0) * quarter as f32 / 4.0).round();
                        hline(window, gy, 0.0, width, palette.grid.opacity(0.45));
                    }
                    // Zero line where the range crosses zero: solid for Δ
                    // (it is the lane's reference: "level with the
                    // reference lap"), dashed for other channels.
                    let range = root.range_in(self.viewport);
                    if range.min < 0.0 && range.max > 0.0 {
                        let zy = (y + 1.0 + (h - 2.0) * (range.max / range.span()) as f32).round();
                        if root.kind == LaneKind::Delta {
                            hline(window, zy, 0.0, width, palette.zero);
                        } else {
                            let mut x = 0.0;
                            while x < width {
                                hline(window, zy, x, (x + 4.0).min(width), palette.grid);
                                x += 8.0;
                            }
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
                        let delta = series.kind == LaneKind::Delta;
                        // An approximate Δ is not a gain/loss verdict.
                        let (loss, gain) = if self.scene.approximate_delta {
                            (palette.label, palette.label)
                        } else {
                            (palette.loss, palette.gain)
                        };
                        let colors = ChannelColors {
                            primary: if delta { palette.delta_line } else { primary },
                            reference,
                            fill_above: if delta { loss } else { primary },
                            fill_below: if delta { gain } else { primary },
                            fill_alpha: style.fill_for(series.kind, &series.key),
                            neighbour: palette.background.blend(primary.opacity(0.5)),
                        };
                        geometry.paint(bounds.origin + point(px(0.), px(y)), &colors, window);
                        vertices += geometry.vertex_count();
                        paths += geometry.path_count();
                    }
                    // The value axis: two or three round values of the
                    // lane's range, right-aligned inside the plot at their
                    // own heights, where the lane is tall enough.
                    if h >= VALUE_AXIS_MIN_HEIGHT && range.span() > 0.0 {
                        let scale = root.display_scale();
                        let span = range.span() * scale;
                        let step = nice_step(span / 3.0);
                        let decimals = axis_decimals(root.kind, step);
                        let lh = tick_height.as_f32();
                        let first = (range.min * scale / step).ceil() as i64;
                        let last = (range.max * scale / step).floor() as i64;
                        for n in first..=last.min(first + 4) {
                            let value = n as f64 * step;
                            let ty = y
                                + 1.0
                                + (h - 2.0) * ((range.max - value / scale) / range.span()) as f32;
                            let top = (ty - lh * 0.5).clamp(y + 1.0, y + h - 1.0 - lh);
                            let line = label::shape(
                                format_value(Some(value), decimals),
                                tick_size,
                                FontWeight::NORMAL,
                                palette.label,
                                window,
                            );
                            let x = width - line.width.as_f32() - 6.0;
                            label::paint(
                                &line,
                                bounds.origin + point(px(x), px(top)),
                                tick_height,
                                window,
                                cx,
                            );
                        }
                    }
                    // Lane separator (owned by the lower lane).
                    if slot_ix > 0 {
                        hline(window, y.round(), 0.0, width, palette.grid_strong);
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
                Bounds::new(bounds.origin, size(px(right), px(height))),
                palette.mask,
            ));
            if lap_start < width {
                window.paint_quad(fill(
                    Bounds::new(
                        bounds.origin + point(px(lap_start - 1.0), px(0.)),
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
                    bounds.origin + point(px(left), px(0.)),
                    size(px(width - left), px(height)),
                ),
                palette.mask,
            ));
            if lap_end > 0.0 {
                window.paint_quad(fill(
                    Bounds::new(
                        bounds.origin + point(px(lap_end - 1.0), px(0.)),
                        size(px(2.), px(height)),
                    ),
                    palette.label,
                ));
            }
        }
        drop(cache);
        let mut cache = self.cache.borrow_mut();
        cache.stats.vertices = vertices;
        cache.stats.paths = paths;
    }
}
