//! Lane plots: decimated series built into GPUI paths, cached per channel.
//!
//! This follows gpui-component's `Plot` idiom (build geometry once per shape
//! key, relative to a zero origin, and move it to the frame's origin at
//! paint), with three deliberate differences measured against the
//! full-session 50 Hz workload:
//!
//! - Geometry is never built through `PathBuilder` or `shape::Line/Area`:
//!   those skip non-finite points instead of lifting the pen and tessellate
//!   through lyon with 16-bit indices. Triangles from [`crate::mesh`] are
//!   written straight into `Path::vertices`.
//! - The cache key is the inputs (scene generation, viewport, lane size,
//!   dpr, stroke width, fill), not a hash of projected points, and it never
//!   includes a colour: a theme change repaints without rebuilding. The
//!   generation comes from the [`TraceScene`] itself, which allocates a new
//!   one for any new data, so a key cannot survive a data change.

//! - Paths keep their vertex capacity between rebuilds, so a zoom sweep
//!   allocates nothing after warm-up. (`PathCache::get` replaces its path on
//!   every rebuild.)
//!
//! Paint order within a lane is primary area → reference outline → primary
//! outline, so a strong fill never buries the reference.

use std::hash::{DefaultHasher, Hash, Hasher};

use gpui_kit::{
    Background, Bounds, ContentMask, Hsla, Path, PathVertex, Pixels, Point, Window,
    linear_color_stop, linear_gradient, point, px,
};

use crate::decimate::{DecimateParams, PathPoint, PlotRect, decimate, device_columns};
use crate::mesh::{BandColumn, TriangleSink, fill_band, fill_to_baseline, stroke};
use crate::scale::Viewport;
use crate::scene::{FractionMap, LaneKind, LaneSeries, LaneStyle, TraceScene, YRange};

/// Vertices per path chunk (1024 triangles, 96 KiB).
///
/// `Window::paint_path` copies a path twice (device scale, scene insert); chunks below
/// the allocator's mmap threshold recycle heap memory instead of page-faulting fresh
/// mappings on every copy, which dominated full-lap frames.
pub const CHUNK_VERTICES: usize = 3 * 1024;

/// A GPUI path being filled with triangles, relative to a zero origin, in
/// chunks of at most [`CHUNK_VERTICES`] vertices.
#[derive(Default)]
pub struct PathBuffer {
    chunks: Vec<Path<Pixels>>,
    bounds: Vec<([f32; 2], [f32; 2])>,
    used: usize,
    /// Forced vertical extent (a gradient spans each chunk's bounds).
    extent: Option<(f32, f32)>,
}

const EMPTY_BOUNDS: ([f32; 2], [f32; 2]) = ([f32::INFINITY; 2], [f32::NEG_INFINITY; 2]);

impl PathBuffer {
    /// Drop the triangles, keeping the allocations.
    pub fn clear(&mut self) {
        for chunk in &mut self.chunks[..self.used] {
            chunk.vertices.clear();
        }
        self.used = 0;
        self.extent = None;
    }

    pub fn is_empty(&self) -> bool {
        self.used == 0
    }

    pub fn vertex_count(&self) -> usize {
        self.chunks[..self.used]
            .iter()
            .map(|c| c.vertices.len())
            .sum()
    }

    /// Paths in use (one `paint_path` each).
    pub fn chunks(&self) -> &[Path<Pixels>] {
        &self.chunks[..self.used]
    }

    /// Force the vertical extent of every chunk's bounds (a gradient spans
    /// the path bounds, so this pins where it starts and ends).
    pub fn include_vertical(&mut self, top: f32, bottom: f32) {
        self.extent = Some(match self.extent {
            Some((t, b)) => (t.min(top), b.max(bottom)),
            None => (top.min(bottom), top.max(bottom)),
        });
    }

    /// Seal each chunk's bounds from its vertices. Call once after the last
    /// triangle: GPUI clips a path to its bounds, so an unsealed buffer
    /// paints nothing.
    pub fn finish(&mut self) {
        for (chunk, (min, max)) in self.chunks[..self.used].iter_mut().zip(&self.bounds) {
            let (mut top, mut bottom) = (min[1], max[1]);
            if let Some((t, b)) = self.extent {
                top = top.min(t);
                bottom = bottom.max(b);
            }
            if min[0].is_finite() {
                chunk.bounds =
                    Bounds::from_corners(point(px(min[0]), px(top)), point(px(max[0]), px(bottom)));
            }
        }
    }

    /// Copies moved to `origin`, ready for `Window::paint_path` (which takes
    /// a path by value).
    pub fn translated(&self, origin: Point<Pixels>) -> impl Iterator<Item = Path<Pixels>> + '_ {
        self.chunks().iter().map(move |chunk| {
            let mut path = chunk.clone();
            path.bounds.origin += origin;
            for vertex in &mut path.vertices {
                vertex.xy_position += origin;
            }
            path
        })
    }

    fn paint(
        &self,
        origin: Point<Pixels>,
        color: impl Into<Background> + Copy,
        window: &mut Window,
    ) {
        for path in self.translated(origin) {
            window.paint_path(path, color);
        }
    }

    #[inline]
    fn chunk(&mut self) -> usize {
        let full = self.used == 0 || self.chunks[self.used - 1].vertices.len() + 3 > CHUNK_VERTICES;
        if full {
            if self.used == self.chunks.len() {
                let mut path = Path::new(point(px(0.), px(0.)));
                path.vertices.reserve(CHUNK_VERTICES);
                self.chunks.push(path);
                self.bounds.push(EMPTY_BOUNDS);
            }
            self.bounds[self.used] = EMPTY_BOUNDS;
            self.used += 1;
        }
        self.used - 1
    }
}

impl TriangleSink for PathBuffer {
    #[inline]
    fn triangle(&mut self, a: [f32; 2], b: [f32; 2], c: [f32; 2]) {
        let ix = self.chunk();
        let (min, max) = &mut self.bounds[ix];
        let vertices = &mut self.chunks[ix].vertices;
        for v in [a, b, c] {
            *min = [min[0].min(v[0]), min[1].min(v[1])];
            *max = [max[0].max(v[0]), max[1].max(v[1])];
            vertices.push(PathVertex {
                xy_position: point(px(v[0]), px(v[1])),
                // Interior coverage: the rasterizer paints the triangle solid.
                st_position: point(0., 1.),
                content_mask: ContentMask::default(),
            });
        }
    }
}

/// Reusable decimation buffers shared by every lane of a frame.
#[derive(Default)]
pub struct Scratch {
    points: Vec<PathPoint>,
    step: Vec<PathPoint>,
    band: Vec<BandColumn>,
}

/// Inputs that shape one channel's geometry. Never a colour.
///
/// Built from the scene the series belongs to, so the scene's generation and
/// reference map always travel together with the data.
#[derive(Clone, Copy)]
pub struct BuildInput<'a> {
    generation: u64,
    series: &'a LaneSeries,
    map: Option<&'a dyn FractionMap>,
    viewport: Viewport,
    /// The series' range in this viewport ([`LaneSeries::range_in`]).
    y_range: YRange,
    /// Lane size in logical pixels.
    width: f32,
    height: f32,
    dpr: f32,
    stroke_width: f32,
    fill: bool,
}

impl<'a> BuildInput<'a> {
    /// Geometry of `series`, one of `scene`'s lanes, over `viewport` in a
    /// lane of `width` × `height` logical pixels; DPR 1, the default stroke
    /// width, no fill.
    pub fn new(
        scene: &'a TraceScene,
        series: &'a LaneSeries,
        viewport: Viewport,
        width: f32,
        height: f32,
    ) -> Self {
        debug_assert!(
            scene.lanes().iter().any(|lane| std::ptr::eq(lane, series)),
            "the series must be one of the scene's lanes"
        );
        Self {
            generation: scene.generation(),
            series,
            map: scene.map(),
            viewport,
            y_range: series.range_in(viewport),
            width,
            height,
            dpr: 1.0,
            stroke_width: LaneStyle::default().stroke_width,
            fill: false,
        }
    }

    /// Device pixel ratio.
    #[must_use]
    pub fn with_dpr(mut self, dpr: f32) -> Self {
        self.dpr = dpr;
        self
    }

    /// Stroke width in logical pixels.
    #[must_use]
    pub fn with_stroke_width(mut self, stroke_width: f32) -> Self {
        self.stroke_width = stroke_width;
        self
    }

    /// Whether the lane fills to its baseline.
    #[must_use]
    pub fn with_fill(mut self, fill: bool) -> Self {
        self.fill = fill;
        self
    }
    fn key(&self) -> u64 {
        let mut hasher = DefaultHasher::new();
        self.generation.hash(&mut hasher);
        self.series.key.hash(&mut hasher);
        self.series.kind.hash(&mut hasher);
        self.y_range.min.to_bits().hash(&mut hasher);
        self.y_range.max.to_bits().hash(&mut hasher);
        self.viewport.start.to_bits().hash(&mut hasher);
        self.viewport.end.to_bits().hash(&mut hasher);
        self.width.to_bits().hash(&mut hasher);
        self.height.to_bits().hash(&mut hasher);
        self.dpr.to_bits().hash(&mut hasher);
        self.stroke_width.to_bits().hash(&mut hasher);
        self.fill.hash(&mut hasher);
        self.map.is_some().hash(&mut hasher);
        hasher.finish()
    }

    /// The data rectangle inside a lane (one pixel of air top and bottom).
    fn rect(&self) -> PlotRect {
        PlotRect::new(
            0.0,
            1.0,
            f64::from(self.width),
            (f64::from(self.height) - 2.0).max(1.0),
        )
    }

    fn baseline(&self) -> f64 {
        let rect = self.rect();
        let range = self.y_range;
        (rect.bottom() + range.min / range.span() * rect.height).clamp(rect.top, rect.bottom())
    }
}

/// Colours applied to one channel at paint time.
///
/// [`ChannelColors::new`] gives an unfilled line pair; set the fill and
/// neighbour fields as needed.
#[derive(Clone, Copy, Debug, Default)]
#[non_exhaustive]
pub struct ChannelColors {
    pub primary: Hsla,
    pub reference: Hsla,
    /// Fill above the baseline (Δ: loss) and below it (Δ: gain).
    pub fill_above: Hsla,
    pub fill_below: Hsla,
    /// Peak alpha of the fill gradient.
    pub fill_alpha: f32,
    pub neighbour: Hsla,
    /// The primary stroke's colour below the baseline, when it differs
    /// from above it (the Δ line: loss above zero, gain below). The one
    /// stroke is painted twice under complementary content masks, so the
    /// sign split costs no geometry.
    pub primary_below: Option<Hsla>,
}

impl ChannelColors {
    /// Primary and reference strokes; fills take the primary colour at zero
    /// alpha and neighbouring laps the reference colour.
    pub fn new(primary: Hsla, reference: Hsla) -> Self {
        Self {
            primary,
            reference,
            fill_above: primary,
            fill_below: primary,
            fill_alpha: 0.0,
            neighbour: reference,
            primary_below: None,
        }
    }
}

/// Cached geometry of one channel.
#[derive(Default)]
pub struct ChannelGeometry {
    key: Option<u64>,
    fill_above: PathBuffer,
    fill_below: PathBuffer,
    reference: PathBuffer,
    primary: PathBuffer,
    neighbours: PathBuffer,
    /// The baseline's y inside the lane (zero, or the range's floor).
    baseline: f32,
}

impl ChannelGeometry {
    /// Rebuild when the inputs changed; true when geometry was rebuilt.
    pub fn prepare(&mut self, input: &BuildInput<'_>, scratch: &mut Scratch) -> bool {
        let key = input.key();
        if self.key == Some(key) {
            return false;
        }
        self.key = Some(key);
        self.build(input, scratch);
        true
    }

    /// Forget the cached key (next `prepare` rebuilds).
    pub fn invalidate(&mut self) {
        self.key = None;
    }

    pub fn vertex_count(&self) -> usize {
        self.fill_above.vertex_count()
            + self.fill_below.vertex_count()
            + self.reference.vertex_count()
            + self.primary.vertex_count()
            + self.neighbours.vertex_count()
    }

    /// Number of path submissions (chunks) one paint makes.
    pub fn path_count(&self) -> usize {
        self.buffers().iter().map(|b| b.chunks().len()).sum()
    }

    #[expect(
        clippy::cast_possible_truncation,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    #[expect(
        clippy::too_many_lines,
        reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
    )]
    fn build(&mut self, input: &BuildInput<'_>, scratch: &mut Scratch) {
        for buffer in [
            &mut self.fill_above,
            &mut self.fill_below,
            &mut self.reference,
            &mut self.primary,
            &mut self.neighbours,
        ] {
            buffer.clear();
        }
        let series = input.series;
        let rect = input.rect();
        self.baseline = input.baseline() as f32;
        if series.primary.len() < 2 || rect.width < 2.0 {
            return;
        }
        let dpr = f64::from(input.dpr).max(1.0);
        let params = DecimateParams {
            x_start: input.viewport.start,
            x_span: input.viewport.span(),
            rect,
            y_min: input.y_range.min,
            y_span: input.y_range.span(),
            dpr,
            clip_low: 0.0,
            clip_high: 1.0,
        };
        let width = f64::from(input.stroke_width);
        let step = series.kind == LaneKind::Step;

        // Neighbouring laps, when the viewport runs past the lap.
        if input.viewport.start < 0.0
            && let Some(previous) = &series.previous
        {
            let p = DecimateParams {
                clip_low: -1.0,
                clip_high: 0.0,
                ..params
            };
            decimate(
                previous,
                &|f: f64| (f + 1.0).clamp(0.0, 1.0),
                &p,
                &mut scratch.points,
            );
            stroke(
                stepped(&scratch.points, step, &mut scratch.step),
                width,
                &mut self.neighbours,
            );
        }
        if input.viewport.end > 1.0
            && let Some(next) = &series.next
        {
            let p = DecimateParams {
                clip_low: 1.0,
                clip_high: 2.0,
                ..params
            };
            decimate(
                next,
                &|f: f64| (f - 1.0).clamp(0.0, 1.0),
                &p,
                &mut scratch.points,
            );
            stroke(
                stepped(&scratch.points, step, &mut scratch.step),
                width,
                &mut self.neighbours,
            );
        }

        // Reference through the shared map, evaluated per device column.
        if let Some(reference) = &series.reference
            && series.kind != LaneKind::Delta
        {
            match input.map {
                Some(map) => decimate(
                    reference,
                    &|f: f64| map.reference_fraction(f.clamp(0.0, 1.0)),
                    &params,
                    &mut scratch.points,
                ),
                None => decimate(
                    reference,
                    &|f: f64| f.clamp(0.0, 1.0),
                    &params,
                    &mut scratch.points,
                ),
            }
            stroke(
                stepped(&scratch.points, step, &mut scratch.step),
                width * REFERENCE_STROKE_SCALE,
                &mut self.reference,
            );
        }

        // Primary: decimated once, filled and stroked from the same path.
        decimate(
            &series.primary,
            &|f: f64| f.clamp(0.0, 1.0),
            &params,
            &mut scratch.points,
        );
        let points = stepped(&scratch.points, step, &mut scratch.step);
        if input.fill || series.kind == LaneKind::Delta {
            let baseline = input.baseline();
            let fade = (baseline - rect.top).max(rect.bottom() - baseline).max(1.0);
            fill_to_baseline(points, baseline, &mut self.fill_above, &mut self.fill_below);
            // The gradient spans the path bounds: pin them to the fade range
            // so alpha falls linearly to zero exactly at the baseline.
            self.fill_above
                .include_vertical((baseline - fade) as f32, baseline as f32);
            self.fill_below
                .include_vertical(baseline as f32, (baseline + fade) as f32);
        }
        stroke(points, width, &mut self.primary);

        for buffer in [
            &mut self.fill_above,
            &mut self.fill_below,
            &mut self.reference,
            &mut self.primary,
            &mut self.neighbours,
        ] {
            buffer.finish();
        }
    }

    /// Paint at `origin` (the lane's top-left corner).
    pub fn paint(&self, origin: Point<Pixels>, colors: &ChannelColors, window: &mut Window) {
        self.neighbours.paint(origin, colors.neighbour, window);
        if colors.fill_alpha > 0.0 {
            self.fill_above.paint(
                origin,
                fade(colors.fill_above, colors.fill_alpha, true),
                window,
            );
            self.fill_below.paint(
                origin,
                fade(colors.fill_below, colors.fill_alpha, false),
                window,
            );
        }
        self.reference.paint(origin, colors.reference, window);
        match colors.primary_below {
            Some(below) => {
                // Split at the baseline: the current mask intersects, so
                // each half stays inside its lane.
                let split = origin.y + px(self.baseline);
                let far = px(1e5);
                let above = Bounds::from_corners(
                    point(origin.x - far, split - far),
                    point(origin.x + far, split),
                );
                let under = Bounds::from_corners(
                    point(origin.x - far, split),
                    point(origin.x + far, split + far),
                );
                window.with_content_mask(Some(ContentMask { bounds: above }), |window| {
                    self.primary.paint(origin, colors.primary, window)
                });
                window.with_content_mask(Some(ContentMask { bounds: under }), |window| {
                    self.primary.paint(origin, below, window)
                });
            }
            None => self.primary.paint(origin, colors.primary, window),
        }
    }

    /// The baseline's y inside the lane (logical pixels from its top).
    pub fn baseline(&self) -> f32 {
        self.baseline
    }

    /// The five paths, for benchmarks: (fill above, fill below, reference,
    /// primary, neighbours).
    pub fn buffers(&self) -> [&PathBuffer; 5] {
        [
            &self.fill_above,
            &self.fill_below,
            &self.reference,
            &self.primary,
            &self.neighbours,
        ]
    }
}

/// The reference lap's stroke width as a share of the primary's: the
/// primary is the lap under study and reads first, in either colour mode.
pub const REFERENCE_STROKE_SCALE: f64 = 0.75;

/// Stroke width of one session lap line (Consistency view), logical
/// pixels: thinner than any lane stroke, context rather than data.
pub const SPREAD_LINE_WIDTH: f64 = 0.75;
/// Decimation columns per logical pixel of a session lap line: coarser
/// than a lane's device columns. A 0.75 px context line shows no
/// sub-pixel extrema, and eight laps per lane at device resolution would
/// multiply the frame's vertices several times over (trace_bench,
/// Consistency row). The envelope band keeps device resolution.
pub const SPREAD_LINE_COLUMNS_PER_PX: f64 = 0.5;
/// Columns per logical pixel of the envelope band: a low-alpha fill with no
/// edge stroke reads the same at logical resolution.
pub const SPREAD_BAND_COLUMNS_PER_PX: f64 = 1.0;

/// Cached geometry of one channel's session spread (the Consistency view):
/// the min–max band and a thin line per other lap, all on the primary grid
/// (no reference map). Keyed on the same inputs as the channel's own
/// geometry; the scene generation covers the spread data, and a colour
/// never enters the key.
#[derive(Default)]
pub struct SpreadGeometry {
    key: Option<u64>,
    band: PathBuffer,
    lines: PathBuffer,
}

impl SpreadGeometry {
    /// Rebuild when the inputs changed; true when geometry was rebuilt.
    pub fn prepare(&mut self, input: &BuildInput<'_>, scratch: &mut Scratch) -> bool {
        let key = input.key();
        if self.key == Some(key) {
            return false;
        }
        self.key = Some(key);
        self.build(input, scratch);
        true
    }

    pub fn vertex_count(&self) -> usize {
        self.band.vertex_count() + self.lines.vertex_count()
    }

    pub fn path_count(&self) -> usize {
        self.band.chunks().len() + self.lines.chunks().len()
    }

    /// (band, lines), for benchmarks.
    pub fn buffers(&self) -> [&PathBuffer; 2] {
        [&self.band, &self.lines]
    }

    fn build(&mut self, input: &BuildInput<'_>, scratch: &mut Scratch) {
        self.band.clear();
        self.lines.clear();
        let series = input.series;
        let Some(spread) = series.spread.as_deref() else {
            return;
        };
        let rect = input.rect();
        if rect.width < 2.0 || spread.min.len() < 2 || spread.min.len() != spread.max.len() {
            return;
        }
        let (y_min, y_span) = (input.y_range.min, input.y_range.span());
        let viewport = input.viewport;
        let y_of = |value: f64| -> f64 {
            (rect.bottom() - (value - y_min) / y_span * rect.height).clamp(rect.top, rect.bottom())
        };

        // The band: per device column, the envelope's extent across it.
        let (min, max) = (&spread.min[..], &spread.max[..]);
        let last = (min.len() - 1) as f64;
        let value_at = |values: &[f64], index: f64| -> f64 {
            let lo = index.floor() as usize;
            let t = index - lo as f64;
            if t < 1e-10 || lo as f64 >= last {
                return values[lo.min(values.len() - 1)];
            }
            values[lo] + (values[lo + 1] - values[lo]) * t
        };
        let columns = device_columns(rect.width, SPREAD_BAND_COLUMNS_PER_PX);
        let step = rect.width / columns as f64;
        let band = &mut scratch.band;
        band.clear();
        band.reserve(columns + 2);
        let mut previous: Option<f64> = None;
        for column in 0..=columns {
            let x = rect.left + column as f64 * step;
            let fraction = viewport.start + (x - rect.left) / rect.width * viewport.span();
            if !(0.0..=1.0).contains(&fraction) {
                band.push(BandColumn::GAP);
                previous = None;
                continue;
            }
            let index = fraction * last;
            let (mut low, mut high) = (value_at(min, index), value_at(max, index));
            if let Some(from) = previous {
                let first = (from.floor() as usize + 1).min(min.len());
                let end = (index.ceil() as usize).min(min.len()).max(first);
                for (a, b) in min[first..end].iter().zip(&max[first..end]) {
                    low = low.min(*a);
                    high = high.max(*b);
                }
            }
            previous = Some(index);
            if low.is_finite() && high.is_finite() {
                band.push(BandColumn {
                    x,
                    top: y_of(high),
                    bottom: y_of(low),
                });
            } else {
                band.push(BandColumn::GAP);
            }
        }
        fill_band(band, &mut self.band);

        // One quiet line per other lap, decimated like the primary.
        let params = DecimateParams {
            x_start: viewport.start,
            x_span: viewport.span(),
            rect,
            y_min,
            y_span,
            dpr: SPREAD_LINE_COLUMNS_PER_PX,
            clip_low: 0.0,
            clip_high: 1.0,
        };
        let step_kind = series.kind == LaneKind::Step;
        for lap in &spread.laps {
            decimate(
                lap,
                &|f: f64| f.clamp(0.0, 1.0),
                &params,
                &mut scratch.points,
            );
            stroke(
                stepped(&scratch.points, step_kind, &mut scratch.step),
                SPREAD_LINE_WIDTH,
                &mut self.lines,
            );
        }
        self.band.finish();
        self.lines.finish();
    }

    /// Paint at `origin` (the lane's top-left corner): band, then lines.
    pub fn paint(&self, origin: Point<Pixels>, band: Hsla, line: Hsla, window: &mut Window) {
        self.band.paint(origin, band, window);
        self.lines.paint(origin, line, window);
    }
}

/// A vertical gradient: `alpha` at the far edge, transparent at the baseline.
fn fade(color: Hsla, alpha: f32, above: bool) -> Background {
    let solid = color.opacity(alpha);
    let clear = color.opacity(0.0);
    if above {
        linear_gradient(
            180.,
            linear_color_stop(solid, 0.),
            linear_color_stop(clear, 1.),
        )
    } else {
        linear_gradient(
            180.,
            linear_color_stop(clear, 0.),
            linear_color_stop(solid, 1.),
        )
    }
}

/// Held values: insert the horizontal run before each change.
#[expect(
    clippy::float_cmp,
    reason = "Exact geometry equality identifies a zero-height segment; epsilon comparisons would discard thin detail."
)]
fn stepped<'a>(
    points: &'a [PathPoint],
    step: bool,
    out: &'a mut Vec<PathPoint>,
) -> &'a [PathPoint] {
    if !step {
        return points;
    }
    out.clear();
    for (i, point) in points.iter().enumerate() {
        if i > 0 {
            let previous = points[i - 1];
            if !previous.is_pen_up() && !point.is_pen_up() && previous.y != point.y {
                out.push(PathPoint::new(point.x, previous.y));
            }
        }
        out.push(*point);
    }
    out
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use super::*;
    use crate::scene::{LaneSeries, LaneSpread};

    fn series(kind: LaneKind) -> LaneSeries {
        let primary: Arc<[f64]> = (0..4500)
            .map(|i| (f64::from(i) * 0.01).sin() * 100.0)
            .collect();
        let reference: Arc<[f64]> = (0..4600)
            .map(|i| (f64::from(i) * 0.0098).sin() * 95.0)
            .collect();
        LaneSeries::new("speed", "Speed", kind, primary).with_reference(Some(reference))
    }

    fn scene_of(lane: LaneSeries) -> TraceScene {
        let distance: Arc<[f64]> = (0..4500).map(f64::from).collect();
        let time: Arc<[f64]> = (0..4500).map(|i| f64::from(i) / 50.0).collect();
        TraceScene::new(distance, time).with_lanes(vec![lane])
    }

    fn input(scene: &TraceScene, viewport: Viewport) -> BuildInput<'_> {
        BuildInput::new(scene, &scene.lanes()[0], viewport, 1200.0, 120.0)
            .with_dpr(2.0)
            .with_stroke_width(1.25)
            .with_fill(true)
    }

    #[test]
    fn spread_geometry_is_a_band_and_one_line_per_lap_cached_on_inputs() {
        let laps: Vec<Arc<[f64]>> = (0..3)
            .map(|k| {
                (0..4500)
                    .map(|i| ((i as f64) * 0.01).sin() * 100.0 + k as f64 * 5.0)
                    .collect()
            })
            .collect();
        let min: Arc<[f64]> = (0..4500).map(|i| laps[0][i]).collect();
        let max: Arc<[f64]> = (0..4500).map(|i| laps[2][i]).collect();
        let lane = series(LaneKind::Line)
            .with_spread(Some(Arc::new(LaneSpread::new(laps, min, max))))
            .with_y_range(YRange::new(-120.0, 120.0));
        let scene = scene_of(lane);
        let mut scratch = Scratch::default();
        let mut spread = SpreadGeometry::default();
        assert!(spread.prepare(&input(&scene, Viewport::FULL), &mut scratch));
        assert!(!spread.prepare(&input(&scene, Viewport::FULL), &mut scratch));
        let [band, lines] = spread.buffers();
        // One quad per logical column (1200 px), two triangles each.
        assert_eq!(band.vertex_count(), 1200 * 6);
        assert!(lines.vertex_count() > 3 * 6 * 100, "three lap lines");
        // Every band vertex lies inside the plot.
        for chunk in band.chunks() {
            for v in &chunk.vertices {
                let y = v.xy_position.y.as_f32();
                assert!((0.0..=120.0).contains(&y), "{y}");
            }
        }
        // Zooming rebuilds; a lane without a spread builds nothing.
        assert!(spread.prepare(&input(&scene, Viewport::new(0.2, 0.3)), &mut scratch));
        let bare = scene_of(series(LaneKind::Line));
        let mut empty = SpreadGeometry::default();
        empty.prepare(&input(&bare, Viewport::FULL), &mut scratch);
        assert_eq!(empty.vertex_count(), 0);
    }

    #[test]
    fn cache_rebuilds_only_when_inputs_change() {
        let lane = scene_of(series(LaneKind::Area));
        let mut geometry = ChannelGeometry::default();
        let mut scratch = Scratch::default();
        assert!(geometry.prepare(&input(&lane, Viewport::FULL), &mut scratch));
        assert!(!geometry.prepare(&input(&lane, Viewport::FULL), &mut scratch));
        assert!(geometry.prepare(&input(&lane, Viewport::new(0.1, 0.2)), &mut scratch));
        assert!(geometry.vertex_count() > 0);
        assert!(
            geometry.path_count() >= 4,
            "fill above, fill below, reference, primary"
        );
        // Chunks never exceed the budget and no triangle is split.
        for buffer in geometry.buffers() {
            for chunk in buffer.chunks() {
                assert!(chunk.vertices.len() <= CHUNK_VERTICES);
                assert_eq!(chunk.vertices.len() % 3, 0);
            }
        }
    }

    /// Regression: the generation used to be a caller-set field, so a new
    /// scene (a lap swap, a manual offset's new map) built with the old
    /// number silently reused the old geometry.
    #[test]
    fn a_new_scene_with_the_same_shape_rebuilds() {
        let first = scene_of(series(LaneKind::Line));
        let mut geometry = ChannelGeometry::default();
        let mut scratch = Scratch::default();
        assert!(geometry.prepare(&input(&first, Viewport::FULL), &mut scratch));
        let same = first.clone();
        assert_eq!(same.generation(), first.generation());
        assert!(
            !geometry.prepare(&input(&same, Viewport::FULL), &mut scratch),
            "a clone is the same data"
        );
        let swapped = scene_of(series(LaneKind::Line));
        assert_ne!(swapped.generation(), first.generation());
        assert!(geometry.prepare(&input(&swapped, Viewport::FULL), &mut scratch));
        let remapped = swapped.clone().with_map(None);
        assert_ne!(remapped.generation(), swapped.generation());
        assert!(geometry.prepare(&input(&remapped, Viewport::FULL), &mut scratch));
    }

    #[test]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
    )]
    fn fill_bounds_span_the_fade_range() {
        let lane = scene_of(series(LaneKind::Area));
        let mut geometry = ChannelGeometry::default();
        geometry.prepare(&input(&lane, Viewport::FULL), &mut Scratch::default());
        let [above, below, ..] = geometry.buffers();
        let baseline = input(&lane, Viewport::FULL).baseline() as f32;
        assert!(!above.chunks().is_empty() && !below.chunks().is_empty());
        for chunk in above.chunks() {
            let a = chunk.bounds;
            assert!((a.origin.y.as_f32() + a.size.height.as_f32() - baseline).abs() < 1e-3);
            assert!(a.origin.y.as_f32() <= 1.0 + 1e-3);
        }
        for chunk in below.chunks() {
            assert!((chunk.bounds.origin.y.as_f32() - baseline).abs() < 1e-3);
        }
    }

    #[test]
    fn translated_moves_every_vertex() {
        let lane = scene_of(series(LaneKind::Line));
        let mut geometry = ChannelGeometry::default();
        geometry.prepare(&input(&lane, Viewport::FULL), &mut Scratch::default());
        let primary = geometry.buffers()[3];
        let moved: Vec<_> = primary.translated(point(px(10.), px(20.))).collect();
        assert_eq!(moved.len(), primary.chunks().len());
        let a = &primary.chunks()[0].vertices[0].xy_position;
        let b = &moved[0].vertices[0].xy_position;
        assert_eq!(b.x - a.x, px(10.));
        assert_eq!(b.y - a.y, px(20.));
    }

    #[test]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
    )]
    fn neighbour_laps_draw_past_the_lap_edges() {
        let lane = series(LaneKind::Line);
        let previous: Arc<[f64]> = (0..4500)
            .map(|i| (f64::from(i) * 0.02).cos() * 80.0)
            .collect();
        let lane = scene_of(lane.with_neighbours(Some(previous), None));
        let mut geometry = ChannelGeometry::default();
        let mut scratch = Scratch::default();
        geometry.prepare(&input(&lane, Viewport::new(0.2, 0.8)), &mut scratch);
        assert!(
            geometry.buffers()[4].is_empty(),
            "inside the lap: no neighbours"
        );
        geometry.prepare(&input(&lane, Viewport::focus_on(0.0, 0.03)), &mut scratch);
        let neighbours = geometry.buffers()[4];
        assert!(!neighbours.is_empty());
        // The neighbour stays left of the lap start.
        let lap_start = Viewport::focus_on(0.0, 0.03).x_for_fraction(0.0, 0.0, 1200.0) as f32;
        for chunk in neighbours.chunks() {
            let right = chunk.bounds.origin.x + chunk.bounds.size.width;
            assert!(right.as_f32() <= lap_start + 1.0, "{right:?} > {lap_start}");
        }
    }

    #[test]
    fn step_lanes_hold_values() {
        let points = [PathPoint::new(0.0, 10.0), PathPoint::new(5.0, 20.0)];
        let mut out = Vec::new();
        let stepped = stepped(&points, true, &mut out);
        assert_eq!(
            stepped,
            &[
                PathPoint::new(0.0, 10.0),
                PathPoint::new(5.0, 10.0),
                PathPoint::new(5.0, 20.0)
            ]
        );
    }
}
