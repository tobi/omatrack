//! Typed Track Atlas model over the embedded `motorsport-track-atlas`
//! catalog: facilities, layouts, centerlines, and the two corner scopes the
//! product treats as first class — individual `corner_ranges` and grouped
//! `corner_complexes` (never flattened into each other).
//!
//! Track Atlas data is ODbL; the app shows [`ATTRIBUTION`].

use crate::atlas_spatial::{self, Point};
use crate::unify::UnifiedLap;
use motorsport_track_atlas as atlas;
use serde::Deserialize;

/// Upstream attribution (the embedded catalog's `data/ATTRIBUTION.md`),
/// shown wherever atlas geometry or corner names are.
pub const ATTRIBUTION: &str = "\
Track data: Track Atlas (https://github.com/tobi/track-atlas).
Geometry and named-corner coordinates are derived from OpenStreetMap data via \
the Overpass API: \u{a9} OpenStreetMap contributors, licensed under the Open \
Database License (ODbL), https://opendatacommons.org/licenses/odbl/.
Ordered corner metadata and the colloquial corner-name base layer come from \
Lovely-Sim-Racing/lovely-track-data \
(https://github.com/Lovely-Sim-Racing/lovely-track-data); corner and straight \
names are credited upstream to Racing Circuits.
Curated overrides (official names, complex grouping) are offered under the \
Track Atlas MIT license.";

/// Track Atlas git revision embedded in this build.
pub fn atlas_revision() -> &'static str {
    atlas::TRACK_ATLAS_REVISION
}

/// A named station marker inside a range (apex, brake board, ...).
#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct RangePoint {
    #[serde(default)]
    pub id: String,
    #[serde(default)]
    pub role: String,
    #[serde(default)]
    pub label: String,
    /// Station fraction [0, 1].
    #[serde(default)]
    pub marker: f64,
}

/// One individual corner and its analysis range, in station fractions.
#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct CornerRange {
    pub id: String,
    #[serde(default)]
    pub label: String,
    pub start: f64,
    pub end: f64,
    #[serde(default)]
    pub anchor: Option<String>,
    #[serde(default)]
    pub points: Vec<RangePoint>,
}

/// A named contiguous group of corners drivers discuss as one section.
#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct CornerComplex {
    pub id: String,
    #[serde(default)]
    pub label: String,
    pub start: f64,
    pub end: f64,
    /// Member `CornerRange` ids, in track order.
    #[serde(default)]
    pub members: Vec<String>,
    #[serde(default)]
    pub points: Vec<RangePoint>,
}

/// A labelled corner point (the `corners` point layer).
#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct CornerPoint {
    pub id: String,
    #[serde(default)]
    pub label: String,
    #[serde(default)]
    pub labels: std::collections::BTreeMap<String, String>,
    #[serde(default)]
    pub marker: f64,
    #[serde(default)]
    pub direction: Option<String>,
    /// (lon, lat) when the atlas places it.
    #[serde(default)]
    pub location: Option<[f64; 2]>,
}

#[derive(Debug, Deserialize)]
struct Layer<T> {
    id: String,
    #[serde(default = "Vec::new")]
    items: Vec<T>,
}

/// One resolved layout of a facility.
#[derive(Debug, Clone, PartialEq)]
pub struct TrackLayout {
    pub track_slug: &'static str,
    pub track_name: &'static str,
    pub timezone: &'static str,
    pub layout_id: &'static str,
    pub layout_name: &'static str,
    pub length_m: Option<f64>,
    pub direction: Option<&'static str>,
    /// Centerline as (lon, lat).
    pub centerline: Vec<Point>,
    pub corner_ranges: Vec<CornerRange>,
    pub corner_complexes: Vec<CornerComplex>,
    pub corner_points: Vec<CornerPoint>,
    /// Timing sectors, when the layout defines them.
    pub sectors: Vec<CornerRange>,
}

fn layer_items<T: for<'de> Deserialize<'de>>(json: &str, id: &str) -> Vec<T> {
    let Ok(layers) = serde_json::from_str::<Vec<serde_json::Value>>(json) else {
        return Vec::new();
    };
    layers
        .into_iter()
        .filter(|layer| layer.get("id").and_then(|v| v.as_str()) == Some(id))
        .find_map(|layer| serde_json::from_value::<Layer<T>>(layer).ok())
        .map(|layer| {
            debug_assert_eq!(layer.id, id);
            layer.items
        })
        .unwrap_or_default()
}

impl TrackLayout {
    fn from_atlas(track: &'static atlas::Track, layout: &'static atlas::Layout) -> Self {
        Self {
            track_slug: track.slug,
            track_name: track.name,
            timezone: track.timezone,
            layout_id: layout.id,
            layout_name: layout.name,
            length_m: layout.length_m,
            direction: layout.direction,
            centerline: atlas_spatial::parse_centerline(layout.centerline_geojson),
            corner_ranges: layer_items(layout.range_layers_json, "corner_ranges"),
            corner_complexes: layer_items(layout.range_layers_json, "corner_complexes"),
            corner_points: layer_items(layout.point_layers_json, "corners"),
            sectors: layer_items(layout.range_layers_json, "timing_sectors"),
        }
    }

    /// Driver-facing label of a corner range: the anchor point's `driver`
    /// label (the atlas default label model), then the range label, then
    /// the numbered label, then the point label, then the anchor id.
    pub fn corner_label(&self, range: &CornerRange) -> String {
        let anchor = range.anchor.as_deref().unwrap_or("");
        let point = self.corner_points.iter().find(|p| p.id == anchor);
        let from_labels = |key: &str| point.and_then(|p| p.labels.get(key)).cloned();
        from_labels("driver")
            .filter(|s| !s.is_empty())
            .or_else(|| Some(range.label.clone()).filter(|s| !s.is_empty()))
            .or_else(|| from_labels("numbered").filter(|s| !s.is_empty()))
            .or_else(|| point.map(|p| p.label.clone()).filter(|s| !s.is_empty()))
            .unwrap_or_else(|| anchor.to_string())
    }
}

/// Lowercase ASCII alphanumerics only (the atlas name normalization).
pub fn normalize_atlas_name(value: &str) -> String {
    value
        .chars()
        .flat_map(char::to_lowercase)
        .filter(|c| c.is_ascii_lowercase() || c.is_ascii_digit())
        .collect()
}

/// Best facility for a track name or slug: exact normalized name/slug/alias
/// wins; otherwise the closest containment match.
pub fn find_track(name_or_slug: &str) -> Option<&'static atlas::Track> {
    if let Some(track) = atlas::find_track(name_or_slug) {
        return Some(track);
    }
    let wanted = normalize_atlas_name(name_or_slug);
    if wanted.is_empty() {
        return None;
    }
    let mut best: Option<(&'static atlas::Track, usize)> = None;
    for track in atlas::tracks() {
        let names = [track.slug, track.name]
            .into_iter()
            .chain(track.aka.iter().copied());
        for name in names {
            let candidate = normalize_atlas_name(name);
            if candidate.is_empty() {
                continue;
            }
            let score = if candidate == wanted {
                0
            } else if candidate.contains(&wanted) || wanted.contains(&candidate) {
                10 + candidate.len().abs_diff(wanted.len())
            } else {
                continue;
            };
            if best.is_none_or(|(_, s)| score < s) {
                best = Some((track, score));
            }
        }
    }
    best.map(|(track, _)| track)
}

/// Nearest facility within 20 km of a GPS fix.
pub fn find_track_by_gps(latitude: f64, longitude: f64) -> Option<&'static atlas::Track> {
    atlas::match_track(latitude, longitude, 20_000.0).map(|m| m.track)
}

/// IANA timezone for a venue name, slug or alias.
pub fn timezone_for_venue(venue: &str) -> Option<&'static str> {
    atlas::timezone_for_venue(venue)
}

/// The layout of `track` whose declared length is closest to `lap_length_m`
/// (first layout when the lap length is unknown).
pub fn resolve_layout(track: &'static atlas::Track, lap_length_m: f64) -> Option<TrackLayout> {
    let mut best: Option<(&'static atlas::Layout, f64)> = None;
    for layout in track.layouts {
        let score = match layout.length_m {
            Some(declared) if declared > 0.0 && lap_length_m > 0.0 => {
                (declared - lap_length_m).abs()
            }
            _ => 100_000.0,
        };
        if best.is_none_or(|(_, s)| score < s) {
            best = Some((layout, score));
        }
    }
    best.map(|(layout, _)| TrackLayout::from_atlas(track, layout))
}

/// Resolve a layout from what is known about a lap: an explicit track name
/// or slug first, then the lap's own GPS.
pub fn resolve_for_lap(track_hint: Option<&str>, lap: &UnifiedLap) -> Option<TrackLayout> {
    let track = track_hint.and_then(find_track).or_else(|| {
        (0..lap.gps_lat.len())
            .find(|&i| lap.gps_lat[i].is_finite() && lap.gps_lon[i].is_finite())
            .and_then(|i| find_track_by_gps(lap.gps_lat[i], lap.gps_lon[i]))
    })?;
    resolve_layout(track, lap.total_distance())
}
