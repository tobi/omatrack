//! The session model on synthetic recordings: load pipeline stages, cancel,
//! corner sources, the comparison, swapping roles and manual offsets.

#![cfg(test)]

use omatrack_core::alignment::Strategy;
use omatrack_core::corners::{CornerZone, ZoneSource};
use omatrack_core::laps::LapKind;
use omatrack_core::session::{
    self, CornerSource, IdentityState, LapStripKind, MarkerKind, StrategyRequest,
};
use omatrack_core::{Analysis, Lap, LoadOptions, RawChannel, Recording, SessionError, load_lap};
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

const RATE: f64 = 50.0;
const LAP_SECONDS: f64 = 20.0;
const FIRST_LAP_START: f64 = 5.0;
const DURATION: f64 = 90.0;

/// Complete laps 1-4 (lap 2 fastest, lap 4 with a stop at its end), an out
/// fragment before and an in fragment after.
fn lap_list() -> Vec<Lap> {
    let mut laps = Vec::new();
    let mut out = Lap::new(0, 0.0, FIRST_LAP_START, FIRST_LAP_START * 1000.0, false);
    out.kind = LapKind::Out;
    laps.push(out);
    for (id, time_ms) in [(1, 20_100.0), (2, 19_900.0), (3, 20_000.0), (4, 20_500.0)] {
        let start = FIRST_LAP_START + f64::from(id - 1) * LAP_SECONDS;
        let mut lap = Lap::new(id, start, start + LAP_SECONDS, time_ms, true);
        lap.source_number = Some(id);
        lap.kind = LapKind::Flying;
        laps.push(lap);
    }
    let mut last = Lap::new(5, 85.0, DURATION, 5_000.0, false);
    last.kind = LapKind::In;
    laps.push(last);
    laps
}

struct Sample {
    speed: f64,
    throttle: f64,
    brake: f64,
    steering: f64,
    gear: f64,
    damper: f64,
}

/// Two braked corners per lap; each lap brakes a little later and carries
/// a little less speed than the one before.
fn sample_at(t: f64) -> Sample {
    let lap = ((t - FIRST_LAP_START) / LAP_SECONDS).floor();
    let phase = (t - FIRST_LAP_START) / LAP_SECONDS - lap;
    let k = lap.clamp(0.0, 4.0);
    let mut s = Sample {
        speed: 200.0 - 2.0 * k,
        throttle: 100.0,
        brake: 0.0,
        steering: 0.0,
        gear: 6.0,
        damper: 5.0 * (7.3 * t).sin() + 2.0 * (2.1 * t).cos(),
    };
    for (corner_start, corner_end) in [(0.25, 0.40), (0.65, 0.80)] {
        let start = corner_start + 0.004 * k;
        if phase >= start && phase < corner_end {
            let local = (phase - start) / (corner_end - start);
            let apex = 0.5;
            let depth = 110.0 + k;
            s.speed = if local < apex {
                s.speed - depth * local / apex
            } else {
                s.speed - depth * (1.0 - local) / apex
            };
            s.throttle = if local < 0.6 { 0.0 } else { 100.0 };
            s.brake = if local < 0.45 {
                80.0 * (1.0 - local / 0.45)
            } else {
                0.0
            };
            s.steering = 60.0 * (std::f64::consts::PI * local).sin();
            s.gear = if local < 0.2 { 5.0 } else { 3.0 };
        }
    }
    // Lap 4 ends stationary: the pit stop the strip must not stretch.
    if (82.0..85.0).contains(&t) {
        s.speed = 0.0;
        s.throttle = 0.0;
    }
    s
}

#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn recording() -> Arc<Recording> {
    let count = (DURATION * RATE) as usize;
    let mut columns: [Vec<f64>; 7] = Default::default();
    for i in 0..count {
        let t = i as f64 / RATE;
        let s = sample_at(t);
        for (column, value) in columns.iter_mut().zip([
            s.speed,
            s.throttle,
            s.brake,
            s.steering,
            s.gear,
            s.damper,
            5000.0 + s.speed * 30.0,
        ]) {
            column.push(value);
        }
    }
    let [speed, throttle, brake, steering, gear, damper, rpm] = columns;
    let channel = |name: &str, unit: &str, values: Vec<f64>| {
        RawChannel::synthetic(name, unit, RATE, DURATION, values)
    };
    let mut recording = Recording::synthetic(
        "",
        vec![
            channel("Speed", "km/h", speed),
            channel("Throttle Pos", "%", throttle),
            channel("Brake Pressure F", "bar", brake),
            channel("Steering Angle", "deg", steering),
            channel("Gear", "", gear),
            channel("Damper Travel FL", "mm", damper.clone()),
            channel("Damper Travel FR", "mm", damper),
            channel("RPM", "rpm", rpm),
        ],
    );
    recording.set_source_laps(lap_list());
    Arc::new(recording)
}

fn not_cancelled() -> AtomicBool {
    AtomicBool::new(false)
}

#[test]
#[expect(
    clippy::cast_possible_wrap,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn load_lap_runs_every_stage() {
    let recording = recording();
    let lap = load_lap(
        recording.clone(),
        2,
        &LoadOptions::default(),
        &not_cancelled(),
    )
    .unwrap();
    assert_eq!(lap.lap_id(), 2);
    assert_eq!(lap.laps().len(), 6);
    assert!(Arc::ptr_eq(lap.recording(), &recording));
    // 20 s at 50 Hz.
    assert!((lap.unified().len() as i64 - 1000).abs() <= 1);
    assert!(lap.layout().is_none(), "no GPS and no track hint");
    assert!(lap.video().is_none(), "a synthetic recording has no video");

    let standard = &lap.overlays()[0];
    assert_eq!(standard.provider, "standard");
    for key in [
        "speed",
        "throttle",
        "brake",
        "steering",
        "gear",
        "damper_fl",
    ] {
        let channel = standard.channel(key).unwrap_or_else(|| panic!("{key}"));
        assert_eq!(channel.values.len(), lap.unified().len());
    }
    assert_eq!(lap.overlays().len(), 1, "raw channels are opt-in");

    let strip = lap.strip();
    let labels: Vec<&str> = strip.iter().map(|c| c.label.as_str()).collect();
    assert_eq!(labels, ["Out", "L1", "L2", "L3", "L4", "In"]);
    let kinds: Vec<LapStripKind> = strip.iter().map(|c| c.kind).collect();
    assert_eq!(kinds[0], LapStripKind::Out);
    assert_eq!(kinds[1], LapStripKind::Flying);
    assert_eq!(kinds[5], LapStripKind::In);
    let best: Vec<i32> = strip.iter().filter(|c| c.best).map(|c| c.lap_id).collect();
    assert_eq!(best, [2]);
    // Driven time drops the stationary tail of lap 4, nothing else.
    let lap4 = strip.iter().find(|c| c.lap_id == 4).unwrap();
    assert!(lap4.driven_s < 20.5 - 2.0, "{}", lap4.driven_s);
    assert!(lap4.driven_s > 20.5 - 4.0, "{}", lap4.driven_s);
    let lap3 = strip.iter().find(|c| c.lap_id == 3).unwrap();
    assert!((lap3.driven_s - 20.0).abs() < 1e-9);

    assert_eq!(lap.neighbour_lap(-1).map(|l| l.id), Some(1));
    assert_eq!(lap.neighbour_lap(1).map(|l| l.id), Some(3));
}

#[test]
fn load_lap_reports_missing_laps_and_honours_cancel() {
    let recording = recording();
    assert_eq!(
        load_lap(
            recording.clone(),
            42,
            &LoadOptions::default(),
            &not_cancelled()
        )
        .unwrap_err(),
        SessionError::NoSuchLap(42)
    );
    let cancel = AtomicBool::new(true);
    assert_eq!(
        load_lap(recording, 2, &LoadOptions::default(), &cancel).unwrap_err(),
        SessionError::Cancelled
    );
}

#[test]
fn raw_source_channels_are_an_opt_in_provider() {
    let options = LoadOptions::default()
        .with_source_channel_keys(vec!["raw:RPM".into(), "raw:Missing".into()]);
    assert_eq!(options.source_channel_keys().len(), 2);
    let lap = load_lap(recording(), 1, &options, &not_cancelled()).unwrap();
    let source = lap
        .overlays()
        .iter()
        .find(|g| g.provider == "source")
        .expect("source group");
    // An unknown key is dropped, not fabricated.
    assert_eq!(source.channels.len(), 1);
    assert_eq!(source.channels[0].key, "raw:RPM");
    assert_eq!(source.channels[0].values.len(), lap.unified().len());

    let off = options.with_source_channel_keys(Vec::new());
    assert!(off.providers().iter().all(|p| p.id() != "source"));
}

#[test]
fn single_lap_analysis_generates_corners_without_deltas() {
    let lap = load_lap(recording(), 2, &LoadOptions::default(), &not_cancelled()).unwrap();
    let analysis = Analysis::build(
        &lap,
        None,
        StrategyRequest::Auto,
        0.0,
        None,
        &not_cancelled(),
    )
    .unwrap();
    assert!(analysis.comparison().is_none());
    assert!(analysis.delta().is_empty());
    assert_eq!(analysis.lap_time_delta(), None);
    assert_eq!(analysis.corner_source(), CornerSource::Generated);
    assert_eq!(analysis.corners().len(), 2);
    assert_eq!(analysis.rows().len(), 2);
    for row in analysis.rows() {
        assert!(row.primary.valid);
        assert!(row.reference.is_none());
        assert!(row.dt.is_nan() && row.score.is_nan());
        assert!(row.time > 0.0);
        assert!(row.speeds.apex < row.speeds.entry);
        assert_eq!(row.markers.len(), 4);
        let brake = row
            .markers
            .iter()
            .find(|m| m.kind == MarkerKind::Brake)
            .unwrap();
        assert!(brake.fraction >= row.zone.start && brake.fraction <= row.zone.end);
        assert!(brake.reference_fraction.is_none());
        assert!(row.notes.iter().all(|n| n.id != "matched"));
    }
    // A single lap swaps to itself.
    let same = analysis.swapped(&not_cancelled()).unwrap();
    assert_eq!(same.primary().lap_id(), 2);
}

fn pair(request: StrategyRequest, offset: f64) -> (Analysis, AtomicBool) {
    let recording = recording();
    let primary = load_lap(
        recording.clone(),
        3,
        &LoadOptions::default(),
        &not_cancelled(),
    )
    .unwrap();
    let reference = load_lap(recording, 2, &LoadOptions::default(), &not_cancelled()).unwrap();
    let cancel = not_cancelled();
    let analysis =
        Analysis::build(&primary, Some(&reference), request, offset, None, &cancel).unwrap();
    (analysis, cancel)
}

#[test]
fn pair_analysis_shares_one_comparison() {
    let (analysis, _) = pair(StrategyRequest::Auto, 0.0);
    let comparison = analysis.comparison().expect("comparison");
    assert!(!analysis.delta().is_empty());
    assert_eq!(analysis.delta().len(), analysis.primary().unified().len());
    assert!(
        analysis
            .available_strategies()
            .contains(&Strategy::LapPercentage)
    );
    assert!(
        analysis
            .available_strategies()
            .contains(&Strategy::ManualDampers)
    );
    assert_eq!(analysis.strategy(), Some(comparison.strategy()));
    assert_eq!(analysis.rows().len(), 2);
    for row in analysis.rows() {
        let reference = row.reference.as_ref().expect("reference metrics");
        assert!(reference.valid);
        assert!(row.dt.is_finite());
        // The row's time comes from the delta lane, never a second clock.
        let expected =
            comparison.time_delta_at(row.zone.end) - comparison.time_delta_at(row.zone.start);
        assert!((row.dt - expected).abs() < 1e-9);
        assert!((row.reference_time - (row.time - row.dt)).abs() < 1e-12);
        assert!((row.entry_dt + row.exit_dt - row.dt).abs() < 1e-9);
        assert!(row.score.is_finite() && (0.0..=100.0).contains(&row.score));
        assert!(!row.notes.is_empty(), "a note or 'Closely matched'");
        assert!(row.markers.iter().all(|m| m.reference_fraction.is_some()));
        // Lap 3 brakes later than lap 2 in every corner.
        assert!(row.brake_point_delta > 0.0, "{}", row.brake_point_delta);
    }
}

#[test]
fn a_lap_time_base_places_no_time_loss() {
    // The synthetic recording has no logger distance and no GPS: the map is
    // lap time %, whose delta spreads the lap-time gap evenly. It still
    // feeds the delta lane, but no loss rate or corner/straight split.
    let (analysis, _) = pair(StrategyRequest::Auto, 0.0);
    let comparison = analysis.comparison().expect("comparison");
    assert_eq!(comparison.basis(), "Lap time %");
    assert!(!analysis.delta().is_empty());
    assert!(!analysis.time_loss_placed());
    assert!(analysis.time_split().is_none());
    assert!(analysis.loss_rate().is_empty());

    // A single lap has neither.
    let (single, _) = {
        let recording = recording();
        let lap = load_lap(recording, 2, &LoadOptions::default(), &not_cancelled()).unwrap();
        let cancel = not_cancelled();
        (
            Analysis::build(&lap, None, StrategyRequest::Auto, 0.0, None, &cancel).unwrap(),
            cancel,
        )
    };
    assert!(!single.time_loss_placed());
    assert!(single.time_split().is_none());
    assert!(single.loss_rate().is_empty());
}

#[test]
#[expect(
    clippy::float_cmp,
    reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
)]
fn swapping_twice_restores_the_analysis() {
    let (analysis, cancel) = pair(StrategyRequest::Prefer(Strategy::ManualDampers), 0.001);
    let comparison = analysis.comparison().unwrap();
    assert_eq!(comparison.strategy(), Strategy::ManualDampers);
    assert_eq!(comparison.manual_offset(), 0.001);

    let swapped = analysis.swapped(&cancel).unwrap();
    // The lap-time difference is the laps' own times, inverted by a swap.
    let expected =
        (analysis.primary().lap().time_ms - analysis.reference().unwrap().lap().time_ms) / 1000.0;
    assert_eq!(analysis.lap_time_delta(), Some(expected));
    assert_eq!(swapped.lap_time_delta(), Some(-expected));
    assert_eq!(swapped.primary().lap_id(), 2);
    assert_eq!(swapped.reference().unwrap().lap_id(), 3);
    assert_eq!(swapped.comparison().unwrap().manual_offset(), -0.001);
    assert_eq!(swapped.request(), analysis.request());
    // Lap 3 brakes 0.004 of a lap later than lap 2, more than the 0.001
    // offset moves it. With roles exchanged the primary brakes earlier.
    assert!(
        analysis
            .rows()
            .iter()
            .all(|row| row.brake_point_delta > 0.0)
    );
    assert!(swapped.rows().iter().all(|row| row.brake_point_delta < 0.0));

    let back = swapped.swapped(&cancel).unwrap();
    assert_eq!(back.primary().lap_id(), 3);
    assert_eq!(back.comparison().unwrap().manual_offset(), 0.001);
    assert_eq!(back.delta(), analysis.delta());
    assert_eq!(back.corners(), analysis.corners());
    for (a, b) in back.rows().iter().zip(analysis.rows()) {
        assert_eq!(a.dt.to_bits(), b.dt.to_bits());
    }
}

#[test]
#[expect(
    clippy::float_cmp,
    reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
)]
fn manual_offset_rebuilds_only_the_delta_side() {
    let (analysis, cancel) = pair(StrategyRequest::Prefer(Strategy::ManualDampers), 0.0);
    let moved = analysis.with_manual_offset(0.01, &cancel).unwrap();
    assert_eq!(moved.comparison().unwrap().manual_offset(), 0.01);
    assert_ne!(moved.delta(), analysis.delta());
    assert_eq!(moved.corners(), analysis.corners());
    // Other strategies ignore the offset.
    let (auto, cancel) = pair(StrategyRequest::Prefer(Strategy::LapPercentage), 0.0);
    let same = auto.with_manual_offset(0.01, &cancel).unwrap();
    assert_eq!(same.delta(), auto.delta());
}

#[test]
fn analysis_honours_cancel() {
    let lap = load_lap(recording(), 2, &LoadOptions::default(), &not_cancelled()).unwrap();
    let cancel = AtomicBool::new(true);
    let result = Analysis::build(&lap, Some(&lap), StrategyRequest::Auto, 0.0, None, &cancel);
    assert_eq!(result.unwrap_err(), SessionError::Cancelled);
}

#[test]
fn a_corner_override_wins() {
    let lap = load_lap(recording(), 2, &LoadOptions::default(), &not_cancelled()).unwrap();
    let zones = vec![
        CornerZone {
            id: "t2".into(),
            name: "Back".into(),
            start: 0.6,
            end: 0.85,
            source: ZoneSource::Generated,
        },
        CornerZone {
            id: "t1".into(),
            name: "First".into(),
            start: 0.2,
            end: 0.45,
            source: ZoneSource::Generated,
        },
    ];
    let analysis = Analysis::build(
        &lap,
        None,
        StrategyRequest::Auto,
        0.0,
        Some(zones),
        &not_cancelled(),
    )
    .unwrap();
    assert_eq!(analysis.corner_source(), CornerSource::User);
    assert!(analysis.has_corner_override());
    let names: Vec<&str> = analysis.corners().iter().map(|z| z.name.as_str()).collect();
    assert_eq!(names, ["First", "Back"], "sorted by start");
    assert!(
        analysis
            .corners()
            .iter()
            .all(|z| z.source == ZoneSource::User)
    );
    assert!(analysis.row("t2").is_some());
}

#[test]
fn a_track_hint_maps_atlas_corners_by_distance() {
    let options = LoadOptions::default().with_track_hint(Some("road-atlanta".into()));
    assert_eq!(options.track_hint(), Some("road-atlanta"));
    let lap = load_lap(recording(), 2, &options, &not_cancelled()).unwrap();
    let layout = lap.layout().expect("layout from the hint");
    assert_eq!(layout.track_slug, "road-atlanta");
    let analysis = Analysis::build(
        &lap,
        None,
        StrategyRequest::Auto,
        0.0,
        None,
        &not_cancelled(),
    )
    .unwrap();
    assert_eq!(analysis.corner_source(), CornerSource::Atlas);
    assert!(analysis.corners().len() >= 10);
    // Complex members point at rows of this analysis.
    for complex in analysis.complex_rows() {
        assert!(complex.dt.is_nan(), "no delta without a reference");
        for &member in &complex.members {
            assert!(
                complex
                    .zone
                    .members
                    .contains(&analysis.rows()[member].zone.id)
            );
        }
    }
}

#[test]
fn lap_labels_follow_source_numbers_and_fragment_roles() {
    let mut laps = lap_list();
    laps[2].source_number = None; // sequential fallback: second complete lap
    let mut pit = Lap::new(9, 50.0, 60.0, 10_000.0, false);
    pit.kind = LapKind::Pit;
    laps.insert(3, pit);
    let mut frag = Lap::new(10, 60.0, 61.0, 1_000.0, false);
    frag.kind = LapKind::Unknown;
    laps.insert(4, frag);
    let labels = session::lap_labels(&laps);
    assert_eq!(labels, ["Out", "L1", "L2", "Pit", "Frag", "L3", "L4", "In"]);
    assert_eq!(session::best_lap_id(&laps), Some(2));
    assert_eq!(session::best_lap_id(&laps[..1]), None);
}

#[test]
fn recording_paths_are_recognised_by_extension() {
    for path in [
        "a.MP4",
        "b.pds",
        "c.ld",
        "d.vbo",
        "e.telemetry",
        "f.jsonl",
        "g.mtj.zst",
    ] {
        assert!(session::is_recording_path(Path::new(path)), "{path}");
    }
    // MTX sidecars describe a recording; they are not one.
    for path in ["notes.txt", "TRACK.yml", "clip.mov", "noext", "h.mtx.jsonl"] {
        assert!(!session::is_recording_path(Path::new(path)), "{path}");
    }
    assert!(session::is_video_path(Path::new("x.mp4")));
    assert!(!session::is_video_path(Path::new("x.pds")));
}

#[test]
fn identity_state_trust() {
    assert!(IdentityState::ExactSource.is_trusted());
    assert!(IdentityState::VerifiedHash.is_trusted());
    assert!(!IdentityState::NotChecked.is_trusted());
    assert!(!IdentityState::Mismatch.is_trusted());
    assert!(!IdentityState::Unverified.is_trusted());
}
