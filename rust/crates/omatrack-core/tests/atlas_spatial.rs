//! Port of tests/TrackAtlasSpatialTest.cpp, plus the typed atlas model.

use omatrack_core::UnifiedLap;
use omatrack_core::atlas_spatial::*;
use omatrack_core::corners::zones::{StationMapper, atlas_complex_zones, atlas_corner_zones};
use omatrack_core::track;
use std::f64::consts::PI;

const CENTER_LAT: f64 = 43.8;
const CENTER_LON: f64 = -88.0;
const METERS_PER_DEGREE: f64 = 111_319.490_793_273_57;

fn longitude_scale() -> f64 {
    METERS_PER_DEGREE * (CENTER_LAT * PI / 180.0).cos()
}

fn circular_centerline() -> Vec<Point> {
    (0..=500)
        .map(|segment| {
            let angle = 2.0 * PI * f64::from(segment) / 500.0;
            Point {
                x: CENTER_LON + angle.cos() * 700.0 / longitude_scale(),
                y: CENTER_LAT + angle.sin() * 700.0 / METERS_PER_DEGREE,
            }
        })
        .collect()
}

fn spatial_lap(accuracy: f64) -> UnifiedLap {
    let mut lap = UnifiedLap::default();
    for sample in 0..1001 {
        let fraction = f64::from(sample) / 1000.0;
        let angle = 2.0 * PI * fraction * fraction;
        lap.gps_lon
            .push(CENTER_LON + angle.cos() * 700.0 / longitude_scale());
        lap.gps_lat
            .push(CENTER_LAT + angle.sin() * 700.0 / METERS_PER_DEGREE);
        lap.gps_position_accuracy.push(accuracy);
        // Unrelated to the station: the map must use the coordinates.
        lap.distance.push(10000.0 * fraction);
    }
    lap
}

#[test]
fn parses_geojson_centerline() {
    let geojson = r#"{"type":"FeatureCollection","features":[
      {"type":"Feature","properties":{"role":"pit_lane"},
       "geometry":{"type":"LineString","coordinates":[[-88.0,43.8],[-88.1,43.9]]}},
      {"type":"Feature","properties":{"role":"outline"},
       "geometry":{"type":"LineString","coordinates":[[-88.0,43.8],[-88.01,43.81],[-88.02,43.8],[-88.0,43.8]]}}]}"#;
    let centerline = parse_centerline(geojson);
    assert_eq!(centerline.len(), 4);
    assert_eq!(centerline[0], Point { x: -88.0, y: 43.8 });
}

#[test]
fn rejects_garbage_geojson() {
    assert!(parse_centerline("not json").is_empty());
    assert!(parse_centerline("{}").is_empty());
    assert!(parse_centerline(r#"{"type":"FeatureCollection","features":[]}"#).is_empty());
    assert!(
        parse_centerline(r#"{"type":"LineString","coordinates":[[-88,43],[-88.01,43.01]]}"#)
            .is_empty()
    );
    let bare = r#"{"type":"LineString","coordinates":[[-88.0,43.8],[-88.01,43.81],[-88.02,43.8],[-88.0,43.8]]}"#;
    assert_eq!(parse_centerline(bare).len(), 4);
}

#[test]
fn rejects_absent_short_and_nan_gps() {
    let mut lap = UnifiedLap {
        gps_lat: vec![0.0; 100],
        gps_lon: vec![0.0; 100],
        ..Default::default()
    };
    assert!(!has_positional_gps(&lap));
    lap.gps_lat = vec![43.8, 43.81, 43.82];
    lap.gps_lon = vec![-88.0, -88.01, -88.02];
    assert!(!has_positional_gps(&lap));
    lap.gps_lat = vec![f64::NAN; 20];
    lap.gps_lon = vec![f64::NAN; 20];
    assert!(!has_positional_gps(&lap));
}

#[test]
fn maps_atlas_stations_from_coordinates() {
    let lap = spatial_lap(2.0);
    assert!(has_positional_gps(&lap));
    let mapping = spatial_station_map(&lap, &circular_centerline());
    assert!(mapping.len() > 100);
    assert!((lap_fraction_at_station(&mapping, 0.25) - 0.5).abs() < 0.02);
    assert!((lap_fraction_at_station(&mapping, 0.81) - 0.9).abs() < 0.02);
    for w in mapping.windows(2) {
        assert!(w[1].x > w[0].x && w[1].y >= w[0].y);
    }
}

#[test]
fn rejects_poor_accuracy_and_distant_traces() {
    let lap = spatial_lap(250.0);
    assert!(has_positional_gps(&lap));
    assert!(spatial_station_map(&lap, &circular_centerline()).is_empty());
    let mut lap = spatial_lap(0.0);
    lap.gps_lat.iter_mut().for_each(|v| *v += 0.02);
    assert!(has_positional_gps(&lap));
    assert!(spatial_station_map(&lap, &circular_centerline()).is_empty());
}

#[test]
fn empty_mapping_yields_no_station() {
    assert_eq!(lap_fraction_at_station(&[], 0.25), -1.0);
    let mapping = [Point { x: 0.0, y: 0.0 }, Point { x: 1.0, y: 1.0 }];
    assert_eq!(lap_fraction_at_station(&mapping, 0.5), 0.5);
    assert_eq!(lap_fraction_at_station(&mapping, -1.0), 0.0);
    assert_eq!(lap_fraction_at_station(&mapping, 2.0), 1.0);
}

#[test]
fn short_centerline_cannot_map() {
    assert!(spatial_station_map(&spatial_lap(2.0), &[]).is_empty());
    let two = [
        Point { x: -88.0, y: 43.8 },
        Point {
            x: -88.01,
            y: 43.81,
        },
    ];
    assert!(spatial_station_map(&spatial_lap(2.0), &two).is_empty());
}

// ── typed atlas model ───────────────────────────────────────────────

#[test]
fn resolves_road_atlanta_with_both_corner_scopes() {
    let facility = track::find_track("Road Atlanta").expect("road atlanta in the atlas");
    assert_eq!(facility.slug, "road-atlanta");
    assert_eq!(
        track::find_track("road-atlanta").map(|t| t.slug),
        Some("road-atlanta")
    );
    assert_eq!(
        track::timezone_for_venue("Road Atlanta"),
        Some("America/New_York")
    );
    let layout = track::resolve_layout(facility, 4088.0).unwrap();
    assert_eq!(layout.layout_id, "gp");
    assert_eq!(layout.corner_ranges.len(), 14);
    assert!(layout.corner_complexes.len() >= 10);
    assert!(layout.centerline.len() > 100);
    // Complexes keep their members; they are never flattened into corners.
    assert!(
        layout
            .corner_complexes
            .iter()
            .all(|c| !c.members.is_empty())
    );
    assert_eq!(layout.corner_label(&layout.corner_ranges[0]), "Turn 1");
    assert_eq!(
        track::find_track_by_gps(34.1413, -83.8173).map(|t| t.slug),
        Some("road-atlanta")
    );
}

#[test]
fn distance_fallback_maps_atlas_ranges_without_gps() {
    let layout = track::resolve_layout(track::find_track("road-atlanta").unwrap(), 4088.0).unwrap();
    let mut lap = UnifiedLap::default();
    for i in 0..=4088 {
        lap.time.push(f64::from(i) / 50.0);
        lap.distance.push(f64::from(i));
    }
    let mapper = StationMapper::new(&lap, &layout).unwrap();
    assert!(!mapper.gps);
    let zones = atlas_corner_zones(&layout, &mapper);
    assert_eq!(zones.len(), 14);
    assert!((zones[0].start - 0.07265).abs() < 1e-3);
    assert!(zones.windows(2).all(|w| w[0].start <= w[1].start));
    let complexes = atlas_complex_zones(&layout, &mapper);
    assert!(!complexes.is_empty());
}
