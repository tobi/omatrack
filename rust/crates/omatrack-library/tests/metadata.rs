//! TRACK.yml inheritance and atomic updates, and the five-layer metadata
//! precedence (mirrors YamlConfigTest's track-metadata cases).

mod common;

use omatrack_library::config::Config;
use omatrack_library::metadata::{
    MetadataLayer, MetadataSources, effective_metadata, inferred_car_class, inferred_car_number,
    inferred_session_name,
};
use omatrack_library::track_yml::{
    self, FolderMetadataCache, driver_name_for_id, normalized_driver_mapping_key,
};
use serde_yaml::Mapping;
use std::path::Path;

fn yaml(text: &str) -> Mapping {
    serde_yaml::from_str(text).unwrap()
}

fn text<'a>(map: &'a Mapping, path: &[&str]) -> Option<&'a str> {
    let mut value = map.get(path[0])?;
    for key in &path[1..] {
        value = value.get(*key)?;
    }
    value.as_str()
}

#[test]
fn every_parent_merges_root_to_leaf() {
    let dir = tempfile::tempdir().unwrap();
    let parent = dir.path().join("event");
    let child = parent.join("run");
    std::fs::create_dir_all(&child).unwrap();
    std::fs::write(
        parent.join("TRACK.yml"),
        "car: {number: \"7\", class: LMP2}\nchannels: {speed: Speed}\nevent: \"Parent\"\n",
    )
    .unwrap();
    std::fs::write(
        child.join("TRACK.yml"),
        "car: {number: \"8\"}\nchannels: {brake: Brake_Pressure}\nevent: \"  \"\n",
    )
    .unwrap();

    let (merged, paths) = track_yml::read_hierarchy(&child, true);
    assert_eq!(
        paths.last().unwrap(),
        &std::fs::canonicalize(child.join("TRACK.yml")).unwrap()
    );
    assert_eq!(text(&merged, &["car", "number"]), Some("8"));
    assert_eq!(text(&merged, &["car", "class"]), Some("LMP2"));
    assert_eq!(text(&merged, &["channels", "speed"]), Some("Speed"));
    assert_eq!(
        text(&merged, &["channels", "brake"]),
        Some("Brake_Pressure")
    );
    assert_eq!(
        text(&merged, &["event"]),
        Some("Parent"),
        "blank never erases"
    );

    let (without_target, paths) = track_yml::read_hierarchy(&child, false);
    assert_eq!(paths.len(), 1);
    assert_eq!(text(&without_target, &["car", "number"]), Some("7"));

    // The memoized per-scan snapshot agrees with a direct read.
    let mut cache = FolderMetadataCache::new();
    assert_eq!(cache.metadata(&child), merged);
    assert_eq!(cache.metadata(&parent), without_target);
    assert!(track_yml::file_path(&dir.path().join("missing")).is_none());
}

#[test]
fn merge_overlays_nested_maps() {
    let mut base = yaml("car: {number: \"7\", class: LMP2}\nkeep: yes\n");
    track_yml::merge(&mut base, &yaml("car: {number: \"8\"}\ndriver: A\n"));
    assert_eq!(text(&base, &["car", "number"]), Some("8"));
    assert_eq!(text(&base, &["car", "class"]), Some("LMP2"));
    assert_eq!(text(&base, &["driver"]), Some("A"));
    assert_eq!(text(&base, &["keep"]), Some("yes"));
    // A nested map that ends up empty is removed.
    let mut base = Mapping::new();
    track_yml::merge(&mut base, &yaml("car: {number: \"\"}\n"));
    assert!(base.get("car").is_none());
}

#[test]
fn update_replaces_owned_keys_and_keeps_the_rest() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("TRACK.yml");
    std::fs::write(
        &path,
        "schema: \"1\"\nfolder: {name: Old}\ncar: {number: old}\nfiles: {onboard.mp4: {offset: \"1.25\"}}\ncustom: keep\n",
    )
    .unwrap();
    let written = track_yml::update(
        dir.path(),
        &yaml("schema: \"2\"\nfolder: {name: Weekend share}\nevent: Road America\nnot_owned: dropped\n"),
    )
    .unwrap();
    assert_eq!(written, std::fs::canonicalize(&path).unwrap());
    let updated = track_yml::read_document(&path).unwrap();
    assert_eq!(
        text(&updated, &["files", "onboard.mp4", "offset"]),
        Some("1.25")
    );
    assert_eq!(text(&updated, &["custom"]), Some("keep"));
    assert_eq!(text(&updated, &["event"]), Some("Road America"));
    assert_eq!(text(&updated, &["folder", "name"]), Some("Weekend share"));
    assert!(
        updated.get("car").is_none(),
        "owned keys not given are removed"
    );
    assert!(updated.get("not_owned").is_none());
    let leftovers: Vec<_> = std::fs::read_dir(dir.path()).unwrap().collect();
    assert_eq!(leftovers.len(), 1, "no temporary files");

    // An empty update removes only owned keys; a missing file is created.
    track_yml::update(dir.path(), &Mapping::new()).unwrap();
    let updated = track_yml::read_document(&path).unwrap();
    assert!(updated.get("schema").is_none() && updated.get("folder").is_none());
    assert!(updated.get("files").is_some());
    let fresh = dir.path().join("selected");
    std::fs::create_dir(&fresh).unwrap();
    track_yml::update(&fresh, &yaml("series: IMSA\n")).unwrap();
    assert_eq!(
        text(
            &track_yml::read_document(&fresh.join("TRACK.yml")).unwrap(),
            &["series"]
        ),
        Some("IMSA")
    );
    assert!(track_yml::update(&dir.path().join("missing"), &Mapping::new()).is_err());
}

#[test]
fn driver_mapping_keys_and_lookup() {
    assert_eq!(normalized_driver_mapping_key("  *  ").as_deref(), Some("*"));
    assert_eq!(
        normalized_driver_mapping_key("02.500").as_deref(),
        Some("2.5")
    );
    assert_eq!(
        normalized_driver_mapping_key("3.25").as_deref(),
        Some("3.25")
    );
    assert_eq!(normalized_driver_mapping_key("12").as_deref(), Some("12"));
    assert!(normalized_driver_mapping_key("0").is_none());
    assert!(normalized_driver_mapping_key("all").is_none());
    let metadata =
        yaml("driver: {mappings: {\"*\": Any Driver, \"2.50\": Exact Driver, invalid: Ignored}}\n");
    assert_eq!(
        driver_name_for_id(&metadata, 2.5).as_deref(),
        Some("Exact Driver")
    );
    assert_eq!(
        driver_name_for_id(&metadata, 7.25).as_deref(),
        Some("Any Driver")
    );
    assert!(driver_name_for_id(&metadata, 0.0).is_none());
}

#[test]
fn filename_inference() {
    assert_eq!(
        inferred_session_name("26IMSA17_T07_PLM_CT1_Run1_MB", "CT1").as_deref(),
        Some("CT1")
    );
    assert_eq!(inferred_session_name("x_fp2_y", "").as_deref(), Some("FP2"));
    assert_eq!(inferred_session_name("Race", "").as_deref(), Some("Race"));
    assert_eq!(
        inferred_session_name("a-qualy", "").as_deref(),
        Some("Qualifying")
    );
    assert_eq!(
        inferred_session_name("warm-up 3", "").as_deref(),
        Some("Warmup")
    );
    assert_eq!(
        inferred_session_name("nothing", "Practice").as_deref(),
        Some("Practice")
    );
    assert_eq!(
        inferred_session_name("Racecar", ""),
        None,
        "whole tokens only"
    );
    assert_eq!(
        inferred_car_number("Sebring_Car52_Lap").as_deref(),
        Some("52")
    );
    assert_eq!(inferred_car_number("x #7 y").as_deref(), Some("7"));
    assert_eq!(inferred_car_number("oscar1"), None);
    assert_eq!(
        inferred_car_class("Event LMP2 #52").as_deref(),
        Some("LMP2")
    );
    assert_eq!(
        inferred_car_class("Event GT_LMP2 #52").as_deref(),
        Some("GT_LMP2")
    );
    assert_eq!(inferred_car_class("#52 only"), None);
}

#[test]
fn five_layers_in_order() {
    let path = Path::new("/events/sebring/CT1/26IMSA17_T07_PLM_CT1_Run1_MB.mp4");
    let mut summary = common::summary(&[80_000.0, 79_000.0], -1, "");
    summary.driver_id = 12.0;
    let folder = yaml(
        "track: {name: Road Atlanta, slug: road-atlanta}\nevent: Test\nchannels: {brake: Brake_F, nonsense: X}\ndriver: {mappings: {\"12\": Folder Name}}\n",
    );
    let mut config = Config::default();
    config
        .driver_mappings
        .insert("12".into(), "Preference Name".into());
    let sources = MetadataSources::new(&folder, &config).with_summary(Some(&summary));

    // Layer 2 over 3 over 4; inference last.
    let meta = effective_metadata(path, sources);
    assert_eq!(meta.track_name(), Some("Road Atlanta"));
    assert_eq!(
        meta.track_slug.as_ref().unwrap().layer,
        MetadataLayer::FolderMetadata
    );
    assert_eq!(meta.driver(), Some("Folder Name"));
    assert_eq!(
        meta.driver.as_ref().unwrap().layer,
        MetadataLayer::FolderMetadata
    );
    assert_eq!(meta.session(), Some("CT1"));
    assert_eq!(
        meta.session.as_ref().unwrap().layer,
        MetadataLayer::Inferred
    );
    assert_eq!(
        meta.channel_overrides.get("brake").map(String::as_str),
        Some("Brake_F")
    );
    assert!(!meta.channel_overrides.contains_key("nonsense"));

    // Layer 1: the per-recording override beats the folder.
    config.recording_metadata.insert(
        path.to_string_lossy().into_owned(),
        yaml("driver: {mappings: {\"12\": Override Name}}\nsession: Private\n"),
    );
    let meta = effective_metadata(
        path,
        MetadataSources::new(&folder, &config).with_summary(Some(&summary)),
    );
    assert_eq!(meta.driver(), Some("Override Name"));
    assert_eq!(
        meta.driver.as_ref().unwrap().layer,
        MetadataLayer::RecordingOverride
    );
    assert_eq!(meta.session(), Some("Private"));
    assert_eq!(text(&meta.document, &["session"]), Some("Private"));

    // Layer 3 when the folder says nothing about the driver or track.
    config.recording_metadata.clear();
    config
        .track_assignments
        .insert("2026-09-02".into(), "sebring".into());
    let bare = Mapping::new();
    let meta = effective_metadata(
        path,
        MetadataSources::new(&bare, &config)
            .with_summary(Some(&summary))
            .with_event_date(Some("2026-09-02")),
    );
    assert_eq!(meta.driver(), Some("Preference Name"));
    assert_eq!(
        meta.driver.as_ref().unwrap().layer,
        MetadataLayer::Preferences
    );
    assert_eq!(meta.track_slug(), Some("sebring"));
    assert_eq!(
        meta.track_slug.as_ref().unwrap().layer,
        MetadataLayer::Preferences
    );
    assert!(meta.track_name().unwrap().contains("Sebring"));

    // Layer 4 (raw id), then layer 5 (folder name) for the track.
    config.driver_mappings.clear();
    config.track_assignments.clear();
    let meta = effective_metadata(
        path,
        MetadataSources::new(&bare, &config).with_summary(Some(&summary)),
    );
    assert_eq!(meta.driver(), Some("Driver id 12"));
    assert_eq!(
        meta.driver.as_ref().unwrap().layer,
        MetadataLayer::Recording
    );
    assert_eq!(meta.track_name(), Some("CT1"));
    assert_eq!(
        meta.track_name.as_ref().unwrap().layer,
        MetadataLayer::Inferred
    );
    assert!(meta.track_slug.is_none());

    // GPS on the recording resolves the facility (layer 4).
    summary.gps = Some([34.1413, -83.8173]);
    let meta = effective_metadata(
        path,
        MetadataSources::new(&bare, &config).with_summary(Some(&summary)),
    );
    assert_eq!(meta.track_slug(), Some("road-atlanta"));
    assert_eq!(
        meta.track_slug.as_ref().unwrap().layer,
        MetadataLayer::Recording
    );
}
