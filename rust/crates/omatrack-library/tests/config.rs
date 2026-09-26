//! `omatrack.yml`: typed keys, unknown keys kept at every level, lenient
//! scalars, atomic saves, and a malformed document never overwritten.

use omatrack_core::alignment::Strategy;
use omatrack_core::corners::{CornerZone, ZoneSource};
use omatrack_core::playback::ReferencePlayback;
use omatrack_library::config::{
    ChannelStyle, Config, ConfigError, ConfigFile, LocationConfig, TraceViewMode, XAxis, track_key,
};
use omatrack_library::recent::{MAX_RECENT_FILES, prune_recent, push_recent};
use serde_yaml::Value;

const DOCUMENT: &str = r##"
future_root_key: {keep: me}
locations:
  - id: "abc"
    type: "folder"
    target: "/data/telemetry"
    enabled: true
    name: "Team drive"
    color_hint: "blue"
  - id: "s3-1"
    type: "s3"
    target: "bucket/prefix"
    options: {region: "eu-west-1"}
recent_files:
  - "/a.mp4"
  - "/b.pds"
channels:
  speed:
    visible: "true"
    color: "#ffd400"
    weight: "2.5"
    stroke_width: 9
    note: "hand written"
  brake:
    combine_with_previous: true
    fill_opacity: 0.4
trace:
  fit_channels: false
  x_axis: time
  view_mode: consistency
  lane_gap: 3
video:
  muted: true
  reference_sync: "manual-dampers"
  reference_playback: "recording"
  continuous_playback: "yes"
  hud_position: {x: 0.25, y: "0.75"}
  image_telemetry: false
driver_mappings:
  12: "Ada"
  "2.5": "Bea"
track_assignments:
  "2026-09-02": "road-atlanta"
recording_metadata:
  "/a.mp4":
    driver: {name: "Override"}
tracks:
  road_atlanta:
    corners:
      - {name: "T1", start: "0.10", end: "0.14"}
      - {name: "Bad", start: "x", end: 0.2}
    atlas_note: "keep"
selection:
  primary_key: "/a.mp4"
  primary_lap: 8
  compare_key: "/b.pds"
  compare_lap: -1
  extra_selection: 1
workspace:
  layout: {version: 1, center: ["traces", "video"]}
  theme_hint: dark
"##;

fn extra_key<'a>(map: &'a serde_yaml::Mapping, key: &str) -> Option<&'a Value> {
    map.get(key)
}

#[test]
fn typed_keys_parse_leniently() {
    let config = Config::from_yaml_str(DOCUMENT).unwrap();
    let folders: Vec<_> = config.folder_locations().collect();
    assert_eq!(folders.len(), 1);
    assert_eq!(folders[0].resolved_id(), "abc");
    assert_eq!(folders[0].display_name(), "Team drive");
    assert!(folders[0].is_enabled());
    assert!(matches!(
        config.locations.as_ref().unwrap()[1],
        LocationConfig::Other(_)
    ));
    assert_eq!(config.recent_files, ["/a.mp4", "/b.pds"]);

    let speed = config.channel_style("speed");
    assert!(speed.visible);
    assert_eq!(speed.color.as_deref(), Some("#ffd400"));
    assert_eq!(speed.weight, 2.5);
    assert_eq!(speed.stroke_width, 4.0, "clamped");
    let brake = config.channel_style("brake");
    assert!(brake.combine_with_previous);
    assert_eq!(brake.fill_opacity, 0.4);
    let gear = config.channel_style("gear");
    assert_eq!(gear, ChannelStyle::defaults("gear"));
    assert!(!ChannelStyle::defaults("clutch").visible);
    // Brake has its own lane by default; `combine_with_previous` opts in.
    assert!(!ChannelStyle::defaults("brake").combine_with_previous);

    assert!(!config.trace.is_fitting_channels());
    assert_eq!(config.trace.x_axis(), XAxis::Time);
    assert_eq!(config.trace.view_mode(), TraceViewMode::Consistency);
    assert_eq!(Config::default().trace.view_mode(), TraceViewMode::Lap);
    assert!(config.video.is_muted());
    assert!(config.video.is_continuous_playback());
    assert_eq!(config.video.reference_sync(), Some(Strategy::ManualDampers));
    assert_eq!(
        config.video.reference_playback(),
        ReferencePlayback::Recording
    );
    let hud = config.video.hud_position().unwrap();
    assert_eq!((hud.x, hud.y), (0.25, 0.75));

    assert_eq!(config.driver_mappings["12"], "Ada");
    assert_eq!(config.driver_mappings["2.5"], "Bea");
    assert_eq!(config.track_assignments["2026-09-02"], "road-atlanta");
    assert!(
        config
            .recording_override(std::path::Path::new("/a.mp4"))
            .is_some()
    );

    let zones = config.track_corners("Road Atlanta").unwrap();
    assert_eq!(zones.len(), 1, "the unreadable zone is dropped");
    assert_eq!(zones[0].name, "T1");
    assert_eq!((zones[0].start, zones[0].end), (0.10, 0.14));
    assert_eq!(zones[0].source, ZoneSource::User);

    assert_eq!(config.selection.primary(), Some(("/a.mp4", 8)));
    assert_eq!(config.selection.reference(), None, "-1 is no lap");
    assert_eq!(
        config.workspace.layout.as_ref().unwrap()["center"][0],
        "traces"
    );
}

#[test]
fn unknown_keys_survive_a_round_trip_at_every_level() {
    let config = Config::from_yaml_str(DOCUMENT).unwrap();
    let text = config.to_yaml_string().unwrap();
    let again = Config::from_yaml_str(&text).unwrap();
    assert_eq!(
        again.to_yaml_string().unwrap(),
        text,
        "stable second round trip"
    );

    let root: Value = serde_yaml::from_str(&text).unwrap();
    assert_eq!(root["future_root_key"]["keep"], "me");
    assert_eq!(root["channels"]["speed"]["note"], "hand written");
    assert_eq!(root["trace"]["lane_gap"], 3);
    assert_eq!(root["video"]["image_telemetry"], false);
    assert_eq!(root["tracks"]["road_atlanta"]["atlas_note"], "keep");
    assert_eq!(root["selection"]["extra_selection"], 1);
    assert_eq!(root["workspace"]["theme_hint"], "dark");
    assert_eq!(root["locations"][0]["color_hint"], "blue");
    assert_eq!(root["locations"][0]["type"], "folder");
    // A location type this build does not interpret is kept verbatim.
    assert_eq!(root["locations"][1]["type"], "s3");
    assert_eq!(root["locations"][1]["options"]["region"], "eu-west-1");
    assert!(extra_key(&again.extra, "future_root_key").is_some());
    assert!(extra_key(&again.channels["speed"].extra, "note").is_some());
}

#[test]
fn save_is_atomic_and_load_round_trips() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("omatrack").join("omatrack.yml");
    assert_eq!(
        Config::load(&path).unwrap(),
        Config::default(),
        "missing file"
    );

    let mut config = Config::from_yaml_str(DOCUMENT).unwrap();
    config.push_recent_file("/c.ld");
    config.save(&path).unwrap();
    let loaded = Config::load(&path).unwrap();
    assert_eq!(loaded.recent_files[0], "/c.ld");
    assert_eq!(
        loaded.to_yaml_string().unwrap(),
        config.to_yaml_string().unwrap()
    );
    // Only the document itself is left behind: no temporary files.
    let names: Vec<_> = std::fs::read_dir(path.parent().unwrap())
        .unwrap()
        .map(|e| e.unwrap().file_name().into_string().unwrap())
        .collect();
    assert_eq!(names, ["omatrack.yml"]);
}

#[test]
fn a_malformed_document_is_never_overwritten() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("omatrack.yml");
    std::fs::write(&path, "channels: [\n").unwrap();
    let mut file = ConfigFile::open(&path);
    assert!(!file.is_writable());
    assert!(matches!(file.load_error(), Some(ConfigError::Parse { .. })));
    file.config_mut().push_recent_file("/must-not-write");
    assert!(matches!(file.save(), Err(ConfigError::Readonly { .. })));
    assert_eq!(std::fs::read_to_string(&path).unwrap(), "channels: [\n");

    let fresh = dir.path().join("fresh.yml");
    let mut file = ConfigFile::open(&fresh);
    assert!(file.is_writable());
    file.config_mut().push_recent_file("/x.mp4");
    file.save().unwrap();
    assert_eq!(Config::load(&fresh).unwrap().recent_files, ["/x.mp4"]);
}

#[test]
fn fresh_install_gets_the_default_location_once() {
    let mut config = Config::default();
    assert!(config.ensure_default_location(std::path::Path::new("/home/u/Documents/Telemetry")));
    assert!(!config.ensure_default_location(std::path::Path::new("/elsewhere")));
    assert_eq!(config.folder_locations().count(), 1);
    assert!(!config.add_folder_location(std::path::Path::new("/home/u/Documents/Telemetry")));
    assert!(config.add_folder_location(std::path::Path::new("/data")));
    let id = config
        .folder_locations()
        .find(|f| f.target.as_deref() == Some("/data"))
        .unwrap()
        .resolved_id();
    assert!(config.remove_location(&id));
    assert_eq!(config.folder_locations().count(), 1);
    // An explicitly empty library stays empty.
    let mut empty = Config::from_yaml_str("locations: []\n").unwrap();
    assert!(!empty.ensure_default_location(std::path::Path::new("/x")));
}

#[test]
fn corner_overrides_are_keyed_by_track() {
    assert_eq!(track_key("Road Atlanta"), "road_atlanta");
    assert_eq!(
        track_key("Circuit de Spa-Francorchamps!"),
        "circuit_de_spa_francorchamps"
    );
    assert_eq!(track_key("   "), "___");
    assert_eq!(track_key("!!!"), "unknown");
    let mut config = Config::default();
    let zones = [CornerZone {
        id: "t1".into(),
        name: "Turn 1".into(),
        start: 0.1,
        end: 0.2,
        source: ZoneSource::User,
    }];
    config.set_track_corners("Road Atlanta", Some(&zones));
    let text = config.to_yaml_string().unwrap();
    let again = Config::from_yaml_str(&text).unwrap();
    assert_eq!(again.track_corners("road atlanta").unwrap(), zones);
    let mut again = again;
    again.set_track_corners("Road Atlanta", None);
    assert!(again.tracks.is_empty());
}

#[test]
fn recent_files_are_capped_deduplicated_and_most_recent_first() {
    let mut list = Vec::new();
    for i in 0..10 {
        push_recent(&mut list, &format!("/r{i}.mp4"));
    }
    assert_eq!(list.len(), MAX_RECENT_FILES);
    assert_eq!(list[0], "/r9.mp4");
    push_recent(&mut list, "/r6.mp4");
    assert_eq!(list[0], "/r6.mp4");
    assert_eq!(list.len(), MAX_RECENT_FILES);
    assert_eq!(list.iter().filter(|p| *p == "/r6.mp4").count(), 1);
    push_recent(&mut list, "  ");
    assert_eq!(list[0], "/r6.mp4");

    let dir = tempfile::tempdir().unwrap();
    let kept = dir.path().join("kept.mp4");
    std::fs::write(&kept, b"x").unwrap();
    let kept = kept.to_string_lossy().into_owned();
    let mut list = vec![kept.clone(), "/missing.mp4".into(), kept.clone()];
    assert!(prune_recent(&mut list));
    assert_eq!(list, [kept]);
    assert!(!prune_recent(&mut list));
}
