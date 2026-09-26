//! Driving events on a lap: brake onsets, lift-offs, gear changes, and the
//! corner notes placed at their corner.
//!
//! [`detect`] scans one [`UnifiedLap`] once; [`analysis_events`] runs it on
//! both laps of an [`Analysis`], places the reference's events on the
//! primary lap through the shared station map (the one map every surface
//! uses) and adds each corner's [`crate::corners::CornerNote`]s at the zone
//! start. Thresholds are named constants: the numbers are the product
//! decision. Plain data, no UI.

use crate::monotonic::interpolate_fraction;
use crate::session::Analysis;
use crate::unify::UnifiedLap;

/// Brake pressure (bar; pedal percent on pedal-only loggers) at which a
/// brake application starts.
pub const BRAKE_ONSET_BAR: f64 = 5.0;
/// Time the brake must have been released (below [`BRAKE_ONSET_BAR`])
/// before a new application counts as a new brake point, seconds.
pub const BRAKE_REARM_SECONDS: f64 = 0.5;
/// A lift starts from at least this throttle (0–1)...
pub const LIFT_FROM_THROTTLE: f64 = 0.95;
/// ...and counts once the throttle falls below this.
pub const LIFT_TO_THROTTLE: f64 = 0.80;
/// A new gear counts once it has been held this long, seconds (a logger's
/// gear channel can flicker through a shift).
pub const GEAR_HOLD_SECONDS: f64 = 0.2;

/// What happened.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum LapEventKind {
    /// The brake pressure crossed [`BRAKE_ONSET_BAR`] after a release.
    BrakeOnset,
    /// The throttle fell from full ([`LIFT_FROM_THROTTLE`]) to below
    /// [`LIFT_TO_THROTTLE`].
    LiftOff,
    Upshift,
    Downshift,
    /// A corner note of the analysis, at its corner.
    Note,
}

impl LapEventKind {
    /// The lane channel an event belongs on (`delta` for notes).
    pub fn channel(self) -> &'static str {
        match self {
            Self::BrakeOnset => "brake",
            Self::LiftOff => "throttle",
            Self::Upshift | Self::Downshift => "gear",
            Self::Note => "delta",
        }
    }
}

/// Which lap an event is on.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum EventLap {
    Primary,
    Reference,
}

/// One event of one lap, placed on the primary lap (plain data).
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct LapEvent {
    pub kind: LapEventKind,
    pub lap: EventLap,
    /// Primary lap fraction. A reference event is mapped through the
    /// analysis's station map.
    pub fraction: f64,
    /// Metres from the primary lap's start at `fraction`.
    pub distance: f64,
    /// Interface copy: `Brake`, `Lift`, `Up to 5`, `Down to 3`, or the
    /// note's sentence prefixed by the corner (`T5: Brake 12 m later.`).
    pub label: String,
    /// The gear after a shift.
    pub gear: Option<i32>,
    /// The corner zone id of a note.
    pub corner: Option<String>,
}

/// One event of [`detect`], on its own lap's fraction axis.
#[derive(Debug, Clone, Copy, PartialEq)]
#[non_exhaustive]
pub struct Detected {
    pub kind: LapEventKind,
    /// Fraction of the lap's own samples.
    pub fraction: f64,
    pub gear: Option<i32>,
}

fn fraction_of(index: usize, count: usize) -> f64 {
    if count < 2 {
        0.0
    } else {
        index as f64 / (count - 1) as f64
    }
}

/// Brake onsets, lift-offs and gear changes of one lap, in lap order within
/// each kind. One pass per channel; a missing channel detects nothing.
pub fn detect(lap: &UnifiedLap) -> Vec<Detected> {
    let count = lap.time.len();
    let mut out = Vec::new();
    if count < 2 {
        return out;
    }
    let time = |i: usize| lap.time[i];

    // Brake onsets: a release of at least BRAKE_REARM_SECONDS, then a
    // crossing. Braking already under way at the lap start is the previous
    // lap's brake point.
    if lap.brake.len() == count {
        let mut released_since: Option<f64> = None;
        for (i, &brake) in lap.brake.iter().enumerate() {
            if !brake.is_finite() {
                continue;
            }
            if brake < BRAKE_ONSET_BAR {
                released_since.get_or_insert(time(i));
            } else if let Some(since) = released_since.take()
                && time(i) - since >= BRAKE_REARM_SECONDS
            {
                out.push(Detected {
                    kind: LapEventKind::BrakeOnset,
                    fraction: fraction_of(i, count),
                    gear: None,
                });
            }
        }
    }

    // Lift-offs: armed at full throttle, placed where the throttle last
    // was full once it has fallen below LIFT_TO_THROTTLE.
    if lap.throttle.len() == count {
        let mut last_full: Option<usize> = None;
        for (i, &throttle) in lap.throttle.iter().enumerate() {
            if !throttle.is_finite() {
                continue;
            }
            if throttle >= LIFT_FROM_THROTTLE {
                last_full = Some(i);
            } else if throttle < LIFT_TO_THROTTLE
                && let Some(full) = last_full.take()
            {
                out.push(Detected {
                    kind: LapEventKind::LiftOff,
                    fraction: fraction_of(full, count),
                    gear: None,
                });
            }
        }
    }

    // Gear changes: a new (non-neutral) gear held for GEAR_HOLD_SECONDS,
    // placed where it was first engaged.
    if lap.gear.len() == count {
        let mut current: Option<i32> = None;
        let mut candidate: Option<(i32, usize)> = None;
        for (i, &gear) in lap.gear.iter().enumerate() {
            if gear <= 0 {
                candidate = None;
                continue;
            }
            let Some(held) = current else {
                current = Some(gear);
                continue;
            };
            if gear == held {
                candidate = None;
                continue;
            }
            let start = match candidate {
                Some((g, start)) if g == gear => start,
                _ => {
                    candidate = Some((gear, i));
                    i
                }
            };
            if time(i) - time(start) + 1e-9 >= GEAR_HOLD_SECONDS {
                out.push(Detected {
                    kind: if gear > held {
                        LapEventKind::Upshift
                    } else {
                        LapEventKind::Downshift
                    },
                    fraction: fraction_of(start, count),
                    gear: Some(gear),
                });
                current = Some(gear);
                candidate = None;
            }
        }
    }
    out
}

fn label(kind: LapEventKind, gear: Option<i32>) -> String {
    match (kind, gear) {
        (LapEventKind::BrakeOnset, _) => "Brake".into(),
        (LapEventKind::LiftOff, _) => "Lift".into(),
        (LapEventKind::Upshift, Some(gear)) => format!("Up to {gear}"),
        (LapEventKind::Downshift, Some(gear)) => format!("Down to {gear}"),
        (LapEventKind::Upshift, None) => "Upshift".into(),
        (LapEventKind::Downshift, None) => "Downshift".into(),
        (LapEventKind::Note, _) => String::new(),
    }
}

/// Every event of `analysis`, ordered by primary fraction: both laps'
/// brake onsets, lifts and shifts (the reference's through the shared map)
/// and every corner note at its zone start.
pub fn analysis_events(analysis: &Analysis) -> Vec<LapEvent> {
    let primary = analysis.primary().unified();
    let distance_at = |fraction: f64| -> f64 {
        match primary.distance.first() {
            Some(origin) => interpolate_fraction(&primary.distance, fraction) - origin,
            None => f64::NAN,
        }
    };
    let mut events = Vec::new();
    let mut push = |detected: Detected, lap: EventLap, fraction: f64| {
        if !fraction.is_finite() {
            return;
        }
        events.push(LapEvent {
            kind: detected.kind,
            lap,
            fraction,
            distance: distance_at(fraction),
            label: label(detected.kind, detected.gear),
            gear: detected.gear,
            corner: None,
        });
    };
    for detected in detect(primary) {
        push(detected, EventLap::Primary, detected.fraction);
    }
    if let (Some(reference), Some(comparison)) = (analysis.reference(), analysis.comparison()) {
        for detected in detect(reference.unified()) {
            let fraction = comparison.primary_fraction_for_compare_fraction(detected.fraction);
            push(detected, EventLap::Reference, fraction);
        }
    }
    for row in analysis.rows() {
        for note in &row.notes {
            let fraction = row.zone.start;
            if !fraction.is_finite() {
                continue;
            }
            events.push(LapEvent {
                kind: LapEventKind::Note,
                lap: EventLap::Primary,
                fraction,
                distance: distance_at(fraction),
                label: format!("{}: {}", row.zone.name, note.sentence()),
                gear: None,
                corner: Some(row.zone.id.clone()),
            });
        }
    }
    events.sort_by(|a, b| a.fraction.total_cmp(&b.fraction));
    events
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A 20 s lap at 50 Hz with channels given per sample time.
    fn lap(
        brake: impl Fn(f64) -> f64,
        throttle: impl Fn(f64) -> f64,
        gear: impl Fn(f64) -> i32,
    ) -> UnifiedLap {
        let mut lap = UnifiedLap::default();
        for i in 0..1000 {
            let t = i as f64 / 50.0;
            lap.time.push(t);
            lap.distance.push(t * 50.0);
            lap.speed.push(180.0);
            lap.brake.push(brake(t));
            lap.throttle.push(throttle(t));
            lap.gear.push(gear(t));
        }
        lap
    }

    fn of(events: &[Detected], kind: LapEventKind) -> Vec<&Detected> {
        events.iter().filter(|e| e.kind == kind).collect()
    }

    #[test]
    fn brake_onsets_need_a_release_first() {
        // Braking at the lap start (previous lap's), a real stop at 5 s, a
        // 0.2 s release and re-press at 7 s (same application), a new
        // application at 12 s.
        let brake = |t: f64| {
            if t < 1.0
                || (5.0..6.9).contains(&t)
                || (7.1..8.0).contains(&t)
                || (12.0..13.0).contains(&t)
            {
                40.0
            } else {
                0.0
            }
        };
        let events = detect(&lap(brake, |_| 1.0, |_| 5));
        let onsets = of(&events, LapEventKind::BrakeOnset);
        assert_eq!(onsets.len(), 2, "{onsets:?}");
        assert!((onsets[0].fraction - 250.0 / 999.0).abs() < 1e-9);
        assert!((onsets[1].fraction - 600.0 / 999.0).abs() < 1e-9);
    }

    #[test]
    fn a_lift_goes_from_full_to_below_eighty_percent() {
        // Full, a partial dip to 85% (no lift), full, a lift at 10 s.
        let throttle = |t: f64| {
            if (4.0..5.0).contains(&t) {
                0.85
            } else if (10.0..12.0).contains(&t) {
                0.1
            } else {
                1.0
            }
        };
        let events = detect(&lap(|_| 0.0, throttle, |_| 5));
        let lifts = of(&events, LapEventKind::LiftOff);
        assert_eq!(lifts.len(), 1, "{lifts:?}");
        // Placed at the last full-throttle sample, just before 10 s.
        assert!((lifts[0].fraction - 499.0 / 999.0).abs() < 1e-9);
    }

    #[test]
    fn gear_changes_ignore_flicker_and_neutral() {
        // 3 → 4 at 5 s, a one-sample flicker to 5 at 8 s, neutral blips,
        // 4 → 2 at 12 s.
        let gear = |t: f64| {
            if t < 5.0 {
                3
            } else if (8.0..8.02).contains(&t) {
                5
            } else if (10.0..10.1).contains(&t) {
                0
            } else if t < 12.0 {
                4
            } else {
                2
            }
        };
        let events = detect(&lap(|_| 0.0, |_| 1.0, gear));
        let up = of(&events, LapEventKind::Upshift);
        let down = of(&events, LapEventKind::Downshift);
        assert_eq!(up.len(), 1, "{events:?}");
        assert_eq!(up[0].gear, Some(4));
        assert!((up[0].fraction - 250.0 / 999.0).abs() < 1e-9);
        assert_eq!(down.len(), 1, "{events:?}");
        assert_eq!(down[0].gear, Some(2));
        assert!((down[0].fraction - 600.0 / 999.0).abs() < 1e-9);
        assert_eq!(label(LapEventKind::Downshift, Some(2)), "Down to 2");
    }

    #[test]
    fn a_missing_channel_detects_nothing() {
        let mut empty = lap(|_| 0.0, |_| 1.0, |_| 3);
        empty.brake.clear();
        empty.throttle.clear();
        empty.gear.clear();
        assert!(detect(&empty).is_empty());
        assert!(detect(&UnifiedLap::default()).is_empty());
    }
}
