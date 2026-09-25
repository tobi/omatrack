//! Laps: the format-neutral lap model, the fallback split detectors, and
//! classification (port of the lap half of `TelemetryEngine.cpp`).

use crate::num::{llround, llround_i32, max, min, trunc_i32};

/// Upstream `motorsport-telemetry-rs` lap role (`LapKind`), forwarded as-is.
/// `Pit` on an incomplete interval is the stationary time upstream carved out
/// of the lap that held a pit stop: not a lap, just the stop.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
pub enum LapKind {
    #[default]
    Unknown = 0,
    Flying = 1,
    Out = 2,
    In = 3,
    OutIn = 4,
    Pit = 5,
}

impl LapKind {
    /// The bridge's `lap_kind_code` mapping from the upstream enum.
    pub fn from_upstream(kind: motorsport_telemetry_core::LapKind) -> Self {
        use motorsport_telemetry_core::LapKind as Up;
        match kind {
            Up::Unknown => Self::Unknown,
            Up::Flying => Self::Flying,
            Up::Out => Self::Out,
            Up::In => Self::In,
            Up::OutIn => Self::OutIn,
            Up::Pit => Self::Pit,
        }
    }
}

/// One lap (or fragment) of a recording, in seconds from the file start.
#[derive(Debug, Clone, PartialEq)]
pub struct Lap {
    pub id: i32,
    pub start_time: f64,
    pub end_time: f64,
    pub time_ms: f64,
    /// Both bounds are real start/finish crossings. A leading (out) or
    /// trailing (in) fragment of the recording is not a lap.
    pub complete: bool,
    /// Complete crossing whose time is a pit in/out outlier (median * 1.35).
    /// Set by [`classify_laps`]; representative laps are `complete && !is_pit_lap`.
    pub is_pit_lap: bool,
    /// Original source lap number when the parser supplied a unique value.
    pub source_number: Option<i32>,
    /// Presentation-order frame at the lap start from source metadata.
    pub first_video_frame: Option<u64>,
    /// Upstream role; Unknown for laps the fallback detector produced.
    pub kind: LapKind,
}

impl Lap {
    pub fn new(id: i32, start: f64, end: f64, time_ms: f64, complete: bool) -> Self {
        Self {
            id,
            start_time: start,
            end_time: end,
            time_ms,
            complete,
            is_pit_lap: false,
            source_number: None,
            first_video_frame: None,
            kind: LapKind::Unknown,
        }
    }

    /// The carved stationary interval of a pit stop, not a driven lap.
    pub fn is_pit_stop(&self) -> bool {
        self.kind == LapKind::Pit && !self.complete
    }

    /// Representative racing lap: feeds fastest-lap marks and defaults.
    pub fn counts_for_best(&self) -> bool {
        self.complete && !self.is_pit_lap && self.time_ms > 0.0
    }
}

/// Format a lap time in ms as `M:SS.mmm` (printf `%d:%06.3f`).
pub fn format_lap_time(time_ms: f64) -> String {
    let minutes = trunc_i32(time_ms / 60000.0);
    let seconds = (time_ms % 60000.0) / 1000.0;
    crate::sprintf!("%d:%06.3f", minutes, seconds)
}

/// Mark complete laps whose time exceeds the session median * 1.35 as pit
/// in/out laps. The single source of truth for "counts for best".
pub fn classify_laps(laps: &mut [Lap]) {
    let mut timed: Vec<f64> = laps
        .iter()
        .filter(|lap| lap.complete && lap.time_ms.is_finite() && lap.time_ms > 0.0)
        .map(|lap| lap.time_ms)
        .collect();
    if timed.len() >= 3 {
        let limit = crate::num::upper_median(&mut timed) * 1.35;
        for lap in laps.iter_mut() {
            if lap.complete && lap.time_ms > limit {
                lap.is_pit_lap = true;
            }
        }
    }
}

/// Representative fastest lap: complete and not a pit outlier, else the
/// fastest complete lap, else index 0. Classifies `laps` first, exactly as
/// the C++ CLI and GUI do.
pub fn fastest_lap_index(laps: &mut [Lap]) -> usize {
    classify_laps(laps);
    let mut best: Option<usize> = None;
    let mut best_ms = 1e18;
    for (i, lap) in laps.iter().enumerate() {
        if !lap.complete || lap.is_pit_lap || !(lap.time_ms > 0.0) {
            continue;
        }
        if lap.time_ms < best_ms {
            best_ms = lap.time_ms;
            best = Some(i);
        }
    }
    if let Some(best) = best {
        return best;
    }
    for (i, lap) in laps.iter().enumerate() {
        if !lap.complete || !(lap.time_ms > 0.0) {
            continue;
        }
        if lap.time_ms < best_ms {
            best_ms = lap.time_ms;
            best = Some(i);
        }
    }
    best.unwrap_or(0)
}

/// Index of the lap with `id`, if any.
pub fn lap_index_by_id(laps: &[Lap], id: i32) -> Option<usize> {
    laps.iter().position(|lap| lap.id == id)
}

// ── split detectors (port of MoTecParser.pds*Splits) ────────────────

/// Rising-edge splits from a beacon/trigger channel.
pub fn pds_beacon_splits(values: &[f64], freq: i32) -> Vec<f64> {
    let mut splits = Vec::new();
    if freq <= 0 || values.is_empty() {
        return splits;
    }
    let mut in_pulse = false;
    for (i, &value) in values.iter().enumerate() {
        let active = llround(value) != 0;
        if active && !in_pulse {
            splits.push(i as f64 / f64::from(freq));
        }
        in_pulse = active;
    }
    splits
}

/// Splits from a cumulative lap-time channel (backward jumps > 5 s).
pub fn pds_lap_time_splits(values: &[f64], freq: i32) -> Vec<f64> {
    let mut splits = Vec::new();
    if freq <= 0 || values.len() < 2 {
        return splits;
    }
    let mut last_split_index: i32 = -freq.max(1);
    let cluster_gap = (freq / 2).max(1);
    for i in 1..values.len() {
        if values[i - 1] - values[i] > 5.0 && i as i32 - last_split_index >= cluster_gap {
            splits.push(i as f64 / f64::from(freq));
            last_split_index = i as i32;
        }
    }
    splits
}

fn lap_time_seconds(value: f64) -> f64 {
    if !value.is_finite() || value <= 0.0 {
        return -1.0;
    }
    let value = if value > 1000.0 && value < 600000.0 {
        value / 1000.0
    } else {
        value
    };
    if value > 1.0 && value < 600.0 {
        value
    } else {
        -1.0
    }
}

/// Splits from a last/previous-lap-time channel (a new posted time).
pub fn pds_last_lap_time_splits(values: &[f64], freq: i32) -> Vec<f64> {
    let mut splits = Vec::new();
    if freq <= 0 || values.len() < 2 {
        return splits;
    }
    let mut previous = lap_time_seconds(values[0]);
    let mut last_split_index: i32 = -freq.max(1);
    let cluster_gap = (freq / 2).max(1);
    for i in 1..values.len() {
        let current = lap_time_seconds(values[i]);
        if current < 0.0 {
            continue;
        }
        if previous > 0.0
            && (current - previous).abs() > 0.05
            && i as i32 - last_split_index >= cluster_gap
        {
            splits.push(i as f64 / f64::from(freq));
            last_split_index = i as i32;
        }
        previous = current;
    }
    splits
}

/// Splits from a lap-number channel (positive-to-next-positive increments;
/// zero/dropout recovery only re-establishes counter state).
pub fn pds_lap_number_splits(values: &[f64], freq: i32) -> Vec<f64> {
    let mut splits = Vec::new();
    if freq <= 0 || values.len() < 2 {
        return splits;
    }
    let mut prev = llround_i32(values[0]);
    let mut prev_valid = prev > 0;
    for i in 1..values.len() {
        let current = llround_i32(values[i]);
        if current <= 0 {
            prev_valid = false;
            continue;
        }
        if prev_valid && current == prev.wrapping_add(1) {
            splits.push(i as f64 / f64::from(freq));
        }
        prev = current;
        prev_valid = true;
    }
    splits
}

/// Whether a lap-number signal carries authoritative non-zero state.
pub fn lap_number_carries_state(values: &[f64]) -> bool {
    values.iter().any(|v| v.is_finite() && llround(*v) > 0)
}

/// Select the most authoritative boundary source.
pub fn select_lap_splits(
    beacon: &[f64],
    lap_number: &[f64],
    lap_number_active: bool,
    lap_time: &[f64],
    distance: &[f64],
) -> Vec<f64> {
    if lap_number_active && lap_number.len() >= 2 {
        return lap_number.to_vec();
    }
    if beacon.len() >= 2 {
        return beacon.to_vec();
    }
    if lap_time.len() >= 2 {
        return lap_time.to_vec();
    }
    if distance.len() >= 2 {
        return distance.to_vec();
    }
    Vec::new()
}

/// Splits from a wrapping lap-distance channel: a reset is a drop of more
/// than half the observed peak (works for m, km, % and 0-1 fractions).
pub fn pds_distance_splits(values: &[f64], freq: i32) -> Vec<f64> {
    let mut splits = Vec::new();
    if freq <= 0 || values.len() < 2 {
        return splits;
    }
    let mut peak = 0.0;
    for &value in values {
        if value.is_finite() && value > peak {
            peak = value;
        }
    }
    if !(peak > 0.0) {
        return splits;
    }
    let drop_threshold = peak * 0.5;
    let mut last_split_index: i32 = -freq.max(1);
    let cluster_gap = (freq / 2).max(1);
    for i in 1..values.len() {
        if values[i - 1] - values[i] > drop_threshold && i as i32 - last_split_index >= cluster_gap
        {
            splits.push(i as f64 / f64::from(freq));
            last_split_index = i as i32;
        }
    }
    splits
}

/// Build laps from split times. Head/tail fragments are incomplete;
/// heuristic crossings much shorter than the median can be rejected.
pub fn build_laps_from_splits(
    split_times: &[f64],
    duration: f64,
    reject_short_crossings: bool,
) -> Vec<Lap> {
    if duration <= 0.0 {
        return Vec::new();
    }
    // Two crossings within 10 s are one beacon seen twice: collapse onto the
    // first so consecutive laps stay contiguous.
    const MINIMUM_CROSSING_GAP_SECONDS: f64 = 10.0;
    let mut filtered: Vec<f64> = split_times
        .iter()
        .copied()
        .filter(|s| *s > 0.0 && *s < duration)
        .collect();
    filtered.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    filtered.dedup();
    let mut splits: Vec<f64> = Vec::with_capacity(filtered.len());
    for s in filtered {
        if let Some(&last) = splits.last()
            && s - last <= MINIMUM_CROSSING_GAP_SECONDS
        {
            continue;
        }
        splits.push(s);
    }
    let whole = || vec![Lap::new(0, 0.0, duration, duration * 1000.0, false)];
    if splits.len() < 2 {
        return whole();
    }
    struct Bound {
        start: f64,
        end: f64,
        complete: bool,
    }
    let mut bounds: Vec<Bound> = splits
        .windows(2)
        .map(|w| Bound {
            start: w[0],
            end: w[1],
            complete: true,
        })
        .collect();
    if bounds.is_empty() {
        return whole();
    }
    let mut durations: Vec<f64> = bounds.iter().map(|b| b.end - b.start).collect();
    let median = crate::num::upper_median(&mut durations);
    let tail = duration - splits[splits.len() - 1];
    if tail > max(10.0, median * 0.5) && tail < median * 1.8 {
        bounds.push(Bound {
            start: splits[splits.len() - 1],
            end: duration,
            complete: false,
        });
    }
    let head = splits[0];
    if head > max(10.0, median * 0.5) && head < median * 1.8 {
        bounds.insert(
            0,
            Bound {
                start: 0.0,
                end: head,
                complete: false,
            },
        );
    }
    let mut result: Vec<Lap> = bounds
        .iter()
        .enumerate()
        .map(|(i, b)| {
            Lap::new(
                i as i32,
                b.start,
                b.end,
                (b.end - b.start) * 1000.0,
                b.complete,
            )
        })
        .collect();
    if reject_short_crossings {
        mark_short_crossings_incomplete(&mut result);
    }
    result
}

/// Mark complete crossings much shorter than the session median incomplete.
pub fn mark_short_crossings_incomplete(laps: &mut [Lap]) {
    let mut seconds: Vec<f64> = laps
        .iter()
        .filter(|lap| lap.complete && lap.time_ms.is_finite() && lap.time_ms > 0.0)
        .map(|lap| lap.time_ms / 1000.0)
        .collect();
    if seconds.len() < 2 {
        return;
    }
    let count = seconds.len();
    let median = crate::num::upper_median(&mut seconds);
    // With only two timed laps, fall back to a conservative floor: no
    // circuit lap is under half a minute.
    let min_seconds = if count >= 3 {
        median * 0.5
    } else {
        max(median * 0.5, 30.0)
    };
    for lap in laps.iter_mut() {
        if lap.complete && lap.time_ms / 1000.0 < min_seconds {
            lap.complete = false;
        }
    }
}

/// Override lap times from a "previous lap time" channel when it agrees with
/// the crossing-derived estimate.
pub fn pds_apply_previous_lap_times(
    laps: &[Lap],
    previous_lap_time_values: &[f64],
    freq: i32,
    reject_mismatches: bool,
) -> Vec<Lap> {
    if laps.is_empty() || previous_lap_time_values.is_empty() || freq <= 0 {
        return laps.to_vec();
    }
    let seconds = |value: f64| -> f64 {
        if !value.is_finite() {
            return -1.0;
        }
        let value = if value > 1000.0 && value < 600000.0 {
            value / 1000.0
        } else {
            value
        };
        if value > 1.0 && value < 600.0 {
            value
        } else {
            -1.0
        }
    };
    let sample = |time: f64| -> f64 {
        let center = llround_i32(time * f64::from(freq));
        for delta in 0..=2 {
            for sign in [-1, 1] {
                let idx = center.wrapping_add(sign * delta);
                if idx < 0 || idx as usize >= previous_lap_time_values.len() {
                    continue;
                }
                let value = seconds(previous_lap_time_values[idx as usize]);
                if value > 0.0 {
                    return value;
                }
            }
        }
        -1.0
    };
    let mut out = laps.to_vec();
    for lap in out.iter_mut() {
        // A head/tail fragment ends at the recording boundary, not at a
        // crossing; its "previous lap time" describes the lap before it.
        if !lap.complete {
            continue;
        }
        let prev_lap_sec = sample(lap.end_time);
        if prev_lap_sec < 0.0 {
            continue;
        }
        let crossing_sec = lap.end_time - lap.start_time;
        let tolerance = max(3.0, crossing_sec * 0.15);
        if (prev_lap_sec - crossing_sec).abs() > tolerance {
            if reject_mismatches {
                lap.complete = false;
            }
            continue;
        }
        lap.time_ms = prev_lap_sec * 1000.0;
    }
    out
}

/// Reject crossing pairs that cover substantially less of a lap-position
/// signal than the typical crossing pair in the same recording.
pub fn pds_apply_lap_distance_coverage(laps: &[Lap], lap_distance: &[f64], freq: i32) -> Vec<Lap> {
    if laps.is_empty() || lap_distance.len() < 2 || freq <= 0 {
        return laps.to_vec();
    }
    let range = |begin: usize, end: usize| -> f64 {
        let mut low = f64::INFINITY;
        let mut high = f64::NEG_INFINITY;
        let end = end.min(lap_distance.len());
        for &value in &lap_distance[begin.min(end)..end] {
            if !value.is_finite() {
                continue;
            }
            low = min(low, value);
            high = max(high, value);
        }
        if high >= low { high - low } else { 0.0 }
    };
    let session_range = range(0, lap_distance.len());
    if !(session_range > 0.0) {
        return laps.to_vec();
    }
    let freq = f64::from(freq);
    let mut coverage = vec![-1.0; laps.len()];
    for (i, lap) in laps.iter().enumerate() {
        if !lap.complete {
            continue;
        }
        let begin = max(0.0, (lap.start_time * freq).floor()) as usize;
        let end = max(0.0, (lap.end_time * freq).ceil() + 1.0) as usize;
        if begin >= lap_distance.len() || end > lap_distance.len() {
            continue;
        }
        coverage[i] = range(begin, end) / session_range;
    }
    let mut typical: Vec<f64> = coverage.iter().copied().filter(|v| *v >= 0.0).collect();
    if typical.len() < 2 {
        return laps.to_vec();
    }
    let median_coverage = crate::num::upper_median(&mut typical);
    // A session-cumulative distance channel cannot validate individual laps.
    if median_coverage < 0.75 {
        return laps.to_vec();
    }
    let mut out = laps.to_vec();
    for (i, lap) in out.iter_mut().enumerate() {
        if lap.complete && coverage[i] >= 0.0 && coverage[i] < median_coverage * 0.5 {
            lap.complete = false;
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lap_time_text_matches_printf() {
        assert_eq!(format_lap_time(83550.0), "1:23.550");
        assert_eq!(format_lap_time(59999.0), "0:59.999");
        assert_eq!(format_lap_time(600000.0), "10:00.000");
    }
}
