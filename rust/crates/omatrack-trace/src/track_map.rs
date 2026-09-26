//! `TrackMap`: the atlas centerline with the primary and reference GPS laps.
//!
//! Geometry is projected equirectangularly around the centroid of everything
//! drawn, in metres (east, north), then fitted aspect-preserving and centred
//! into the element's bounds. Paint order is the centerline ribbon, the
//! reference lap, then the primary lap coloured by the slope of the
//! cumulative delta: gaining time in the `success` role, losing it in
//! `danger`, level in the primary role. Each colour is its own set of
//! triangle meshes built from same-sign runs (never per-vertex colour), with
//! a shared vertex at every run boundary so the line stays continuous.
//!
//! Meshes come from [`crate::mesh::stroke`] into reusable [`PathBuffer`]s
//! (plain triangles, 32-bit addressed; never `PathBuilder`). They are cached
//! per data generation, size and DPR and never keyed on colour.
//!
//! Static and overlay are separate passes (the trace stack's rule): the
//! meshes are painted by a static layer entity embedded with
//! `Entity::cached`, notified only on new data or a theme change and never
//! reading [`CursorState`], so GPUI replays its primitives on a cursor or
//! hover frame. The cursor dots (P and R through the shared
//! [`FractionMap`]), the hover ring and the corner labels are the overlay,
//! painted on top every frame: a few quads and text runs.
//!
//! **Heat mode** ([`TrackMapData::with_heat`]): the primary lap is coloured
//! by time lost per metre on a ramp of [`HEAT_LEVELS`] steps from a quiet
//! muted tone to `danger` ([`TracePalette::heat`]), "less — more". Gaining
//! stations sit at the quiet end: the ramp answers where time goes, and a
//! second hue for gains would compete with it (gains stay readable in the
//! delta lane). The reference lap and the centerline ribbon are left out so
//! the one line reads; the cursor is a ring on it.
//!
//! Interaction: hovering reports the lap fraction of the nearest primary
//! sample ([`TrackMapEvent::MapHover`], `None` on leave or when nothing is
//! near); a click reports [`TrackMapEvent::MapClicked`]. Without a primary
//! GPS lap the centerline station fraction is reported instead.
//!
//! Positions inside the element are measured runtime geometry (the
//! documented `px` exception); colours come from [`TracePalette`].

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use gpui_kit::AppContext as _;
use gpui_kit::base::TestSupportExt as _;
use gpui_kit::component::{ActiveTheme as _, Theme};
use gpui_kit::{
    App, Bounds, Context, CursorStyle, DispatchPhase, Element, ElementId, Entity, EventEmitter,
    FontWeight, GlobalElementId, Hitbox, HitboxBehavior, Hsla, InspectorElementId,
    InteractiveElement as _, IntoElement, LayoutId, MouseButton, MouseDownEvent, MouseMoveEvent,
    MouseUpEvent, ParentElement as _, Pixels, Point, Position, Render, Role, SharedString,
    StatefulInteractiveElement as _, Style, StyleRefinement, Styled as _, Subscription, WeakEntity,
    Window, div, fill, point, prelude::FluentBuilder as _, px, relative, size,
};

use omatrack_ui::TypeStep;

use crate::decimate::PathPoint;
use crate::interaction::CLICK_SLOP;
use crate::label;
use crate::lanes::PathBuffer;
use crate::mesh::stroke;
use crate::palette::TracePalette;
use crate::scale::value_at_fraction;
use crate::scene::FractionMap;
use crate::state::CursorState;

const METERS_PER_DEGREE: f64 = 111_319.490_793_273_57;
/// Pointer distance (logical pixels) within which a hover snaps to the lap.
pub const HOVER_DISTANCE: f64 = 24.0;
/// Half-width of the delta slope window, samples (±0.24 s at 50 Hz).
pub const SLOPE_HALF_WINDOW: usize = 12;
/// Δt change over the slope window below which the lap is drawn level.
pub const SLOPE_DEADBAND_S: f64 = 0.002;
/// Steps of the heat ramp, the quiet one included.
pub const HEAT_LEVELS: usize = 6;
/// Share of the losing stations below the top of the heat ramp: a robust
/// maximum, so one spike does not flatten the rest of the lap to quiet.
pub const HEAT_SCALE_QUANTILE: f64 = 0.95;
/// Share of the losing stations at the quiet end of the heat ramp.
///
/// The lap's background loss stays quiet so corners stand out. A straight under a
/// time-share alignment still loses time in proportion to its duration.
pub const HEAT_FLOOR_QUANTILE: f64 = 0.25;
/// Narrowest stroke of the heat lap, logical pixels (it widens with the
/// drawing, up to 6 px).
const HEAT_STROKE: f64 = 3.0;

/// A WGS84 position.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct GeoPoint {
    pub lon: f64,
    pub lat: f64,
}

impl GeoPoint {
    pub const fn new(lon: f64, lat: f64) -> Self {
        Self { lon, lat }
    }

    pub fn is_valid(&self) -> bool {
        self.lon.is_finite()
            && self.lat.is_finite()
            && self.lat.abs() <= 90.0
            && self.lon.abs() <= 180.0
            && (self.lat.abs() >= 0.001 || self.lon.abs() >= 0.001)
    }
}

/// Track Atlas centerline points are `(lon, lat)` in `x`, `y`.
impl From<omatrack_core::atlas_spatial::Point> for GeoPoint {
    fn from(point: omatrack_core::atlas_spatial::Point) -> Self {
        Self::new(point.x, point.y)
    }
}

/// A GPS lap on its own 50 Hz grid (lap fraction `i / (n - 1)`).
#[derive(Clone)]
pub struct GpsTrack {
    latitude: Arc<[f64]>,
    longitude: Arc<[f64]>,
}

impl GpsTrack {
    pub fn new(latitude: Arc<[f64]>, longitude: Arc<[f64]>) -> Self {
        Self {
            latitude,
            longitude,
        }
    }

    pub fn len(&self) -> usize {
        self.latitude.len().min(self.longitude.len())
    }

    pub fn is_empty(&self) -> bool {
        self.len() < 2
    }

    fn at(&self, index: usize) -> GeoPoint {
        GeoPoint::new(self.longitude[index], self.latitude[index])
    }

    /// Interpolated position at a lap fraction; `None` outside the lap or in
    /// a GPS gap.
    pub fn position_at(&self, fraction: f64) -> Option<GeoPoint> {
        if self.is_empty() || !(0.0..=1.0).contains(&fraction) {
            return None;
        }
        let n = self.len();
        let point = GeoPoint::new(
            value_at_fraction(&self.longitude[..n], fraction),
            value_at_fraction(&self.latitude[..n], fraction),
        );
        point.is_valid().then_some(point)
    }

    /// The position at `fraction`, else the valid fix nearest to it within
    /// `start..=end` (a corner's zone): a GPS gap at a corner's midpoint
    /// places it on the nearest real fix instead of dropping it. `None` when
    /// the zone has no fix at all.
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        clippy::cast_sign_loss,
        reason = "Clamped lap fractions map to indices in resident sample buffers; interpolation intentionally uses f64."
    )]
    pub fn position_near(&self, fraction: f64, start: f64, end: f64) -> Option<GeoPoint> {
        if let Some(point) = self.position_at(fraction) {
            return Some(point);
        }
        let n = self.len();
        if n < 2 {
            return None;
        }
        let last = (n - 1) as f64;
        let index = |f: f64| (f.clamp(0.0, 1.0) * last).round() as usize;
        let (lo, mid, hi) = (
            index(start.min(end)),
            index(fraction),
            index(start.max(end)),
        );
        let mid = mid.clamp(lo, hi);
        (0..=(hi - lo))
            .flat_map(|step| [mid.checked_sub(step), mid.checked_add(step)])
            .flatten()
            .filter(|i| (lo..=hi).contains(i))
            .map(|i| self.at(i))
            .find(GeoPoint::is_valid)
    }
}

/// A corner label placed on the map.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct MapCorner {
    /// Stable corner id (as [`crate::CornerBand::id`]).
    pub id: u32,
    pub label: SharedString,
    /// `T10A` for `Turn 10A`, what the map draws; the full label otherwise.
    pub short: SharedString,
    pub position: GeoPoint,
}

impl MapCorner {
    pub fn new(id: u32, label: impl Into<SharedString>, position: GeoPoint) -> Self {
        let label = label.into();
        let short = crate::corner_ruler::short_label(&label).unwrap_or_else(|| label.clone());
        Self {
            id,
            label,
            short,
            position,
        }
    }
}

static NEXT_GENERATION: AtomicU64 = AtomicU64::new(1);

/// Everything a track map draws. Immutable once built; every builder step
/// allocates a new generation, which the geometry cache keys on.
#[derive(Clone, Default)]
pub struct TrackMapData {
    generation: u64,
    centerline: Arc<[GeoPoint]>,
    primary: Option<GpsTrack>,
    reference: Option<GpsTrack>,
    delta: Option<Arc<[f64]>>,
    map: Option<Arc<dyn FractionMap>>,
    corners: Vec<MapCorner>,
    heat: Option<Arc<[f64]>>,
}

fn next_generation() -> u64 {
    NEXT_GENERATION.fetch_add(1, Ordering::Relaxed)
}

impl TrackMapData {
    pub fn new() -> Self {
        Self {
            generation: next_generation(),
            ..Self::default()
        }
    }
    /// The layout centerline (Track Atlas), as a closed loop.
    #[must_use]
    pub fn with_centerline(mut self, centerline: impl IntoIterator<Item = GeoPoint>) -> Self {
        self.centerline = centerline.into_iter().collect();
        self.generation = next_generation();
        self
    }
    #[must_use]
    pub fn with_primary(mut self, track: Option<GpsTrack>) -> Self {
        self.primary = track.filter(|t| !t.is_empty());
        self.generation = next_generation();
        self
    }
    #[must_use]
    pub fn with_reference(mut self, track: Option<GpsTrack>) -> Self {
        self.reference = track.filter(|t| !t.is_empty());
        self.generation = next_generation();
        self
    }
    /// Cumulative Δt (s, positive: primary slower) on the primary grid; its
    /// slope colours the primary lap.
    #[must_use]
    pub fn with_delta(mut self, delta: Option<Arc<[f64]>>) -> Self {
        self.delta = delta;
        self.generation = next_generation();
        self
    }
    /// The shared primary → reference map (places the R dot).
    #[must_use]
    pub fn with_map(mut self, map: Option<Arc<dyn FractionMap>>) -> Self {
        self.map = map;
        self.generation = next_generation();
        self
    }
    #[must_use]
    pub fn with_corners(mut self, corners: Vec<MapCorner>) -> Self {
        self.corners = corners;
        self.generation = next_generation();
        self
    }
    /// Heat mode: time lost per metre (s/m) on the primary grid colours the
    /// primary lap on the loss ramp (see the module docs). Ignored unless it
    /// is as long as the primary lap.
    #[must_use]
    pub fn with_heat(mut self, loss_rate: Option<Arc<[f64]>>) -> Self {
        self.heat = loss_rate;
        self.generation = next_generation();
        self
    }
    /// The loss rate driving heat mode, when set.
    pub fn heat(&self) -> Option<&[f64]> {
        self.heat.as_deref()
    }
    /// Whether the primary lap is drawn as heat.
    pub fn is_heat(&self) -> bool {
        match (&self.primary, &self.heat) {
            (Some(primary), Some(heat)) => heat.len() == primary.len(),
            _ => false,
        }
    }

    pub fn generation(&self) -> u64 {
        self.generation
    }
    pub fn centerline(&self) -> &[GeoPoint] {
        &self.centerline
    }
    pub fn primary(&self) -> Option<&GpsTrack> {
        self.primary.as_ref()
    }
    pub fn reference(&self) -> Option<&GpsTrack> {
        self.reference.as_ref()
    }
    pub fn corners(&self) -> &[MapCorner] {
        &self.corners
    }

    /// Whether there is anything to draw.
    pub fn is_empty(&self) -> bool {
        self.centerline.iter().filter(|p| p.is_valid()).count() < 2
            && self.primary.is_none()
            && self.reference.is_none()
    }

    /// Every drawn position, for fitting.
    fn points(&self) -> impl Iterator<Item = GeoPoint> + '_ {
        let track = |t: &Option<GpsTrack>| {
            t.iter()
                .flat_map(|t| (0..t.len()).map(move |i| t.at(i)))
                .collect::<Vec<_>>()
        };
        self.centerline
            .iter()
            .copied()
            .chain(track(&self.primary))
            .chain(track(&self.reference))
            .filter(GeoPoint::is_valid)
    }

    fn reference_fraction(&self, primary: f64) -> f64 {
        self.map
            .as_ref()
            .map_or(primary, |m| m.reference_fraction(primary))
    }
}

/// Equirectangular projection around a centroid, fitted into a rectangle,
/// north up unless a rotation fills the rectangle clearly better (a long
/// circuit in a wide panel).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MapProjection {
    lon0: f64,
    lat0: f64,
    east_scale: f64,
    /// Rotation of the drawing (counter-clockwise), as cosine and sine.
    cos: f64,
    sin: f64,
    /// Logical pixels per metre.
    scale: f64,
    /// Pixel position of the metre origin (the centroid), y down.
    origin_x: f64,
    origin_y: f64,
}

/// Rotations tried when fitting, in degrees apart (0 to 180).
const FIT_ROTATION_STEP_DEGREES: f64 = 5.0;
/// A rotation replaces north-up only when it draws the circuit this much
/// larger.
const FIT_ROTATION_MIN_GAIN: f64 = 1.15;

impl MapProjection {
    /// Fit `points` into `width` × `height` logical pixels with `padding` on
    /// every side, preserving the aspect ratio and centring the drawing.
    /// `None` without at least two distinct valid points or without room.
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        clippy::cast_sign_loss,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    pub fn fit(
        points: impl IntoIterator<Item = GeoPoint>,
        width: f64,
        height: f64,
        padding: f64,
    ) -> Option<Self> {
        let points: Vec<GeoPoint> = points.into_iter().filter(GeoPoint::is_valid).collect();
        if points.len() < 2 {
            return None;
        }
        let n = points.len() as f64;
        let lon0 = points.iter().map(|p| p.lon).sum::<f64>() / n;
        let lat0 = points.iter().map(|p| p.lat).sum::<f64>() / n;
        let mut projection = Self {
            lon0,
            lat0,
            east_scale: (lat0.to_radians()).cos(),
            cos: 1.0,
            sin: 0.0,
            scale: 1.0,
            origin_x: 0.0,
            origin_y: 0.0,
        };
        let (room_x, room_y) = (width - 2.0 * padding, height - 2.0 * padding);
        if room_x <= 0.0 || room_y <= 0.0 {
            return None;
        }
        let metres: Vec<(f64, f64)> = points.iter().map(|p| projection.metres(*p)).collect();
        // Bounding box and scale of the drawing rotated by (cos, sin).
        let fit_at = |cos: f64, sin: f64| {
            let (mut min_x, mut max_x) = (f64::INFINITY, f64::NEG_INFINITY);
            let (mut min_y, mut max_y) = (f64::INFINITY, f64::NEG_INFINITY);
            for &(east, north) in &metres {
                let (x, y) = (east * cos - north * sin, east * sin + north * cos);
                min_x = min_x.min(x);
                max_x = max_x.max(x);
                min_y = min_y.min(y);
                max_y = max_y.max(y);
            }
            let (span_x, span_y) = (max_x - min_x, max_y - min_y);
            let scale = match (span_x > 1e-9, span_y > 1e-9) {
                (true, true) => (room_x / span_x).min(room_y / span_y),
                (true, false) => room_x / span_x,
                (false, true) => room_y / span_y,
                (false, false) => return None,
            };
            Some((scale, min_x, max_x, min_y, max_y))
        };
        let north_up = fit_at(1.0, 0.0)?;
        let mut best = (north_up, 1.0, 0.0);
        let steps = (180.0 / FIT_ROTATION_STEP_DEGREES) as usize;
        for step in 1..steps {
            let angle = (step as f64 * FIT_ROTATION_STEP_DEGREES).to_radians();
            let (sin, cos) = angle.sin_cos();
            if let Some(fit) = fit_at(cos, sin)
                && fit.0 > best.0.0
            {
                best = (fit, cos, sin);
            }
        }
        if best.0.0 < north_up.0 * FIT_ROTATION_MIN_GAIN {
            best = (north_up, 1.0, 0.0);
        }
        let ((scale, min_x, max_x, min_y, max_y), cos, sin) = best;
        projection.cos = cos;
        projection.sin = sin;
        projection.scale = scale;
        // Centre the rotated metre bounding box; y flips (up is +y).
        projection.origin_x = width * 0.5 - (min_x + max_x) * 0.5 * scale;
        projection.origin_y = height * 0.5 + (min_y + max_y) * 0.5 * scale;
        Some(projection)
    }

    /// Metres east and north of the centroid.
    pub fn metres(&self, point: GeoPoint) -> (f64, f64) {
        (
            (point.lon - self.lon0) * METERS_PER_DEGREE * self.east_scale,
            (point.lat - self.lat0) * METERS_PER_DEGREE,
        )
    }

    /// Logical pixel position inside the fitted rectangle (y down).
    pub fn project(&self, point: GeoPoint) -> (f64, f64) {
        let (east, north) = self.metres(point);
        let (x, y) = (
            east * self.cos - north * self.sin,
            east * self.sin + north * self.cos,
        );
        (
            self.origin_x + x * self.scale,
            self.origin_y - y * self.scale,
        )
    }

    /// Whether the drawing is north-up (not rotated to fill the rectangle).
    pub fn is_north_up(&self) -> bool {
        self.sin == 0.0
    }

    /// Logical pixels per metre.
    pub fn scale(&self) -> f64 {
        self.scale
    }
}

/// How the primary lap is doing at a sample.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SlopeSign {
    /// Losing less than the deadband either way (or no delta).
    Level,
    /// Δt falling: the primary lap is gaining time.
    Gain,
    /// Δt rising: the primary lap is losing time.
    Loss,
}

/// Per-sample sign of the cumulative delta's slope, over a centred window of
/// `±half_window` samples with a `deadband` in seconds.
pub fn delta_slope_signs(delta: &[f64], half_window: usize, deadband: f64) -> Vec<SlopeSign> {
    let n = delta.len();
    (0..n)
        .map(|i| {
            let a = delta[i.saturating_sub(half_window)];
            let b = delta[(i + half_window).min(n - 1)];
            let change = b - a;
            if !change.is_finite() || change.abs() <= deadband {
                SlopeSign::Level
            } else if change > 0.0 {
                SlopeSign::Loss
            } else {
                SlopeSign::Gain
            }
        })
        .collect()
}

/// Ramp step (0 = quiet ..
///
/// `levels - 1` = hottest) of every loss rate: the losing stations spread from their
/// [`HEAT_FLOOR_QUANTILE`] (quiet) to their [`HEAT_SCALE_QUANTILE`] (hottest); gains
/// and non-finite values quiet.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Quantiles select bounded sample indices and normalized heat values are rounded to palette entries."
)]
pub fn heat_levels(loss_rate: &[f64], levels: usize) -> Vec<u8> {
    let top = levels.clamp(1, usize::from(u8::MAX)) - 1;
    let mut losing: Vec<f64> = loss_rate
        .iter()
        .copied()
        .filter(|r| r.is_finite() && *r > 0.0)
        .collect();
    let (floor, ceiling) = if losing.is_empty() {
        (0.0, 0.0)
    } else {
        let mut quantile = |q: f64| {
            let ix = ((losing.len() - 1) as f64 * q).round() as usize;
            *losing.select_nth_unstable_by(ix, f64::total_cmp).1
        };
        let ceiling = quantile(HEAT_SCALE_QUANTILE);
        let floor = quantile(HEAT_FLOOR_QUANTILE);
        // A flat loss (every station alike) ramps from zero instead.
        (if floor < ceiling { floor } else { 0.0 }, ceiling)
    };
    let span = ceiling - floor;
    loss_rate
        .iter()
        .map(|&rate| {
            if span > 0.0 && rate.is_finite() && rate > 0.0 {
                (((rate - floor) / span).clamp(0.0, 1.0) * top as f64).round() as u8
            } else {
                0
            }
        })
        .collect()
}

/// Index of the nearest `(x, y)` within `max_distance`, if any.
pub fn nearest_point(points: &[(f32, f32)], x: f32, y: f32, max_distance: f32) -> Option<usize> {
    let mut best = None;
    let mut best_distance = max_distance * max_distance;
    for (index, (px_, py_)) in points.iter().enumerate() {
        let d = (px_ - x) * (px_ - x) + (py_ - y) * (py_ - y);
        if d <= best_distance {
            best_distance = d;
            best = Some(index);
        }
    }
    best
}

/// User intent reported by a [`TrackMap`].
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub enum TrackMapEvent {
    /// The pointer is over the lap at this primary lap fraction; `None` when
    /// it leaves the map or is not near the lap.
    MapHover(Option<f64>),
    /// A click on the lap at this primary lap fraction.
    MapClicked(f64),
}

/// Cached map geometry for one data generation, size and DPR.
#[derive(Default)]
struct MapGeometry {
    key: Option<(u64, u32, u32, u32)>,
    projection: Option<MapProjection>,
    centerline: PathBuffer,
    reference: PathBuffer,
    level: PathBuffer,
    gain: PathBuffer,
    loss: PathBuffer,
    /// Heat mode: one buffer per ramp step.
    heat: [PathBuffer; HEAT_LEVELS],
    /// Hover targets: projected points and their lap fraction.
    targets: Vec<(f32, f32)>,
    target_fractions: Vec<f64>,
    points: Vec<PathPoint>,
    builds: usize,
    /// Times the static layer rendered (a cached replay does not count).
    renders: usize,
    /// Window origin of the last paint.
    origin: Point<Pixels>,
    /// Corner label boxes placed this frame (retained, cleared per paint).
    labels: Vec<Bounds<Pixels>>,
}

/// Where a label of `size` goes beside a dot at `centre` with `radius`:
/// first away from the drawing along `outward` (the unit direction from the
/// map's centre to the dot, so labels sit outside the lap rather than on
/// it), then to the right, the left, above and below; inside `area` and
/// clear of every `placed` box. `None` when nothing fits (the dot stays,
/// the label is dropped).
fn place_label(
    centre: Point<Pixels>,
    radius: Pixels,
    size: gpui_kit::Size<Pixels>,
    area: Bounds<Pixels>,
    placed: &[Bounds<Pixels>],
    outward: (f32, f32),
) -> Option<Bounds<Pixels>> {
    let gap = radius + px(3.);
    let (w, h) = (size.width, size.height);
    let top = centre.y - h * 0.5;
    let (dx, dy) = outward;
    let outward = (dx.is_finite() && dy.is_finite() && dx.hypot(dy) > 0.5).then(|| {
        // The box touching the dot's gap circle in the outward direction.
        let reach = gap.as_f32() + 0.5 * (w.as_f32() * dx.abs() + h.as_f32() * dy.abs());
        point(
            centre.x + px(dx * reach) - w * 0.5,
            centre.y + px(dy * reach) - h * 0.5,
        )
    });
    outward
        .into_iter()
        .chain([
            point(centre.x + gap, top),
            point(centre.x - gap - w, top),
            // Above and below clear the side boxes' rows.
            point(centre.x - w * 0.5, top - h - px(2.)),
            point(centre.x - w * 0.5, top + h + px(2.)),
        ])
        .map(|origin| Bounds::new(origin, size))
        .find(|candidate| {
            area.contains(&candidate.origin)
                && area.contains(&candidate.bottom_right())
                && !placed.iter().any(|b| b.intersects(candidate))
        })
}

impl MapGeometry {
    fn prepare(&mut self, data: &TrackMapData, width: f32, height: f32, dpr: f32) {
        let key = (
            data.generation,
            width.to_bits(),
            height.to_bits(),
            dpr.to_bits(),
        );
        if self.key == Some(key) {
            return;
        }
        self.key = Some(key);
        self.builds += 1;
        for buffer in self.buffers_mut() {
            buffer.clear();
        }
        self.build(data, width, height, dpr);
        // Paths are clipped to their bounds: without these they paint nothing.
        for buffer in self.buffers_mut() {
            buffer.finish();
        }
    }

    fn buffers_mut(&mut self) -> impl Iterator<Item = &mut PathBuffer> {
        [
            &mut self.centerline,
            &mut self.reference,
            &mut self.level,
            &mut self.gain,
            &mut self.loss,
        ]
        .into_iter()
        .chain(self.heat.iter_mut())
    }

    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    #[expect(
        clippy::too_many_lines,
        reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
    )]
    fn build(&mut self, data: &TrackMapData, width: f32, height: f32, dpr: f32) {
        // Strokes follow the drawing's size: a panel-wide map reads with a
        // bolder lap than a thumbnail.
        let side = f64::from(width.min(height));
        let lap_stroke = (side / 140.0).clamp(2.0, 3.5);
        let heat_stroke = (side / 75.0).clamp(HEAT_STROKE, 6.0);
        self.targets.clear();
        self.target_fractions.clear();
        let padding = (f64::from(width.min(height)) * 0.06).clamp(8.0, 24.0);
        self.projection =
            MapProjection::fit(data.points(), f64::from(width), f64::from(height), padding);
        let Some(projection) = self.projection else {
            return;
        };
        let min_step = 0.75 / f64::from(dpr.max(1.0));
        let project = |p: GeoPoint| {
            let (x, y) = projection.project(p);
            PathPoint::new(x, y)
        };

        let heat = data.is_heat();
        // Centerline: a closed ribbon under the laps.
        let centerline: Vec<GeoPoint> = data
            .centerline
            .iter()
            .copied()
            .filter(GeoPoint::is_valid)
            .collect();
        if centerline.len() >= 2 && !heat {
            self.points.clear();
            self.points.extend(
                centerline
                    .iter()
                    .chain(centerline.first())
                    .map(|p| project(*p)),
            );
            stroke(&self.points, 5.0, &mut self.centerline);
        }

        if let Some(track) = data.reference.as_ref().filter(|_| !heat) {
            polyline(track, &project, min_step, &mut self.points);
            stroke(&self.points, 1.25, &mut self.reference);
        }

        if let Some(track) = &data.primary {
            let n = track.len();
            let signs = data
                .delta
                .as_ref()
                .filter(|d| d.len() == n)
                .map(|d| delta_slope_signs(d, SLOPE_HALF_WINDOW, SLOPE_DEADBAND_S));
            let levels = data
                .heat
                .as_ref()
                .filter(|_| heat)
                .map(|h| heat_levels(h, HEAT_LEVELS));
            // One bucket per colour: a slope sign, or a heat step.
            let sign_at = |i: usize| match (&levels, &signs) {
                (Some(levels), _) => Bucket::Heat(levels[i]),
                (None, Some(signs)) => Bucket::Slope(signs[i]),
                (None, None) => Bucket::Slope(SlopeSign::Level),
            };
            // Walk the lap, stroking each same-bucket run into its colour's
            // buffer; consecutive runs share their boundary vertex.
            let mut run: Vec<PathPoint> = Vec::with_capacity(n.min(8192));
            let mut run_sign = sign_at(0);
            let last = (n - 1).max(1) as f64;
            let flush = |run: &mut Vec<PathPoint>, bucket: Bucket, geometry: &mut Self| {
                let (buffer, width) = match bucket {
                    Bucket::Slope(SlopeSign::Level) => (&mut geometry.level, lap_stroke),
                    Bucket::Slope(SlopeSign::Gain) => (&mut geometry.gain, lap_stroke),
                    Bucket::Slope(SlopeSign::Loss) => (&mut geometry.loss, lap_stroke),
                    Bucket::Heat(level) => (
                        &mut geometry.heat[usize::from(level).min(HEAT_LEVELS - 1)],
                        heat_stroke,
                    ),
                };
                stroke(run, width, buffer);
            };
            for i in 0..n {
                let geo = track.at(i);
                if !geo.is_valid() {
                    flush(&mut run, run_sign, self);
                    run.clear();
                    continue;
                }
                let p = project(geo);
                let sign = sign_at(i);
                if sign != run_sign {
                    run.push(p);
                    flush(&mut run, run_sign, self);
                    run.clear();
                    run_sign = sign;
                    run.push(p);
                } else if run
                    .last()
                    .is_none_or(|q| (q.x - p.x).hypot(q.y - p.y) >= min_step || i == n - 1)
                {
                    run.push(p);
                }
                self.targets.push((p.x as f32, p.y as f32));
                self.target_fractions.push(i as f64 / last);
            }
            flush(&mut run, run_sign, self);
        } else if centerline.len() >= 2 {
            // No primary lap: hover along the centerline by station.
            let mut cumulative = Vec::with_capacity(centerline.len());
            let mut total = 0.0;
            for (i, point) in centerline.iter().enumerate() {
                if i > 0 {
                    let (ax, ay) = projection.metres(centerline[i - 1]);
                    let (bx, by) = projection.metres(*point);
                    total += (bx - ax).hypot(by - ay);
                }
                cumulative.push(total);
            }
            for (point, distance) in centerline.iter().zip(cumulative) {
                let p = project(*point);
                self.targets.push((p.x as f32, p.y as f32));
                self.target_fractions
                    .push(if total > 0.0 { distance / total } else { 0.0 });
            }
        }
    }
}

/// The colour a primary run is stroked in.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Bucket {
    Slope(SlopeSign),
    Heat(u8),
}

/// Project a GPS lap into `points` with pen-ups at gaps, dropping vertices
/// closer than `min_step` logical pixels to the previous one.
fn polyline(
    track: &GpsTrack,
    project: &dyn Fn(GeoPoint) -> PathPoint,
    min_step: f64,
    points: &mut Vec<PathPoint>,
) {
    points.clear();
    let n = track.len();
    for i in 0..n {
        let geo = track.at(i);
        if !geo.is_valid() {
            if points.last().is_some_and(|p| !p.is_pen_up()) {
                points.push(PathPoint::PEN_UP);
            }
            continue;
        }
        let p = project(geo);
        match points.last() {
            Some(q) if !q.is_pen_up() && (q.x - p.x).hypot(q.y - p.y) < min_step && i != n - 1 => {}
            _ => points.push(p),
        }
    }
}

/// The track map view. Shares the application's [`CursorState`]; see the
/// module docs.
pub struct TrackMap {
    cursor: Entity<CursorState>,
    data: Arc<TrackMapData>,
    focused: Option<u32>,
    hover: Option<f64>,
    press: Option<Point<Pixels>>,
    geometry: Rc<RefCell<MapGeometry>>,
    layer: Entity<MapLayer>,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<TrackMapEvent> for TrackMap {}

impl TrackMap {
    pub fn new(cursor: Entity<CursorState>, cx: &mut Context<'_, Self>) -> Self {
        let subscriptions = vec![cx.observe(&cursor, |_, _, cx| cx.notify())];
        let data = Arc::new(TrackMapData::default());
        let geometry: Rc<RefCell<MapGeometry>> = Rc::default();
        let layer = cx.new(|cx| MapLayer::new(data.clone(), geometry.clone(), cx));
        Self {
            cursor,
            data,
            focused: None,
            hover: None,
            press: None,
            geometry,
            layer,
            _subscriptions: subscriptions,
        }
    }

    pub fn set_data(&mut self, data: Arc<TrackMapData>, cx: &mut Context<'_, Self>) {
        self.data = data.clone();
        self.hover = None;
        self.layer.update(cx, |layer, cx| layer.set_data(data, cx));
        cx.notify();
    }

    pub fn data(&self) -> &Arc<TrackMapData> {
        &self.data
    }

    /// The focused corner (controlled by the owner).
    pub fn set_focused_corner(&mut self, id: Option<u32>, cx: &mut Context<'_, Self>) {
        if self.focused != id {
            self.focused = id;
            cx.notify();
        }
    }

    pub fn focused_corner(&self) -> Option<u32> {
        self.focused
    }

    /// The lap fraction under the pointer, as last reported.
    pub fn hover(&self) -> Option<f64> {
        self.hover
    }

    /// Times the map geometry was built (a cursor move must not change it).
    pub fn geometry_builds(&self) -> usize {
        self.geometry.borrow().builds
    }

    /// Times the static layer (the meshes) rendered; a cursor or hover
    /// frame replays it from GPUI's cache and does not count.
    pub fn static_renders(&self) -> usize {
        self.geometry.borrow().renders
    }

    /// Window position of the primary lap at a lap fraction, as last
    /// painted; `None` before the first paint, without a primary GPS lap, or
    /// in a GPS gap.
    #[expect(
        clippy::cast_possible_truncation,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    pub fn primary_position(&self, fraction: f64) -> Option<Point<Pixels>> {
        let geometry = self.geometry.borrow();
        let projection = geometry.projection?;
        let position = self.data.primary.as_ref()?.position_at(fraction)?;
        let (x, y) = projection.project(position);
        Some(geometry.origin + point(px(x as f32), px(y as f32)))
    }

    #[expect(
        clippy::cast_possible_truncation,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    fn target_at(&self, position: Point<Pixels>) -> Option<f64> {
        let geometry = self.geometry.borrow();
        let index = nearest_point(
            &geometry.targets,
            position.x.as_f32(),
            position.y.as_f32(),
            HOVER_DISTANCE as f32,
        )?;
        geometry.target_fractions.get(index).copied()
    }

    fn pointer_move(&mut self, local: Option<Point<Pixels>>, cx: &mut Context<'_, Self>) {
        let hover = local.and_then(|p| self.target_at(p));
        if hover != self.hover {
            self.hover = hover;
            cx.emit(TrackMapEvent::MapHover(hover));
            cx.notify();
        }
    }

    fn pointer_down(&mut self, local: Point<Pixels>) {
        self.press = Some(local);
    }

    fn pointer_up(&mut self, local: Point<Pixels>, cx: &mut Context<'_, Self>) {
        let Some(press) = self.press.take() else {
            return;
        };
        let moved = (local.x - press.x)
            .as_f32()
            .hypot((local.y - press.y).as_f32());
        if f64::from(moved) < CLICK_SLOP
            && let Some(fraction) = self.target_at(local)
        {
            cx.emit(TrackMapEvent::MapClicked(fraction));
        }
    }
}

impl Render for TrackMap {
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let theme = cx.theme();
        let palette = TracePalette::from_theme(theme);
        let cursor = self.cursor.read(cx).fraction();
        let empty = self.data.is_empty();
        let overlay = MapOverlay {
            map: cx.entity().downgrade(),
            data: self.data.clone(),
            geometry: self.geometry.clone(),
            cursor,
            hover: self.hover,
            focused: self.focused,
            palette,
            label: theme.muted_foreground,
            strong: theme.foreground,
        };
        let spoken: SharedString = if empty {
            "Track map: no GPS or track layout".into()
        } else {
            let corners = match self.data.corners.len() {
                1 => "1 corner".to_string(),
                n => format!("{n} corners"),
            };
            let gps = if self.data.primary.is_some() {
                ""
            } else {
                ", no GPS lap"
            };
            format!("Track map, {corners}{gps}").into()
        };
        div()
            .id("track-map")
            .role(Role::Figure)
            .aria_label(spoken)
            .test_support()
            .relative()
            .size_full()
            .min_h_0()
            .overflow_hidden()
            .bg(theme.background)
            .child(
                self.layer
                    .clone()
                    .cached(StyleRefinement::default().size_full()),
            )
            .child(overlay)
            .when(empty, |el| {
                el.child(
                    div()
                        .absolute()
                        .inset_0()
                        .flex()
                        .items_center()
                        .justify_center()
                        .p_4()
                        .text_sm()
                        .text_color(theme.muted_foreground)
                        .child("No GPS or track layout for this lap"),
                )
            })
    }
}

/// The static pass: the centerline, reference and primary meshes. Owned by
/// a [`TrackMap`] and embedded through `Entity::cached`; notified only on
/// new data or a theme change, it never reads the cursor.
struct MapLayer {
    data: Arc<TrackMapData>,
    geometry: Rc<RefCell<MapGeometry>>,
    _theme: Subscription,
}

impl MapLayer {
    fn new(
        data: Arc<TrackMapData>,
        geometry: Rc<RefCell<MapGeometry>>,
        cx: &mut Context<'_, Self>,
    ) -> Self {
        Self {
            data,
            geometry,
            // A theme change recolours the cached meshes (no rebuild).
            _theme: cx.observe_global::<Theme>(|_, cx| cx.notify()),
        }
    }

    fn set_data(&mut self, data: Arc<TrackMapData>, cx: &mut Context<'_, Self>) {
        if !Arc::ptr_eq(&self.data, &data) {
            self.data = data;
            cx.notify();
        }
    }
}

impl Render for MapLayer {
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        self.geometry.borrow_mut().renders += 1;
        let theme = cx.theme();
        MapStaticElement {
            data: self.data.clone(),
            geometry: self.geometry.clone(),
            palette: TracePalette::from_theme(theme),
            ribbon: theme.muted,
        }
    }
}

/// Size the map's geometry for `bounds` (a no-op when nothing changed).
fn prepare_geometry(
    geometry: &RefCell<MapGeometry>,
    data: &TrackMapData,
    bounds: Bounds<Pixels>,
    window: &Window,
) {
    let mut geometry = geometry.borrow_mut();
    geometry.prepare(
        data,
        bounds.size.width.as_f32(),
        bounds.size.height.as_f32(),
        window.scale_factor(),
    );
    geometry.origin = bounds.origin;
}

struct MapStaticElement {
    data: Arc<TrackMapData>,
    geometry: Rc<RefCell<MapGeometry>>,
    palette: TracePalette,
    ribbon: Hsla,
}

impl IntoElement for MapStaticElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for MapStaticElement {
    type RequestLayoutState = ();
    type PrepaintState = ();

    fn id(&self) -> Option<ElementId> {
        Some("track-map-static".into())
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
        (): &mut (),
        window: &mut Window,
        _: &mut App,
    ) {
        prepare_geometry(&self.geometry, &self.data, bounds, window);
    }

    #[expect(
        clippy::cast_precision_loss,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        (): &mut (),
        (): &mut (),
        window: &mut Window,
        _: &mut App,
    ) {
        let geometry = self.geometry.borrow();
        let palette = self.palette;
        for (buffer, color) in [
            (&geometry.centerline, self.ribbon),
            (&geometry.reference, palette.reference),
            (&geometry.level, palette.primary),
            (&geometry.gain, palette.gain),
            (&geometry.loss, palette.loss),
        ] {
            for path in buffer.translated(bounds.origin) {
                window.paint_path(path, color);
            }
        }
        // Heat steps, quiet first so hotter runs sit on top at the joins.
        for (level, buffer) in geometry.heat.iter().enumerate() {
            let color = palette.heat(level as f32 / (HEAT_LEVELS - 1) as f32);
            for path in buffer.translated(bounds.origin) {
                window.paint_path(path, color);
            }
        }
    }
}

/// The overlay pass of one map frame, snapshotted by [`TrackMap::render`]:
/// corner labels, hover ring, cursor dots and pointer input.
struct MapOverlay {
    map: WeakEntity<TrackMap>,
    data: Arc<TrackMapData>,
    geometry: Rc<RefCell<MapGeometry>>,
    cursor: Option<f64>,
    hover: Option<f64>,
    focused: Option<u32>,
    palette: TracePalette,
    label: Hsla,
    strong: Hsla,
}

impl IntoElement for MapOverlay {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for MapOverlay {
    type RequestLayoutState = ();
    type PrepaintState = Hitbox;

    fn id(&self) -> Option<ElementId> {
        Some("track-map-surface".into())
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
        let mut style = Style {
            position: Position::Absolute,
            ..Style::default()
        };
        style.inset.top = px(0.).into();
        style.inset.left = px(0.).into();
        style.size.width = relative(1.).into();
        style.size.height = relative(1.).into();
        (window.request_layout(style, [], cx), ())
    }

    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        (): &mut (),
        window: &mut Window,
        _: &mut App,
    ) -> Hitbox {
        // Same bounds as the static layer; a no-op unless that layer was
        // replayed from cache after nothing changed.
        prepare_geometry(&self.geometry, &self.data, bounds, window);
        window.insert_hitbox(bounds, HitboxBehavior::Normal)
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        (): &mut (),
        hitbox: &mut Hitbox,
        window: &mut Window,
        cx: &mut App,
    ) {
        let projection = self.geometry.borrow().projection;
        if let Some(projection) = projection {
            self.paint_marks(&projection, bounds, window, cx);
        }
        window.set_cursor_style(
            if self.hover.is_some() {
                CursorStyle::Crosshair
            } else {
                CursorStyle::Arrow
            },
            hitbox,
        );
        self.register_input(bounds, hitbox.clone(), window);
    }
}

impl MapOverlay {
    #[expect(
        clippy::cast_possible_truncation,
        reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
    )]
    #[expect(
        clippy::too_many_lines,
        reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
    )]
    fn paint_marks(
        &self,
        projection: &MapProjection,
        bounds: Bounds<Pixels>,
        window: &mut Window,
        cx: &mut App,
    ) {
        let origin = bounds.origin;
        let palette = &self.palette;
        let at = |p: GeoPoint| {
            let (x, y) = projection.project(p);
            origin + point(px(x as f32), px(y as f32))
        };
        let dot =
            |window: &mut Window, centre: Point<Pixels>, radius: f32, color: Hsla, ring: Hsla| {
                window.paint_quad(
                    fill(
                        Bounds::new(
                            centre - point(px(radius), px(radius)),
                            size(px(radius * 2.0), px(radius * 2.0)),
                        ),
                        color,
                    )
                    .corner_radii(px(radius))
                    .border_widths(px(1.5))
                    .border_color(ring),
                );
            };

        // Corner dots, then their short labels: the focused one first and
        // emphasised, the rest in lap order wherever they fit without
        // overlapping (a crowded map drops labels, never stacks them).
        let text_size = TypeStep::Caption.size(window);
        let text_height = px(text_size.as_f32() * 1.25);
        let style = |focused: bool| {
            if focused {
                (self.strong, FontWeight::SEMIBOLD, 3.5)
            } else {
                (self.label, FontWeight::NORMAL, 2.0)
            }
        };
        let corners = || {
            self.data
                .corners
                .iter()
                .filter(|corner| corner.position.is_valid())
        };
        for corner in corners() {
            let focused = self.focused == Some(corner.id);
            let (_, _, radius) = style(focused);
            let fill = if focused { palette.primary } else { self.label };
            dot(
                window,
                at(corner.position),
                radius,
                fill,
                palette.background,
            );
        }
        let mut geometry = self.geometry.borrow_mut();
        let placed = &mut geometry.labels;
        placed.clear();
        let focused_first = corners()
            .filter(|c| self.focused == Some(c.id))
            .chain(corners().filter(|c| self.focused != Some(c.id)));
        for corner in focused_first {
            let focused = self.focused == Some(corner.id);
            let (color, weight, radius) = style(focused);
            let line = label::shape(corner.short.clone(), text_size, weight, color, window);
            let centre = at(corner.position);
            let middle = bounds.center();
            let (dx, dy) = (
                (centre.x - middle.x).as_f32(),
                (centre.y - middle.y).as_f32(),
            );
            let length = dx.hypot(dy);
            let outward = if length > 0.0 {
                (dx / length, dy / length)
            } else {
                (0.0, 0.0)
            };
            let Some(rect) = place_label(
                centre,
                px(radius),
                size(line.width, text_height),
                bounds,
                placed,
                outward,
            ) else {
                continue;
            };
            placed.push(rect);
            label::paint(&line, rect.origin, text_height, window, cx);
        }
        drop(geometry);

        // Hover ring on the lap.
        if let Some(hover) = self.hover
            && let Some(position) = self
                .data
                .primary
                .as_ref()
                .and_then(|t| t.position_at(hover))
        {
            dot(
                window,
                at(position),
                5.0,
                palette.background.opacity(0.0),
                palette.hover,
            );
        }

        // Cursor dots: reference through the shared map, primary on top
        // (heat mode draws no reference lap, so no reference dot either).
        if let Some(cursor) = self.cursor {
            if !self.data.is_heat()
                && let Some(position) = self
                    .data
                    .reference
                    .as_ref()
                    .and_then(|t| t.position_at(self.data.reference_fraction(cursor)))
            {
                dot(
                    window,
                    at(position),
                    4.0,
                    palette.reference,
                    palette.background,
                );
            }
            if let Some(position) = self
                .data
                .primary
                .as_ref()
                .and_then(|t| t.position_at(cursor))
            {
                if self.data.is_heat() {
                    // A ring, so the heat under the cursor stays visible.
                    dot(
                        window,
                        at(position),
                        6.0,
                        palette.background.opacity(0.0),
                        palette.foreground,
                    );
                } else {
                    dot(
                        window,
                        at(position),
                        4.5,
                        palette.primary,
                        palette.background,
                    );
                }
            }
        }
    }

    fn register_input(&self, bounds: Bounds<Pixels>, hitbox: Hitbox, window: &mut Window) {
        let origin = bounds.origin;

        let map = self.map.clone();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble {
                return;
            }
            let local = hit.is_hovered(window).then(|| event.position - origin);
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no input/deferred update; the weak entity handle may already be gone.")]
            let _ = map.update(cx, |map, cx| map.pointer_move(local, cx));
        });

        let map = self.map.clone();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseDownEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble
                || event.button != MouseButton::Left
                || !hit.is_hovered(window)
            {
                return;
            }
            let local = event.position - origin;
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no input/deferred update; the weak entity handle may already be gone.")]
            let _ = map.update(cx, |map, _| map.pointer_down(local));
        });

        let map = self.map.clone();
        window.on_mouse_event(move |event: &MouseUpEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble
                || event.button != MouseButton::Left
                || !hitbox.is_hovered(window)
            {
                return;
            }
            let local = event.position - origin;
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no input/deferred update; the weak entity handle may already be gone.")]
            let _ = map.update(cx, |map, cx| map.pointer_up(local, cx));
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A 1 km × 500 m rectangle near Road Atlanta.
    fn rectangle() -> Vec<GeoPoint> {
        let (lon0, lat0): (f64, f64) = (-83.81, 34.15);
        let dlat = 500.0 / METERS_PER_DEGREE;
        let dlon = 1000.0 / (METERS_PER_DEGREE * lat0.to_radians().cos());
        vec![
            GeoPoint::new(lon0, lat0),
            GeoPoint::new(lon0 + dlon, lat0),
            GeoPoint::new(lon0 + dlon, lat0 + dlat),
            GeoPoint::new(lon0, lat0 + dlat),
        ]
    }

    #[test]
    fn projection_is_metric_and_north_up() {
        let points = rectangle();
        let projection = MapProjection::fit(points.clone(), 1000.0, 1000.0, 0.0).unwrap();
        // Width-bound: 1000 m across 1000 px.
        assert!(
            (projection.scale() - 1.0).abs() < 1e-3,
            "{}",
            projection.scale()
        );
        let (x0, y0) = projection.project(points[0]);
        let (x1, _) = projection.project(points[1]);
        let (_, y3) = projection.project(points[3]);
        assert!((x1 - x0 - 1000.0).abs() < 1.0, "east is +x");
        assert!((y0 - y3 - 500.0).abs() < 1.0, "north is up");
        // Metres are east/north of the centroid.
        let (east, north) = projection.metres(points[2]);
        assert!((east - 500.0).abs() < 1.0 && (north - 250.0).abs() < 1.0);
    }

    #[test]
    fn a_long_circuit_turns_to_fill_a_wide_rectangle() {
        // 1000 m north-south, 100 m wide, in a 400 x 200 px box: north-up
        // it would be 20 px wide.
        let (lon0, lat0): (f64, f64) = (-83.81, 34.14);
        let dlat = 1000.0 / METERS_PER_DEGREE;
        let dlon = 100.0 / (METERS_PER_DEGREE * lat0.to_radians().cos());
        let points = vec![
            GeoPoint::new(lon0, lat0),
            GeoPoint::new(lon0 + dlon, lat0),
            GeoPoint::new(lon0 + dlon, lat0 + dlat),
            GeoPoint::new(lon0, lat0 + dlat),
        ];
        let projection = MapProjection::fit(points.clone(), 400.0, 200.0, 0.0).unwrap();
        assert!(!projection.is_north_up());
        assert!(projection.scale() > 0.3, "{}", projection.scale());
        for point in points {
            let (x, y) = projection.project(point);
            assert!((-1e-6..=400.0 + 1e-6).contains(&x), "{x}");
            assert!((-1e-6..=200.0 + 1e-6).contains(&y), "{y}");
        }
        // A square box keeps it north-up.
        let square = MapProjection::fit(rectangle(), 400.0, 400.0, 0.0).unwrap();
        assert!(square.is_north_up());
    }

    #[test]
    fn fit_preserves_aspect_and_centres() {
        let points = rectangle();
        let projection = MapProjection::fit(points.clone(), 400.0, 400.0, 20.0).unwrap();
        let xs: Vec<(f64, f64)> = points.iter().map(|p| projection.project(*p)).collect();
        let min_x = xs.iter().map(|p| p.0).fold(f64::INFINITY, f64::min);
        let max_x = xs.iter().map(|p| p.0).fold(f64::NEG_INFINITY, f64::max);
        let min_y = xs.iter().map(|p| p.1).fold(f64::INFINITY, f64::min);
        let max_y = xs.iter().map(|p| p.1).fold(f64::NEG_INFINITY, f64::max);
        // The long side fills the padded width; the short side keeps 2:1.
        assert!((min_x - 20.0).abs() < 1e-6 && (max_x - 380.0).abs() < 1e-6);
        assert!(((max_x - min_x) / (max_y - min_y) - 2.0).abs() < 1e-3);
        // Vertically centred.
        assert!(((min_y + max_y) * 0.5 - 200.0).abs() < 1e-6);
        // Degenerate input has no projection.
        assert!(MapProjection::fit([points[0]], 400.0, 400.0, 20.0).is_none());
        assert!(MapProjection::fit(points.clone(), 30.0, 400.0, 20.0).is_none());
        assert!(
            MapProjection::fit([GeoPoint::new(f64::NAN, 1.0), points[0]], 400.0, 400.0, 0.0)
                .is_none()
        );
    }

    #[test]
    fn slope_signs_follow_the_delta_with_a_deadband() {
        // Level, then losing 10 ms per sample, then gaining.
        let mut delta = vec![0.0; 40];
        for i in 40..80 {
            delta.push(f64::from(i - 39) * 0.01);
        }
        let top = *delta.last().unwrap();
        for i in 80..120 {
            delta.push(top - f64::from(i - 79) * 0.01);
        }
        let signs = delta_slope_signs(&delta, 3, 0.002);
        assert_eq!(signs[10], SlopeSign::Level);
        assert_eq!(signs[60], SlopeSign::Loss);
        assert_eq!(signs[100], SlopeSign::Gain);
        let noisy = [0.0, 0.0005, 0.0, 0.0005, 0.0];
        assert!(
            delta_slope_signs(&noisy, 1, 0.002)
                .iter()
                .all(|s| *s == SlopeSign::Level)
        );
        assert!(
            delta_slope_signs(&[f64::NAN, 1.0], 1, 0.002)
                .iter()
                .all(|s| *s == SlopeSign::Level)
        );
    }

    #[test]
    fn nearest_point_respects_the_distance() {
        let points = [(0.0, 0.0), (10.0, 0.0), (20.0, 0.0)];
        assert_eq!(nearest_point(&points, 11.0, 2.0, 5.0), Some(1));
        assert_eq!(nearest_point(&points, 11.0, 20.0, 5.0), None);
    }

    #[test]
    #[expect(
        clippy::cast_precision_loss,
        reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
    )]
    fn primary_runs_split_by_sign_and_share_boundaries() {
        let n = 200;
        let lat: Arc<[f64]> = (0..n).map(|i| 34.15 + i as f64 * 1e-5).collect();
        let lon: Arc<[f64]> = (0..n).map(|_| -83.81).collect();
        let delta: Arc<[f64]> = (0..n)
            .map(|i| {
                if i < 100 {
                    i as f64 * 0.01
                } else {
                    1.0 - (i - 100) as f64 * 0.01
                }
            })
            .collect();
        let data = TrackMapData::new()
            .with_primary(Some(GpsTrack::new(lat, lon)))
            .with_delta(Some(delta));
        let mut geometry = MapGeometry::default();
        geometry.prepare(&data, 300.0, 300.0, 2.0);
        assert!(geometry.loss.vertex_count() > 0);
        assert!(geometry.gain.vertex_count() > 0);
        assert_eq!(geometry.targets.len(), n);
        assert_eq!(geometry.builds, 1);
        // Same key: no rebuild.
        geometry.prepare(&data, 300.0, 300.0, 2.0);
        assert_eq!(geometry.builds, 1);
        geometry.prepare(&data, 301.0, 300.0, 2.0);
        assert_eq!(geometry.builds, 2);
    }

    #[test]
    #[expect(
        clippy::cast_possible_truncation,
        reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
    )]
    fn heat_levels_ramp_losses_and_keep_gains_quiet() {
        let mut rate: Vec<f64> = (0..100).map(|i| f64::from(i) * 1e-4).collect();
        rate.extend([-0.01, f64::NAN, 1.0]);
        let levels = heat_levels(&rate, HEAT_LEVELS);
        assert_eq!(levels.len(), rate.len());
        assert_eq!(levels[0], 0, "no loss is quiet");
        assert_eq!(levels[100], 0, "a gain is quiet");
        assert_eq!(levels[101], 0, "unknown is quiet");
        assert_eq!(levels[102], (HEAT_LEVELS - 1) as u8, "a spike clamps");
        assert_eq!(levels[99], (HEAT_LEVELS - 1) as u8);
        assert_eq!(levels[20], 0, "the background loss is quiet");
        assert!(levels[60] > 0 && levels[60] < (HEAT_LEVELS - 1) as u8);
        assert!(levels.windows(2).take(99).all(|w| w[0] <= w[1]));
        // A flat loss ramps from zero: every losing station is hot.
        assert!(
            heat_levels(&[0.01; 8], HEAT_LEVELS)
                .iter()
                .all(|l| usize::from(*l) == HEAT_LEVELS - 1)
        );
        assert!(
            heat_levels(&[0.0, -1.0], HEAT_LEVELS)
                .iter()
                .all(|l| *l == 0)
        );
    }

    #[test]
    #[expect(
        clippy::cast_precision_loss,
        reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
    )]
    fn heat_mode_strokes_the_ramp_and_drops_the_reference() {
        let n = 200;
        let lat: Arc<[f64]> = (0..n).map(|i| 34.15 + i as f64 * 1e-5).collect();
        let lon: Arc<[f64]> = (0..n).map(|_| -83.81).collect();
        let rate: Arc<[f64]> = (0..n).map(|i| if i < 100 { 0.0 } else { 0.01 }).collect();
        let data = TrackMapData::new()
            .with_centerline(rectangle())
            .with_primary(Some(GpsTrack::new(lat.clone(), lon.clone())))
            .with_reference(Some(GpsTrack::new(lat, lon)))
            .with_heat(Some(rate));
        assert!(data.is_heat());
        let mut geometry = MapGeometry::default();
        geometry.prepare(&data, 300.0, 300.0, 2.0);
        assert!(geometry.heat[0].vertex_count() > 0);
        assert!(geometry.heat[HEAT_LEVELS - 1].vertex_count() > 0);
        assert!(geometry.reference.is_empty() && geometry.centerline.is_empty());
        assert!(geometry.level.is_empty());
        assert_eq!(geometry.targets.len(), n);
        // A heat array of another length is ignored.
        let short = TrackMapData::new()
            .with_primary(data.primary().cloned())
            .with_heat(Some(Arc::from([0.0; 3].as_slice())));
        assert!(!short.is_heat());
    }

    #[test]
    fn labels_go_outward_then_around_and_never_overlap() {
        let area = Bounds::new(point(px(0.), px(0.)), size(px(200.), px(100.)));
        let label = size(px(30.), px(12.));
        let centre = point(px(100.), px(50.));
        let none = (0.0, 0.0);
        let right = place_label(centre, px(2.), label, area, &[], none).unwrap();
        assert!(right.origin.x > centre.x);
        let left = place_label(centre, px(2.), label, area, &[right], none).unwrap();
        assert!(left.bottom_right().x < centre.x);
        let above = place_label(centre, px(2.), label, area, &[right, left], none).unwrap();
        assert!(above.bottom_right().y < centre.y);
        let below = place_label(centre, px(2.), label, area, &[right, left, above], none).unwrap();
        assert!(below.origin.y > centre.y);
        assert_eq!(
            place_label(
                centre,
                px(2.),
                label,
                area,
                &[right, left, above, below],
                none
            ),
            None
        );
        // Outward first: a dot on the upper edge of the drawing labels above.
        let up = place_label(centre, px(2.), label, area, &[], (0.0, -1.0)).unwrap();
        assert!(up.bottom_right().y < centre.y);
        assert!((up.center().x - centre.x).abs() < px(0.5));
        // At the right edge only the left side fits.
        let edge = point(px(190.), px(50.));
        let placed = place_label(edge, px(2.), label, area, &[], (1.0, 0.0)).unwrap();
        assert!(placed.bottom_right().x < edge.x);
    }

    #[test]
    fn a_gps_gap_at_the_midpoint_uses_the_nearest_fix_in_the_zone() {
        let n = 101;
        let mut lat: Vec<f64> = (0..n).map(|i| 34.15 + f64::from(i) * 1e-5).collect();
        let lon: Arc<[f64]> = (0..n).map(|_| -83.81).collect();
        for value in &mut lat[45..=52] {
            *value = f64::NAN;
        }
        let track = GpsTrack::new(lat.into(), lon);
        assert_eq!(track.position_at(0.5), None);
        let near = track.position_near(0.5, 0.40, 0.60).unwrap();
        assert!((near.lat - (34.15 + 53.0 * 1e-5)).abs() < 1e-9, "{near:?}");
        // Nothing valid inside the zone: no fabricated position.
        assert_eq!(track.position_near(0.5, 0.46, 0.51), None);
    }

    #[test]
    fn map_corners_draw_their_short_names() {
        let corner = MapCorner::new(1, "Turn 10A", GeoPoint::new(0.0, 0.0));
        assert_eq!(corner.short.as_ref(), "T10A");
        let corner = MapCorner::new(2, "Esses", GeoPoint::new(0.0, 0.0));
        assert_eq!(corner.short.as_ref(), "Esses");
    }

    #[test]
    fn every_mesh_chunk_carries_the_bounds_of_its_triangles() {
        // GPUI clips a path to its bounds: zero bounds paint nothing.
        let n = 200;
        let lat: Arc<[f64]> = (0..n).map(|i| 34.15 + f64::from(i) * 1e-5).collect();
        let lon: Arc<[f64]> = (0..n).map(|i| -83.81 + f64::from(i) * 1e-5).collect();
        let data = TrackMapData::new()
            .with_centerline(rectangle())
            .with_primary(Some(GpsTrack::new(lat.clone(), lon.clone())))
            .with_reference(Some(GpsTrack::new(lat, lon)));
        let mut geometry = MapGeometry::default();
        geometry.prepare(&data, 300.0, 200.0, 1.0);
        for buffer in [&geometry.centerline, &geometry.reference, &geometry.level] {
            assert!(!buffer.is_empty());
            for chunk in buffer.chunks() {
                assert!(chunk.bounds.size.width > px(1.) && chunk.bounds.size.height > px(1.));
                let (min, max) = (chunk.bounds.origin, chunk.bounds.bottom_right());
                for v in &chunk.vertices {
                    let p = v.xy_position;
                    assert!(p.x >= min.x && p.y >= min.y && p.x <= max.x && p.y <= max.y);
                }
            }
        }
    }
}
