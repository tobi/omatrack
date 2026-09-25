//! The library pipeline on the real AiM recordings (read-only). Ignored by
//! default; run with
//! `OMATRACK_FIXTURES=~/Documents/Telemetry/26T07_PLM cargo test -- --include-ignored real_`.
//! Every cache write goes to a temporary XDG cache root.

use omatrack_core::format_lap_time;
use omatrack_core::session::{Analysis, LoadOptions, StrategyRequest, load_lap};
use omatrack_library::location::{Cancel, FolderLocation, Location, LocationId, OpenMode};
use omatrack_library::{Config, IndexCache, Paths, SessionNode, scan_library};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;

fn fixtures() -> PathBuf {
    let Ok(root) = std::env::var("OMATRACK_FIXTURES") else {
        panic!("set OMATRACK_FIXTURES to the 26T07_PLM folder of AiM MP4 recordings");
    };
    PathBuf::from(root)
}

struct Library {
    _xdg: tempfile::TempDir,
    cache: IndexCache,
    location: Arc<FolderLocation>,
    locations: Vec<Arc<dyn Location>>,
}

fn library() -> Library {
    let xdg = tempfile::tempdir().unwrap();
    let paths = Paths::with_roots(
        xdg.path().join("config"),
        xdg.path().join("cache"),
        xdg.path().join("state"),
    );
    let cache = IndexCache::new(paths.index_cache_root());
    let location = Arc::new(FolderLocation::new(
        LocationId::new("fixtures"),
        "Fixtures",
        fixtures(),
    ));
    let locations: Vec<Arc<dyn Location>> = vec![location.clone()];
    Library {
        _xdg: xdg,
        cache,
        location,
        locations,
    }
}

fn session<'a>(sessions: &'a [&'a SessionNode], run: &str) -> &'a SessionNode {
    sessions
        .iter()
        .find(|s| s.file_name().contains(run))
        .unwrap_or_else(|| panic!("{run} session"))
}

#[test]
#[ignore]
fn real_scan_builds_the_road_atlanta_library_and_hits_the_cache() {
    let library = library();
    let config = Config::default();
    let first = scan_library(
        &library.locations,
        &library.cache,
        &config,
        &Cancel::new(),
        &mut |_| {},
    )
    .unwrap();
    assert!(first.failures.is_empty(), "{:?}", first.failures);
    assert_eq!(first.cache_misses, 3);
    let snapshot = &first.snapshot;
    assert_eq!(snapshot.tracks().len(), 1);
    let track = &snapshot.tracks()[0];
    assert_eq!(track.name, "Road Atlanta");
    assert_eq!(track.slug, "road-atlanta");
    assert_eq!(snapshot.recording_count(), 3);

    let sessions: Vec<&SessionNode> = snapshot.sessions().collect();
    for session in &sessions {
        assert_eq!(session.session_name.as_deref(), Some("CT1"));
        assert!(session.has_video);
        assert!(session.driver.is_some());
        eprintln!(
            "{} | {} | {} | {} laps | best {:?} | {}",
            session.id,
            session.title,
            track
                .dates
                .iter()
                .find(|d| d.sessions.iter().any(|s| s.id == session.id))
                .unwrap()
                .heading,
            session.lap_count,
            session.best_lap_id,
            session.file_name(),
        );
    }
    let run1 = session(&sessions, "Run1");
    assert_eq!(run1.best_lap_id, Some(8));
    let best = run1.lap(8).unwrap();
    assert_eq!(best.label, "L8");
    assert!(best.best);
    assert_eq!(format_lap_time(best.time_ms), "1:13.644");

    let second = scan_library(
        &library.locations,
        &library.cache,
        &config,
        &Cancel::new(),
        &mut |_| {},
    )
    .unwrap();
    assert_eq!(second.cache_hits, 3);
    assert_eq!(second.cache_misses, 0);
    assert_eq!(second.snapshot, first.snapshot);
}

#[test]
#[ignore]
fn real_run4_against_run1_analysis_through_the_library() {
    let library = library();
    let config = Config::default();
    let outcome = scan_library(
        &library.locations,
        &library.cache,
        &config,
        &Cancel::new(),
        &mut |_| {},
    )
    .unwrap();
    let sessions: Vec<&SessionNode> = outcome.snapshot.sessions().collect();
    let cancel = AtomicBool::new(false);
    let load = |run: &str| {
        let node = session(&sessions, run);
        let recording = library
            .location
            .open(&node.file, OpenMode::Full)
            .expect("full open");
        let options = LoadOptions::default()
            .with_overrides(node.metadata.channel_overrides.clone())
            .with_track_hint(node.metadata.track_slug().map(str::to_string))
            .with_video_path(library.location.media_path(&node.file));
        load_lap(
            Arc::new(recording),
            node.best_lap_id.expect("best lap"),
            &options,
            &cancel,
        )
        .unwrap()
    };
    let primary = load("Run4");
    let reference = load("Run1");
    assert_eq!(reference.lap_id(), 8);
    let analysis = Analysis::build(
        &primary,
        Some(&reference),
        StrategyRequest::Auto,
        0.0,
        None,
        &cancel,
    )
    .unwrap();
    let comparison = analysis.comparison().expect("comparison");
    assert!(!analysis.delta().is_empty());
    assert_eq!(analysis.delta().len(), primary.unified().len());
    assert!(analysis.rows().len() >= 10, "{}", analysis.rows().len());
    eprintln!(
        "basis {} confidence {} corners {} complexes {}",
        comparison.basis(),
        comparison.confidence(),
        analysis.rows().len(),
        analysis.complexes().len()
    );
}
