//! Effective recording metadata: the one precedence rule every consumer
//! (library rows, track and driver display, lap loads, HUD) goes through.
//! Higher wins:
//!
//! 1. `recording_metadata[<path>]` in `omatrack.yml` (per-recording override);
//! 2. the `TRACK.yml` chain, merged root to leaf;
//! 3. preferences that are not path metadata: `track_assignments` (by event
//!    date, then folder) and `driver_mappings` (by driver id);
//! 4. what the recording says (index summary: driver id, GPS -> Track Atlas);
//! 5. inference from the file and folder names.
//!
//! The index cache holds layer 4 only, so layers 1-3 never invalidate it.

use crate::config::Config;
use crate::fsutil::{nested_text, scalar_text};
use crate::summary::RecordingSummary;
use crate::track_yml::{driver_id_key, driver_name_for_id, merge};
use omatrack_core::mapping::{CHANNEL_ALIASES, ChannelOverrides};
use omatrack_core::track;
use serde_yaml::{Mapping, Value};
use std::path::Path;

/// Which precedence layer supplied a value (1 = highest).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum MetadataLayer {
    RecordingOverride = 1,
    FolderMetadata = 2,
    Preferences = 3,
    Recording = 4,
    Inferred = 5,
}

/// A value and the layer it came from (plain data).
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub struct Sourced<T> {
    pub value: T,
    pub layer: MetadataLayer,
}

impl<T> Sourced<T> {
    pub fn new(value: T, layer: MetadataLayer) -> Self {
        Self { value, layer }
    }
}

/// A recording's resolved metadata (plain data).
#[derive(Debug, Clone, PartialEq, Default)]
#[non_exhaustive]
pub struct EffectiveMetadata {
    /// The `TRACK.yml` chain with the recording override merged over it.
    pub document: Mapping,
    pub track_name: Option<Sourced<String>>,
    /// Track Atlas slug.
    pub track_slug: Option<Sourced<String>>,
    pub event: Option<Sourced<String>>,
    pub series: Option<Sourced<String>>,
    pub session: Option<Sourced<String>>,
    pub driver: Option<Sourced<String>>,
    pub car_number: Option<Sourced<String>>,
    pub car_class: Option<Sourced<String>>,
    /// `folder.name`: the folder's display name.
    pub folder_name: Option<Sourced<String>>,
    /// Concept -> source channel overrides for unification.
    pub channel_overrides: ChannelOverrides,
}

impl EffectiveMetadata {
    fn text(field: &Option<Sourced<String>>) -> Option<&str> {
        field.as_ref().map(|sourced| sourced.value.as_str())
    }
    pub fn track_name(&self) -> Option<&str> {
        Self::text(&self.track_name)
    }
    pub fn track_slug(&self) -> Option<&str> {
        Self::text(&self.track_slug)
    }
    pub fn driver(&self) -> Option<&str> {
        Self::text(&self.driver)
    }
    pub fn session(&self) -> Option<&str> {
        Self::text(&self.session)
    }
    pub fn event(&self) -> Option<&str> {
        Self::text(&self.event)
    }
}

/// Inputs that do not come from the recording's own folder.
///
/// Build it with [`MetadataSources::new`] and the `with_*` builders.
#[derive(Debug, Clone, Copy)]
#[non_exhaustive]
pub struct MetadataSources<'a> {
    /// Layer 2: the merged `TRACK.yml` chain of the recording's folder.
    pub folder: &'a Mapping,
    /// Layers 1 and 3.
    pub config: &'a Config,
    /// Layer 4.
    pub summary: Option<&'a RecordingSummary>,
    /// The recording's event date (`yyyy-mm-dd`) for `track_assignments`.
    pub event_date: Option<&'a str>,
}

impl<'a> MetadataSources<'a> {
    /// The folder chain and the configuration; no summary, no event date.
    pub fn new(folder: &'a Mapping, config: &'a Config) -> Self {
        Self {
            folder,
            config,
            summary: None,
            event_date: None,
        }
    }

    /// Layer 4: the recording's parsed summary.
    pub fn with_summary(mut self, summary: Option<&'a RecordingSummary>) -> Self {
        self.summary = summary;
        self
    }

    /// The recording's event date (`yyyy-mm-dd`) for `track_assignments`.
    pub fn with_event_date(mut self, event_date: Option<&'a str>) -> Self {
        self.event_date = event_date;
        self
    }
}

/// Resolve every field of a recording's metadata by the five-layer rule.
pub fn effective_metadata(path: &Path, sources: MetadataSources<'_>) -> EffectiveMetadata {
    let empty = Mapping::new();
    let recording_override = sources.config.recording_override(path).unwrap_or(&empty);
    let mut document = sources.folder.clone();
    merge(&mut document, recording_override);

    let document_text = |field: &[&str]| -> Option<Sourced<String>> {
        nested_text(recording_override, field)
            .map(|value| Sourced::new(value, MetadataLayer::RecordingOverride))
            .or_else(|| {
                nested_text(sources.folder, field)
                    .map(|value| Sourced::new(value, MetadataLayer::FolderMetadata))
            })
    };

    let stem = path
        .file_stem()
        .map(|stem| stem.to_string_lossy().into_owned())
        .unwrap_or_default();
    let folder = path
        .parent()
        .and_then(Path::file_name)
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();

    // Track: explicit slug, explicit name, assignment, GPS, folder name.
    let named = document_text(&["track", "name"]);
    let mut slug =
        document_text(&["track", "slug"]).map(|s| Sourced::new(s.value.to_lowercase(), s.layer));
    if slug.is_none() {
        slug = assigned_track(sources.config, path, sources.event_date)
            .map(|value| Sourced::new(value, MetadataLayer::Preferences));
    }
    if slug.is_none()
        && let Some(named) = &named
    {
        slug =
            track::find_track(&named.value).map(|t| Sourced::new(t.slug.to_string(), named.layer));
    }
    if slug.is_none()
        && let Some([lat, lon]) = sources.summary.and_then(|s| s.gps)
    {
        slug = track::find_track_by_gps(lat, lon)
            .map(|t| Sourced::new(t.slug.to_string(), MetadataLayer::Recording));
    }
    let track_name = named
        .or_else(|| {
            let slug = slug.as_ref()?;
            track::find_track(&slug.value).map(|t| Sourced::new(t.name.to_string(), slug.layer))
        })
        .or_else(|| {
            (!folder.is_empty()).then(|| Sourced::new(folder.clone(), MetadataLayer::Inferred))
        });

    // Driver: TRACK.yml mapping by id, preferences mapping, raw id.
    let driver_id = sources.summary.and_then(RecordingSummary::driver_id);
    let driver = driver_id
        .and_then(|id| {
            driver_name_for_id(recording_override, id)
                .map(|name| Sourced::new(name, MetadataLayer::RecordingOverride))
                .or_else(|| {
                    driver_name_for_id(sources.folder, id)
                        .map(|name| Sourced::new(name, MetadataLayer::FolderMetadata))
                })
        })
        .or_else(|| document_text(&["driver", "name"]))
        .or_else(|| {
            let key = driver_id_key(driver_id?);
            let mappings = &sources.config.driver_mappings;
            mappings
                .get(&key)
                .or_else(|| mappings.get("*"))
                .map(|name| Sourced::new(name.clone(), MetadataLayer::Preferences))
        })
        .or_else(|| {
            driver_id.map(|id| {
                Sourced::new(
                    format!("Driver id {}", driver_id_key(id)),
                    MetadataLayer::Recording,
                )
            })
        });

    let session = document_text(&["session"]).or_else(|| {
        inferred_session_name(&stem, &folder).map(|s| Sourced::new(s, MetadataLayer::Inferred))
    });
    let car_number = document_text(&["car", "number"])
        .or_else(|| inferred_car_number(&stem).map(|s| Sourced::new(s, MetadataLayer::Inferred)));
    let car_class = document_text(&["car", "class"])
        .or_else(|| inferred_car_class(&stem).map(|s| Sourced::new(s, MetadataLayer::Inferred)));

    EffectiveMetadata {
        channel_overrides: channel_overrides(&document),
        track_name,
        track_slug: slug,
        event: document_text(&["event"]),
        series: document_text(&["series"]),
        session,
        driver,
        car_number,
        car_class,
        folder_name: document_text(&["folder", "name"]),
        document,
    }
}

/// `track_assignments` by event date, then by the recording's folder.
fn assigned_track(config: &Config, path: &Path, event_date: Option<&str>) -> Option<String> {
    let assignments = &config.track_assignments;
    event_date
        .and_then(|date| assignments.get(date))
        .or_else(|| {
            let folder = path.parent()?.to_string_lossy().into_owned();
            assignments.get(&folder)
        })
        .map(|slug| slug.trim().to_lowercase())
        .filter(|slug| !slug.is_empty())
}

/// `channels` in metadata: concept -> source channel name, for concepts the
/// mapper knows.
pub fn channel_overrides(document: &Mapping) -> ChannelOverrides {
    let mut overrides = ChannelOverrides::new();
    let Some(Value::Mapping(channels)) = document.get("channels") else {
        return overrides;
    };
    for (key, value) in channels {
        let (Some(key), Some(value)) = (scalar_text(key), scalar_text(value)) else {
            continue;
        };
        let (key, value) = (key.trim(), value.trim());
        if !value.is_empty() && CHANNEL_ALIASES.iter().any(|(concept, _)| *concept == key) {
            overrides.insert(key.to_string(), value.to_string());
        }
    }
    overrides
}

fn is_separator(c: char) -> bool {
    c == '_' || c == ' ' || c == '-'
}

fn normalize_session_token(token: &str) -> String {
    let upper = token.to_uppercase();
    if upper.starts_with("FP") || upper.starts_with("CT") {
        return upper;
    }
    match upper.as_str() {
        "QUALI" | "QUALY" | "QUALIFYING" => "Qualifying".to_string(),
        "QUALYSIM" => "QualySim".to_string(),
        "WARM-UP" | "WARMUP" => "Warmup".to_string(),
        _ => {
            let lower = token.to_lowercase();
            let mut chars = lower.chars();
            chars
                .next()
                .map(|first| first.to_uppercase().chain(chars).collect())
                .unwrap_or_default()
        }
    }
}

fn session_token(text: &str) -> Option<String> {
    let tokens: Vec<&str> = text.split(is_separator).collect();
    for (i, token) in tokens.iter().enumerate() {
        let upper = token.to_ascii_uppercase();
        let numbered = |prefix: &str| {
            upper
                .strip_prefix(prefix)
                .is_some_and(|rest| !rest.is_empty() && rest.bytes().all(|b| b.is_ascii_digit()))
        };
        if numbered("FP")
            || numbered("CT")
            || matches!(
                upper.as_str(),
                "QUALIFYING" | "QUALYSIM" | "QUALI" | "QUALY" | "PRACTICE" | "RACE" | "WARMUP"
            )
        {
            return Some(normalize_session_token(token));
        }
        if upper == "WARM"
            && tokens
                .get(i + 1)
                .is_some_and(|next| next.eq_ignore_ascii_case("up"))
        {
            return Some("Warmup".to_string());
        }
    }
    None
}

/// Session name from a file stem, else its folder name (`CT1`, `FP2`,
/// `Qualifying`, `Race`, ...).
pub fn inferred_session_name(stem: &str, folder: &str) -> Option<String> {
    session_token(stem).or_else(|| session_token(folder))
}

/// Car number from `Car<digits>` (after `_`, a space or the start), else
/// `#<digits>`.
pub fn inferred_car_number(stem: &str) -> Option<String> {
    let lower = stem.to_ascii_lowercase();
    let bytes = lower.as_bytes();
    for (at, _) in lower.match_indices("car") {
        if at > 0 && !matches!(bytes[at - 1], b'_' | b' ') {
            continue;
        }
        let digits: String = stem[at + 3..]
            .chars()
            .take_while(char::is_ascii_digit)
            .collect();
        if !digits.is_empty() {
            return Some(digits);
        }
    }
    let at = stem.find('#')?;
    let digits: String = stem[at + 1..]
        .chars()
        .take_while(char::is_ascii_digit)
        .collect();
    (!digits.is_empty()).then_some(digits)
}

fn class_word(text: &str) -> Option<usize> {
    let mut chars = text.char_indices();
    let (_, first) = chars.next()?;
    if !first.is_ascii_uppercase() {
        return None;
    }
    Some(
        chars
            .find(|(_, c)| !(c.is_ascii_uppercase() || c.is_ascii_digit() || *c == '-'))
            .map(|(i, _)| i)
            .unwrap_or(text.len()),
    )
}

/// `CLASS` or `CLASS_SUB` fully matching `text`.
fn is_class(text: &str) -> bool {
    let Some(end) = class_word(text) else {
        return false;
    };
    let rest = &text[end..];
    if rest.is_empty() {
        return true;
    }
    let Some(second) = rest.strip_prefix(['_', ' ']) else {
        return false;
    };
    class_word(second) == Some(second.len())
}

/// Car class: the upper-case word (or two) right before a `#<digits>` car
/// marker (`..._LMP2 #52` -> `LMP2`).
pub fn inferred_car_class(stem: &str) -> Option<String> {
    let bytes = stem.as_bytes();
    let marker = (0..bytes.len()).rev().find(|&i| {
        bytes[i] == b'#' && bytes.get(i + 1).is_some_and(u8::is_ascii_digit) && {
            let mut j = i + 1;
            while bytes.get(j).is_some_and(u8::is_ascii_digit) {
                j += 1;
            }
            j == bytes.len() || !bytes[j].is_ascii_digit()
        }
    })?;
    let before = stem[..marker].trim_end();
    if before.is_empty() {
        return None;
    }
    let before = before.trim();
    (0..before.len())
        .filter(|&i| before.is_char_boundary(i))
        .find(|&i| is_class(&before[i..]))
        .map(|i| before[i..].to_string())
}
