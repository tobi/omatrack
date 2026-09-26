//! What a range selection says: the time gained or lost across it and the
//! speed span of both laps inside it.

use omatrack_trace::scale::value_at_fraction;
use omatrack_trace::{Selection, TraceScene};

/// Statistics of one selected range.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RangeStats {
    pub selection: Selection,
    /// Δt accumulated across the range, seconds (positive: primary slower).
    pub dt: Option<f64>,
    /// Primary speed minimum and maximum inside the range.
    pub primary_speed: Option<(f64, f64)>,
    /// Reference speed minimum and maximum over the mapped range.
    pub reference_speed: Option<(f64, f64)>,
    /// Distance covered by the range on the primary lap, metres.
    pub length_m: f64,
}

impl RangeStats {
    /// The statistics of `selection` over `scene` (O(samples in range)).
    pub fn of(scene: &TraceScene, selection: Selection) -> Self {
        let (start, end) = (
            selection.start.clamp(0.0, 1.0),
            selection.end.clamp(0.0, 1.0),
        );
        let dt = scene
            .lane(super::scene_build::DELTA_KEY)
            .map(|lane| {
                value_at_fraction(&lane.primary, end) - value_at_fraction(&lane.primary, start)
            })
            .filter(|dt| dt.is_finite());
        let speed = scene.lane("speed");
        let primary_speed = speed.and_then(|lane| span_of(&lane.primary, start, end));
        let reference_speed = speed.and_then(|lane| {
            let values = lane.reference.as_deref()?;
            let (a, b) = (
                scene.reference_fraction(start),
                scene.reference_fraction(end),
            );
            span_of(values, a.min(b), a.max(b))
        });
        let length_m = scene.distance_at(end) - scene.distance_at(start);
        Self {
            selection,
            dt,
            primary_speed,
            reference_speed,
            length_m,
        }
    }

    /// One line for the toolbar chip and its accessible label.
    pub fn summary(&self) -> String {
        let mut parts = Vec::new();
        if self.length_m.is_finite() {
            parts.push(format!("{:.0} m", self.length_m));
        }
        if let Some(dt) = self.dt {
            parts.push(format!("Δt {dt:+.3} s"));
        }
        let span = |(low, high): (f64, f64)| format!("{low:.0}–{high:.0}");
        match (self.primary_speed, self.reference_speed) {
            (Some(p), Some(r)) => parts.push(format!("P {} · R {} km/h", span(p), span(r))),
            (Some(p), None) => parts.push(format!("{} km/h", span(p))),
            _ => {}
        }
        parts.join(" · ")
    }
}

/// Minimum and maximum of the finite samples between two lap fractions,
/// both ends included.
fn span_of(values: &[f64], start: f64, end: f64) -> Option<(f64, f64)> {
    if values.len() < 2 || !(start.is_finite() && end.is_finite()) {
        return None;
    }
    let last = (values.len() - 1) as f64;
    let from = (start.clamp(0.0, 1.0) * last).floor() as usize;
    let to = ((end.clamp(0.0, 1.0) * last).ceil() as usize).min(values.len() - 1);
    let mut span: Option<(f64, f64)> = None;
    for value in values[from..=to.max(from)].iter().copied() {
        if value.is_finite() {
            span = Some(match span {
                Some((low, high)) => (low.min(value), high.max(value)),
                None => (value, value),
            });
        }
    }
    span
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    use omatrack_trace::{LaneKind, LaneSeries};

    fn scene() -> TraceScene {
        let n = 101;
        let ramp: Arc<[f64]> = (0..n).map(|i| i as f64).collect();
        let delta: Arc<[f64]> = (0..n).map(|i| i as f64 * 0.01).collect();
        let reference: Arc<[f64]> = (0..n).map(|i| 200.0 - i as f64).collect();
        TraceScene::new(ramp.clone(), ramp.clone()).with_lanes(vec![
            LaneSeries::new("delta", "Δt", LaneKind::Delta, delta),
            LaneSeries::new("speed", "Speed", LaneKind::Line, ramp).with_reference(Some(reference)),
        ])
    }

    #[test]
    fn stats_cover_the_selected_range() {
        let stats = RangeStats::of(&scene(), Selection::new(0.2, 0.5));
        assert!((stats.dt.unwrap() - 0.3).abs() < 1e-9);
        assert_eq!(stats.primary_speed, Some((20.0, 50.0)));
        assert_eq!(stats.reference_speed, Some((150.0, 180.0)));
        assert!((stats.length_m - 30.0).abs() < 1e-9);
        assert_eq!(
            stats.summary(),
            "30 m · Δt +0.300 s · P 20–50 · R 150–180 km/h"
        );
    }

    #[test]
    fn without_a_reference_only_the_primary_speaks() {
        let n = 11;
        let ramp: Arc<[f64]> = (0..n).map(|i| i as f64 * 10.0).collect();
        let scene = TraceScene::new(ramp.clone(), ramp.clone()).with_lanes(vec![LaneSeries::new(
            "speed",
            "Speed",
            LaneKind::Line,
            ramp,
        )]);
        let stats = RangeStats::of(&scene, Selection::new(0.0, 1.0));
        assert_eq!(stats.dt, None);
        assert_eq!(stats.reference_speed, None);
        assert_eq!(stats.summary(), "100 m · 0–100 km/h");
    }
}
