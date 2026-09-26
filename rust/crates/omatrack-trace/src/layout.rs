//! Lane sizing and lane layout: a port of `TraceLaneSizing.h` and the
//! geometry half of `TraceLaneLayout.cpp`, plus pinned lanes.
//!
//! FIT distributes positive finite weights over the available height above a
//! readable minimum. When the minimums alone exceed the pane, every lane keeps
//! the minimum and the scroll region overflows: a lane is never crushed below
//! readable. Manual mode uses exact percentages of the trace area and scrolls
//! when they exceed it. Consecutive visible sample channels flagged
//! `combine_with_previous` share the previous lane (independent vertical
//! scales). Hidden channels and overlay-group boundaries break that
//! adjacency. Pinned lanes sit in a region above the scroll region and never
//! scroll.

use smallvec::SmallVec;

/// Readable lane minimum in logical pixels: the lane chrome (title and two
/// readout lines) over a plot tall enough to read a shape. FIT never
/// allocates less; lanes that do not fit scroll instead.
pub const MIN_LANE_HEIGHT: f64 = 44.0;

/// Default height share of a channel's lane, percent of the trace area,
/// unless `channels.<key>.height_percent` is configured. In FIT these are the
/// relative weights: speed leads, the pedals and Δ are first-class lanes, a
/// held channel such as gear needs less.
pub fn default_height_percent(key: &str) -> f64 {
    match key {
        "speed" => 34.0,
        "throttle" | "brake" => 24.0,
        "delta" => 20.0,
        "steering" => 16.0,
        "gear" => 9.0,
        _ if key.to_ascii_lowercase().contains("rpm") => 14.0,
        _ => 12.0,
    }
}

/// A positive finite weight, or 1.
pub fn valid_lane_weight(value: f64) -> f64 {
    if value.is_finite() && value > 0.0 {
        value
    } else {
        1.0
    }
}

/// Default boost for the channel everyone reads first.
pub fn lane_height_boost(key: &str) -> f64 {
    if key == "speed" { 1.35 } else { 1.0 }
}

/// Proportional allocation above the readable minimum. When the pane cannot
/// fit the minimum for every lane, every lane gets the minimum and the sum
/// exceeds `available` (the caller scrolls). Weights are normalized before
/// summing so hand-edited, very large finite weights cannot overflow.
pub fn fit_lane_heights(mut weights: Vec<f64>, available: f64) -> Vec<f64> {
    let mut heights = vec![0.0; weights.len()];
    if weights.is_empty() || !available.is_finite() || available <= 0.0 {
        return heights;
    }
    let mut largest: f64 = 1.0;
    for weight in weights.iter_mut() {
        *weight = valid_lane_weight(*weight);
        largest = largest.max(*weight);
    }
    let floor = MIN_LANE_HEIGHT;
    if floor * weights.len() as f64 >= available {
        heights.fill(floor);
        return heights;
    }
    let mut remaining = available;
    let mut total = 0.0;
    for i in 0..weights.len() {
        weights[i] /= largest;
        if weights[i] == 0.0 {
            heights[i] = floor;
            remaining -= floor;
        } else {
            total += weights[i];
        }
    }
    loop {
        let mut changed = false;
        for i in 0..weights.len() {
            if weights[i] <= 0.0 || total <= 0.0 {
                continue;
            }
            if remaining * (weights[i] / total) < floor {
                heights[i] = floor;
                remaining -= floor;
                total -= weights[i];
                weights[i] = 0.0;
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    for i in 0..weights.len() {
        if weights[i] > 0.0 && total > 0.0 {
            heights[i] = remaining.max(0.0) * (weights[i] / total);
        }
    }
    heights
}

/// Move the divider after lane `upper` by `delta`. The neighbour gives way
/// first; once it reaches its minimum, the drag borrows from further lanes,
/// so one drag can make a trace nearly pane-sized.
pub fn resize_lane_boundary(original: &[f64], upper: usize, delta: f64) -> Vec<f64> {
    let mut heights = original.to_vec();
    if upper + 1 >= heights.len() || !delta.is_finite() {
        return heights;
    }
    let mut floor = MIN_LANE_HEIGHT;
    for &height in &heights {
        if !height.is_finite() || height < 0.0 {
            return original.to_vec();
        }
        floor = floor.min(height);
    }
    let mut needed = delta.abs();
    let receiver = if delta >= 0.0 { upper } else { upper + 1 };
    if delta >= 0.0 {
        let mut i = upper + 1;
        while i < heights.len() && needed > 0.0 {
            let moved = needed.min(heights[i] - floor);
            heights[i] -= moved;
            heights[receiver] += moved;
            needed -= moved;
            i += 1;
        }
    } else {
        let mut i = upper + 1;
        while i > 0 && needed > 0.0 {
            i -= 1;
            let moved = needed.min(heights[i] - floor);
            heights[i] -= moved;
            heights[receiver] += moved;
            needed -= moved;
        }
    }
    heights
}

/// Sizing inputs of one channel, in display order.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct LaneSizing {
    pub visible: bool,
    /// FIT weight (`channels.<key>.weight`), already including any boost.
    pub weight: f64,
    /// Manual height, percent of the trace area (1–100).
    pub height_percent: f64,
    /// Share the previous visible sample lane.
    pub combine_with_previous: bool,
    /// Overlay group; a boundary breaks `combine_with_previous`.
    pub group: Option<u64>,
    /// Draw in the pinned region above the scroll region.
    pub pinned: bool,
}

impl Default for LaneSizing {
    fn default() -> Self {
        Self {
            visible: true,
            weight: 1.0,
            height_percent: 20.0,
            combine_with_previous: false,
            group: None,
            pinned: false,
        }
    }
}

impl LaneSizing {
    pub fn with_weight(mut self, weight: f64) -> Self {
        self.weight = weight;
        self
    }
    pub fn with_height_percent(mut self, percent: f64) -> Self {
        self.height_percent = percent;
        self
    }
    pub fn visible(mut self, visible: bool) -> Self {
        self.visible = visible;
        self
    }
    pub fn combine_with_previous(mut self, combine: bool) -> Self {
        self.combine_with_previous = combine;
        self
    }
    pub fn pinned(mut self, pinned: bool) -> Self {
        self.pinned = pinned;
        self
    }
    pub fn with_group(mut self, group: Option<u64>) -> Self {
        self.group = group;
        self
    }

    fn share(&self) -> f64 {
        let percent = if self.height_percent.is_finite() {
            self.height_percent
        } else {
            20.0
        };
        (percent / 100.0).clamp(0.01, 1.0)
    }
}

/// One visible lane: the root channel and the channels drawn into it.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct LaneSlot {
    /// Index of the root channel in the input slice.
    pub root: usize,
    /// Further channels sharing this lane.
    pub overlays: SmallVec<[usize; 2]>,
    /// Top edge in logical pixels, relative to the trace area top. Scroll
    /// lanes already include the current scroll offset.
    pub y: f64,
    pub height: f64,
    pub pinned: bool,
}

impl LaneSlot {
    /// Root then overlays, in display order.
    pub fn channels(&self) -> impl Iterator<Item = usize> + '_ {
        std::iter::once(self.root).chain(self.overlays.iter().copied())
    }
}

/// Resolved lane geometry of the trace area.
#[derive(Clone, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct LaneLayout {
    /// Pinned lanes first, then scroll lanes, in display order.
    pub slots: Vec<LaneSlot>,
    /// Height of the pinned region; the scroll region starts here.
    pub pinned_height: f64,
    /// Visible height of the scroll region.
    pub scroll_viewport: f64,
    /// Full height of the scroll content (unscrolled).
    pub scroll_content: f64,
    /// Scroll offset actually applied (clamped).
    pub scroll: f64,
}

impl LaneLayout {
    pub fn max_scroll(&self) -> f64 {
        (self.scroll_content - self.scroll_viewport).max(0.0)
    }
    pub fn overflows(&self) -> bool {
        self.max_scroll() > 0.5
    }
    /// The slot under a y coordinate (relative to the trace area top). Scroll
    /// lanes hidden under the pinned region do not answer.
    pub fn slot_at(&self, y: f64) -> Option<usize> {
        self.slots.iter().position(|slot| {
            let visible_top = if slot.pinned {
                slot.y
            } else {
                slot.y.max(self.pinned_height)
            };
            y >= visible_top && y < slot.y + slot.height
        })
    }
}

/// How the trace area is sized.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct LayoutMode {
    /// FIT (weights) versus manual percentages.
    pub fit: bool,
    /// Resize editing projects lanes into FIT without changing the mode.
    pub resizing: bool,
}

impl Default for LayoutMode {
    fn default() -> Self {
        Self {
            fit: true,
            resizing: false,
        }
    }
}

impl LayoutMode {
    pub fn fit(fit: bool) -> Self {
        Self {
            fit,
            resizing: false,
        }
    }
    pub fn resizing(mut self, resizing: bool) -> Self {
        self.resizing = resizing;
        self
    }
    fn uses_weights(&self) -> bool {
        self.fit || self.resizing
    }
}

struct Grouped {
    root: usize,
    overlays: SmallVec<[usize; 2]>,
    pinned: bool,
}

/// Lane roots in display order. Resolved once so rendering, labels, edits
/// and resets all consume the same grouping.
fn group_lanes(channels: &[LaneSizing]) -> Vec<Grouped> {
    let mut lanes: Vec<Grouped> = Vec::new();
    // Whether the previous channel was a visible sample that can accept an
    // overlay; hidden channels and group boundaries break adjacency.
    let mut open = false;
    let mut previous_group: Option<Option<u64>> = None;
    for (index, channel) in channels.iter().enumerate() {
        if !channel.visible {
            open = false;
            continue;
        }
        let same_group = previous_group == Some(channel.group);
        if channel.combine_with_previous
            && open
            && same_group
            && let Some(last) = lanes.last_mut()
        {
            last.overlays.push(index);
        } else {
            lanes.push(Grouped {
                root: index,
                overlays: SmallVec::new(),
                pinned: channel.pinned,
            });
        }
        open = true;
        previous_group = Some(channel.group);
    }
    lanes
}

/// Resolve lane geometry for a trace area `available` pixels tall with the
/// requested `scroll` offset (clamped).
pub fn layout_lanes(
    channels: &[LaneSizing],
    mode: LayoutMode,
    available: f64,
    scroll: f64,
) -> LaneLayout {
    let lanes = group_lanes(channels);
    if lanes.is_empty() || !available.is_finite() || available <= 0.0 {
        return LaneLayout::default();
    }
    // Pinned lanes first; relative order is kept within each region.
    let (pinned, unpinned): (Vec<Grouped>, Vec<Grouped>) =
        lanes.into_iter().partition(|lane| lane.pinned);
    let ordered: Vec<Grouped> = pinned.into_iter().chain(unpinned).collect();

    let mut layout = LaneLayout::default();
    if mode.uses_weights() {
        let weights: Vec<f64> = ordered
            .iter()
            .map(|lane| {
                let channel = &channels[lane.root];
                let share = channel.share();
                valid_lane_weight(channel.weight).min(f64::MAX / share) * share
            })
            .collect();
        let heights = fit_lane_heights(weights, available);
        let mut y = 0.0;
        for (lane, height) in ordered.into_iter().zip(heights) {
            if lane.pinned {
                layout.pinned_height = y + height;
            }
            layout.slots.push(LaneSlot {
                root: lane.root,
                overlays: lane.overlays,
                y,
                height,
                pinned: lane.pinned,
            });
            y += height;
        }
        layout.scroll_viewport = (available - layout.pinned_height).max(0.0);
        layout.scroll_content = y - layout.pinned_height;
        // Lanes at their readable minimum that still do not fit scroll
        // under the pinned region, exactly like manual mode.
        let scroll = if scroll.is_finite() { scroll } else { 0.0 };
        if layout.overflows() {
            layout.scroll = scroll.clamp(0.0, layout.max_scroll());
        }
        for slot in layout.slots.iter_mut().filter(|slot| !slot.pinned) {
            slot.y -= layout.scroll;
        }
        return layout;
    }

    // Manual: exact percentages of the whole trace area. Pinned lanes are
    // capped so the scroll region keeps at least one readable lane.
    let pinned_cap = (available - MIN_LANE_HEIGHT).max(0.0);
    let mut y = 0.0;
    let mut scroll_y = 0.0;
    let mut slots = Vec::with_capacity(ordered.len());
    for lane in ordered {
        let height = available * channels[lane.root].share();
        if lane.pinned {
            let height = height.min((pinned_cap - y).max(0.0));
            slots.push(LaneSlot {
                root: lane.root,
                overlays: lane.overlays,
                y,
                height,
                pinned: true,
            });
            y += height;
        } else {
            slots.push(LaneSlot {
                root: lane.root,
                overlays: lane.overlays,
                y: scroll_y,
                height,
                pinned: false,
            });
            scroll_y += height;
        }
    }
    layout.pinned_height = y;
    layout.scroll_viewport = (available - y).max(0.0);
    layout.scroll_content = scroll_y;
    let scroll = if scroll.is_finite() { scroll } else { 0.0 };
    layout.scroll = scroll.clamp(0.0, layout.max_scroll());
    for slot in slots.iter_mut().filter(|slot| !slot.pinned) {
        slot.y += layout.pinned_height - layout.scroll;
    }
    layout.slots = slots;
    layout
}

#[cfg(test)]
mod tests {
    use super::*;

    fn close(a: &[f64], b: &[f64]) -> bool {
        a.len() == b.len() && a.iter().zip(b).all(|(x, y)| (x - y).abs() < 1e-8)
    }

    // Ported from tests/TraceLaneSizingTest.cpp.
    #[test]
    fn proportional_fit() {
        assert_eq!(
            fit_lane_heights(vec![1.0, 2.0, 1.0], 400.0),
            vec![100.0, 200.0, 100.0]
        );
    }

    #[test]
    fn no_fixed_multiplier_cap() {
        assert_eq!(
            fit_lane_heights(vec![10000.0, 1.0, 1.0, 1.0], 600.0),
            vec![468.0, 44.0, 44.0, 44.0]
        );
    }

    #[test]
    fn small_panes_keep_the_minimum_and_overflow() {
        for height in fit_lane_heights(vec![1.0, 100.0, 0.001], 30.0) {
            assert!((height - MIN_LANE_HEIGHT).abs() < 1e-8);
        }
    }

    #[test]
    fn large_finite_weights_do_not_overflow() {
        assert_eq!(
            fit_lane_heights(vec![1e300, 1e300, 1.0], 420.0),
            vec![188.0, 188.0, 44.0]
        );
    }

    #[test]
    fn invalid_weights_are_safe() {
        assert_eq!(
            fit_lane_heights(vec![-1.0, 0.0, f64::NAN], 300.0),
            vec![100.0, 100.0, 100.0]
        );
        assert!(fit_lane_heights(vec![], 300.0).is_empty());
        assert_eq!(fit_lane_heights(vec![1.0, 1.0], 0.0), vec![0.0, 0.0]);
    }

    #[test]
    fn divider_borrows_across_neighbours() {
        let original = [100.0, 100.0, 100.0, 100.0];
        let heights = resize_lane_boundary(&original, 0, 100.0);
        assert_eq!(heights, vec![200.0, 44.0, 56.0, 100.0]);
        assert_eq!(heights.iter().sum::<f64>(), 400.0);
        assert_eq!(original[0], 100.0);
        assert_eq!(
            resize_lane_boundary(&original, 0, 10000.0),
            vec![268.0, 44.0, 44.0, 44.0]
        );
    }

    #[test]
    fn last_lane_can_take_space_from_above() {
        assert_eq!(
            resize_lane_boundary(&[100.0, 100.0, 100.0, 100.0], 2, -100.0),
            vec![100.0, 56.0, 44.0, 200.0]
        );
    }

    #[test]
    fn preview_weights_round_trip_to_exact_heights() {
        let keys = ["speed", "brake", "throttle", "raw:test"];
        let desired = resize_lane_boundary(&[140.0, 100.0, 100.0, 100.0], 1, 130.0);
        let total: f64 = desired.iter().sum();
        let normal: f64 = keys.iter().map(|k| lane_height_boost(k)).sum();
        let effective: Vec<f64> = keys
            .iter()
            .enumerate()
            .map(|(i, key)| {
                let weight = desired[i] / total * normal / lane_height_boost(key);
                weight * lane_height_boost(key)
            })
            .collect();
        assert!(close(&fit_lane_heights(effective, total), &desired));
    }

    fn channel() -> LaneSizing {
        LaneSizing::default()
    }

    #[test]
    fn combine_joins_previous_visible_lane() {
        let channels = [
            channel().with_weight(2.0),
            channel(),
            channel().combine_with_previous(true),
            channel().with_weight(0.5),
        ];
        let layout = layout_lanes(&channels, LayoutMode::default(), 450.0, 0.0);
        assert_eq!(layout.slots.len(), 3);
        assert_eq!(layout.slots[1].root, 1);
        assert_eq!(layout.slots[1].overlays.as_slice(), &[2]);
        assert!(close(
            &layout.slots.iter().map(|s| s.height).collect::<Vec<_>>(),
            &[
                257.142_857_142_857_1,
                128.571_428_571_428_58,
                64.285_714_285_714_29
            ]
        ));
        assert!(!layout.overflows());
    }

    #[test]
    fn hidden_channels_and_group_boundaries_break_adjacency() {
        let channels = [
            channel(),
            channel().visible(false),
            channel().combine_with_previous(true),
            channel().with_group(Some(7)).combine_with_previous(true),
        ];
        let layout = layout_lanes(&channels, LayoutMode::default(), 300.0, 0.0);
        let roots: Vec<usize> = layout.slots.iter().map(|s| s.root).collect();
        assert_eq!(roots, vec![0, 2, 3]);
        assert!(layout.slots.iter().all(|s| s.overlays.is_empty()));
    }

    #[test]
    fn manual_mode_scrolls_and_clamps() {
        let channels = [
            channel().with_height_percent(50.0),
            channel().with_height_percent(50.0),
            channel().with_height_percent(50.0),
        ];
        let layout = layout_lanes(&channels, LayoutMode::fit(false), 400.0, 10_000.0);
        assert!(layout.overflows());
        assert_eq!(layout.scroll_content, 600.0);
        assert_eq!(layout.max_scroll(), 200.0);
        assert_eq!(layout.scroll, 200.0);
        assert_eq!(layout.slots[0].y, -200.0);
        // Resize projects into FIT without leaving manual mode.
        let fit = layout_lanes(
            &channels,
            LayoutMode::fit(false).resizing(true),
            400.0,
            50.0,
        );
        assert!(!fit.overflows());
    }

    #[test]
    fn pinned_lanes_sit_above_the_scroll_region() {
        let channels = [
            channel().with_height_percent(40.0),
            channel().with_height_percent(40.0),
            channel().with_height_percent(10.0).pinned(true),
            channel().with_height_percent(40.0),
        ];
        let layout = layout_lanes(&channels, LayoutMode::fit(false), 500.0, 30.0);
        assert_eq!(layout.slots[0].root, 2);
        assert!(layout.slots[0].pinned);
        assert_eq!(layout.slots[0].y, 0.0);
        assert_eq!(layout.pinned_height, 50.0);
        assert_eq!(layout.scroll_viewport, 450.0);
        assert_eq!(layout.scroll_content, 600.0);
        assert_eq!(layout.scroll, 30.0);
        // The first scroll lane starts under the pinned region, scrolled.
        assert_eq!(layout.slots[1].y, 50.0 - 30.0);
        // A scrolled lane hidden under the pinned region does not hit-test.
        assert_eq!(layout.slot_at(25.0), Some(0));
        assert_eq!(layout.slot_at(55.0), Some(1));
        // FIT keeps pinned lanes first and fits everything.
        let fit = layout_lanes(&channels, LayoutMode::default(), 500.0, 30.0);
        assert_eq!(fit.slots[0].root, 2);
        assert!((fit.slots.iter().map(|s| s.height).sum::<f64>() - 500.0).abs() < 1e-9);
        assert_eq!(fit.scroll, 0.0);
    }

    #[test]
    fn fit_overflows_into_a_scroll_instead_of_crushing_lanes() {
        let mut channels = vec![channel().pinned(true)];
        channels.extend((0..6).map(|_| channel()));
        // 7 lanes need 308 px at the minimum; the pane has 200.
        let layout = layout_lanes(&channels, LayoutMode::default(), 200.0, 1_000.0);
        assert!(layout.slots.iter().all(|s| s.height >= MIN_LANE_HEIGHT));
        assert!(layout.overflows());
        assert_eq!(layout.pinned_height, MIN_LANE_HEIGHT);
        assert!((layout.max_scroll() - 108.0).abs() < 1e-9);
        assert!((layout.scroll - 108.0).abs() < 1e-9);
        // The pinned lane stays put; the scroll lanes move under it.
        assert_eq!(layout.slots[0].y, 0.0);
        assert!((layout.slots[1].y - (MIN_LANE_HEIGHT - 108.0)).abs() < 1e-9);
        // A fitting pane never scrolls, whatever was requested.
        let fits = layout_lanes(&channels, LayoutMode::default(), 700.0, 50.0);
        assert!(!fits.overflows());
        assert_eq!(fits.scroll, 0.0);
    }

    #[test]
    fn default_heights_lead_with_speed_and_keep_gear_readable() {
        assert!(default_height_percent("speed") > default_height_percent("throttle"));
        assert!(default_height_percent("gear") < default_height_percent("steering"));
        assert_eq!(default_height_percent("raw:Engine RPM"), 14.0);
        assert_eq!(default_height_percent("damper_fl"), 12.0);
    }
}
