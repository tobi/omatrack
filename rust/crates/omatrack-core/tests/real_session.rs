//! The session model on the real AiM recordings (read-only). Ignored by
//! default; run with
//! `OMATRACK_FIXTURES=~/Documents/Telemetry/26T07_PLM cargo test -- --include-ignored real_`.

use omatrack_core::consistency::SessionLaps;
use omatrack_core::session::{CornerSource, IdentityState, LoadedLap, MarkerKind, StrategyRequest};
use omatrack_core::{
    Analysis, LoadOptions, Recording, fastest_lap_index, format_lap_time, load_lap,
};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

fn mp4(run: &str) -> PathBuf {
    let Ok(root) = std::env::var("OMATRACK_FIXTURES") else {
        panic!("set OMATRACK_FIXTURES to the 26T07_PLM folder of AiM MP4 recordings");
    };
    let dir = PathBuf::from(root).join("CT1");
    std::fs::read_dir(&dir)
        .unwrap()
        .flatten()
        .map(|e| e.path())
        .find(|p| {
            p.to_string_lossy().contains(run)
                && p.extension().is_some_and(|e| e.eq_ignore_ascii_case("mp4"))
        })
        .unwrap_or_else(|| panic!("{run} fixture"))
}

fn fastest(run: &str, cancel: &AtomicBool) -> LoadedLap {
    let recording = Arc::new(Recording::open(mp4(run)).unwrap());
    let mut laps = recording.detect_laps();
    let index = fastest_lap_index(&mut laps);
    let best = laps[index].id;
    let options = LoadOptions::default().with_track_hint(Some("road-atlanta".into()));
    load_lap(recording, best, &options, cancel).unwrap()
}

#[test]
#[ignore]
fn real_session_pair_run4_against_run1() {
    let cancel = AtomicBool::new(false);
    let reference = fastest("Run1", &cancel);
    assert_eq!(reference.lap_id(), 8);
    assert_eq!(format_lap_time(reference.lap().time_ms), "1:13.644");
    assert_eq!(reference.unified().len(), 3683);
    let video = reference.video().expect("the MP4 is its own video");
    assert_eq!(video.identity, IdentityState::ExactSource);
    assert!(video.clock.valid());
    assert_eq!(reference.layout().unwrap().track_slug, "road-atlanta");
    let best: Vec<_> = reference.strip().iter().filter(|c| c.best).collect();
    assert_eq!(best.len(), 1);
    assert_eq!(best[0].label, "L8");
    assert!(
        reference
            .strip()
            .iter()
            .all(|c| c.driven_s <= c.time_ms / 1000.0 + 1e-9)
    );

    let primary = fastest("Run4", &cancel);
    let analysis = Analysis::build(
        &primary,
        Some(&reference),
        StrategyRequest::Auto,
        0.0,
        None,
        &cancel,
    )
    .unwrap();
    let comparison = analysis.comparison().unwrap();
    assert!(!analysis.delta().is_empty());
    // Run4 L10's GPS misses the atlas centerline (the C++ hid its corners);
    // Run1 L8's matches, so its ranges carry over through the shared map.
    assert!(
        matches!(
            analysis.corner_source(),
            CornerSource::Atlas | CornerSource::Reference
        ),
        "{:?}",
        analysis.corner_source()
    );
    assert_eq!(analysis.rows().len(), 14);
    assert!(!analysis.complexes().is_empty());
    eprintln!(
        "Run4 L{} vs Run1 L{}: {} ({}), corners {:?}, total delta {:+.3}s",
        primary.lap_id(),
        reference.lap_id(),
        comparison.basis(),
        comparison.confidence(),
        analysis.corner_source(),
        analysis.delta().last().unwrap()
    );
    for row in analysis.rows() {
        assert!(row.primary.valid, "{}", row.zone.name);
        assert!(row.dt.is_finite(), "{}", row.zone.name);
        assert!(!row.notes.is_empty());
        assert_eq!(row.markers.len(), 4);
        let apex = row
            .markers
            .iter()
            .find(|m| m.kind == MarkerKind::Apex)
            .unwrap();
        // The metrics scan whole samples: floor(start) ..= ceil(end).
        let sample = 1.0 / (primary.unified().len() - 1) as f64;
        assert!(
            apex.fraction >= row.zone.start - sample && apex.fraction <= row.zone.end + sample,
            "{} {} {:?}",
            row.zone.name,
            apex.fraction,
            (row.zone.start, row.zone.end)
        );
        eprintln!(
            "  {:<9} dt {:+.3}s  entry {:+.1}  apex {:+.1}  exit {:+.1} km/h  brake {:+.0}m  turn-in {:+.0}m  | {}",
            row.zone.name,
            row.dt,
            row.speeds.entry - row.reference_speeds.unwrap().entry,
            row.speeds.apex - row.reference_speeds.unwrap().apex,
            row.speeds.exit - row.reference_speeds.unwrap().exit,
            row.brake_point_delta,
            row.turn_in_delta,
            row.notes
                .iter()
                .map(|n| n.text.as_str())
                .collect::<Vec<_>>()
                .join(" · ")
        );
    }
    // The corner rows add up to the delta lane through their zones.
    let sum: f64 = analysis.rows().iter().map(|r| r.dt).sum();
    assert!(sum.is_finite());

    let swapped = analysis.swapped(&cancel).unwrap();
    assert_eq!(swapped.primary().lap_id(), 8);
    assert_eq!(swapped.corner_source(), CornerSource::Atlas);
    assert_eq!(swapped.rows().len(), 14);
    let own = Analysis::build(
        &reference,
        Some(&primary),
        StrategyRequest::Auto,
        0.0,
        None,
        &cancel,
    )
    .unwrap();
    assert_eq!(own.corners(), swapped.corners());
    let back = swapped.swapped(&cancel).unwrap();
    assert_eq!(back.delta(), analysis.delta());
}

#[test]
#[ignore]
fn real_session_consistency() {
    let cancel = AtomicBool::new(false);
    let primary = fastest("Run1", &cancel);
    let laps = SessionLaps::for_primary(&primary, &cancel).unwrap();
    assert!(laps.len() >= 3, "{}", laps.len());
    let confidence = laps.trace_confidence(&primary, &cancel).unwrap();
    assert!(confidence.lap_count() >= 2);
    let speed = confidence.band("speed").expect("speed band");
    assert!(speed.is_valid());
    assert_eq!(speed.median.len(), primary.unified().len());
    let finite = confidence
        .consistency()
        .iter()
        .filter(|v| v.is_finite())
        .count();
    assert!(finite > primary.unified().len() / 2);
    assert!(
        confidence
            .consistency()
            .iter()
            .filter(|v| v.is_finite())
            .all(|v| (0.0..=1.0).contains(v))
    );

    let analysis =
        Analysis::build(&primary, None, StrategyRequest::Auto, 0.0, None, &cancel).unwrap();
    let t5 = analysis.row("t5").expect("T5");
    let corner = laps.corner_consistency(&primary, t5.zone.start, t5.zone.end);
    assert!(corner.valid_lap_count >= 1);
    eprintln!(
        "confidence over {} laps; T5 brake points over {} laps: median {:.1} m, sd {:.1} m, range {:.1} m",
        confidence.lap_count(),
        corner.braking_lap_count,
        corner.median_brake_point,
        corner.brake_point_std_dev,
        corner.brake_point_range
    );
}
