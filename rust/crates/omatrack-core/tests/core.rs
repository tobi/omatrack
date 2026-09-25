//! Port of tests/CoreTest.cpp: the analysis core on synthetic data.
//! (MTX sidecar and `.telemetry` writer cases are out of scope for the port.)

use omatrack_core::laps::*;
use omatrack_core::mapping::*;
use omatrack_core::meta::{session_meta_from_filename, utc_start_ns_from_gps};
use omatrack_core::monotonic::*;
use omatrack_core::report::compare_telemetry_sources;
use omatrack_core::unify::resample;
use omatrack_core::video_clock::{VideoClock, VideoFileReference};
use omatrack_core::{RawChannel, Recording};

fn ch(name: &str, unit: &str, freq: f64, dur: f64, samples: Vec<f64>) -> RawChannel {
    RawChannel::synthetic(name, unit, freq, dur, samples)
}

fn src(channels: Vec<RawChannel>) -> Recording {
    Recording::synthetic("", channels)
}

fn overrides(pairs: &[(&str, &str)]) -> ChannelOverrides {
    pairs
        .iter()
        .map(|(k, v)| ((*k).to_string(), (*v).to_string()))
        .collect()
}

fn none() -> ChannelOverrides {
    ChannelOverrides::new()
}

// ── normalizeChannelName / GPS units ────────────────────────────────

#[test]
fn gps_coordinate_units_share_one_rule() {
    assert!((gps_coordinate_degrees(29.19 * 60.0, "min", false) - 29.19).abs() < 1e-9);
    assert!((gps_coordinate_degrees(81.07 * 60.0, "min", true) + 81.07).abs() < 1e-9);
    assert!((gps_coordinate_degrees(std::f64::consts::PI / 2.0, "rad", false) - 90.0).abs() < 1e-9);
    assert_eq!(gps_coordinate_degrees(-81.07, "deg", true), -81.07);
    assert_eq!(gps_coordinate_degrees(-81.07, "", true), -81.07);
}

#[test]
fn normalize_channel_name_cases() {
    assert_eq!(normalize_channel_name("GroundSpeed"), "groundspeed");
    assert_eq!(
        normalize_channel_name("Brake Pressure F!"),
        "brakepressuref"
    );
    assert_eq!(
        normalize_channel_name("lap_distance corrected"),
        "lapdistancecorrected"
    );
    assert!(normalize_channel_name("").is_empty());
    assert!(normalize_channel_name("___- -!").is_empty());
    assert_eq!(normalize_channel_name("Gear 2 Pos"), "gear2pos");
    assert_eq!(normalize_channel_name("GPS_Lat-N"), "gpslatn");
}

// ── formatLapTime ───────────────────────────────────────────────────

#[test]
fn format_lap_time_cases() {
    assert_eq!(format_lap_time(83550.0), "1:23.550");
    assert_eq!(format_lap_time(60000.0), "1:00.000");
    assert_eq!(format_lap_time(23450.0), "0:23.450");
    assert_eq!(format_lap_time(0.0), "0:00.000");
    assert_eq!(format_lap_time(183550.0), "3:03.550");
    assert_eq!(format_lap_time(83551.0), "1:23.551");
    assert_eq!(format_lap_time(600000.0), "10:00.000");
    assert_eq!(format_lap_time(59999.0), "0:59.999");
}

// ── sessionMetaFromFilename ─────────────────────────────────────────

#[test]
fn session_meta_from_filename_cases() {
    let m = session_meta_from_filename("260805143022_MQ12Di_LMP2");
    assert_eq!(m.date, "05/08/2026");
    assert_eq!(m.time, "14:30:22");
    assert_eq!(m.event_name, "260805143022_MQ12Di_LMP2");
    let m = session_meta_from_filename("practice_session");
    assert!(m.date.is_empty() && m.time.is_empty());
    assert_eq!(m.event_name, "practice_session");
    assert!(session_meta_from_filename("260805").date.is_empty());
    assert_eq!(
        session_meta_from_filename("260101120000_Race").event_name,
        "260101120000_Race"
    );
    let m = session_meta_from_filename("");
    assert!(m.date.is_empty() && m.event_name.is_empty());
    assert!(session_meta_from_filename("Q260805143022").date.is_empty());
}

// ── resample ────────────────────────────────────────────────────────

#[test]
fn resample_cases() {
    assert!(resample(&[], 100.0, 50.0, 1.0).is_empty());
    let v = [1.0, 2.0, 3.0, 4.0, 5.0];
    assert_eq!(resample(&v, 100.0, 100.0, 0.04), v.to_vec());
    assert_eq!(
        resample(&[0.0, 10.0], 10.0, 20.0, 0.1),
        vec![0.0, 5.0, 10.0]
    );
    let v: Vec<f64> = (0..11).map(f64::from).collect();
    let out = resample(&v, 100.0, 50.0, 0.1);
    assert_eq!(out.len(), 6);
    assert_eq!((out[0], out[1], out[5]), (0.0, 2.0, 10.0));
    let out = resample(&[42.0], 100.0, 50.0, 0.1);
    assert_eq!(out.len(), 6);
    assert!(out.iter().all(|s| *s == 42.0));
    assert!(resample(&[1.0, 2.0], 0.0, 50.0, 1.0).is_empty());
    assert!(resample(&[1.0, 2.0], 50.0, 0.0, 1.0).is_empty());
    assert!(resample(&[1.0, 2.0], 50.0, 50.0, 0.0).is_empty());
}

// ── split detectors ─────────────────────────────────────────────────

#[test]
fn beacon_splits() {
    let mut v = vec![0.0; 50];
    for i in 10..13 {
        v[i] = 1.0;
    }
    for i in 30..33 {
        v[i] = 1.0;
    }
    assert_eq!(pds_beacon_splits(&v, 10), vec![1.0, 3.0]);
    assert!(pds_beacon_splits(&[], 10).is_empty());
    assert!(pds_beacon_splits(&[1.0, 0.0, 1.0], 0).is_empty());
    assert!(pds_beacon_splits(&[0.0; 100], 10).is_empty());
    assert_eq!(pds_beacon_splits(&[1.0; 100], 10), vec![0.0]);
}

#[test]
fn lap_time_splits() {
    let mut v: Vec<f64> = (0..900).map(|i| f64::from(i) / 10.0).collect();
    v.extend((0..900).map(|i| f64::from(i) / 10.0));
    let splits = pds_lap_time_splits(&v, 10);
    assert!(!splits.is_empty());
    assert_eq!(splits[0], 90.0);
    let v: Vec<f64> = (0..1000).map(|i| f64::from(i) / 10.0).collect();
    assert!(pds_lap_time_splits(&v, 10).is_empty());
    assert!(pds_lap_time_splits(&[5.0], 10).is_empty());
    let mut v = vec![0.0; 20];
    v[5] = 100.0;
    v[6] = 0.0;
    v[8] = 100.0;
    v[9] = 0.0;
    assert_eq!(pds_lap_time_splits(&v, 10), vec![0.6]);
}

#[test]
fn last_lap_time_splits() {
    let mut v = vec![76852.0; 50];
    v.extend(std::iter::repeat_n(77034.0, 50));
    assert_eq!(pds_last_lap_time_splits(&v, 5), vec![10.0]);
    assert!(pds_last_lap_time_splits(&[0.0; 40], 5).is_empty());
    assert!(pds_last_lap_time_splits(&[76852.0], 5).is_empty());
}

#[test]
fn lap_number_splits() {
    let v: Vec<f64> = (1..=3)
        .flat_map(|lap| std::iter::repeat_n(f64::from(lap), 100))
        .collect();
    assert_eq!(pds_lap_number_splits(&v, 10), vec![10.0, 20.0]);
    assert!(pds_lap_number_splits(&[1.0; 200], 10).is_empty());
    assert!(pds_lap_number_splits(&[1.0], 10).is_empty());
    assert_eq!(
        pds_lap_number_splits(&[13.0, 0.0, 14.0, 14.0, 15.0], 10),
        vec![0.4]
    );
    assert!(pds_lap_number_splits(&[13.0, 0.0, 1.0, 1.0], 10).is_empty());

    assert!(lap_number_carries_state(&[13.0, 13.0, 0.0, 1.0]));
    let selected = select_lap_splits(&[], &[105.2], true, &[2.3, 13.5, 25.4], &[]);
    assert_eq!(selected, vec![2.3, 13.5, 25.4]);
    assert!(!lap_number_carries_state(&[0.0; 20]));
    assert_eq!(
        select_lap_splits(&[], &[], false, &[90.0, 180.0], &[]),
        vec![90.0, 180.0]
    );
    assert_eq!(
        select_lap_splits(
            &[],
            &[100.0, 200.0],
            true,
            &[12.0, 27.0, 43.0],
            &[101.0, 201.0]
        ),
        vec![100.0, 200.0]
    );
    assert_eq!(
        select_lap_splits(&[90.0, 180.0], &[100.0, 200.0], true, &[], &[]),
        vec![100.0, 200.0]
    );
    assert_eq!(
        select_lap_splits(&[90.0, 180.0], &[], true, &[12.0], &[]),
        vec![90.0, 180.0]
    );
}

#[test]
fn distance_splits() {
    let mut v: Vec<f64> = (0..500).map(|i| f64::from(i) * 10.0).collect();
    v.extend((0..500).map(|i| f64::from(i) * 10.0));
    assert_eq!(pds_distance_splits(&v, 10)[0], 50.0);
    let v: Vec<f64> = (0..500).map(|i| f64::from(i) * 10.0).collect();
    assert!(pds_distance_splits(&v, 10).is_empty());
    let mut v: Vec<f64> = (0..400).map(|i| f64::from(i) * 0.01).collect();
    v.extend((0..400).map(|i| f64::from(i) * 0.01));
    assert_eq!(pds_distance_splits(&v, 10)[0], 40.0);
}

// ── buildLapsFromSplits and friends ─────────────────────────────────

fn lap(id: i32, start: f64, end: f64, ms: f64, complete: bool) -> Lap {
    Lap::new(id, start, end, ms, complete)
}

#[test]
fn build_laps_from_splits_cases() {
    let laps = build_laps_from_splits(&[], 120.0, true);
    assert_eq!(laps.len(), 1);
    assert!(!laps[0].complete);
    assert_eq!((laps[0].start_time, laps[0].end_time), (0.0, 120.0));

    let laps = build_laps_from_splits(&[60.0], 120.0, true);
    assert_eq!(laps.len(), 1);
    assert!(!laps[0].complete);

    let laps = build_laps_from_splits(&[30.0, 120.0], 150.0, true);
    assert_eq!(laps.len(), 1);
    assert!(laps[0].complete);
    assert_eq!(
        (laps[0].start_time, laps[0].end_time, laps[0].time_ms),
        (30.0, 120.0, 90000.0)
    );

    let laps = build_laps_from_splits(&[30.0, 120.0, 210.0], 240.0, true);
    assert!(laps.len() >= 2 && laps[0].complete && laps[1].complete);

    let laps = build_laps_from_splits(&[10.0, 100.0, 115.0], 130.0, true);
    let short: Vec<_> = laps
        .iter()
        .filter(|l| l.end_time - l.start_time < 30.0)
        .collect();
    assert!(!short.is_empty() && short.iter().all(|l| !l.complete));

    let laps = build_laps_from_splits(&[30.0, 120.0, 123.0, 210.0], 240.0, true);
    let complete: Vec<_> = laps.iter().filter(|l| l.complete).collect();
    assert_eq!(complete.len(), 2);
    assert_eq!(
        (complete[0].start_time, complete[0].end_time),
        (30.0, 120.0)
    );
    assert_eq!(
        (complete[1].start_time, complete[1].end_time),
        (120.0, 210.0)
    );

    let laps = build_laps_from_splits(&[60.0, 150.0], 180.0, true);
    assert!(!laps[0].complete);
    assert!(build_laps_from_splits(&[30.0], 0.0, true).is_empty());
    let laps = build_laps_from_splits(&[-5.0, 60.0, 200.0], 120.0, true);
    assert_eq!(laps.len(), 1);
    assert!(!laps[0].complete);
}

#[test]
fn mark_short_crossings_rejects_authoritative_out_laps() {
    let mut laps = vec![
        lap(1, 0.0, 18.0, 18000.0, true),
        lap(2, 18.0, 118.0, 100000.0, true),
        lap(3, 118.0, 218.0, 100000.0, true),
    ];
    mark_short_crossings_incomplete(&mut laps);
    assert!(!laps[0].complete && laps[1].complete && laps[2].complete);
}

#[test]
fn previous_lap_times() {
    let mut previous = vec![0.0; 131];
    previous[128] = 117831.0;
    let confirmed =
        pds_apply_previous_lap_times(&[lap(0, 10.0, 128.0, 118000.0, true)], &previous, 1, true);
    assert!(confirmed[0].complete);
    assert_eq!(confirmed[0].time_ms, 117831.0);

    let out = pds_apply_previous_lap_times(
        &[lap(11, 1507.5, 1635.0, 127500.0, false)],
        &vec![116172.0; 1636],
        1,
        true,
    );
    assert!(!out[0].complete);
    assert_eq!(out[0].time_ms, 127500.0);

    let mut previous = vec![0.0; 513];
    previous[510] = 30500.0;
    let laps = [lap(0, 491.5, 510.3, 18800.0, true)];
    let confirmed = pds_apply_previous_lap_times(&laps, &previous, 1, true);
    assert!(!confirmed[0].complete);
    assert_eq!(confirmed[0].time_ms, 18800.0);
    let confirmed = pds_apply_previous_lap_times(&laps, &previous, 1, false);
    assert!(confirmed[0].complete);
    assert_eq!(confirmed[0].time_ms, 18800.0);
}

#[test]
fn lap_distance_coverage() {
    let laps = [
        lap(0, 0.0, 99.0, 99000.0, true),
        lap(1, 101.0, 200.0, 99000.0, true),
    ];
    let mut position = vec![0.0; 201];
    for i in 0..=99 {
        position[i] = i as f64;
    }
    for i in 100..=200 {
        position[i] = 40.0 + (i - 100) as f64 * 0.2;
    }
    let checked = pds_apply_lap_distance_coverage(&laps, &position, 1);
    assert!(checked[0].complete && !checked[1].complete);

    let laps = [
        lap(0, 0.0, 100.0, 100000.0, true),
        lap(1, 100.0, 200.0, 100000.0, true),
    ];
    let cumulative: Vec<f64> = (0..=200).map(f64::from).collect();
    let checked = pds_apply_lap_distance_coverage(&laps, &cumulative, 1);
    assert!(checked[0].complete && checked[1].complete);

    let laps = [
        lap(0, 0.0, 90.0, 90000.0, true),
        lap(1, 120.0, 210.0, 90000.0, true),
    ];
    let position: Vec<f64> = (0..100).map(f64::from).collect();
    let checked = pds_apply_lap_distance_coverage(&laps, &position, 1);
    assert!(checked[0].complete && checked[1].complete);

    let laps = [
        lap(0, 0.0, 40.0, 40000.0, true),
        lap(1, 40.0, 80.0, 40000.0, true),
        lap(2, 80.0, 120.0, 40000.0, true),
        lap(3, 120.0, 160.0, 40000.0, true),
    ];
    let mut position = vec![0.0; 161];
    for i in 0..=120 {
        position[i] = (i % 41) as f64;
    }
    for i in 121..=160 {
        position[i] = i as f64;
    }
    let checked = pds_apply_lap_distance_coverage(&laps, &position, 1);
    assert!(checked.iter().all(|l| l.complete));
}

#[test]
fn similar_duration_does_not_establish_missing_lap_boundaries() {
    let mut source = src(vec![]);
    source.set_source_laps(vec![
        lap(1, 0.0, 80.0, 80000.0, false),
        lap(2, 80.0, 160.0, 80000.0, false),
        lap(3, 160.0, 241.0, 81000.0, false),
        lap(4, 241.0, 260.0, 19000.0, false),
    ]);
    let mut laps = source.detect_laps();
    classify_laps(&mut laps);
    assert_eq!(laps.len(), 4);
    assert!(laps.iter().all(|l| !l.complete));
}

#[test]
fn long_head_and_tail_fragments_remain_incomplete() {
    let mut source = src(vec![]);
    source.set_source_laps(vec![
        lap(1, 0.0, 60.0, 60000.0, false),
        lap(2, 60.0, 160.0, 100000.0, true),
        lap(3, 160.0, 260.0, 100000.0, true),
        lap(4, 260.0, 330.0, 70000.0, false),
    ]);
    let laps = source.detect_laps();
    assert_eq!(laps.len(), 4);
    assert!(!laps[0].complete && laps[1].complete && laps[2].complete && !laps[3].complete);
}

// ── scoreChannelMatch ───────────────────────────────────────────────

#[test]
fn score_channel_match_cases() {
    assert!(score_channel_match("speed", "speed", 8) > 9000);
    assert!(
        score_channel_match("speed", "speed", 1) > score_channel_match("ground speed", "speed", 0)
    );
    assert_eq!(score_channel_match("ground speed", "speed", 0), 7000);
    assert_eq!(score_channel_match("throttle", "brake", 0), i32::MIN);
    assert_eq!(score_channel_match("", "speed", 0), i32::MIN);
    assert!(score_channel_match("speed", "speed", 0) > score_channel_match("speed", "speed", 5));
    assert_eq!(score_channel_match("speed", "ground speed", 0), 6000);
    assert_eq!(score_channel_match("tpsreal", "tps", 0), i32::MIN);
    assert_eq!(score_channel_match("speed", "", 0), i32::MIN);
}

// ── dominantDriverId ────────────────────────────────────────────────

#[test]
fn dominant_driver_id_cases() {
    assert_eq!(dominant_driver_id(&[0.0, 3.0, 3.0, 3.0, 5.0, 5.0], 0), 3.0);
    assert_eq!(dominant_driver_id(&[0.0, 7.0, 7.0, 9.0, 9.0], 0), 7.0);
    assert_eq!(dominant_driver_id(&[0.0, -1.0, -2.0, 0.0], 0), 0.0);
    assert_eq!(dominant_driver_id(&[], 0), 0.0);
    assert_eq!(dominant_driver_id(&[0.0, 0.0, 42.0, 0.0], 0), 42.0);
    assert_eq!(
        dominant_driver_id(&[0.0, -5.0, 0.0, -5.0, 3.0, 3.0, 3.0], 0),
        3.0
    );
    assert_eq!(dominant_driver_id(&[0.0, 2.5, 2.5, 3.75], 0), 2.5);
    let f = f64::from(2.1f32);
    assert_eq!(dominant_driver_id(&[f, f, 3.0], 6), 2.1);
    let code = 2.12345678901234;
    assert_eq!(dominant_driver_id(&[code, code, 3.0], 7), code);
    assert_eq!(dominant_driver_id(&[f64::NAN, f64::INFINITY, 4.5], 0), 4.5);
}

// ── VideoClock ──────────────────────────────────────────────────────

#[test]
fn video_clock_maps_positive_and_negative_offsets() {
    let positive = VideoClock {
        presentation_offset_ns: Some(100_000_000),
        ..Default::default()
    };
    assert_eq!(
        positive.presentation_time_ns(400_000_000, None),
        Some(500_000_000)
    );
    assert_eq!(
        positive.telemetry_time_ns(500_000_000, None),
        Some(400_000_000)
    );
    let negative = VideoClock {
        presentation_offset_ns: Some(-100_000_000),
        ..Default::default()
    };
    assert_eq!(
        negative.presentation_time_ns(400_000_000, None),
        Some(300_000_000)
    );
    assert_eq!(
        negative.telemetry_time_ns(300_000_000, None),
        Some(400_000_000)
    );
}

#[test]
fn video_clock_picks_last_presented_frame() {
    let clock = VideoClock {
        presentation_offset_ns: Some(100_000_000),
        presentation_times_ns: vec![100_000_000, 140_000_000, 220_000_000, 300_000_000],
        files: vec![],
    };
    assert_eq!(clock.frame_at(60_000_000), Some(1));
    assert_eq!(clock.frame_at(119_000_000), Some(1));
    assert_eq!(clock.frame_at(120_000_000), Some(2));
}

#[test]
fn video_clock_applies_offsets_per_file() {
    let clock = VideoClock {
        presentation_offset_ns: None,
        presentation_times_ns: vec![],
        files: vec![
            VideoFileReference {
                filename: "active.mp4".into(),
                index: 0,
                presentation_offset_ns: Some(100_000_000),
                ..Default::default()
            },
            VideoFileReference {
                filename: "reference.mp4".into(),
                index: 1,
                presentation_offset_ns: Some(250_000_000),
                ..Default::default()
            },
        ],
    };
    assert_eq!(clock.file_named("active.mp4").map(|f| f.index), Some(0));
    assert_eq!(clock.file_named("reference.mp4").map(|f| f.index), Some(1));
    assert!(clock.file_named("missing.mp4").is_none());
    assert_eq!(
        clock.presentation_time_ns(400_000_000, Some(0)),
        Some(500_000_000)
    );
    assert_eq!(
        clock.presentation_time_ns(400_000_000, Some(1)),
        Some(650_000_000)
    );
    assert_eq!(
        clock.telemetry_time_ns(650_000_000, Some(1)),
        Some(400_000_000)
    );
}

// ── Recording with synthetic data ───────────────────────────────────

#[test]
fn stopped_time_keeps_slow_pit_driving_and_unknown_samples() {
    let mut samples = vec![0.0; 481];
    for s in &mut samples[120..360] {
        *s = 20.0;
    }
    let with =
        |unit: &str, samples: Vec<f64>| src(vec![ch("Ground Speed", unit, 4.0, 0.0, samples)]);
    let stopped = with("km/h", samples)
        .stopped_duration(0.0, 120.0, &none())
        .unwrap();
    assert!((stopped - 60.0).abs() < 0.3);
    assert_eq!(
        with("km/h", vec![20.0; 481]).stopped_duration(0.0, 120.0, &none()),
        Some(0.0)
    );
    assert_eq!(
        with("m/s", vec![1.0; 481]).stopped_duration(0.0, 120.0, &none()),
        Some(0.0)
    );
    assert!(
        with("m/s", vec![f64::NAN; 481])
            .stopped_duration(0.0, 120.0, &none())
            .is_none()
    );
    let rpm = with("rpm", vec![0.0; 481]);
    assert!(rpm.stopped_duration(0.0, 120.0, &none()).is_none());
    assert!(rpm.stopped_duration(120.0, 0.0, &none()).is_none());
}

#[test]
fn stopped_time_uses_the_current_speed_override() {
    let source = src(vec![
        ch("Ground Speed", "km/h", 4.0, 0.0, vec![0.0; 481]),
        ch("Custom speed", "m/s", 4.0, 0.0, vec![5.0; 481]),
    ]);
    assert_eq!(source.stopped_duration(0.0, 120.0, &none()), Some(120.0));
    assert_eq!(
        source.stopped_duration(0.0, 120.0, &overrides(&[("speed", "Custom speed")])),
        Some(0.0)
    );
}

#[test]
fn map_channels_finds_concepts() {
    let s = src(vec![ch(
        "Ground Speed",
        "km/h",
        100.0,
        0.0,
        vec![0.0, 50.0, 100.0, 150.0],
    )]);
    assert_eq!(s.map_channels(&none()).get("speed"), Some(&0));
    let s = src(vec![
        ch("Ground Speed", "km/h", 0.0, 0.0, vec![0.0, 100.0]),
        ch("Throttle Pos", "%", 0.0, 0.0, vec![0.0, 50.0]),
        ch("Gear", "", 0.0, 0.0, vec![1.0, 2.0, 3.0]),
    ]);
    let m = s.map_channels(&none());
    assert_eq!((m["speed"], m["throttle"], m["gear"]), (0, 1, 2));
}

#[test]
fn map_driving_channels_without_confusing_engine_and_vehicle_speed() {
    let c = |name: &str, unit: &str, value: f64| ch(name, unit, 2.0, 1.0, vec![value, value]);
    let s = src(vec![
        c("Engine_Speed", "RPM", 6000.0),
        c("Throttle_Pedal", "%", 75.0),
        c("Vehicle_Speed", "kmh", 250.0),
        c("Brake_Pressure_Front", "bar", 40.0),
    ]);
    let m = s.map_channels(&none());
    assert_eq!(m["speed"], 2);
    assert_eq!(m["throttle"], 1);
    assert_eq!(m["driver_throttle"], 1);
    assert_eq!(m["brake"], 3);
    let lap = s.unify_lap(0.0, 1.0, &none());
    assert_eq!(lap.speed[25], 250.0);
    assert_eq!(lap.throttle[25], 0.75);
    assert_eq!(lap.driver_throttle[25], 0.75);
    assert_eq!(lap.brake[25], 40.0);
    assert!(*lap.distance.last().unwrap() > 60.0);
}

#[test]
fn map_speed_rejects_angular_units_and_ambiguous_substring() {
    for (name, unit) in [
        ("Speed", "RPM"),
        ("Ground Speed", "rad/s"),
        ("Engine_Speed", "RPM"),
        ("Engine_Speed", ""),
    ] {
        let s = src(vec![ch(name, unit, 0.0, 0.0, vec![6000.0, 6000.0])]);
        assert!(!s.map_channels(&none()).contains_key("speed"), "{name}");
    }
}

#[test]
fn map_speed_falls_back_to_ground_velocity() {
    for name in ["GPS Speed", "velocity kmh"] {
        let s = src(vec![
            ch("Engine_Speed", "RPM", 0.0, 0.0, vec![6000.0, 6000.0]),
            ch(name, "km/h", 2.0, 1.0, vec![220.0, 220.0]),
        ]);
        assert_eq!(s.map_channels(&none())["speed"], 1);
        assert_eq!(s.unify_lap(0.0, 1.0, &none()).speed[25], 220.0);
    }
}

#[test]
fn map_fuel_ignores_the_refuelling_probe() {
    let s = src(vec![ch("Fuel_Probe", "", 0.0, 0.0, vec![0.0, 1.0])]);
    assert!(!s.map_channels(&none()).contains_key("fuel"));
}

#[test]
fn map_channels_overrides() {
    let s = src(vec![
        ch("Ground Speed", "", 0.0, 0.0, vec![10.0, 20.0]),
        ch("Speed_Ref", "", 0.0, 0.0, vec![30.0, 40.0]),
    ]);
    assert_eq!(
        s.map_channels(&overrides(&[("speed", "speed_ref")]))["speed"],
        1
    );
    let s = src(vec![ch("Ground Speed", "", 0.0, 0.0, vec![10.0, 20.0])]);
    assert!(
        !s.map_channels(&overrides(&[("speed", "missing_channel")]))
            .contains_key("speed")
    );
}

#[test]
fn map_channels_skips_empty_and_unknown() {
    let s = src(vec![
        ch("Speed", "", 0.0, 0.0, vec![]),
        ch("Throttle", "", 0.0, 0.0, vec![0.0, 50.0]),
    ]);
    let m = s.map_channels(&none());
    assert!(!m.contains_key("speed") && m.contains_key("throttle"));
    let s = src(vec![ch(
        "Unknown Channel",
        "",
        0.0,
        0.0,
        vec![1.0, 2.0, 3.0],
    )]);
    assert!(s.map_channels(&none()).is_empty());
}

#[test]
fn detect_driver_id_cases() {
    let s = src(vec![ch(
        "DriverID",
        "",
        0.0,
        0.0,
        vec![0.0, 7.0, 7.0, 7.0, 3.0, 3.0],
    )]);
    assert_eq!(s.detect_driver_id(&none()), 7.0);
    let mut c = ch("DriverID", "", 0.0, 0.0, vec![0.0, 2.5, 2.5, 3.0]);
    c.sample_type_code = 6;
    assert_eq!(src(vec![c]).detect_driver_id(&none()), 2.5);
    let f = f64::from(2.1f32);
    let mut c = ch("DriverID", "", 0.0, 0.0, vec![f, f, 3.0]);
    c.sample_type_code = 6;
    assert_eq!(src(vec![c]).detect_driver_id(&none()), 2.1);
    assert_eq!(
        src(vec![ch("Speed", "", 0.0, 0.0, vec![100.0, 200.0])]).detect_driver_id(&none()),
        0.0
    );
    let s = src(vec![
        ch("X2LNK_driverID", "", 0.0, 0.0, vec![7.0, 7.0, 7.0]),
        ch("driver_id", "", 0.0, 0.0, vec![2.0, 2.0, 3.0]),
    ]);
    assert_eq!(
        s.detect_driver_id(&overrides(&[("driver_id", "driver_id")])),
        2.0
    );
    let s = src(vec![ch("DriverID", "", 0.0, 0.0, vec![7.0, 7.0, 7.0])]);
    assert_eq!(
        s.detect_driver_id(&overrides(&[("driver_id", "Missing Selector")])),
        0.0
    );
}

#[test]
fn sample_at_synthetic_channels() {
    let s = src(vec![ch("", "", 2.0, 1.0, vec![0.0, 10.0, 20.0])]);
    assert_eq!(s.sample_at(0, 0.25, true), Some(5.0));
    assert_eq!(s.sample_at(0, 1.5, true), None);
    let s = src(vec![ch("", "", 2.0, 1.0, vec![6.0, 3.0])]);
    assert_eq!(s.sample_at(0, 0.25, true), Some(4.5));
    assert_eq!(s.sample_at(0, 0.1, false), Some(6.0));
    assert_eq!(s.sample_at(0, 0.4, false), Some(3.0));
    let s = src(vec![ch("", "", 2.0, 0.0, vec![1.0, 2.0])]);
    assert_eq!(s.sample_at(0, -0.1, true), None);
    assert_eq!(s.sample_at(3, 0.0, true), None);
}

#[test]
fn throttle_prefers_powertrain_over_pedal() {
    let s = src(vec![
        ch("Driver Throttle Pos", "", 0.0, 0.0, vec![1.0, 2.0]),
        ch("TPS", "", 0.0, 0.0, vec![3.0, 4.0]),
    ]);
    let m = s.map_channels(&none());
    assert_eq!((m["throttle"], m["driver_throttle"]), (1, 0));
}

fn unify_one(name: &str, unit: &str, samples: Vec<f64>) -> omatrack_core::UnifiedLap {
    src(vec![ch(name, unit, 2.0, 1.0, samples)]).unify_lap(0.0, 1.0, &none())
}

#[test]
fn unify_lap_units_and_encodings() {
    let lap = unify_one("Gear", "", vec![6.0, 1.0]);
    assert!(lap.gear.len() > 2 && lap.gear.iter().all(|g| *g == 6 || *g == 1));
    assert_eq!(
        unify_one("Speed", "kph", vec![100.0, 100.0]).speed[25],
        100.0
    );
    assert_eq!(
        unify_one("Brake Pressure F", "MPa", vec![1.0, 1.0]).brake[25],
        10.0
    );
    let lap = unify_one("Speed", "km/h", vec![10.0, 10.0]);
    assert!(!lap.gps_lat[0].is_finite() && !lap.gps_lon[0].is_finite());
    assert_eq!(
        unify_one("Speed_Wspd_App", "", vec![250.0, 250.0]).speed[25],
        250.0
    );
    assert!((unify_one("Speed", "mph", vec![100.0, 100.0]).speed[25] - 160.934).abs() < 0.01);
    assert_eq!(unify_one("Speed", "m/s", vec![10.0, 10.0]).speed[25], 36.0);
    assert!(
        (unify_one("Brake Pressure F", "psi", vec![100.0, 100.0]).brake[25] - 6.89476).abs() < 1e-4
    );
    assert_eq!(
        unify_one("Brake Pressure F", "kPa", vec![100.0, 100.0]).brake[25],
        1.0
    );
    assert_eq!(unify_one("Brake Pos", "", vec![0.5, 0.5]).brake[25], 50.0);
    let steer = unify_one(
        "Steering Angle",
        "rad",
        vec![std::f64::consts::FRAC_PI_2; 2],
    )
    .steering[25];
    assert!((steer - 90.0).abs() < 1e-6);
    let lap = unify_one("Gear", "", vec![2.0, 7.0]);
    assert_eq!((lap.gear[0], *lap.gear.last().unwrap()), (1, 6));
    let lap = unify_one("Gear", "", vec![2.0, 3.0]);
    assert_eq!((lap.gear[0], *lap.gear.last().unwrap()), (2, 3));
    assert_eq!(
        unify_one("GPS Position Accuracy", "cm", vec![250.0, 250.0]).gps_position_accuracy[25],
        2.5
    );
    let lap = unify_one("Speed", "km/h", vec![100.0, 10000.0, 100.0]);
    assert!(lap.speed.iter().all(|v| *v <= 500.0) && lap.distance.iter().all(|v| v.is_finite()));
}

#[test]
fn unify_lap_multi_channel_normalization() {
    let s = src(vec![
        ch("Throttle Pos", "%", 2.0, 1.0, vec![75.0, 75.0]),
        ch("Clutch Pos", "%", 2.0, 1.0, vec![25.0, 25.0]),
    ]);
    let lap = s.unify_lap(0.0, 1.0, &none());
    assert_eq!((lap.throttle[25], lap.clutch[25]), (0.75, 0.25));

    let s = src(vec![
        ch("latitude", "min", 2.0, 1.0, vec![1800.0, 1800.0]),
        ch("longitude", "min", 2.0, 1.0, vec![4800.0, 4800.0]),
    ]);
    let lap = s.unify_lap(0.0, 1.0, &none());
    assert_eq!((lap.gps_lat[25], lap.gps_lon[25]), (30.0, -80.0));

    let s = src(vec![
        ch("GPS Latitude", "deg", 2.0, 0.5, vec![10.0, 11.0]),
        ch("GPS Longitude", "deg", 2.0, 0.5, vec![20.0, 21.0]),
    ]);
    let lap = s.unify_lap(0.0, 1.0, &none());
    assert!(lap.gps_lat[0].is_finite());
    assert!(!lap.gps_lat.last().unwrap().is_finite() && !lap.gps_lon.last().unwrap().is_finite());

    let s = src(vec![
        ch("Speed", "km/h", 2.0, 1.0, vec![0.0, 100.0, 200.0]),
        ch("Throttle", "%", 2.0, 1.0, vec![0.0, 50.0, 100.0]),
        ch("GPS Latitude", "rad", 2.0, 1.0, vec![1.0; 3]),
        ch("GPS Longitude", "deg", 2.0, 1.0, vec![2.0; 3]),
    ]);
    let lap = s.unify_lap(0.0, 1.0, &none());
    assert_eq!(lap.len(), 51);
    assert_eq!(
        (lap.speed[25], lap.throttle[25], lap.gps_lon[25]),
        (100.0, 0.5, 2.0)
    );
    assert!((lap.gps_lat[25] - 57.29577951308232).abs() < 1e-9);

    let s = src(vec![
        ch("Damper Travel FL", "", 2.0, 1.0, vec![12.0, 12.0]),
        ch("G Force Lat", "", 2.0, 1.0, vec![1.2, 1.2]),
    ]);
    let m = s.map_channels(&none());
    assert_eq!((m["damper_fl"], m["g_lat"]), (0, 1));
    let lap = s.unify_lap(0.0, 1.0, &none());
    assert_eq!((lap.damper_fl[25], lap.g_force_lat[25]), (12.0, 1.2));

    let lap = unify_one("Speed", "km/h", vec![72.0, 72.0]);
    assert_eq!(lap.distance[0], 0.0);
    assert!(lap.distance.windows(2).all(|w| w[1] >= w[0]));
    assert!((lap.time[1] - 0.02).abs() < 1e-9);
    assert_eq!(lap.sample_rate, 50);
}

#[test]
fn unify_lap_rejects_degenerate_bounds() {
    let s = src(vec![]);
    assert!(s.unify_lap(1.0, 1.0, &none()).is_empty());
    assert!(s.unify_lap(2.0, 1.0, &none()).is_empty());
}

#[test]
fn default_constructed_source_is_safe() {
    let s = src(vec![]);
    assert!(s.channels().is_empty() && s.source_laps().is_empty());
    assert!(s.path().is_empty() && s.format_name().is_empty());
    assert!(s.map_channels(&none()).is_empty() && s.detect_laps().is_empty());
    assert_eq!(s.detect_driver_id(&none()), 0.0);
    assert_eq!(s.sample_at(0, 0.0, true), None);
}

fn timer(name: &str, freq: f64, dur: f64, samples: Vec<f64>) -> RawChannel {
    ch(name, "", freq, dur, samples)
}

#[test]
fn detect_laps_fallback_sources() {
    let running: Vec<f64> = (0..3)
        .flat_map(|_| (0..800).map(|i| f64::from(i) * 100.0))
        .collect();
    let laps = src(vec![timer(
        "Current_Lap_Time",
        10.0,
        240.0,
        running.clone(),
    )])
    .detect_laps();
    assert!(laps.iter().any(|l| l.complete));

    let laps = src(vec![
        timer("Delta_Lap_Time", 10.0, 240.0, vec![0.0; 2400]),
        timer("Current_Lap_Time", 10.0, 240.0, running),
    ])
    .detect_laps();
    assert!(laps.iter().any(|l| l.complete));

    let mut previous = vec![76852.0; 400];
    previous.extend(std::iter::repeat_n(77034.0, 400));
    previous.extend(std::iter::repeat_n(77200.0, 400));
    let laps = src(vec![timer("Previous_LT", 5.0, 240.0, previous)]).detect_laps();
    assert!(laps.iter().any(|l| l.complete));

    let counter: Vec<f64> = (1..=3)
        .flat_map(|n| std::iter::repeat_n(f64::from(n), 15))
        .collect();
    let laps = src(vec![timer("Lap Number", 1.0, 45.0, counter.clone())]).detect_laps();
    assert!(laps.iter().any(|l| l.complete));

    let mut s = src(vec![timer("Lap Number", 1.0, 45.0, counter)]);
    s.set_source_laps(vec![lap(42, 5.0, 25.0, 19750.0, true)]);
    let laps = s.detect_laps();
    assert_eq!(laps.len(), 1);
    assert_eq!(
        (
            laps[0].id,
            laps[0].start_time,
            laps[0].end_time,
            laps[0].time_ms
        ),
        (42, 5.0, 25.0, 19750.0)
    );
    assert!(laps[0].complete && laps[0].source_number.is_none());
}

#[test]
fn detect_laps_marks_authoritative_short_laps_incomplete() {
    let mut s = src(vec![]);
    s.set_source_laps(vec![
        lap(1, 0.0, 20.0, 20000.0, true),
        lap(2, 20.0, 120.0, 100000.0, true),
        lap(3, 120.0, 220.0, 100000.0, true),
    ]);
    let laps = s.detect_laps();
    assert_eq!(laps.len(), 3);
    assert!(!laps[0].complete && laps[1].complete && laps[2].complete);
}

#[test]
fn compare_reports_channel_delta() {
    let left = src(vec![
        ch("Speed", "km/h", 2.0, 1.0, vec![100.0, 110.0]),
        ch("GPS Latitude", "", 2.0, 1.0, vec![43.8, 43.9]),
    ]);
    let right = src(vec![
        ch("Speed", "km/h", 2.0, 1.0, vec![100.0, 120.0]),
        ch("GPS Latitude", "", 2.0, 1.0, vec![43.8, 43.9]),
    ]);
    let report = compare_telemetry_sources(&left, &right, "aimd", "telemetry");
    assert!(report.contains("gps_lat"));
    assert!(report.contains("d=10"));
}

#[test]
fn opens_a_racelogic_vbo() {
    let dir = std::env::temp_dir().join(format!("omatrack-core-test-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let vbo = dir.join("run.vbo");
    std::fs::write(
        &vbo,
        "[header]\ntime\nvelocity kmh\n[column names]\ntime velocity\n[data]\n\
120000.0 10\n120000.5 20\n120001.0 30\n",
    )
    .unwrap();
    let source = Recording::open(&vbo).expect("vbo opens");
    assert_eq!(source.format_name(), "vbo");
    assert!(!source.channels().is_empty());
    assert!(source.map_channels(&none()).contains_key("speed"));
    std::fs::remove_dir_all(&dir).unwrap();
}

#[test]
fn open_reports_unsupported_and_missing_files() {
    let error = Recording::open("/no/such/dir/file.xyz").unwrap_err();
    assert_eq!(error.0, "unsupported telemetry format: \"xyz\"");
    assert!(Recording::open("/no/such/dir/file.mp4").is_err());
}

#[test]
fn gps_week_and_itow_become_utc_at_t0() {
    assert_eq!(
        utc_start_ns_from_gps(2429.0, 493904000.0, 0.0),
        1785517886000000000
    );
    assert_eq!(
        utc_start_ns_from_gps(2429.0, 493904000.0, 2.0),
        1785517884000000000
    );
    assert_eq!(utc_start_ns_from_gps(-1.0, 1.0, 0.0), -1);
}

// ── MonotonicSeries ─────────────────────────────────────────────────

#[test]
fn monotonic_view() {
    let x = [0.0, 1.0, 2.0, 3.0];
    let y = [10.0, 20.0, 30.0, 40.0];
    let view = MonotonicView::new(&x, &y);
    assert_eq!(
        (view.at(0.0), view.at(3.0), view.at(-1.0), view.at(5.0)),
        (10.0, 40.0, 10.0, 40.0)
    );
    assert_eq!(
        (view.at(0.5), view.at(1.5), view.at(2.25)),
        (15.0, 25.0, 32.5)
    );
    assert!((view.invert(15.0) - 0.5).abs() < 1e-9);
    assert!((view.invert(25.0) - 1.5).abs() < 1e-9);
    assert!((view.invert(35.0) - 2.5).abs() < 1e-9);
    assert_eq!((view.invert(5.0), view.invert(45.0)), (0.0, 3.0));
    assert_eq!(
        (
            view.lower_index(0.0),
            view.lower_index(1.5),
            view.lower_index(3.0),
            view.lower_index(5.0)
        ),
        (0, 2, 3, 3)
    );
    let (x, y) = ([0.0, 10.0, 20.0], [0.0, 100.0, 200.0]);
    assert_eq!(
        (
            interpolate(&x, &y, 5.0),
            interpolate(&x, &y, 0.0),
            interpolate(&x, &y, 20.0)
        ),
        (50.0, 0.0, 200.0)
    );
}

#[test]
fn monotonic_fractions() {
    let y = [0.0, 10.0, 20.0, 30.0];
    assert_eq!(
        (interpolate_fraction(&y, 0.0), interpolate_fraction(&y, 1.0)),
        (0.0, 30.0)
    );
    assert!((interpolate_fraction(&y, 0.5) - 15.0).abs() < 1e-9);
    assert!((interpolate_fraction(&y, 0.25) - 7.5).abs() < 1e-9);
    assert_eq!(interpolate_fraction(&[], 0.5), 0.0);
    assert_eq!(
        (
            interpolate_fraction(&[0.0, 10.0, 20.0], -0.5),
            interpolate_fraction(&[0.0, 10.0, 20.0], 1.5)
        ),
        (0.0, 20.0)
    );
    assert_eq!(interpolate_fraction(&[42.0], 0.5), 42.0);
    for f in [0.1, 0.25, 0.5, 0.75, 0.9] {
        assert!((invert_fraction(&y, interpolate_fraction(&y, f)) - f).abs() < 1e-9);
    }
    assert_eq!(invert_fraction(&[], 5.0), 0.0);
    assert_eq!(invert_fraction(&[5.0, 5.0, 5.0], 5.0), 0.0);
}

#[test]
fn converter_generation_matches_the_bridge() {
    // The C++ bridge derives `{FORMAT_VERSION}-{rev[..12]}` from its own
    // manifest; both pin the same upstream revision.
    assert_eq!(omatrack_core::converter_generation(), "10-cac837feb12f");
}
