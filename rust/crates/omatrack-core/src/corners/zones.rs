//! Corner zones in lap fractions: Track Atlas ranges mapped onto one lap,
//! and the brake-zone auto-generator used when no atlas data applies.

use crate::atlas_spatial::{self, Point};
use crate::monotonic;
use crate::num::{clamp, max};
use crate::track::TrackLayout;
use crate::unify::UnifiedLap;

/// Where a zone came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
pub enum ZoneSource {
    /// Mapped from Track Atlas `corner_ranges`.
    #[default]
    Atlas,
    /// The user's per-track override in `omatrack.yml`.
    User,
    /// Brake-zone detection on the primary lap.
    Generated,
}

/// One individual corner range on the primary lap.
#[derive(Debug, Clone, PartialEq)]
pub struct CornerZone {
    /// Stable id (atlas range id, or `tN` for generated/user zones).
    pub id: String,
    pub name: String,
    /// Lap fractions.
    pub start: f64,
    pub end: f64,
    pub source: ZoneSource,
}

/// A named corner complex mapped onto the lap, with its member zone ids.
#[derive(Debug, Clone, PartialEq)]
pub struct ComplexZone {
    pub id: String,
    pub name: String,
    pub start: f64,
    pub end: f64,
    pub members: Vec<String>,
}

/// Maps atlas station fractions to lap fractions for one lap: spatially
/// through GPS when the lap has it, else by lap distance.
#[derive(Debug, Clone)]
pub struct StationMapper {
    spatial: Vec<Point>,
    distance: Vec<f64>,
    lap_length: f64,
    /// True when the GPS station map was used.
    pub gps: bool,
}

impl StationMapper {
    /// `None` when the lap has GPS but it does not match the centerline:
    /// then corners are hidden rather than fabricated.
    pub fn new(lap: &UnifiedLap, layout: &TrackLayout) -> Option<Self> {
        let lap_length = lap.distance.last().copied().unwrap_or(0.0);
        if atlas_spatial::has_positional_gps(lap) {
            let spatial = atlas_spatial::spatial_station_map(lap, &layout.centerline);
            if spatial.is_empty() {
                return None;
            }
            return Some(Self {
                spatial,
                distance: Vec::new(),
                lap_length,
                gps: true,
            });
        }
        Some(Self {
            spatial: Vec::new(),
            distance: lap.distance.clone(),
            lap_length,
            gps: false,
        })
    }

    /// Lap fraction at an atlas station fraction.
    pub fn fraction(&self, station: f64) -> f64 {
        if self.gps {
            return atlas_spatial::lap_fraction_at_station(&self.spatial, station);
        }
        if self.distance.len() < 2 || self.lap_length <= 0.0 {
            return clamp(station, 0.0, 1.0);
        }
        let target = clamp(station, 0.0, 1.0) * self.lap_length;
        monotonic::invert_fraction(&self.distance, target)
    }
}

/// Atlas `corner_ranges` as zones on this lap, sorted by start.
pub fn atlas_corner_zones(layout: &TrackLayout, mapper: &StationMapper) -> Vec<CornerZone> {
    let mut zones: Vec<CornerZone> = layout
        .corner_ranges
        .iter()
        .filter_map(|range| {
            let zone = CornerZone {
                id: range.id.clone(),
                name: layout.corner_label(range),
                start: mapper.fraction(range.start),
                end: mapper.fraction(range.end),
                source: ZoneSource::Atlas,
            };
            (!zone.name.is_empty() && zone.end > zone.start).then_some(zone)
        })
        .collect();
    zones.sort_by(|a, b| {
        a.start
            .partial_cmp(&b.start)
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    zones
}

/// Atlas `corner_complexes` on this lap, sorted by start.
pub fn atlas_complex_zones(layout: &TrackLayout, mapper: &StationMapper) -> Vec<ComplexZone> {
    let mut zones: Vec<ComplexZone> = layout
        .corner_complexes
        .iter()
        .filter_map(|complex| {
            let zone = ComplexZone {
                id: complex.id.clone(),
                name: if complex.label.is_empty() {
                    complex.id.clone()
                } else {
                    complex.label.clone()
                },
                start: mapper.fraction(complex.start),
                end: mapper.fraction(complex.end),
                members: complex.members.clone(),
            };
            (zone.end > zone.start).then_some(zone)
        })
        .collect();
    zones.sort_by(|a, b| {
        a.start
            .partial_cmp(&b.start)
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    zones
}

/// Brake-zone corners for a lap without atlas data (port of
/// `TelemetryStore::autoGenerateCorners`).
pub fn auto_generate_corners(lap: &UnifiedLap) -> Vec<CornerZone> {
    if lap.brake.len() < 100 {
        return Vec::new();
    }
    let mut peak = 0.0;
    for &b in &lap.brake {
        peak = max(peak, b);
    }
    if peak <= 2.0 {
        return Vec::new();
    }
    let threshold = peak * 0.15;
    let n = lap.brake.len() as i64;
    let mut zones: Vec<(i64, i64)> = Vec::new();
    let mut in_zone = false;
    let mut zone_start = 0i64;
    for i in 0..n {
        let b = lap.brake[i as usize];
        if !in_zone && b > threshold {
            in_zone = true;
            zone_start = i;
        } else if in_zone && b < threshold * 0.5 {
            in_zone = false;
            if i - zone_start > 5 {
                zones.push((zone_start, i));
            }
        }
    }
    if in_zone && n - zone_start > 5 {
        zones.push((zone_start, n - 1));
    }
    let mut merged: Vec<(i64, i64)> = Vec::new();
    for zone in zones {
        match merged.last_mut() {
            Some(last) if zone.0 - last.1 < 50 => last.1 = zone.1,
            _ => merged.push(zone),
        }
    }
    merged
        .iter()
        .enumerate()
        .map(|(i, &(first, last))| {
            let width = last - first;
            let approach = (width as f64 * 0.3) as i64;
            let exit_ext = (width as f64 * 0.6) as i64;
            let start = (first - approach).max(0);
            let end = (n - 1).min(last + exit_ext);
            CornerZone {
                id: format!("t{}", i + 1),
                name: format!("Turn {}", i + 1),
                start: start as f64 / (n - 1) as f64,
                end: end as f64 / (n - 1) as f64,
                source: ZoneSource::Generated,
            }
        })
        .collect()
}
