//! The two modal editors of the trace workspace, as plain data: the lane
//! resize draft and the corner zone draft. Nothing here touches
//! `omatrack.yml` or the session until the panel saves.

use std::collections::HashMap;

use gpui_kit::SharedString;
use omatrack_core::corners::{CornerZone, ZoneSource};
use omatrack_trace::CornerBand;
use omatrack_trace::layout::lane_height_boost;

use super::scene_build::CornerLink;

/// FIT weights of a lane resize in progress, by lane (root channel) key, in
/// the unit of `channels.<key>.weight` (before the speed boost).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ResizeDraft {
    weights: HashMap<SharedString, f64>,
}

impl ResizeDraft {
    pub fn weights(&self) -> &HashMap<SharedString, f64> {
        &self.weights
    }

    pub fn is_empty(&self) -> bool {
        self.weights.is_empty()
    }

    /// Adopt the lane heights of a divider drag (port of
    /// `TelemetryStore::previewTraceHeights`): each lane keeps its share of
    /// the dragged total, expressed against its configured height share, so
    /// FIT reproduces the drawn heights. Invalid input changes nothing.
    ///
    /// `shares` gives each key's `height_percent / 100`.
    #[expect(
        clippy::neg_cmp_op_on_partial_ord,
        reason = "Negated ordered comparisons deliberately include unordered (NaN) values; preserve that behavior."
    )]
    pub fn apply_heights(
        &mut self,
        keys: &[SharedString],
        heights: &[f64],
        shares: impl Fn(&str) -> f64,
    ) -> bool {
        if keys.is_empty() || keys.len() != heights.len() {
            return false;
        }
        let mut total = 0.0;
        let mut base = 0.0;
        for (ix, (key, height)) in keys.iter().zip(heights).enumerate() {
            if keys[..ix].contains(key) || !height.is_finite() || *height <= 0.0 {
                return false;
            }
            total += height;
            base += shares(key);
        }
        if !(total > 0.0) || !(base > 0.0) {
            return false;
        }
        for (key, height) in keys.iter().zip(heights) {
            let share = shares(key);
            if share > 0.0 {
                let effective = height / total * base / share;
                self.weights
                    .insert(key.clone(), effective / lane_height_boost(key));
            }
        }
        true
    }

    /// Every lane back to weight 1 (the Reset heights preview).
    pub fn reset(&mut self, keys: impl IntoIterator<Item = SharedString>) {
        self.weights.clear();
        for key in keys {
            self.weights.insert(key, 1.0);
        }
    }
}

/// The corner zones as the user dragged them, before saving.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CornerDraft {
    bands: Vec<CornerBand>,
    edited: bool,
}

impl CornerDraft {
    pub fn new(bands: Vec<CornerBand>) -> Self {
        Self {
            bands,
            edited: false,
        }
    }

    pub fn bands(&self) -> &[CornerBand] {
        &self.bands
    }

    /// True once any zone moved.
    pub fn is_edited(&self) -> bool {
        self.edited
    }

    /// Move one zone. Out-of-order or empty ranges are rejected.
    #[expect(
        clippy::float_cmp,
        reason = "Exact equality detects unchanged state or the full-view sentinel; epsilon would hide small changes."
    )]
    pub fn edit(&mut self, band: u32, start: f64, end: f64) -> bool {
        if !(start.is_finite() && end.is_finite() && end > start) {
            return false;
        }
        let Some(corner) = self.bands.iter_mut().find(|corner| corner.id == band) else {
            return false;
        };
        if corner.start == start && corner.end == end {
            return false;
        }
        corner.start = start.clamp(0.0, 1.0);
        corner.end = end.clamp(0.0, 1.0);
        self.edited = true;
        true
    }

    /// The zones to store as the track's override: every analysis zone, the
    /// edited ones moved, all marked as the user's.
    pub fn zones(&self, analysis_zones: &[CornerZone], links: &[CornerLink]) -> Vec<CornerZone> {
        analysis_zones
            .iter()
            .map(|zone| {
                let mut zone = zone.clone();
                let band = links
                    .iter()
                    .find(|link| link.zone.as_ref() == zone.id)
                    .and_then(|link| self.bands.iter().find(|band| band.id == link.band));
                if let Some(band) = band {
                    zone.start = band.start;
                    zone.end = band.end;
                }
                zone.source = ZoneSource::User;
                zone
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn keys(names: &[&str]) -> Vec<SharedString> {
        names.iter().map(|name| SharedString::from(*name)).collect()
    }

    fn share(key: &str) -> f64 {
        match key {
            "speed" => 0.5,
            "throttle" => 0.3,
            _ => 0.05,
        }
    }

    #[test]
    fn heights_become_weights_that_reproduce_them() {
        let mut draft = ResizeDraft::default();
        let names = keys(&["speed", "throttle", "gear"]);
        let heights = [300.0, 200.0, 100.0];
        assert!(draft.apply_heights(&names, &heights, share));
        // FIT allocates weight * boost * share; that must be proportional
        // to the dragged heights.
        let effective: Vec<f64> = names
            .iter()
            .map(|key| draft.weights()[key] * lane_height_boost(key) * share(key))
            .collect();
        for (value, height) in effective.iter().zip(heights) {
            assert!((value / effective[0] - height / heights[0]).abs() < 1e-9);
        }
    }

    #[test]
    fn invalid_heights_leave_the_draft_alone() {
        let mut draft = ResizeDraft::default();
        assert!(!draft.apply_heights(&keys(&["speed"]), &[1.0, 2.0], share));
        assert!(!draft.apply_heights(&keys(&["speed", "speed"]), &[1.0, 2.0], share));
        assert!(!draft.apply_heights(&keys(&["speed", "gear"]), &[1.0, f64::NAN], share));
        assert!(draft.is_empty());
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn reset_sets_every_lane_to_one() {
        let mut draft = ResizeDraft::default();
        draft.reset(keys(&["speed", "gear"]));
        assert_eq!(draft.weights().len(), 2);
        assert!(draft.weights().values().all(|w| *w == 1.0));
    }

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn corner_edits_become_user_zones() {
        let zones = vec![
            CornerZone {
                id: "a".into(),
                name: "T1".into(),
                start: 0.1,
                end: 0.2,
                source: ZoneSource::Atlas,
            },
            CornerZone {
                id: "b".into(),
                name: "T2".into(),
                start: 0.5,
                end: 0.6,
                source: ZoneSource::Atlas,
            },
        ];
        let links = vec![
            CornerLink {
                band: 1,
                zone: "a".into(),
            },
            CornerLink {
                band: 2,
                zone: "b".into(),
            },
        ];
        let mut draft = CornerDraft::new(vec![
            CornerBand::new(1, "T1", 0.1, 0.2),
            CornerBand::new(2, "T2", 0.5, 0.6),
        ]);
        assert!(!draft.is_edited());
        assert!(!draft.edit(2, 0.7, 0.6), "an inverted zone is rejected");
        assert!(draft.edit(2, 0.48, 0.6));
        assert!(draft.is_edited());
        let saved = draft.zones(&zones, &links);
        assert_eq!(saved[0].start, 0.1);
        assert_eq!(saved[1].start, 0.48);
        assert!(saved.iter().all(|zone| zone.source == ZoneSource::User));
        assert_eq!(saved[1].name, "T2");
    }
}
