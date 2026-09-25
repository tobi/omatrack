//! GPS-to-centerline station mapping (port of `src/core/TrackAtlasSpatial`
//! plus the app's GeoJSON centerline parser). Maps Track Atlas station
//! fractions onto one GPS lap: project fixes onto the layout centerline,
//! bin by station, and make the station -> lap-fraction map monotonic with
//! pool-adjacent-violators so one noisy fix cannot reverse a corner.

use crate::num::{clamp, llround, max, min};
use crate::unify::UnifiedLap;

/// A 2D point: (longitude, latitude) or (station, fraction).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Point {
    pub x: f64,
    pub y: f64,
}

fn valid_gps_coordinate(latitude: f64, longitude: f64) -> bool {
    latitude.is_finite()
        && longitude.is_finite()
        && latitude.abs() <= 90.0
        && longitude.abs() <= 180.0
        && (latitude.abs() >= 0.001 || longitude.abs() >= 0.001)
}

/// True when the lap carries at least 10 valid fixes that actually move.
pub fn has_positional_gps(lap: &UnifiedLap) -> bool {
    if lap.gps_lat.len() < 10 || lap.gps_lat.len() != lap.gps_lon.len() {
        return false;
    }
    let mut min_lat = f64::MAX;
    let mut max_lat = f64::MIN;
    let mut min_lon = f64::MAX;
    let mut max_lon = f64::MIN;
    let mut valid = 0;
    for i in 0..lap.gps_lat.len() {
        if !valid_gps_coordinate(lap.gps_lat[i], lap.gps_lon[i]) {
            continue;
        }
        valid += 1;
        min_lat = min(min_lat, lap.gps_lat[i]);
        max_lat = max(max_lat, lap.gps_lat[i]);
        min_lon = min(min_lon, lap.gps_lon[i]);
        max_lon = max(max_lon, lap.gps_lon[i]);
    }
    valid >= 10 && ((max_lat - min_lat) > 1e-5 || (max_lon - min_lon) > 1e-5)
}

/// Longest LineString in a GeoJSON FeatureCollection (or a bare
/// LineString), as (lon, lat) points; empty unless it has >= 4 valid points.
pub fn parse_centerline(geojson: &str) -> Vec<Point> {
    let Ok(document) = serde_json::from_str::<serde_json::Value>(geojson) else {
        return Vec::new();
    };
    let line = |coordinates: &serde_json::Value| -> Vec<Point> {
        coordinates
            .as_array()
            .map(|items| {
                items
                    .iter()
                    .filter_map(|c| {
                        let c = c.as_array()?;
                        if c.len() < 2 {
                            return None;
                        }
                        let lon = c[0].as_f64().unwrap_or(f64::NAN);
                        let lat = c[1].as_f64().unwrap_or(f64::NAN);
                        valid_gps_coordinate(lat, lon).then_some(Point { x: lon, y: lat })
                    })
                    .collect()
            })
            .unwrap_or_default()
    };
    if document.get("type").and_then(|t| t.as_str()) == Some("LineString") {
        let points = line(&document["coordinates"]);
        return if points.len() >= 4 {
            points
        } else {
            Vec::new()
        };
    }
    let Some(features) = document.get("features").and_then(|f| f.as_array()) else {
        return Vec::new();
    };
    let mut best: Vec<Point> = Vec::new();
    for feature in features {
        let geometry = &feature["geometry"];
        if geometry.get("type").and_then(|t| t.as_str()) != Some("LineString") {
            continue;
        }
        let points = line(&geometry["coordinates"]);
        if points.len() > best.len() {
            best = points;
        }
    }
    if best.len() >= 4 { best } else { Vec::new() }
}

#[derive(Debug, Clone, Copy)]
struct Projection {
    station: f64,
    error_meters: f64,
}

/// Station -> lap-fraction map from projecting GPS fixes onto a
/// centerline. Empty when the lap or the centerline is unsuitable.
pub fn spatial_station_map(lap: &UnifiedLap, centerline: &[Point]) -> Vec<Point> {
    if !has_positional_gps(lap) || centerline.len() < 3 {
        return Vec::new();
    }
    const METERS_PER_DEGREE: f64 = 111_319.490_793_273_57;
    const RADIANS_PER_DEGREE: f64 = std::f64::consts::PI / 180.0;
    let mut reference_latitude = 0.0;
    for point in centerline {
        reference_latitude += point.y;
    }
    reference_latitude /= centerline.len() as f64;
    let longitude_scale = METERS_PER_DEGREE * (reference_latitude * RADIANS_PER_DEGREE).cos();
    let local_line: Vec<Point> = centerline
        .iter()
        .map(|p| Point {
            x: p.x * longitude_scale,
            y: p.y * METERS_PER_DEGREE,
        })
        .collect();
    let mut cumulative = vec![0.0; centerline.len()];
    for i in 1..local_line.len() {
        let dx = local_line[i].x - local_line[i - 1].x;
        let dy = local_line[i].y - local_line[i - 1].y;
        cumulative[i] = cumulative[i - 1] + dx.hypot(dy);
    }
    let centerline_length = cumulative[cumulative.len() - 1];
    if centerline_length < 100.0 {
        return Vec::new();
    }

    let project = |latitude: f64, longitude: f64| -> Projection {
        let fix = Point {
            x: longitude * longitude_scale,
            y: latitude * METERS_PER_DEGREE,
        };
        let mut best = Projection {
            station: 0.0,
            error_meters: f64::MAX,
        };
        for i in 1..local_line.len() {
            let start = local_line[i - 1];
            let delta = Point {
                x: local_line[i].x - start.x,
                y: local_line[i].y - start.y,
            };
            let length_squared = delta.x * delta.x + delta.y * delta.y;
            if length_squared <= 0.0 {
                continue;
            }
            let relative = Point {
                x: fix.x - start.x,
                y: fix.y - start.y,
            };
            let t = clamp(
                (relative.x * delta.x + relative.y * delta.y) / length_squared,
                0.0,
                1.0,
            );
            let nearest = Point {
                x: start.x + delta.x * t,
                y: start.y + delta.y * t,
            };
            let error = (fix.x - nearest.x).hypot(fix.y - nearest.y);
            if error >= best.error_meters {
                continue;
            }
            best.error_meters = error;
            best.station = (cumulative[i - 1] + length_squared.sqrt() * t) / centerline_length;
        }
        best
    };

    struct Anchor {
        station: f64,
        fraction: f64,
    }
    let mut anchors: Vec<Anchor> = Vec::new();
    let stride = (lap.gps_lat.len() / 2500).max(1);
    let mut minimum_station: f64 = 1.0;
    let mut maximum_station: f64 = 0.0;
    let mut i = 0usize;
    while i < lap.gps_lat.len() {
        let latitude = lap.gps_lat[i];
        let longitude = lap.gps_lon[i];
        let index = i;
        i += stride;
        if !valid_gps_coordinate(latitude, longitude) {
            continue;
        }
        let accuracy = if lap.gps_position_accuracy.len() == lap.gps_lat.len() {
            lap.gps_position_accuracy[index]
        } else {
            0.0
        };
        if accuracy.is_finite() && accuracy > 50.0 {
            continue;
        }
        let maximum_error = if accuracy.is_finite() && accuracy > 0.0 {
            clamp(accuracy * 3.0, 15.0, 75.0)
        } else {
            50.0
        };
        let projection = project(latitude, longitude);
        if projection.error_meters > maximum_error {
            continue;
        }
        let fraction = index as f64 / (lap.gps_lat.len() - 1).max(1) as f64;
        // The centerline is a closed loop: choose the copy nearest this
        // lap's time progress solely to unwrap start/finish.
        let unwrapped = projection.station + (fraction - projection.station).round();
        if !(-0.03..=1.03).contains(&unwrapped) || (unwrapped - fraction).abs() > 0.35 {
            continue;
        }
        let station = clamp(unwrapped, 0.0, 1.0);
        anchors.push(Anchor { station, fraction });
        minimum_station = min(minimum_station, station);
        maximum_station = max(maximum_station, station);
    }
    if anchors.len() < 30 || minimum_station > 0.12 || maximum_station < 0.88 {
        return Vec::new();
    }

    let mut mean_station = 0.0;
    let mut mean_fraction = 0.0;
    for anchor in &anchors {
        mean_station += anchor.station;
        mean_fraction += anchor.fraction;
    }
    mean_station /= anchors.len() as f64;
    mean_fraction /= anchors.len() as f64;
    let mut covariance = 0.0;
    let mut station_variance = 0.0;
    let mut fraction_variance = 0.0;
    for anchor in &anchors {
        let sd = anchor.station - mean_station;
        let fd = anchor.fraction - mean_fraction;
        covariance += sd * fd;
        station_variance += sd * sd;
        fraction_variance += fd * fd;
    }
    let correlation = covariance / (station_variance * fraction_variance).sqrt();
    if !correlation.is_finite() || correlation < 0.75 {
        return Vec::new();
    }

    const STATION_BINS: i64 = 500;
    let mut by_station: Vec<Vec<f64>> = vec![Vec::new(); STATION_BINS as usize + 1];
    for anchor in &anchors {
        let bin = crate::num::clamp_i(
            llround(anchor.station * STATION_BINS as f64) as i32 as i64,
            0,
            STATION_BINS,
        );
        by_station[bin as usize].push(anchor.fraction);
    }
    struct Row {
        station: f64,
        fraction: f64,
        weight: f64,
    }
    let mut rows: Vec<Row> = vec![Row {
        station: 0.0,
        fraction: 0.0,
        weight: 1.0,
    }];
    for bin in 1..STATION_BINS as usize {
        let fractions = &mut by_station[bin];
        if fractions.is_empty() {
            continue;
        }
        fractions.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        rows.push(Row {
            station: bin as f64 / STATION_BINS as f64,
            fraction: fractions[fractions.len() / 2],
            weight: fractions.len() as f64,
        });
    }
    rows.push(Row {
        station: 1.0,
        fraction: 1.0,
        weight: 1.0,
    });
    if rows.len() < 22 {
        return Vec::new();
    }

    // Pool-adjacent-violators: a monotonic station -> time map.
    struct Block {
        first: usize,
        last: usize,
        weighted_fraction: f64,
        weight: f64,
    }
    let mut blocks: Vec<Block> = Vec::new();
    for (row_index, row) in rows.iter().enumerate() {
        blocks.push(Block {
            first: row_index,
            last: row_index,
            weighted_fraction: row.fraction * row.weight,
            weight: row.weight,
        });
        while blocks.len() >= 2 {
            let previous = &blocks[blocks.len() - 2];
            let current = &blocks[blocks.len() - 1];
            if previous.weighted_fraction / previous.weight
                <= current.weighted_fraction / current.weight
            {
                break;
            }
            let merged = Block {
                first: previous.first,
                last: current.last,
                weighted_fraction: previous.weighted_fraction + current.weighted_fraction,
                weight: previous.weight + current.weight,
            };
            blocks.pop();
            blocks.pop();
            blocks.push(merged);
        }
    }
    for block in &blocks {
        let fitted = clamp(block.weighted_fraction / block.weight, 0.0, 1.0);
        for row in &mut rows[block.first..=block.last] {
            row.fraction = fitted;
        }
    }
    let last = rows.len() - 1;
    rows[0].fraction = 0.0;
    rows[last].fraction = 1.0;
    rows.iter()
        .map(|row| Point {
            x: row.station,
            y: row.fraction,
        })
        .collect()
}

/// Lap fraction at a station from a [`spatial_station_map`]; -1 when empty.
pub fn lap_fraction_at_station(mapping: &[Point], station: f64) -> f64 {
    if mapping.len() < 2 {
        return -1.0;
    }
    let station = clamp(station, 0.0, 1.0);
    let upper = mapping.partition_point(|p| p.x < station);
    if upper == 0 {
        return mapping[0].y;
    }
    if upper == mapping.len() {
        return mapping[mapping.len() - 1].y;
    }
    let high = mapping[upper];
    let low = mapping[upper - 1];
    let span = high.x - low.x;
    let local = if span > 0.0 {
        (station - low.x) / span
    } else {
        0.0
    };
    clamp(low.y + (high.y - low.y) * local, 0.0, 1.0)
}
