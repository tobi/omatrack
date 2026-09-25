//! Index cache, catalog and scans on temporary folders (no fixtures).

mod common;

use common::{FakeLocation, summary, utc_ns};
use omatrack_core::session::LapStripKind;
use omatrack_library::catalog::{
    CatalogRecord, DateSource, LibraryFilter, LibrarySnapshot, session_start,
};
use omatrack_library::location::{Cancel, DiscoveredFile, FolderLocation, Location, LocationId};
use omatrack_library::metadata::{EffectiveMetadata, MetadataLayer, Sourced};
use omatrack_library::{CacheOutcome, Config, FileIdentity, IndexCache, scan_library};
use std::path::{Path, PathBuf};
use std::sync::Arc;

fn touch(path: &Path, bytes: &[u8]) {
    std::fs::create_dir_all(path.parent().unwrap()).unwrap();
    std::fs::write(path, bytes).unwrap();
}

#[test]
fn index_cache_hit_miss_failure_and_prune() {
    let data = tempfile::tempdir().unwrap();
    let cache_root = tempfile::tempdir().unwrap();
    let good = data.path().join("good.pds");
    let broken = data.path().join("broken.pds");
    touch(&good, b"telemetry");
    touch(&broken, b"garbage");
    let location = FakeLocation::new(data.path());
    let cache = IndexCache::with_generation(cache_root.path(), "gen-b");
    let file = |path: &Path| {
        DiscoveredFile::new(
            LocationId::new("fake"),
            path.to_path_buf(),
            FileIdentity::of(path).unwrap(),
        )
    };

    let (first, outcome) = cache.summarize(&location, &file(&good)).unwrap();
    assert_eq!(outcome, CacheOutcome::Miss);
    assert_eq!(first.laps.len(), 4);
    let (second, outcome) = cache.summarize(&location, &file(&good)).unwrap();
    assert_eq!(outcome, CacheOutcome::Hit);
    assert_eq!(second, first);
    assert_eq!(location.opens(), 1);

    // A failure is never stored: the next scan tries again.
    assert!(cache.summarize(&location, &file(&broken)).is_err());
    assert!(cache.summarize(&location, &file(&broken)).is_err());
    assert_eq!(location.opens(), 3);
    assert_eq!(std::fs::read_dir(cache.directory()).unwrap().count(), 1);

    // A changed file is a different identity: a miss.
    std::thread::sleep(std::time::Duration::from_millis(20));
    touch(&good, b"telemetry, edited");
    let (_, outcome) = cache.summarize(&location, &file(&good)).unwrap();
    assert_eq!(outcome, CacheOutcome::Miss);

    // Another generation neither reads these entries nor survives a prune.
    let other = IndexCache::with_generation(cache_root.path(), "gen-a");
    assert!(other.load(&FileIdentity::of(&good).unwrap()).is_none());
    touch(&other.directory().join("stale.json"), b"{}");
    assert_eq!(cache.prune_other_generations().unwrap(), 1);
    assert!(!other.directory().exists());
    assert!(cache.directory().exists());
    // Nothing was written beside the sources.
    let mut names: Vec<_> = std::fs::read_dir(data.path())
        .unwrap()
        .map(|e| e.unwrap().file_name().into_string().unwrap())
        .collect();
    names.sort();
    assert_eq!(names, ["broken.pds", "good.pds"]);
}

#[test]
fn folder_location_finds_supported_files_only() {
    let data = tempfile::tempdir().unwrap();
    for name in [
        "a/run1.mp4",
        "a/run2.PDS",
        "b/c/log.ld",
        "b/notes.txt",
        "b/TRACK.yml",
        ".hidden/x.mp4",
        "b/side.mtx.jsonl",
        "b/doc.mtj",
    ] {
        touch(&data.path().join(name), b"x");
    }
    // A linked recording is found; a link loop is skipped, not followed.
    std::os::unix::fs::symlink(
        data.path().join("a/run1.mp4"),
        data.path().join("b/linked.mp4"),
    )
    .unwrap();
    std::os::unix::fs::symlink(data.path(), data.path().join("b/loop")).unwrap();
    let location = FolderLocation::new(LocationId::new("f"), "Folder", data.path());
    let mut found = Vec::new();
    location
        .scan(&Cancel::new(), &mut |file| found.push(file))
        .unwrap();
    let mut names: Vec<String> = found
        .iter()
        .map(|f| {
            f.path()
                .strip_prefix(data.path())
                .unwrap()
                .to_string_lossy()
                .into_owned()
        })
        .collect();
    names.sort();
    assert_eq!(
        names,
        [
            "a/run1.mp4",
            "a/run2.PDS",
            "b/c/log.ld",
            "b/doc.mtj",
            "b/linked.mp4"
        ]
    );
    let mp4 = found
        .iter()
        .find(|f| f.path().ends_with("run1.mp4"))
        .unwrap();
    assert_eq!(location.media_path(mp4), Some(mp4.path().to_path_buf()));
    let ld = found.iter().find(|f| f.path().ends_with("log.ld")).unwrap();
    assert_eq!(location.media_path(ld), None);

    let cancel = Cancel::new();
    cancel.cancel();
    assert!(location.scan(&cancel, &mut |_| {}).is_err());
    let missing = FolderLocation::new(LocationId::new("m"), "Missing", data.path().join("nope"));
    assert!(missing.scan(&Cancel::new(), &mut |_| {}).is_err());
}

fn metadata(track: &str, slug: &str, driver: &str, session: &str) -> EffectiveMetadata {
    let mut meta = EffectiveMetadata::default();
    meta.track_name = Some(Sourced::new(track.into(), MetadataLayer::FolderMetadata));
    meta.track_slug = Some(Sourced::new(slug.into(), MetadataLayer::FolderMetadata));
    meta.driver = Some(Sourced::new(driver.into(), MetadataLayer::Preferences));
    meta.session = Some(Sourced::new(session.into(), MetadataLayer::Inferred));
    meta
}

fn record(path: &str, laps: &[f64], utc: i64, meta: EffectiveMetadata) -> CatalogRecord {
    let file = DiscoveredFile::new(
        LocationId::new("x"),
        PathBuf::from(path),
        FileIdentity::new(1, 2, 3, 0),
    );
    CatalogRecord::new(file, summary(laps, utc, ""), meta)
}

#[test]
fn catalog_groups_track_date_session_laps() {
    // 02:30 UTC on 3 Sep is 22:30 on 2 Sep in Georgia: the venue's day.
    let late = utc_ns("2026-09-03T02:30:00Z");
    let morning = utc_ns("2026-09-02T14:00:00Z");
    let snapshot = LibrarySnapshot::build(vec![
        record(
            "/t/ra/late.pds",
            &[80_000.0, 78_500.0],
            late,
            metadata("Road Atlanta", "road-atlanta", "Ada", "CT1"),
        ),
        record(
            "/t/ra/early.pds",
            &[81_000.0, 79_000.0, 150_000.0, 80_000.0],
            morning,
            metadata("Road Atlanta", "road-atlanta", "Bea", "CT1"),
        ),
        record(
            "/t/seb/one.pds",
            &[120_000.0],
            morning,
            metadata("Sebring", "sebring", "Ada", "FP1"),
        ),
    ]);
    let tracks = snapshot.tracks();
    assert_eq!(
        tracks.iter().map(|t| t.name.as_str()).collect::<Vec<_>>(),
        ["Road Atlanta", "Sebring"]
    );
    let road = &tracks[0];
    assert_eq!(road.id, "trk:road-atlanta");
    assert_eq!(road.dates.len(), 1, "both sessions on the venue's 2 Sep");
    let day = &road.dates[0];
    assert_eq!(day.key, "2026-09-02");
    assert_eq!(day.heading, "Wed 2 Sep 2026");
    assert_eq!(day.id, "trk:road-atlanta/d:2026-09-02");
    // Sessions by start time: 10:00 before 22:30 local.
    let titles: Vec<&str> = day.sessions.iter().map(|s| s.title.as_str()).collect();
    assert_eq!(titles, ["CT1 \u{b7} Bea", "CT1 \u{b7} Ada"]);
    let early = &day.sessions[0];
    assert!(early.id.starts_with("trk:road-atlanta/d:2026-09-02/s:"));
    assert_eq!(early.id.rsplit_once("/s:").unwrap().1.len(), 12);
    assert_eq!(early.start.source, DateSource::VenueClock);
    assert_eq!(early.lap_count, 4);
    // Lap 3 is a pit outlier (median * 1.35): it never counts for best.
    assert_eq!(early.best_lap_id, Some(2));
    assert_eq!(early.best_time_ms, Some(79_000.0));
    let labels: Vec<&str> = early.laps.iter().map(|l| l.label.as_str()).collect();
    assert_eq!(labels, ["Out", "L1", "L2", "L3", "L4"]);
    let pit = early.lap(3).unwrap();
    assert!(pit.pit && !pit.representative && pit.delta_to_best_ms.is_none());
    let l1 = early.lap(1).unwrap();
    assert_eq!(l1.delta_to_best_ms, Some(2_000.0));
    assert_eq!(l1.id, format!("{}/l:1", early.id));
    assert_eq!(early.laps[0].kind, LapStripKind::Out);
    assert!(early.lap(2).unwrap().best);
    assert_eq!(
        early.drive_time_ms,
        5_000.0 + 81_000.0 + 79_000.0 + 150_000.0 + 80_000.0
    );

    // Lookups by id.
    let (session, lap) = snapshot.lap(&l1.id).unwrap();
    assert_eq!((session.id.as_str(), lap.lap_id), (early.id.as_str(), 1));
    assert!(snapshot.session(&early.id).is_some());
    assert_eq!(snapshot.recording_count(), 3);

    // Facets.
    let facets = snapshot.facets();
    let tracks: Vec<(&str, usize)> = facets
        .tracks
        .iter()
        .map(|f| (f.value.as_str(), f.count))
        .collect();
    assert_eq!(tracks, [("road-atlanta", 2), ("sebring", 1)]);
    assert_eq!(facets.years.len(), 1);
    assert_eq!(facets.years[0].value, "2026");
    let drivers: Vec<(&str, usize)> = facets
        .drivers
        .iter()
        .map(|f| (f.value.as_str(), f.count))
        .collect();
    assert_eq!(drivers, [("Ada", 2), ("Bea", 1)]);

    // Search: track, driver, session and file name, case-insensitive.
    assert_eq!(snapshot.search("SEBRING").len(), 1);
    assert_eq!(snapshot.search("ada").len(), 2);
    assert_eq!(snapshot.search("fp1").len(), 1);
    assert_eq!(snapshot.search("late.PDS").len(), 1);
    assert_eq!(snapshot.search("nothing").len(), 0);
    let filtered = snapshot.filtered(&LibraryFilter::new().query("ada"));
    assert_eq!(filtered.recording_count(), 2);
    let filtered = snapshot.filtered(
        &LibraryFilter::new()
            .track(Some("road-atlanta".into()))
            .driver(Some("Ada".into())),
    );
    assert_eq!(filtered.recording_count(), 1);
    assert_eq!(
        filtered.tracks()[0].dates[0].sessions[0].driver.as_deref(),
        Some("Ada")
    );
    assert!(
        snapshot
            .filtered(&LibraryFilter::new().year(Some(2025)))
            .is_empty()
    );
    assert_eq!(snapshot.filtered(&LibraryFilter::new()), snapshot);
}

#[test]
fn dates_follow_venue_then_recording_then_fallbacks() {
    let late = utc_ns("2026-09-03T02:30:00Z");
    let identity = FileIdentity::new(0, 0, 0, 0);
    let path = Path::new("/x/2026-01-05/file.pds");
    let venue = session_start(
        &summary(&[1.0], late, "Asia/Tokyo"),
        Some("road-atlanta"),
        path,
        &identity,
    );
    assert_eq!(venue.date.unwrap().to_string(), "2026-09-02");
    assert_eq!(venue.source, DateSource::VenueClock);
    let recording = session_start(
        &summary(&[1.0], late, "Asia/Tokyo"),
        Some("Nowhere Ring"),
        path,
        &identity,
    );
    assert_eq!(recording.date.unwrap().to_string(), "2026-09-03");
    assert_eq!(recording.source, DateSource::RecordingClock);
    // No wall clock: the PDS filename, then a dated folder.
    let pds = session_start(
        &summary(&[1.0], -1, ""),
        None,
        Path::new("/x/260902101500_car.pds"),
        &identity,
    );
    assert_eq!(pds.date.unwrap().to_string(), "2026-09-02");
    assert_eq!(pds.time.unwrap().to_string(), "10:15:00");
    assert_eq!(pds.source, DateSource::Filename);
    let folder = session_start(&summary(&[1.0], -1, ""), None, path, &identity);
    assert_eq!(folder.date.unwrap().to_string(), "2026-01-05");
    assert_eq!(folder.source, DateSource::Folder);
    let unknown = session_start(
        &summary(&[1.0], -1, ""),
        None,
        Path::new("/x/y/z.pds"),
        &identity,
    );
    assert_eq!(unknown.source, DateSource::Unknown);
}

#[test]
fn scans_are_stable_and_hit_the_cache() {
    let data = tempfile::tempdir().unwrap();
    let cache_root = tempfile::tempdir().unwrap();
    let event = data.path().join("event");
    touch(
        &event.join("TRACK.yml"),
        b"track: {name: Road Atlanta, slug: road-atlanta}\nevent: Test day\n",
    );
    touch(&event.join("CT1").join("TRACK.yml"), b"{}\n");
    for name in ["Run1_CT1.pds", "Run4_CT1.pds", "broken.pds"] {
        touch(&event.join("CT1").join(name), b"x");
    }
    let location = Arc::new(FakeLocation::new(data.path()));
    let locations: Vec<Arc<dyn Location>> = vec![location.clone()];
    let cache = IndexCache::with_generation(cache_root.path(), "gen");
    let config = Config::default();
    let mut updates = Vec::new();

    let first = scan_library(&locations, &cache, &config, &Cancel::new(), &mut |p| {
        updates.push(p)
    })
    .unwrap();
    assert_eq!(first.cache_misses, 2);
    assert_eq!(first.cache_hits, 0);
    assert_eq!(first.failures.len(), 1, "broken.pds is reported");
    let snapshot = &first.snapshot;
    assert_eq!(snapshot.tracks().len(), 1);
    assert_eq!(snapshot.tracks()[0].name, "Road Atlanta");
    assert_eq!(snapshot.recording_count(), 2);
    let session = snapshot.sessions().next().unwrap();
    assert_eq!(session.session_name.as_deref(), Some("CT1"));
    assert_eq!(session.metadata.event(), Some("Test day"));
    assert_eq!(updates.last().unwrap().summarized, 3);

    let second = scan_library(&locations, &cache, &config, &Cancel::new(), &mut |_| {}).unwrap();
    assert_eq!(second.cache_hits, 2);
    assert_eq!(second.cache_misses, 0);
    assert_eq!(
        location.opens(),
        2 + 1 + 1,
        "only the broken file is reopened"
    );
    let ids = |s: &LibrarySnapshot| s.sessions().map(|s| s.id.clone()).collect::<Vec<_>>();
    assert_eq!(
        ids(&second.snapshot),
        ids(snapshot),
        "stable ids across rescans"
    );
    assert_eq!(second.snapshot, first.snapshot);

    let cancel = Cancel::new();
    cancel.cancel();
    assert!(scan_library(&locations, &cache, &config, &cancel, &mut |_| {}).is_err());
}

#[test]
fn locations_come_from_enabled_folder_entries() {
    let config = Config::from_yaml_str(
        "locations:\n  - {type: folder, target: /a, id: a}\n  - {type: folder, target: /b, enabled: false}\n  - {type: webdav, target: x}\n",
    )
    .unwrap();
    let locations = omatrack_library::locations_from_config(&config);
    assert_eq!(locations.len(), 1);
    assert_eq!(locations[0].id().as_str(), "a");
    assert_eq!(locations[0].name(), "a");
}

#[test]
fn library_types_are_send_and_sync() {
    fn check<T: Send + Sync>() {}
    check::<Config>();
    check::<LibrarySnapshot>();
    check::<IndexCache>();
    check::<Cancel>();
    check::<Arc<dyn Location>>();
    check::<omatrack_library::ScanOutcome>();
}
