//! The data a trace stack draws: an immutable, `Arc`-shared snapshot built
//! off the UI thread by the session owner.
//!
//! A scene holds sampled arrays on the primary lap's 50 Hz grid (lap
//! fraction `i / (n - 1)`), plus the reference arrays on the reference lap's
//! own grid. The reference is drawn through the shared [`FractionMap`]
//! evaluated per device column: every lane, the cursor readouts, the delta
//! and the video use the same map. A scene's generation is allocated by the
//! scene itself, fresh for every construction and every `with_*` change, so
//! geometry caches can key on it (never on colours) without a caller able to
//! reuse one for different data.

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use gpui_kit::{Hsla, SharedString};

use crate::layout::{LaneSizing, default_height_percent, lane_height_boost};
use crate::scale::{Viewport, value_at_fraction};

/// Primary lap fraction → reference lap fraction.
pub trait FractionMap: Send + Sync {
    fn reference_fraction(&self, primary: f64) -> f64;
}

impl FractionMap for omatrack_core::Comparison {
    fn reference_fraction(&self, primary: f64) -> f64 {
        self.compare_fraction_for_primary_fraction(primary)
    }
}

/// How a lane draws its channel.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum LaneKind {
    #[default]
    Line,
    /// Line plus a gradient fill fading to a transparent baseline.
    Area,
    /// Held values (gear): horizontal then vertical.
    Step,
    /// Cumulative Δt with a gain/loss diverging fill and a zero line.
    Delta,
}

/// Vertical value range of a lane.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct YRange {
    pub min: f64,
    pub max: f64,
}

impl Default for YRange {
    fn default() -> Self {
        Self { min: 0.0, max: 1.0 }
    }
}

impl YRange {
    pub fn new(min: f64, max: f64) -> Self {
        if min.is_finite() && max.is_finite() && max > min {
            Self { min, max }
        } else {
            Self::default()
        }
    }

    pub fn span(&self) -> f64 {
        (self.max - self.min).max(1e-12)
    }

    /// Auto range over finite samples of both laps: padded by 6% (port of
    /// `TraceLaneLayout::rangeFor`), or symmetric about zero with 8% headroom
    /// rounded up to a readable bound ([`nice_ceiling`]), so a Δ lane's
    /// scale reads as "±0.5 s" rather than "±0.4371 s".
    #[expect(
        clippy::neg_cmp_op_on_partial_ord,
        reason = "Negated ordered comparisons deliberately include unordered (NaN) values; preserve that behavior."
    )]
    pub fn auto(primary: &[f64], reference: Option<&[f64]>, symmetric: bool) -> Self {
        let mut min = f64::INFINITY;
        let mut max = f64::NEG_INFINITY;
        for value in primary.iter().chain(reference.unwrap_or(&[]).iter()) {
            if value.is_finite() {
                min = min.min(*value);
                max = max.max(*value);
            }
        }
        if !(max > min) {
            if min.is_finite() && !symmetric {
                return Self::new(min - 0.5, min + 0.5);
            }
            min = 0.0;
            max = 1.0;
        }
        if symmetric {
            let magnitude = min.abs().max(max.abs());
            if magnitude < 1e-6 {
                return Self::new(-1.0, 1.0);
            }
            let bound = nice_ceiling(magnitude * SYMMETRIC_HEADROOM);
            return Self::new(-bound, bound);
        }
        let padding = (max - min) * 0.06;
        Self::new(min - padding, max + padding)
    }
}

/// The smallest Δ span a lane zooms to (s), so a flat stretch does not
/// magnify noise, and the padding around the visible Δ values.
const DELTA_MINIMUM_SPAN: f64 = 0.02;
const DELTA_WINDOW_PADDING: f64 = 0.08;

/// First and last sample index of a uniform lap-fraction grid of `len`
/// samples that `viewport` shows (one sample of margin each side).
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Clamped lap fractions map to indices in resident sample buffers; interpolation intentionally uses f64."
)]
fn visible_indices(len: usize, viewport: Viewport) -> Option<(usize, usize)> {
    let last = (len - 1) as f64;
    let start = (viewport.start.clamp(0.0, 1.0) * last).floor();
    let end = (viewport.end.clamp(0.0, 1.0) * last).ceil();
    if !(start.is_finite() && end.is_finite()) || end < start {
        return None;
    }
    Some((start as usize, (end as usize).min(len - 1)))
}

/// Default peak fill opacities (the fill fades to nothing at the baseline).
///
/// Pedal and area fills stay light so the reference outline reads through
/// them; the Δ gain/loss fill is the lane's message and is stronger.
pub const PEDAL_FILL: f32 = 0.16;
pub const AREA_FILL: f32 = 0.16;
pub const DELTA_FILL: f32 = 0.42;
/// The speed lane's gradient area under the primary line.
pub const SPEED_FILL: f32 = 0.2;

/// Headroom of a symmetric range above its largest magnitude.
const SYMMETRIC_HEADROOM: f64 = 1.08;

/// The smallest 1, 1.5, 2, 2.5, 3, 4, 5, 6 or 8 × 10^k at or above `value`
/// (positive, finite): readable, and never wastes more than a third of the
/// lane.
pub fn nice_ceiling(value: f64) -> f64 {
    if !value.is_finite() || value <= 0.0 {
        return 1.0;
    }
    let decade = 10f64.powf(value.log10().floor());
    for step in [1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0] {
        let bound = step * decade;
        // Tolerate the rounding of `powf` at exact steps.
        if bound >= value * (1.0 - 1e-12) {
            return bound;
        }
    }
    10.0 * decade
}

/// The factor a channel's samples are multiplied by for display: `%` channels stored as
/// a `0..=1` fraction (throttle, driver throttle, clutch) read as percent.
///
/// `max` is the largest sample (or range bound). The one rule for every readout (lane
/// legends, inspector).
pub fn display_scale(unit: &str, max: f64) -> f64 {
    if unit == "%" && max <= 1.5 {
        100.0
    } else {
        1.0
    }
}

/// One channel of the stack.
#[derive(Clone)]
#[non_exhaustive]
pub struct LaneSeries {
    /// Stable channel key (`speed`, `raw:Oil Temp`, `delta`, …).
    pub key: SharedString,
    pub title: SharedString,
    pub unit: SharedString,
    pub kind: LaneKind,
    /// Primary lap samples on its 50 Hz grid.
    pub primary: Arc<[f64]>,
    /// Reference lap samples on the reference grid, drawn through the map.
    pub reference: Option<Arc<[f64]>>,
    pub y_range: YRange,
    /// Neighbouring primary laps, drawn faintly when the viewport runs past
    /// the lap (focused corner near start/finish).
    pub previous: Option<Arc<[f64]>>,
    pub next: Option<Arc<[f64]>>,
    /// The session spread of this channel (Consistency view), on the
    /// primary grid.
    pub spread: Option<Arc<LaneSpread>>,
}

/// Other laps of the primary's session on the primary's 50 Hz grid, and
/// their per-sample envelope: drawn as thin quiet lines behind the primary
/// over a low-alpha min–max band (the Consistency view).
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct LaneSpread {
    /// One series per other lap, primary grid, fastest first.
    pub laps: Vec<Arc<[f64]>>,
    /// Envelope per primary sample (NaN lifts the band).
    pub min: Arc<[f64]>,
    pub max: Arc<[f64]>,
}

impl LaneSpread {
    pub fn new(laps: Vec<Arc<[f64]>>, min: Arc<[f64]>, max: Arc<[f64]>) -> Self {
        Self { laps, min, max }
    }
}

/// What a trace event marks ([`EventMark`]).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum EventMarkKind {
    BrakeOnset,
    LiftOff,
    Upshift,
    Downshift,
    Note,
}

/// One driving event on the traces (the Events view): a tick on its lane
/// with a label on hover.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct EventMark {
    pub kind: EventMarkKind,
    /// Lane channel key the mark sits on (`brake`, `throttle`, `gear`,
    /// `delta`).
    pub channel: SharedString,
    /// On the reference lap (drawn quieter).
    pub reference: bool,
    /// Primary lap fraction.
    pub fraction: f64,
    /// Hover label (`Brake · 1,234 m`).
    pub label: SharedString,
    /// Short label drawn beside the tick when it fits (`T5`, `↓3`,
    /// `+12 m`); the hover label carries the rest.
    pub tag: Option<SharedString>,
}

impl EventMark {
    pub fn new(
        kind: EventMarkKind,
        channel: impl Into<SharedString>,
        reference: bool,
        fraction: f64,
        label: impl Into<SharedString>,
    ) -> Self {
        Self {
            kind,
            channel: channel.into(),
            reference,
            fraction,
            label: label.into(),
            tag: None,
        }
    }

    /// With a short label beside the tick (see [`Self::tag`]).
    pub fn with_tag(mut self, tag: Option<SharedString>) -> Self {
        self.tag = tag;
        self
    }
}

/// Which optional layers the static layer draws (the trace view mode).
/// A change repaints the static layer; lane geometry is kept.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub struct TraceLayers {
    /// Session spread lines and band ([`LaneSeries::spread`]).
    pub consistency: bool,
    /// Event ticks ([`TraceScene::events`]).
    pub events: bool,
    /// Apex callouts on the speed lane (the Lap and Corners views).
    pub apexes: bool,
}

impl Default for TraceLayers {
    fn default() -> Self {
        Self::LAP
    }
}

impl TraceLayers {
    /// No optional layer.
    pub const NONE: Self = Self {
        consistency: false,
        events: false,
        apexes: false,
    };
    /// The lap view: the two laps with their apex callouts.
    pub const LAP: Self = Self {
        consistency: false,
        events: false,
        apexes: true,
    };
    pub fn consistency(mut self, on: bool) -> Self {
        self.consistency = on;
        self
    }
    pub fn events(mut self, on: bool) -> Self {
        self.events = on;
        self
    }
    pub fn apexes(mut self, on: bool) -> Self {
        self.apexes = on;
        self
    }
}

impl LaneSeries {
    pub fn new(
        key: impl Into<SharedString>,
        title: impl Into<SharedString>,
        kind: LaneKind,
        primary: Arc<[f64]>,
    ) -> Self {
        let y_range = YRange::auto(&primary, None, kind == LaneKind::Delta);
        Self {
            key: key.into(),
            title: title.into(),
            unit: SharedString::default(),
            kind,
            primary,
            reference: None,
            y_range,
            previous: None,
            next: None,
            spread: None,
        }
    }

    #[must_use]
    /// The session spread of this channel (see [`LaneSpread`]).
    pub fn with_spread(mut self, spread: Option<Arc<LaneSpread>>) -> Self {
        self.spread = spread;
        self
    }

    #[must_use]
    pub fn with_unit(mut self, unit: impl Into<SharedString>) -> Self {
        self.unit = unit.into();
        self
    }

    /// See [`display_scale`].
    pub fn display_scale(&self) -> f64 {
        display_scale(&self.unit, self.y_range.max)
    }

    /// The range the lane draws in `viewport`. A Δ lane re-ranges to the
    /// samples in view (zero included while it is near them), so its slope
    /// inside a zoomed corner stays readable; every other lane keeps its
    /// whole-lap range.
    /// One pass over the visible samples, no allocation.
    #[expect(
        clippy::neg_cmp_op_on_partial_ord,
        reason = "Negated ordered comparisons deliberately include unordered (NaN) values; preserve that behavior."
    )]
    pub fn range_in(&self, viewport: Viewport) -> YRange {
        if self.kind != LaneKind::Delta || self.primary.len() < 2 {
            return self.y_range;
        }
        let Some((first, last)) = visible_indices(self.primary.len(), viewport) else {
            return self.y_range;
        };
        let (mut min, mut max) = (f64::INFINITY, f64::NEG_INFINITY);
        for value in &self.primary[first..=last] {
            if value.is_finite() {
                min = min.min(*value);
                max = max.max(*value);
            }
        }
        if !(max >= min) {
            return self.y_range;
        }
        // Zero (level with the reference) stays in view while it is near
        // the visible values; far from them it would flatten the slope.
        let distance = if min > 0.0 {
            min
        } else if max < 0.0 {
            -max
        } else {
            0.0
        };
        if distance <= (max - min).max(DELTA_MINIMUM_SPAN) {
            min = min.min(0.0);
            max = max.max(0.0);
        }
        let span = (max - min).max(DELTA_MINIMUM_SPAN);
        let padding = span * DELTA_WINDOW_PADDING;
        if max - min < DELTA_MINIMUM_SPAN {
            // Level in view: centre the minimum span on what is there.
            let middle = 0.5 * (min + max);
            return YRange::new(middle - 0.5 * span - padding, middle + 0.5 * span + padding);
        }
        YRange::new(min - padding, max + padding)
    }

    /// How much the lane's value changes across `viewport` (a Δ lane: the
    /// time gained or lost in view); NaN when not measurable.
    pub fn change_in(&self, viewport: Viewport) -> f64 {
        let start = viewport.start.clamp(0.0, 1.0);
        let end = viewport.end.clamp(0.0, 1.0);
        value_at_fraction(&self.primary, end) - value_at_fraction(&self.primary, start)
    }

    /// Set the reference and re-derive an auto range over both laps.
    #[must_use]
    pub fn with_reference(mut self, reference: Option<Arc<[f64]>>) -> Self {
        self.y_range = YRange::auto(
            &self.primary,
            reference.as_deref(),
            self.kind == LaneKind::Delta,
        );
        self.reference = reference;
        self
    }

    #[must_use]
    pub fn with_y_range(mut self, range: YRange) -> Self {
        self.y_range = range;
        self
    }

    #[must_use]
    pub fn with_neighbours(
        mut self,
        previous: Option<Arc<[f64]>>,
        next: Option<Arc<[f64]>>,
    ) -> Self {
        self.previous = previous;
        self.next = next;
        self
    }

    /// Values at a primary lap fraction: primary, reference through `map`,
    /// and their difference. Held (step) values use the sample in force.
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        clippy::cast_sign_loss,
        reason = "Clamped lap fractions map to indices in resident sample buffers; interpolation intentionally uses f64."
    )]
    pub fn readout(&self, fraction: f64, map: Option<&dyn FractionMap>) -> Readout {
        let sample = |values: &[f64], at: f64| -> f64 {
            if self.kind == LaneKind::Step {
                if values.is_empty() {
                    return f64::NAN;
                }
                let last = values.len() - 1;
                let index = (at.clamp(0.0, 1.0) * last as f64 + 1e-9).floor() as usize;
                values[index.min(last)]
            } else {
                value_at_fraction(values, at)
            }
        };
        if !(0.0..=1.0).contains(&fraction) {
            return Readout::default();
        }
        let primary = sample(&self.primary, fraction);
        let reference = match (&self.reference, self.kind) {
            (Some(values), kind) if kind != LaneKind::Delta => {
                let at = map.map_or(fraction, |m| m.reference_fraction(fraction));
                sample(values, at)
            }
            _ => f64::NAN,
        };
        Readout {
            primary,
            reference,
            delta: primary - reference,
        }
    }
}

/// Values of one channel at the cursor. NaN when unavailable.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct Readout {
    pub primary: f64,
    pub reference: f64,
    pub delta: f64,
}

impl Default for Readout {
    fn default() -> Self {
        Self {
            primary: f64::NAN,
            reference: f64::NAN,
            delta: f64::NAN,
        }
    }
}

/// A corner zone on the primary lap.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct CornerBand {
    /// Stable corner id (the corner number of the track layout).
    pub id: u32,
    /// Short label, e.g. "T5".
    pub label: SharedString,
    pub start: f64,
    pub end: f64,
    /// Time lost (+) or gained through the zone, seconds, from the
    /// analysis's corner row (the one cached delta). `None` without a
    /// reference or when the map does not place time loss on the lap.
    pub delta: Option<f64>,
}

impl CornerBand {
    pub fn new(id: u32, label: impl Into<SharedString>, start: f64, end: f64) -> Self {
        Self {
            id,
            label: label.into(),
            start,
            end,
            delta: None,
        }
    }

    /// The zone's Δt (see [`Self::delta`]); non-finite values mean none.
    pub fn with_delta(mut self, delta: Option<f64>) -> Self {
        self.delta = delta.filter(|d| d.is_finite());
        self
    }
}

/// The slowest point of a corner on the speed lane: where the primary lap's
/// speed bottoms out, its minimum and the reference's minimum through the
/// same zone (km/h, from the analysis's corner rows).
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct Apex {
    /// Primary lap fraction of the minimum.
    pub fraction: f64,
    pub speed: f64,
    pub reference_speed: Option<f64>,
}

impl Apex {
    pub fn new(fraction: f64, speed: f64, reference_speed: Option<f64>) -> Self {
        Self {
            fraction,
            speed,
            reference_speed: reference_speed.filter(|s| s.is_finite()),
        }
    }
}

/// A named group of corners (a complex), drawn as a bracket.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct ComplexBand {
    pub name: SharedString,
    pub start: f64,
    pub end: f64,
}

impl ComplexBand {
    pub fn new(name: impl Into<SharedString>, start: f64, end: f64) -> Self {
        Self {
            name: name.into(),
            start,
            end,
        }
    }
}

/// Source of scene generations; 0 is left to the empty default scene.
static NEXT_GENERATION: AtomicU64 = AtomicU64::new(1);

fn next_generation() -> u64 {
    NEXT_GENERATION.fetch_add(1, Ordering::Relaxed)
}

/// Everything a trace stack draws for one primary/reference pair.
///
/// Immutable once built: construct it with [`TraceScene::new`] and the
/// `with_*` builders, share it as an `Arc`, and build a new scene for new
/// data (a lap swap, a new reference, a manual offset's new map). Each
/// builder step allocates a new [`TraceScene::generation`].
#[derive(Clone, Default)]
#[non_exhaustive]
pub struct TraceScene {
    generation: u64,
    pub(crate) lanes: Vec<LaneSeries>,
    pub(crate) map: Option<Arc<dyn FractionMap>>,
    /// Primary lap distance (m) and time (s), for the axis.
    pub(crate) distance_m: Arc<[f64]>,
    pub(crate) time_s: Arc<[f64]>,
    pub(crate) corners: Vec<CornerBand>,
    pub(crate) complexes: Vec<ComplexBand>,
    /// Labels of the neighbouring laps shown past the lap edges ("L8").
    pub(crate) previous_label: Option<SharedString>,
    pub(crate) next_label: Option<SharedString>,
    /// The Δ lane is an estimate (LOW alignment confidence): readouts are
    /// marked `≈` and the fill carries no gain/loss colour.
    pub(crate) approximate_delta: bool,
    /// The alignment is only a share of lap time: the Δ lane is a ramp of
    /// the lap-time difference, stated as such in its legend.
    pub(crate) time_share_delta: bool,
    /// Lap labels of the two roles ("L10", "L8"), for the lane legends.
    pub(crate) primary_label: Option<SharedString>,
    pub(crate) reference_label: Option<SharedString>,
    /// Corner apexes, called out on the speed lane.
    pub(crate) apexes: Vec<Apex>,
    /// Driving events of both laps, by fraction (the Events view).
    pub(crate) events: Arc<[EventMark]>,
}

impl TraceScene {
    /// A scene on the primary lap's distance (m) and time (s) arrays.
    pub fn new(distance_m: Arc<[f64]>, time_s: Arc<[f64]>) -> Self {
        Self {
            generation: next_generation(),
            distance_m,
            time_s,
            ..Self::default()
        }
    }
    #[must_use]
    pub fn with_lanes(mut self, lanes: Vec<LaneSeries>) -> Self {
        self.lanes = lanes;
        self.generation = next_generation();
        self
    }
    #[must_use]
    pub fn with_map(mut self, map: Option<Arc<dyn FractionMap>>) -> Self {
        self.map = map;
        self.generation = next_generation();
        self
    }
    #[must_use]
    pub fn with_corners(mut self, corners: Vec<CornerBand>, complexes: Vec<ComplexBand>) -> Self {
        self.corners = corners;
        self.complexes = complexes;
        self.generation = next_generation();
        self
    }
    #[must_use]
    pub fn with_neighbour_labels(
        mut self,
        previous: Option<SharedString>,
        next: Option<SharedString>,
    ) -> Self {
        self.previous_label = previous;
        self.next_label = next;
        self.generation = next_generation();
        self
    }

    /// Mark the Δ lane as approximate (LOW alignment confidence).
    #[must_use]
    pub fn with_approximate_delta(mut self, approximate: bool) -> Self {
        self.approximate_delta = approximate;
        self.generation = next_generation();
        self
    }

    pub fn approximate_delta(&self) -> bool {
        self.approximate_delta
    }

    /// Mark the Δ lane as a lap-time-share estimate (no station alignment).
    #[must_use]
    pub fn with_time_share_delta(mut self, time_share: bool) -> Self {
        self.time_share_delta = time_share;
        self.generation = next_generation();
        self
    }

    pub fn time_share_delta(&self) -> bool {
        self.time_share_delta
    }

    /// Lap labels of the primary and the reference ("L10", "L8").
    pub fn with_lap_labels(
        mut self,
        primary: Option<SharedString>,
        reference: Option<SharedString>,
    ) -> Self {
        self.primary_label = primary;
        self.reference_label = reference;
        self.generation = next_generation();
        self
    }

    /// Driving events, sorted by fraction here.
    pub fn with_events(mut self, mut events: Vec<EventMark>) -> Self {
        events.sort_by(|a, b| a.fraction.total_cmp(&b.fraction));
        self.events = events.into();
        self.generation = next_generation();
        self
    }

    pub fn primary_label(&self) -> Option<&SharedString> {
        self.primary_label.as_ref()
    }

    pub fn reference_label(&self) -> Option<&SharedString> {
        self.reference_label.as_ref()
    }

    /// Corner apexes for the speed lane's callouts.
    pub fn with_apexes(mut self, apexes: Vec<Apex>) -> Self {
        self.apexes = apexes;
        self.generation = next_generation();
        self
    }

    pub fn events(&self) -> &Arc<[EventMark]> {
        &self.events
    }

    /// Attach session spreads by lane key (`None` clears a lane's). A new
    /// generation: the lanes' geometry is rebuilt once.
    pub fn with_spreads(mut self, spread: impl Fn(&str) -> Option<Arc<LaneSpread>>) -> Self {
        for lane in &mut self.lanes {
            lane.spread = spread(&lane.key).filter(|s| s.min.len() == lane.primary.len());
        }
        self.generation = next_generation();
        self
    }

    pub fn apexes(&self) -> &[Apex] {
        &self.apexes
    }

    /// Whether any lane carries a session spread.
    pub fn has_spread(&self) -> bool {
        self.lanes.iter().any(|lane| lane.spread.is_some())
    }

    /// Identity of this scene's contents: unique per construction and
    /// builder step, shared only by clones. Geometry caches key on it.
    pub fn generation(&self) -> u64 {
        self.generation
    }

    pub fn lanes(&self) -> &[LaneSeries] {
        &self.lanes
    }

    /// The primary → reference fraction map, if a reference is shown.
    pub fn map(&self) -> Option<&dyn FractionMap> {
        self.map.as_deref()
    }

    /// Primary lap distance (m) on the lap's sample grid.
    pub fn distance_m(&self) -> &Arc<[f64]> {
        &self.distance_m
    }

    /// Primary lap time (s) on the lap's sample grid.
    pub fn time_s(&self) -> &Arc<[f64]> {
        &self.time_s
    }

    pub fn corners(&self) -> &[CornerBand] {
        &self.corners
    }

    pub fn complexes(&self) -> &[ComplexBand] {
        &self.complexes
    }

    pub fn previous_label(&self) -> Option<&SharedString> {
        self.previous_label.as_ref()
    }

    pub fn next_label(&self) -> Option<&SharedString> {
        self.next_label.as_ref()
    }

    pub fn is_empty(&self) -> bool {
        self.lanes.iter().all(|lane| lane.primary.len() < 2)
    }

    pub fn lane(&self, key: &str) -> Option<&LaneSeries> {
        self.lanes.iter().find(|lane| lane.key.as_ref() == key)
    }

    /// Reference fraction for a primary fraction (identity without a map).
    pub fn reference_fraction(&self, primary: f64) -> f64 {
        self.map
            .as_ref()
            .map_or(primary, |map| map.reference_fraction(primary))
    }

    /// Lap distance (m, from the lap start) at a primary fraction.
    pub fn distance_at(&self, fraction: f64) -> f64 {
        match self.distance_m.first() {
            Some(origin) => value_at_fraction(&self.distance_m, fraction) - origin,
            None => f64::NAN,
        }
    }

    /// Lap time (s, from the lap start) at a primary fraction.
    pub fn time_at(&self, fraction: f64) -> f64 {
        match self.time_s.first() {
            Some(origin) => value_at_fraction(&self.time_s, fraction) - origin,
            None => f64::NAN,
        }
    }
}

/// Per-channel appearance and sizing (`channels.<key>.*` in omatrack.yml).
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct LaneStyle {
    /// Primary-lap colour override (user data).
    pub color: Option<Hsla>,
    /// Reference-lap colour override (user data).
    pub reference_color: Option<Hsla>,
    /// Stroke width in logical pixels (0.5–4).
    pub stroke_width: f32,
    /// Peak fill alpha (0–1); `None` uses the lane kind's default.
    pub fill_opacity: Option<f32>,
    pub sizing: LaneSizing,
}

impl Default for LaneStyle {
    fn default() -> Self {
        Self {
            color: None,
            reference_color: None,
            stroke_width: 1.25,
            fill_opacity: None,
            sizing: LaneSizing::default(),
        }
    }
}

impl LaneStyle {
    /// Omatrack defaults for a channel: [`default_height_percent`] shares,
    /// one lane per channel (brake has its own lane under throttle).
    pub fn default_for(key: &str) -> Self {
        let percent = default_height_percent(key);
        // FIT multiplies the weight by the height share itself.
        let weight = lane_height_boost(key);
        Self {
            sizing: LaneSizing::default()
                .with_height_percent(percent)
                .with_weight(weight),
            ..Self::default()
        }
    }

    #[must_use]
    pub fn with_color(mut self, color: Option<Hsla>) -> Self {
        self.color = color;
        self
    }
    #[must_use]
    pub fn with_reference_color(mut self, color: Option<Hsla>) -> Self {
        self.reference_color = color;
        self
    }
    #[must_use]
    pub fn with_stroke_width(mut self, width: f32) -> Self {
        self.stroke_width = width.clamp(0.5, 4.0);
        self
    }
    #[must_use]
    pub fn with_fill_opacity(mut self, opacity: Option<f32>) -> Self {
        self.fill_opacity = opacity.map(|o| o.clamp(0.0, 1.0));
        self
    }
    #[must_use]
    pub fn with_sizing(mut self, sizing: LaneSizing) -> Self {
        self.sizing = sizing;
        self
    }

    /// Peak fill alpha for a lane kind.
    pub fn fill_for(&self, kind: LaneKind, key: &str) -> f32 {
        self.fill_opacity.unwrap_or(match kind {
            LaneKind::Delta => DELTA_FILL,
            LaneKind::Area => AREA_FILL,
            _ if key == "speed" => SPEED_FILL,
            _ if matches!(key, "throttle" | "brake" | "clutch") => PEDAL_FILL,
            _ => 0.0,
        })
    }
}

/// Styles by channel key; missing keys use [`LaneStyle::default_for`].
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LaneStyles {
    styles: HashMap<SharedString, LaneStyle>,
}

impl LaneStyles {
    pub fn new() -> Self {
        Self::default()
    }
    #[must_use]
    pub fn with(mut self, key: impl Into<SharedString>, style: LaneStyle) -> Self {
        self.styles.insert(key.into(), style);
        self
    }
    pub fn set(&mut self, key: impl Into<SharedString>, style: LaneStyle) {
        self.styles.insert(key.into(), style);
    }
    pub fn get(&self, key: &str) -> LaneStyle {
        self.styles
            .get(key)
            .cloned()
            .unwrap_or_else(|| LaneStyle::default_for(key))
    }
    pub fn get_mut(&mut self, key: &str) -> &mut LaneStyle {
        self.styles
            .entry(SharedString::from(key.to_string()))
            .or_insert_with(|| LaneStyle::default_for(key))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_delta_lane_ranges_to_the_view_with_zero_in_it() {
        // A ramp from 0 to +2 s over the lap.
        let ramp: Arc<[f64]> = (0..=100).map(|i| f64::from(i) * 0.02).collect();
        let lane = LaneSeries::new("delta", "Δt", LaneKind::Delta, ramp);
        let whole = lane.range_in(Viewport::FULL);
        assert!(whole.min <= 0.0 && whole.max >= 2.0, "{whole:?}");
        // The last tenth: +1.8 .. +2.0 s fills the lane; zero is far away.
        let tail = Viewport {
            start: 0.9,
            end: 1.0,
        };
        let range = lane.range_in(tail);
        assert!(range.min > 1.7 && range.min <= 1.8, "{range:?}");
        assert!(range.max >= 2.0 && range.max < 2.1, "{range:?}");
        // Near the start zero is in view.
        let head = lane.range_in(Viewport {
            start: 0.05,
            end: 0.1,
        });
        assert!(head.min <= 0.0 && head.max >= 0.2, "{head:?}");
        assert!((lane.change_in(tail) - 0.2).abs() < 1e-9);
        // Other lanes keep their whole-lap range.
        let speed: Arc<[f64]> = (0..=100).map(f64::from).collect();
        let speed = LaneSeries::new("speed", "Speed", LaneKind::Line, speed);
        assert_eq!(speed.range_in(tail), speed.y_range);
    }

    struct Shift(f64);
    impl FractionMap for Shift {
        fn reference_fraction(&self, primary: f64) -> f64 {
            (primary + self.0).clamp(0.0, 1.0)
        }
    }

    #[test]
    fn readout_goes_through_the_map() {
        let primary: Arc<[f64]> = (0..=100).map(f64::from).collect();
        let reference: Arc<[f64]> = (0..=100).map(|i| f64::from(i) * 2.0).collect();
        let lane = LaneSeries::new("speed", "Speed", LaneKind::Line, primary)
            .with_reference(Some(reference));
        let r = lane.readout(0.5, Some(&Shift(0.1)));
        assert!((r.primary - 50.0).abs() < 1e-9);
        assert!((r.reference - 120.0).abs() < 1e-9);
        assert!((r.delta + 70.0).abs() < 1e-9);
        assert!(lane.readout(1.5, None).primary.is_nan());
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn step_readout_holds_the_value() {
        let gear: Arc<[f64]> = Arc::from(vec![1.0, 2.0, 3.0]);
        let lane = LaneSeries::new("gear", "Gear", LaneKind::Step, gear);
        assert_eq!(lane.readout(0.49, None).primary, 1.0);
        assert_eq!(lane.readout(0.5, None).primary, 2.0);
        assert_eq!(lane.readout(1.0, None).primary, 3.0);
    }

    #[test]
    fn auto_ranges() {
        let r = YRange::auto(&[0.0, 10.0, f64::NAN], None, false);
        assert!((r.min + 0.6).abs() < 1e-9 && (r.max - 10.6).abs() < 1e-9);
        let s = YRange::auto(&[-2.0, 1.0], None, true);
        assert_eq!((s.min, s.max), (-2.5, 2.5));
        let small = YRange::auto(&[-0.12, 0.431], None, true);
        assert!((small.max - 0.5).abs() < 1e-12 && (small.min + 0.5).abs() < 1e-12);
        let flat = YRange::auto(&[3.0, 3.0], None, false);
        assert!(flat.max > flat.min);
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn nice_ceilings() {
        for (value, bound) in [
            (0.9, 1.0),
            (1.0, 1.0),
            (1.3, 1.5),
            (2.2, 2.5),
            (2.64, 3.0),
            (4.5, 5.0),
        ] {
            assert!((nice_ceiling(value) - bound).abs() < 1e-12, "{value}");
        }
        assert!((nice_ceiling(0.037) - 0.04).abs() < 1e-12);
        assert!((nice_ceiling(8.5) - 10.0).abs() < 1e-12);
        assert_eq!(nice_ceiling(f64::NAN), 1.0);
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn default_styles() {
        let styles = LaneStyles::new();
        assert!(!styles.get("brake").sizing.combine_with_previous);
        assert_eq!(
            styles.get("speed").sizing.height_percent,
            default_height_percent("speed")
        );
        assert_eq!(
            styles.get("throttle").fill_for(LaneKind::Line, "throttle"),
            PEDAL_FILL
        );
        assert_eq!(
            styles.get("speed").fill_for(LaneKind::Line, "speed"),
            SPEED_FILL
        );
        assert_eq!(styles.get("rpm").fill_for(LaneKind::Line, "rpm"), 0.0);
    }
}
