//! The session library as a tree: Track > Date > Session > Laps.
//!
//! A [`LibrarySnapshot`] is an immutable presentation snapshot built on a
//! background thread after a scan; the UI swaps whole snapshots and keeps
//! its selection by the stable string ids:
//!
//! ```text
//! trk:<slug>                                   a track
//! trk:<slug>/d:<yyyy-mm-dd>                    a day at that track
//! trk:<slug>/d:<yyyy-mm-dd>/s:<12 hex>         one recording (BLAKE3 of its path)
//! trk:<slug>/d:<yyyy-mm-dd>/s:<12 hex>/l:<id>  one lap
//! ```
//!
//! Days are calendar days where the session happened: the Track Atlas
//! venue's timezone, else the timezone the recording declares, else the
//! local one.

use crate::index_cache::FileIdentity;
use crate::location::DiscoveredFile;
use crate::metadata::EffectiveMetadata;
use crate::summary::RecordingSummary;
use jiff::civil::{Date, Time};
use jiff::tz::TimeZone;
use jiff::{Timestamp, Zoned};
use omatrack_core::session::{LapStripKind, best_lap_id, lap_labels};
use std::collections::BTreeMap;
use std::path::Path;

/// One recording to place in the tree (the scan's output per file).
#[derive(Debug, Clone)]
pub struct CatalogRecord {
    file: DiscoveredFile,
    summary: RecordingSummary,
    metadata: EffectiveMetadata,
}

impl CatalogRecord {
    pub fn new(
        file: DiscoveredFile,
        summary: RecordingSummary,
        metadata: EffectiveMetadata,
    ) -> Self {
        Self {
            file,
            summary,
            metadata,
        }
    }
}

/// Where a session's day came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum DateSource {
    /// The logger's wall clock in the venue's timezone.
    VenueClock,
    /// The logger's wall clock in the recording's declared timezone.
    RecordingClock,
    /// The logger's wall clock in the local timezone.
    LocalClock,
    /// A `YYMMDDHHMMSS` filename.
    Filename,
    /// A `yyyy-mm-dd` folder name.
    Folder,
    /// The file's modification time (local).
    Modified,
    Unknown,
}

/// When a session happened (plain data).
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct SessionStart {
    pub date: Option<Date>,
    /// Local start time of day, when known.
    pub time: Option<Time>,
    pub source: DateSource,
}

fn zoned(nanoseconds: i128, zone: TimeZone) -> Option<Zoned> {
    Timestamp::from_nanosecond(nanoseconds)
        .ok()
        .map(|timestamp| timestamp.to_zoned(zone))
}

/// `dd/mm/yyyy` from a `YYMMDDHHMMSS...` stem, with its time.
fn filename_start(stem: &str) -> Option<(Date, Time)> {
    let meta = omatrack_core::meta::session_meta_from_filename(stem);
    let mut parts = meta.date.split('/');
    let day: i8 = parts.next()?.parse().ok()?;
    let month: i8 = parts.next()?.parse().ok()?;
    let year: i16 = parts.next()?.parse().ok()?;
    let date = Date::new(year, month, day).ok()?;
    let mut clock = meta.time.split(':').map(|part| part.parse::<i8>().ok());
    let time = Time::new(clock.next()??, clock.next()??, clock.next()??, 0).ok()?;
    Some((date, time))
}

/// The day and time a recording happened (see the module docs for the
/// timezone rule). `venue` is the effective track slug or name.
pub fn session_start(
    summary: &RecordingSummary,
    venue: Option<&str>,
    path: &Path,
    identity: &FileIdentity,
) -> SessionStart {
    if summary.utc_start_ns > 0 {
        let venue_zone = venue
            .and_then(omatrack_core::track::timezone_for_venue)
            .and_then(|name| TimeZone::get(name).ok());
        let recording_zone = (!summary.timezone.is_empty())
            .then(|| TimeZone::get(&summary.timezone).ok())
            .flatten();
        let (zone, source) = match (venue_zone, recording_zone) {
            (Some(zone), _) => (zone, DateSource::VenueClock),
            (None, Some(zone)) => (zone, DateSource::RecordingClock),
            (None, None) => (TimeZone::system(), DateSource::LocalClock),
        };
        if let Some(start) = zoned(i128::from(summary.utc_start_ns), zone) {
            return SessionStart {
                date: Some(start.date()),
                time: Some(start.time()),
                source,
            };
        }
    }
    let stem = path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();
    if let Some((date, time)) = filename_start(&stem) {
        return SessionStart {
            date: Some(date),
            time: Some(time),
            source: DateSource::Filename,
        };
    }
    if let Some(date) = path
        .parent()
        .and_then(Path::file_name)
        .and_then(|name| name.to_str())
        .and_then(|name| name.parse::<Date>().ok())
    {
        return SessionStart {
            date: Some(date),
            time: None,
            source: DateSource::Folder,
        };
    }
    if identity.mtime_ns > 0
        && let Some(modified) = zoned(i128::from(identity.mtime_ns), TimeZone::system())
    {
        return SessionStart {
            date: Some(modified.date()),
            time: Some(modified.time()),
            source: DateSource::Modified,
        };
    }
    SessionStart {
        date: None,
        time: None,
        source: DateSource::Unknown,
    }
}

/// `yyyy-mm-dd`, or `unknown`.
pub fn date_key(date: Option<Date>) -> String {
    date.map_or_else(|| "unknown".to_string(), |date| date.to_string())
}

/// `Wed 2 Sep 2026`, or `Unknown date`.
pub fn date_heading(date: Option<Date>) -> String {
    date.map_or_else(
        || "Unknown date".to_string(),
        |date| date.strftime("%a %-d %b %Y").to_string(),
    )
}

/// A URL-safe slug for a track without an atlas slug.
fn slugify(name: &str) -> String {
    let mut slug = String::new();
    for c in name.chars().flat_map(char::to_lowercase) {
        if c.is_ascii_alphanumeric() {
            slug.push(c);
        } else if !slug.ends_with('-') && !slug.is_empty() {
            slug.push('-');
        }
    }
    let slug = slug.trim_end_matches('-').to_string();
    if slug.is_empty() {
        "unknown".to_string()
    } else {
        slug
    }
}

/// First 12 hex digits of the BLAKE3 of a recording's canonical path.
pub fn path_hash(path: &Path) -> String {
    let canonical = std::fs::canonicalize(path).unwrap_or_else(|_| path.to_path_buf());
    let hash = blake3::hash(canonical.as_os_str().as_encoded_bytes()).to_hex();
    hash.as_str()[..12].to_string()
}

/// One lap row (plain data).
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
#[expect(
    clippy::struct_excessive_bools,
    reason = "These are independent flags in a snapshot/input record, not mutually exclusive lifecycle states."
)]
pub struct LapNode {
    pub id: String,
    pub lap_id: i32,
    /// `L8`, `Out`, `In`, `Pit`, `Frag`.
    pub label: String,
    pub time_ms: f64,
    /// Lap time minus the session best, for representative laps.
    pub delta_to_best_ms: Option<f64>,
    pub best: bool,
    /// Complete and not a pit outlier: counts for best.
    pub representative: bool,
    pub complete: bool,
    pub pit: bool,
    pub kind: LapStripKind,
}

/// One recording (plain data).
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct SessionNode {
    pub id: String,
    pub file: DiscoveredFile,
    /// `Session · Driver`, else the file name.
    pub title: String,
    pub session_name: Option<String>,
    pub driver: Option<String>,
    pub start: SessionStart,
    /// Complete laps.
    pub lap_count: usize,
    /// Sum of every lap's time.
    pub drive_time_ms: f64,
    pub best_lap_id: Option<i32>,
    pub best_time_ms: Option<f64>,
    pub format: String,
    pub has_video: bool,
    pub metadata: EffectiveMetadata,
    pub summary: RecordingSummary,
    pub laps: Vec<LapNode>,
}

impl SessionNode {
    pub fn file_name(&self) -> String {
        self.file
            .path()
            .file_name()
            .map(|n| n.to_string_lossy().into_owned())
            .unwrap_or_default()
    }

    pub fn lap(&self, lap_id: i32) -> Option<&LapNode> {
        self.laps.iter().find(|lap| lap.lap_id == lap_id)
    }

    fn sort_key(&self) -> (Option<Time>, String, String) {
        (
            self.start.time,
            self.session_name.clone().unwrap_or_default(),
            self.file.path().to_string_lossy().into_owned(),
        )
    }

    fn matches(&self, track_name: &str, query: &str) -> bool {
        let query = query.trim().to_lowercase();
        if query.is_empty() {
            return true;
        }
        [
            Some(track_name.to_string()),
            self.driver.clone(),
            self.session_name.clone(),
            Some(self.file_name()),
        ]
        .into_iter()
        .flatten()
        .any(|text| text.to_lowercase().contains(&query))
    }
}

/// One day at a track (plain data).
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct DateNode {
    pub id: String,
    pub date: Option<Date>,
    /// `yyyy-mm-dd` or `unknown`.
    pub key: String,
    /// `Wed 2 Sep 2026`.
    pub heading: String,
    pub sessions: Vec<SessionNode>,
}

/// One track (plain data).
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct TrackNode {
    pub id: String,
    pub slug: String,
    pub name: String,
    pub dates: Vec<DateNode>,
}

/// One facet value and how many recordings carry it.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub struct Facet {
    /// Filter value (track slug, year, driver name).
    pub value: String,
    pub label: String,
    pub count: usize,
}

/// Facet values over a snapshot, sorted by label.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
#[non_exhaustive]
pub struct Facets {
    pub tracks: Vec<Facet>,
    pub years: Vec<Facet>,
    pub drivers: Vec<Facet>,
}

/// Search text plus facet selections; empty matches everything.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct LibraryFilter {
    query: String,
    track: Option<String>,
    year: Option<i16>,
    driver: Option<String>,
}

impl LibraryFilter {
    pub fn new() -> Self {
        Self::default()
    }
    /// Case-insensitive substring over track, driver, session and file name.
    #[must_use]
    pub fn query(mut self, query: impl Into<String>) -> Self {
        self.query = query.into();
        self
    }
    /// Track slug.
    #[must_use]
    pub fn track(mut self, slug: Option<String>) -> Self {
        self.track = slug;
        self
    }
    #[must_use]
    pub fn year(mut self, year: Option<i16>) -> Self {
        self.year = year;
        self
    }
    #[must_use]
    pub fn driver(mut self, driver: Option<String>) -> Self {
        self.driver = driver;
        self
    }
    pub fn is_empty(&self) -> bool {
        self.query.trim().is_empty()
            && self.track.is_none()
            && self.year.is_none()
            && self.driver.is_none()
    }
}

/// The library tree (immutable; rebuilt per scan).
#[derive(Debug, Clone, PartialEq, Default)]
pub struct LibrarySnapshot {
    tracks: Vec<TrackNode>,
}

fn lap_nodes(
    session_id: &str,
    summary: &RecordingSummary,
) -> (Vec<LapNode>, Option<i32>, Option<f64>) {
    let laps = summary.laps();
    let labels = lap_labels(&laps);
    let best = best_lap_id(&laps);
    let best_time = best.and_then(|id| laps.iter().find(|l| l.id == id).map(|l| l.time_ms));
    let nodes = laps
        .iter()
        .zip(labels)
        .map(|(lap, label)| {
            let representative = lap.counts_for_best();
            let kind = if lap.is_pit_stop() {
                LapStripKind::PitStop
            } else if lap.complete {
                LapStripKind::Flying
            } else {
                match label.as_str() {
                    "Out" => LapStripKind::Out,
                    "In" => LapStripKind::In,
                    _ => LapStripKind::Fragment,
                }
            };
            LapNode {
                id: format!("{session_id}/l:{}", lap.id),
                lap_id: lap.id,
                label,
                time_ms: lap.time_ms,
                delta_to_best_ms: best_time
                    .filter(|_| representative)
                    .map(|best| lap.time_ms - best),
                best: best == Some(lap.id),
                representative,
                complete: lap.complete,
                pit: lap.is_pit_lap || lap.is_pit_stop(),
                kind,
            }
        })
        .collect();
    (nodes, best, best_time)
}

type DateGroups = BTreeMap<String, (Option<Date>, Vec<SessionNode>)>;
type TrackGroups = BTreeMap<String, (String, DateGroups)>;

impl LibrarySnapshot {
    /// Group records into the tree: tracks by name, days oldest first,
    /// sessions by start time, laps in recording order. Canonicalizes each
    /// path for its id: run it off the UI thread.
    pub fn build(records: Vec<CatalogRecord>) -> Self {
        // slug -> (name, date key -> (date, sessions))
        let mut tracks = TrackGroups::new();
        for record in records {
            let metadata = record.metadata;
            let name = metadata
                .track_name()
                .map_or_else(|| "Unknown track".to_string(), str::to_string);
            let slug = metadata
                .track_slug()
                .map_or_else(|| slugify(&name), str::to_string);
            let start = session_start(
                &record.summary,
                Some(metadata.track_slug().unwrap_or(&name)),
                record.file.path(),
                record.file.identity(),
            );
            let key = date_key(start.date);
            let session_id = format!("trk:{slug}/d:{key}/s:{}", path_hash(record.file.path()));
            let (laps, best_lap_id, best_time_ms) = lap_nodes(&session_id, &record.summary);
            let session_name = metadata.session().map(str::to_string);
            let driver = metadata.driver().map(str::to_string);
            let title = match (&session_name, &driver) {
                (Some(session), Some(driver)) => format!("{session} \u{b7} {driver}"),
                (Some(one), None) | (None, Some(one)) => one.clone(),
                (None, None) => record
                    .file
                    .path()
                    .file_stem()
                    .map(|s| s.to_string_lossy().into_owned())
                    .unwrap_or_default(),
            };
            let session = SessionNode {
                id: session_id,
                title,
                session_name,
                driver,
                start: start.clone(),
                lap_count: laps.iter().filter(|lap| lap.complete).count(),
                drive_time_ms: laps
                    .iter()
                    .map(|lap| lap.time_ms)
                    .filter(|t| t.is_finite() && *t > 0.0)
                    .sum(),
                best_lap_id,
                best_time_ms,
                format: record.summary.format.clone(),
                has_video: record.summary.has_video,
                metadata,
                summary: record.summary,
                laps,
                file: record.file,
            };
            let track = tracks
                .entry(slug)
                .or_insert_with(|| (name, BTreeMap::new()));
            track
                .1
                .entry(key)
                .or_insert_with(|| (start.date, Vec::new()))
                .1
                .push(session);
        }
        let mut nodes: Vec<TrackNode> = tracks
            .into_iter()
            .map(|(slug, (name, dates))| {
                let track_id = format!("trk:{slug}");
                let mut dates: Vec<DateNode> = dates
                    .into_iter()
                    .map(|(key, (date, mut sessions))| {
                        sessions.sort_by_key(SessionNode::sort_key);
                        DateNode {
                            id: format!("{track_id}/d:{key}"),
                            heading: date_heading(date),
                            date,
                            key,
                            sessions,
                        }
                    })
                    .collect();
                // Oldest first; unknown dates last.
                dates.sort_by_key(|node| (node.date.is_none(), node.date));
                TrackNode {
                    id: track_id,
                    slug,
                    name,
                    dates,
                }
            })
            .collect();
        nodes.sort_by_key(|track| (track.name.to_lowercase(), track.slug.clone()));
        Self { tracks: nodes }
    }

    pub fn tracks(&self) -> &[TrackNode] {
        &self.tracks
    }

    pub fn is_empty(&self) -> bool {
        self.tracks.is_empty()
    }

    /// Every recording, in tree order.
    pub fn sessions(&self) -> impl Iterator<Item = &SessionNode> {
        self.tracks
            .iter()
            .flat_map(|track| track.dates.iter())
            .flat_map(|date| date.sessions.iter())
    }

    pub fn recording_count(&self) -> usize {
        self.sessions().count()
    }

    pub fn track(&self, id: &str) -> Option<&TrackNode> {
        self.tracks.iter().find(|track| track.id == id)
    }

    pub fn session(&self, id: &str) -> Option<&SessionNode> {
        self.sessions().find(|session| session.id == id)
    }

    /// The recording at `path`.
    pub fn session_for_path(&self, path: &Path) -> Option<&SessionNode> {
        self.sessions().find(|session| session.file.path() == path)
    }

    /// The session and lap behind a lap id.
    pub fn lap(&self, id: &str) -> Option<(&SessionNode, &LapNode)> {
        let (session_id, _) = id.rsplit_once("/l:")?;
        let session = self.session(session_id)?;
        let lap = session.laps.iter().find(|lap| lap.id == id)?;
        Some((session, lap))
    }

    /// Track, year and driver facets with recording counts.
    pub fn facets(&self) -> Facets {
        let mut tracks: BTreeMap<String, (String, usize)> = BTreeMap::new();
        let mut years: BTreeMap<i16, usize> = BTreeMap::new();
        let mut drivers: BTreeMap<String, usize> = BTreeMap::new();
        for track in &self.tracks {
            for date in &track.dates {
                for session in &date.sessions {
                    tracks
                        .entry(track.slug.clone())
                        .or_insert_with(|| (track.name.clone(), 0))
                        .1 += 1;
                    if let Some(date) = date.date {
                        *years.entry(date.year()).or_default() += 1;
                    }
                    if let Some(driver) = &session.driver {
                        *drivers.entry(driver.clone()).or_default() += 1;
                    }
                }
            }
        }
        let mut facets = Facets {
            tracks: tracks
                .into_iter()
                .map(|(value, (label, count))| Facet {
                    value,
                    label,
                    count,
                })
                .collect(),
            years: years
                .into_iter()
                .map(|(year, count)| Facet {
                    value: year.to_string(),
                    label: year.to_string(),
                    count,
                })
                .collect(),
            drivers: drivers
                .into_iter()
                .map(|(driver, count)| Facet {
                    value: driver.clone(),
                    label: driver,
                    count,
                })
                .collect(),
        };
        facets
            .tracks
            .sort_by_key(|facet| facet.label.to_lowercase());
        facets
    }

    /// The tree restricted to recordings matching `filter`; empty tracks
    /// and days are dropped. Ids are unchanged.
    #[must_use]
    pub fn filtered(&self, filter: &LibraryFilter) -> Self {
        if filter.is_empty() {
            return self.clone();
        }
        let tracks = self
            .tracks
            .iter()
            .filter(|track| filter.track.as_ref().is_none_or(|slug| *slug == track.slug))
            .filter_map(|track| {
                let dates: Vec<DateNode> = track
                    .dates
                    .iter()
                    .filter(|date| {
                        filter
                            .year
                            .is_none_or(|year| date.date.is_some_and(|d| d.year() == year))
                    })
                    .filter_map(|date| {
                        let sessions: Vec<SessionNode> =
                            date.sessions
                                .iter()
                                .filter(|session| {
                                    filter.driver.as_ref().is_none_or(|driver| {
                                        session.driver.as_ref() == Some(driver)
                                    }) && session.matches(&track.name, &filter.query)
                                })
                                .cloned()
                                .collect();
                        (!sessions.is_empty()).then(|| DateNode {
                            sessions,
                            ..date.clone()
                        })
                    })
                    .collect();
                (!dates.is_empty()).then(|| TrackNode {
                    dates,
                    ..track.clone()
                })
            })
            .collect();
        Self { tracks }
    }

    /// Recordings matching a search string, in tree order.
    pub fn search(&self, query: &str) -> Vec<&SessionNode> {
        self.tracks
            .iter()
            .flat_map(|track| {
                track
                    .dates
                    .iter()
                    .flat_map(|date| date.sessions.iter())
                    .filter(move |session| session.matches(&track.name, query))
            })
            .collect()
    }
}
