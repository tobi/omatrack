//! Corner checks: the driver-facing notes a corner comparison produces.
//!
//! Port of the analyzers in `CornerAnalysis.cpp`, which follow ac-tracer's
//! `lib/windows/corner_analysis.lua` (the spec for what a corner comparison
//! should say). Each check is O(1) over precomputed [`CornerMetrics`]
//! scalars; every scan of the sample arrays happened once in
//! `measure_corner`. Registration order is display order.
//!
//! Deliberate deviations from the Lua, kept from the C++: the downshift
//! reaction compares against the reference instead of an absolute 5 m, and
//! the brake-pressure check requires a real brake zone on one of the laps.

use super::metrics::{BLIP_SECONDS, BRAKE_ZONE_BAR, CornerMetrics};
use crate::unify::UnifiedLap;

/// How loud a note is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum NoteSeverity {
    Info,
    Warning,
    Error,
}

impl NoteSeverity {
    /// Lowercase name (`info`, `warning`, `error`).
    pub fn name(self) -> &'static str {
        match self {
            Self::Info => "info",
            Self::Warning => "warning",
            Self::Error => "error",
        }
    }
}

/// One driver-facing observation about a corner.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CornerNote {
    /// Stable check id, for tests and filtering.
    pub id: &'static str,
    pub text: String,
    pub severity: NoteSeverity,
}

/// Inputs to one corner's checks.
#[derive(Debug, Clone)]
pub struct CornerContext<'a> {
    pub primary: &'a UnifiedLap,
    /// `None` for a single lap.
    pub reference: Option<&'a UnifiedLap>,
    pub primary_metrics: CornerMetrics,
    pub reference_metrics: CornerMetrics,
    /// Time lost (+) / gained across the corner, from the cached delta.
    pub time_delta: f64,
    pub entry_time_delta: f64,
    pub exit_time_delta: f64,
    /// Along-track metres after the shared station map; + means the primary
    /// event is later. NaN without a map (checks fall back to each lap's own
    /// metres-from-zone-start).
    pub brake_point_delta: f64,
    pub turn_in_delta: f64,
    pub throttle_point_delta: f64,
}

impl<'a> CornerContext<'a> {
    pub fn new(primary: &'a UnifiedLap, primary_metrics: CornerMetrics) -> Self {
        Self {
            primary,
            reference: None,
            primary_metrics,
            reference_metrics: CornerMetrics::default(),
            time_delta: 0.0,
            entry_time_delta: 0.0,
            exit_time_delta: 0.0,
            brake_point_delta: f64::NAN,
            turn_in_delta: f64::NAN,
            throttle_point_delta: f64::NAN,
        }
    }

    pub fn comparing(&self) -> bool {
        self.reference.is_some() && self.reference_metrics.valid
    }
}

/// One check: stateless, cheap, and free to say nothing.
pub trait CornerCheck: Send + Sync {
    fn id(&self) -> &'static str;
    /// Checks that only describe the primary lap return false.
    fn requires_reference(&self) -> bool {
        true
    }
    fn analyze(&self, context: &CornerContext<'_>, notes: &mut Vec<CornerNote>);
}

/// vsnprintf into a 192-byte buffer: at most 191 bytes survive.
fn text(value: String) -> String {
    if value.len() <= 191 {
        return value;
    }
    let mut end = 191;
    while !value.is_char_boundary(end) {
        end -= 1;
    }
    value[..end].to_string()
}

fn note(id: &'static str, value: String, severity: NoteSeverity) -> CornerNote {
    CornerNote {
        id,
        text: text(value),
        severity,
    }
}

fn aligned_or_raw(aligned: f64, primary: f64, reference: f64) -> f64 {
    if aligned.is_finite() {
        return aligned;
    }
    if !primary.is_finite() || !reference.is_finite() {
        return f64::NAN;
    }
    primary - reference
}

macro_rules! check {
    ($name:ident, $id:literal, $needs_reference:literal, |$ctx:ident, $notes:ident| $body:block) => {
        struct $name;
        impl CornerCheck for $name {
            fn id(&self) -> &'static str {
                $id
            }
            fn requires_reference(&self) -> bool {
                $needs_reference
            }
            fn analyze(&self, $ctx: &CornerContext<'_>, $notes: &mut Vec<CornerNote>) $body
        }
    };
}

check!(EntrySpeed, "entry_speed", true, |c, notes| {
    let delta = c.primary_metrics.entry_speed - c.reference_metrics.entry_speed;
    if delta.abs() < 10.0 {
        return;
    }
    notes.push(note(
        "entry_speed",
        crate::sprintf!(
            "entry %.0f km/h %s",
            delta.abs(),
            if delta > 0.0 { "faster" } else { "slower" }
        ),
        NoteSeverity::Info,
    ));
});

check!(Steering, "steering_input", true, |c, notes| {
    let delta = c.primary_metrics.max_steering - c.reference_metrics.max_steering;
    if delta.abs() <= 10.0 {
        return;
    }
    notes.push(note(
        "steering_input",
        crate::sprintf!(
            "%.0f° %s steering",
            delta.abs(),
            if delta > 0.0 { "more" } else { "less" }
        ),
        NoteSeverity::Info,
    ));
});

check!(Gear, "gear_usage", true, |c, notes| {
    let delta = c.primary_metrics.min_gear - c.reference_metrics.min_gear;
    if delta == 0 || c.primary_metrics.min_gear <= 0 || c.reference_metrics.min_gear <= 0 {
        return;
    }
    let magnitude = delta.abs();
    notes.push(note(
        "gear_usage",
        crate::sprintf!(
            "%d gear%s %s",
            magnitude,
            if magnitude > 1 { "s" } else { "" },
            if delta > 0 { "higher" } else { "lower" }
        ),
        NoteSeverity::Info,
    ));
});

check!(Coasting, "coasting", true, |c, notes| {
    let primary = c.primary_metrics.coast_meters;
    let reference = c.reference_metrics.coast_meters;
    if !primary.is_finite() || !reference.is_finite() {
        return;
    }
    let delta = primary - reference;
    if delta.abs() < 15.0 {
        return;
    }
    notes.push(note(
        "coasting",
        crate::sprintf!(
            "%.0fm %s coasting",
            delta.abs(),
            if delta > 0.0 { "more" } else { "less" }
        ),
        NoteSeverity::Info,
    ));
});

check!(TurnIn, "turn_in", true, |c, notes| {
    let delta = aligned_or_raw(
        c.turn_in_delta,
        c.primary_metrics.turn_in_point,
        c.reference_metrics.turn_in_point,
    );
    if !delta.is_finite() || delta.abs() < 10.0 {
        return;
    }
    notes.push(note(
        "turn_in",
        crate::sprintf!(
            "turn-in %.0fm %s than reference",
            delta.abs(),
            if delta > 0.0 { "later" } else { "earlier" }
        ),
        NoteSeverity::Info,
    ));
});

check!(ThrottleTiming, "throttle_timing", true, |c, notes| {
    let delta = aligned_or_raw(
        c.throttle_point_delta,
        c.primary_metrics.throttle_point,
        c.reference_metrics.throttle_point,
    );
    if !delta.is_finite() || delta.abs() < 15.0 {
        return;
    }
    notes.push(note(
        "throttle_timing",
        crate::sprintf!(
            "throttle %.0fm %s",
            delta.abs(),
            if delta < 0.0 { "early" } else { "late" }
        ),
        NoteSeverity::Info,
    ));
});

check!(BrakePressure, "brake_pressure", true, |c, notes| {
    let primary = c.primary_metrics.max_brake;
    let reference = c.reference_metrics.max_brake;
    // A corner neither lap brakes for must stay silent.
    if crate::num::max(primary, reference) < BRAKE_ZONE_BAR || reference <= 0.0 {
        return;
    }
    let delta = primary - reference;
    if delta.abs() / reference <= 0.10 {
        return;
    }
    notes.push(note(
        "brake_pressure",
        crate::sprintf!(
            "%s braking (%.0f vs %.0f bar)",
            if delta < 0.0 { "lighter" } else { "harder" },
            primary,
            reference
        ),
        NoteSeverity::Info,
    ));
});

check!(BrakeRate, "brake_application_rate", true, |c, notes| {
    let primary = c.primary_metrics.brake_rise_rate;
    let reference = c.reference_metrics.brake_rise_rate;
    if !primary.is_finite() || !reference.is_finite() || reference < 50.0 {
        return;
    }
    let ratio = primary / reference;
    // Only coach a ramp less than half as quick and clearly separated.
    if ratio >= 0.5 || reference - primary < 100.0 {
        return;
    }
    notes.push(note(
        "brake_application_rate",
        crate::sprintf!(
            "brake application rate %.0f%% slower than reference",
            (1.0 - ratio) * 100.0
        ),
        NoteSeverity::Info,
    ));
});

check!(TrailBraking, "trail_braking", true, |c, notes| {
    let primary = c.primary_metrics.trail_brake_seconds;
    let reference = c.reference_metrics.trail_brake_seconds;
    if !primary.is_finite() || !reference.is_finite() {
        return;
    }
    let delta = reference - primary;
    if delta < 0.3 || reference < primary * 1.3 {
        return;
    }
    notes.push(note(
        "trail_braking",
        crate::sprintf!("reference trail-brakes %.1fs longer", delta),
        NoteSeverity::Info,
    ));
});

check!(DownshiftReaction, "downshift_reaction", true, |c, notes| {
    let distance = c.primary_metrics.downshift_distance;
    let reaction = c.primary_metrics.downshift_first_ms;
    let reference_reaction = c.reference_metrics.downshift_first_ms;
    if !distance.is_finite() || !reaction.is_finite() || !reference_reaction.is_finite() {
        return;
    }
    // The reference driver defines normal: every LMP2 corner clears the
    // Lua's absolute 5 m.
    let delta = reaction - reference_reaction;
    if delta <= 200.0 || distance <= 5.0 {
        return;
    }
    notes.push(note(
        "downshift_reaction",
        crate::sprintf!(
            "first downshift %.0fms later than reference (%.0fm into braking)",
            delta,
            distance
        ),
        NoteSeverity::Info,
    ));
});

check!(DownshiftTiming, "downshift_timing", true, |c, notes| {
    let primary = c.primary_metrics.downshift_last_ms;
    let reference = c.reference_metrics.downshift_last_ms;
    if !primary.is_finite() || !reference.is_finite() {
        return;
    }
    let delta = primary - reference;
    if delta < 300.0 {
        return;
    }
    notes.push(note(
        "downshift_timing",
        crate::sprintf!("last downshift %.1fs later than reference", delta / 1000.0),
        NoteSeverity::Info,
    ));
});

check!(
    BrakeThrottleOverlap,
    "brake_throttle_overlap",
    false,
    |c, notes| {
        let seconds = c.primary_metrics.brake_throttle_overlap_seconds;
        if seconds <= BLIP_SECONDS {
            return;
        }
        notes.push(note(
            "brake_throttle_overlap",
            crate::sprintf!("throttle while braking (%.1fs)", seconds),
            NoteSeverity::Error,
        ));
    }
);

struct CombinedGrip {
    early: bool,
}

impl CornerCheck for CombinedGrip {
    fn id(&self) -> &'static str {
        if self.early {
            "combined_grip_early"
        } else {
            "combined_grip_mid"
        }
    }
    fn analyze(&self, c: &CornerContext<'_>, notes: &mut Vec<CornerNote>) {
        let (primary, reference) = if self.early {
            (
                c.primary_metrics.combined_grip_early,
                c.reference_metrics.combined_grip_early,
            )
        } else {
            (
                c.primary_metrics.combined_grip_mid,
                c.reference_metrics.combined_grip_mid,
            )
        };
        if !primary.is_finite() || !reference.is_finite() || reference < 0.2 {
            return;
        }
        let deficit = reference - primary;
        if deficit < 0.15 || primary / reference >= 0.88 {
            return;
        }
        notes.push(note(
            self.id(),
            crate::sprintf!(
                "%.2fg less combined grip %s corner",
                deficit,
                if self.early { "early" } else { "mid" }
            ),
            NoteSeverity::Info,
        ));
    }
}

/// The built-in checks in registration (= display) order: the comparisons
/// a driver reads first, then technique, then mistakes.
pub static REGISTRY: &[&dyn CornerCheck] = &[
    &EntrySpeed,
    &TurnIn,
    &Steering,
    &Gear,
    &Coasting,
    &BrakePressure,
    &BrakeRate,
    &TrailBraking,
    &ThrottleTiming,
    &DownshiftReaction,
    &DownshiftTiming,
    &CombinedGrip { early: true },
    &CombinedGrip { early: false },
    &BrakeThrottleOverlap,
];

/// Run every registered check in order.
pub fn run(context: &CornerContext<'_>) -> Vec<CornerNote> {
    run_with(REGISTRY, context)
}

/// Run a given check list (an out-of-tree list can extend [`REGISTRY`]).
pub fn run_with(checks: &[&dyn CornerCheck], context: &CornerContext<'_>) -> Vec<CornerNote> {
    let mut notes = Vec::new();
    if !context.primary_metrics.valid {
        return notes;
    }
    let comparing = context.comparing();
    for check in checks {
        if check.requires_reference() && !comparing {
            continue;
        }
        check.analyze(context, &mut notes);
    }
    notes
}
