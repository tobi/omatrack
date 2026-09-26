//! Real-recording checks against copied `AiM` MP4s (read-only). Ignored by
//! default; run with
//! `OMATRACK_FIXTURES=~/Documents/Telemetry/26T07_PLM cargo test -- --include-ignored real_`.

#![cfg(test)]

use omatrack_core::alignment::{self, Options};
use omatrack_core::corners::zones::{StationMapper, atlas_corner_zones};
use omatrack_core::{ChannelOverrides, Recording, fastest_lap_index, format_lap_time, track};
use std::path::PathBuf;

fn mp4s() -> Vec<PathBuf> {
    let Ok(root) = std::env::var("OMATRACK_FIXTURES") else {
        panic!("set OMATRACK_FIXTURES to a folder of copied AiM MP4 recordings");
    };
    let mut files: Vec<PathBuf> = walk(&PathBuf::from(root))
        .into_iter()
        .filter(|p| p.extension().is_some_and(|e| e.eq_ignore_ascii_case("mp4")))
        .collect();
    files.sort();
    assert!(!files.is_empty(), "no MP4 fixtures found");
    files
}

fn walk(dir: &std::path::Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for entry in std::fs::read_dir(dir).unwrap().flatten() {
        let path = entry.path();
        if path.is_dir() {
            out.extend(walk(&path));
        } else {
            out.push(path);
        }
    }
    out
}

#[test]
#[ignore = "requires private telemetry/video fixtures; set OMATRACK_FIXTURES"]
#[expect(
    clippy::float_cmp,
    reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
)]
fn real_every_mp4_opens_with_laps_and_unifies() {
    for path in mp4s() {
        let recording = Recording::open(&path).unwrap();
        assert_eq!(recording.format_name(), "aimd", "{}", path.display());
        assert!(recording.utc_start_ns() > 0);
        assert!(recording.video_clock().valid());
        let mut laps = recording.detect_laps();
        assert!(!laps.is_empty(), "{}: no laps", path.display());
        let best = fastest_lap_index(&mut laps);
        let lap = &laps[best];
        assert!(lap.complete && !lap.is_pit_lap);
        let u = recording.unify_lap(lap.start_time, lap.end_time, &ChannelOverrides::new());
        assert!(u.len() > 1000);
        for (name, len) in [
            ("speed", u.speed.len()),
            ("throttle", u.throttle.len()),
            ("brake", u.brake.len()),
            ("steering", u.steering.len()),
            ("gear", u.gear.len()),
            ("distance", u.distance.len()),
            ("gps_lat", u.gps_lat.len()),
            ("fuel", u.fuel.len()),
        ] {
            assert_eq!(len, u.len(), "{name} misaligned in {}", path.display());
        }
        assert_eq!(u.distance[0], 0.0);
        assert!(
            u.distance.windows(2).all(|w| w[1] >= w[0]),
            "distance not monotonic"
        );
        assert!(u.speed.iter().copied().fold(0.0, f64::max) > 200.0);
    }
}

#[test]
#[ignore = "requires private telemetry/video fixtures; set OMATRACK_FIXTURES"]
fn real_run1_fastest_lap_is_lap_8() {
    let path = mp4s()
        .into_iter()
        .find(|p| p.to_string_lossy().contains("Run1"))
        .expect("Run1 fixture");
    let recording = Recording::open(&path).unwrap();
    assert_eq!(recording.channels().len(), 54);
    let mut laps = recording.detect_laps();
    assert_eq!(laps.len(), 11);
    let best = fastest_lap_index(&mut laps);
    assert_eq!(laps[best].id, 8);
    assert_eq!(format_lap_time(laps[best].time_ms), "1:13.644");
    let u = recording.unify_lap(
        laps[best].start_time,
        laps[best].end_time,
        &ChannelOverrides::new(),
    );
    assert_eq!(u.len(), 3683);
}

#[test]
#[ignore = "requires private telemetry/video fixtures; set OMATRACK_FIXTURES"]
fn real_index_open_agrees_with_full_open() {
    for path in mp4s() {
        let full = Recording::open(&path).unwrap();
        let index = Recording::open_index(&path).unwrap();
        assert_eq!(full.channels().len(), index.channels().len());
        // The index view omits the video-frame table (no first frames) and
        // may bound the trailing fragment differently; complete laps agree.
        let complete = |laps: Vec<omatrack_core::Lap>| -> Vec<(i32, f64, f64, f64)> {
            laps.into_iter()
                .filter(|l| l.complete)
                .map(|l| (l.id, l.start_time, l.end_time, l.time_ms))
                .collect()
        };
        assert_eq!(
            complete(full.detect_laps()),
            complete(index.detect_laps()),
            "{}",
            path.display()
        );
        // Index channels decode lazily, on first use. (An AiM index view
        // keeps only representative samples: analysis uses a full open.)
        let mapping = index.map_channels(&ChannelOverrides::new());
        let speed = mapping["speed"];
        assert!(index.channels()[speed].decoded().is_none());
        assert!(!index.samples(speed).is_empty());
        assert!(index.channels()[speed].decoded().is_some());
        assert!(index.samples(speed).len() <= full.samples(speed).len());
    }
}

#[test]
#[ignore = "requires private telemetry/video fixtures; set OMATRACK_FIXTURES"]
fn real_atlas_corners_map_through_gps() {
    let path = mp4s()[0].clone();
    let recording = Recording::open(&path).unwrap();
    let mut laps = recording.detect_laps();
    let best = fastest_lap_index(&mut laps);
    let u = recording.unify_lap(
        laps[best].start_time,
        laps[best].end_time,
        &ChannelOverrides::new(),
    );
    let layout = track::resolve_for_lap(Some("road-atlanta"), &u).expect("layout");
    let mapper = StationMapper::new(&u, &layout).expect("GPS matches the centerline");
    assert!(mapper.gps);
    let zones = atlas_corner_zones(&layout, &mapper);
    assert_eq!(zones.len(), 14);
    assert!(zones.iter().all(|z| z.end > z.start));
    // GPS resolution alone finds the facility too.
    assert_eq!(
        track::resolve_for_lap(None, &u).map(|l| l.track_slug),
        Some("road-atlanta")
    );
}

#[test]
#[ignore = "requires private telemetry/video fixtures; set OMATRACK_FIXTURES"]
fn real_alignment_basis_between_drivers() {
    let files = mp4s();
    let open = |i: usize| {
        let r = Recording::open(&files[i]).unwrap();
        let mut laps = r.detect_laps();
        let best = fastest_lap_index(&mut laps);
        r.unify_lap(
            laps[best].start_time,
            laps[best].end_time,
            &ChannelOverrides::new(),
        )
    };
    let (a, b) = (open(0), open(1));
    let result = alignment::compute(&a, &b, &Options::default());
    assert_eq!(result.fraction.len(), a.len());
    eprintln!(
        "alignment {} anchors={} rejected={} gps_available={}",
        result.basis,
        result.gps_anchors,
        result.gps_rejected,
        alignment::gps_available(&a, &b)
    );
}
