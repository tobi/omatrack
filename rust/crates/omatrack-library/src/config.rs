//! `omatrack.yml`: the single, hand-editable configuration document.
//!
//! The typed [`Config`] covers the keys this build interprets. Every other
//! key, at every level, is kept verbatim in an `extra` mapping and written
//! back, so a newer build's settings (or a hand-written note) survive a
//! round trip. Values are read the way they would be typed: `"true"` and
//! `true` are both a boolean, `"1.5"` and `1.5` both a number, and a value
//! that makes no sense falls back to the default instead of failing the
//! whole document. A document that is not YAML at all is never overwritten
//! ([`ConfigFile`]).
//!
//! Key names follow the existing Omatrack document (`recent_files`,
//! `driver_mappings`, `selection`, `video/*`, `trace/*`, `tracks.<key>`),
//! plus the new `trace.x_axis` and `workspace.layout`.

use crate::fsutil::{scalar_text, write_atomic};
use omatrack_core::alignment::Strategy;
use omatrack_core::corners::{CornerZone, ZoneSource};
use omatrack_core::playback::ReferencePlayback;
use serde::de::{DeserializeOwned, Deserializer};
use serde::{Deserialize, Serialize, Serializer};
use serde_yaml::{Mapping, Value};
use std::collections::BTreeMap;
use std::io;
use std::path::{Path, PathBuf};

/// Reading or writing `omatrack.yml` failed.
#[derive(Debug, thiserror::Error)]
#[non_exhaustive]
pub enum ConfigError {
    #[error("{path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("{path}: not a valid configuration document: {message}")]
    Parse { path: PathBuf, message: String },
    #[error("{path}: left untouched because it could not be read")]
    Readonly { path: PathBuf },
}

// ── lenient scalar reading ──────────────────────────────────────────

fn value_or_default<T: DeserializeOwned + Default>(value: Value, what: &str) -> T {
    serde_yaml::from_value(value).unwrap_or_else(|error| {
        log::warn!("omatrack.yml: ignoring unreadable {what}: {error}");
        T::default()
    })
}

/// Any value that parses as `T`, else `T::default()`.
fn lenient<'de, D, T>(deserializer: D) -> Result<T, D::Error>
where
    D: Deserializer<'de>,
    T: DeserializeOwned + Default,
{
    let value = Value::deserialize(deserializer)?;
    Ok(value_or_default(value, std::any::type_name::<T>()))
}

fn text_bool(value: &Value) -> Option<bool> {
    match value {
        Value::Bool(flag) => Some(*flag),
        Value::String(text) => match text.trim().to_ascii_lowercase().as_str() {
            "true" | "yes" | "on" | "1" => Some(true),
            "false" | "no" | "off" | "0" => Some(false),
            _ => None,
        },
        Value::Number(number) => number.as_i64().map(|n| n != 0),
        _ => None,
    }
}

fn text_f64(value: &Value) -> Option<f64> {
    match value {
        Value::Number(number) => number.as_f64(),
        Value::String(text) => text.trim().parse().ok(),
        _ => None,
    }
    .filter(|number: &f64| number.is_finite())
}

fn opt_bool<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Option<bool>, D::Error> {
    Ok(text_bool(&Value::deserialize(deserializer)?))
}

fn opt_f64<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Option<f64>, D::Error> {
    Ok(text_f64(&Value::deserialize(deserializer)?))
}

fn opt_i32<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Option<i32>, D::Error> {
    Ok(text_f64(&Value::deserialize(deserializer)?)
        .filter(|n| n.fract() == 0.0 && *n >= f64::from(i32::MIN) && *n <= f64::from(i32::MAX))
        .map(|n| n as i32))
}

fn opt_text<'de, D: Deserializer<'de>>(deserializer: D) -> Result<Option<String>, D::Error> {
    Ok(scalar_text(&Value::deserialize(deserializer)?)
        .map(|text| text.trim().to_string())
        .filter(|text| !text.is_empty()))
}

fn f64_or_nan<'de, D: Deserializer<'de>>(deserializer: D) -> Result<f64, D::Error> {
    Ok(text_f64(&Value::deserialize(deserializer)?).unwrap_or(f64::NAN))
}

/// A mapping of scalars to scalars, both as text (numeric keys included).
fn text_map<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> Result<BTreeMap<String, String>, D::Error> {
    let value = Value::deserialize(deserializer)?;
    let mut out = BTreeMap::new();
    if let Value::Mapping(map) = value {
        for (key, value) in map {
            if let (Some(key), Some(value)) = (scalar_text(&key), scalar_text(&value)) {
                let (key, value) = (key.trim().to_string(), value.trim().to_string());
                if !key.is_empty() && !value.is_empty() {
                    out.insert(key, value);
                }
            }
        }
    }
    Ok(out)
}

/// Mapping entries keyed by text, each parsed leniently.
fn keyed<'de, D, T>(deserializer: D) -> Result<BTreeMap<String, T>, D::Error>
where
    D: Deserializer<'de>,
    T: DeserializeOwned + Default,
{
    let value = Value::deserialize(deserializer)?;
    let mut out = BTreeMap::new();
    if let Value::Mapping(map) = value {
        for (key, value) in map {
            if let Some(key) = scalar_text(&key) {
                out.insert(key, value_or_default(value, "entry"));
            }
        }
    }
    Ok(out)
}

// ── sections ────────────────────────────────────────────────────────

/// One library location. Only `type: folder` is interpreted; any other
/// type (a connection a different build understands) is kept verbatim and
/// ignored.
#[derive(Debug, Clone, PartialEq)]
pub enum LocationConfig {
    Folder(FolderLocationConfig),
    Other(Mapping),
}

/// A local folder scanned recursively.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct FolderLocationConfig {
    #[serde(default, deserialize_with = "opt_text")]
    pub id: Option<String>,
    #[serde(default, deserialize_with = "opt_text")]
    pub name: Option<String>,
    #[serde(default, deserialize_with = "opt_text")]
    pub target: Option<String>,
    /// Disabled locations stay configured and are skipped by every scan.
    #[serde(default, deserialize_with = "opt_bool")]
    pub enabled: Option<bool>,
    #[serde(flatten)]
    pub extra: Mapping,
}

impl FolderLocationConfig {
    /// A folder location for `target`, enabled, with a derived id.
    pub fn new(target: impl Into<PathBuf>) -> Self {
        let target = target.into().to_string_lossy().into_owned();
        Self {
            id: Some(folder_location_id(&target)),
            target: Some(target),
            ..Self::default()
        }
    }
    /// The configured id, else one derived from the target.
    pub fn resolved_id(&self) -> String {
        self.id
            .clone()
            .unwrap_or_else(|| folder_location_id(self.target.as_deref().unwrap_or("")))
    }
    pub fn is_enabled(&self) -> bool {
        self.enabled.unwrap_or(true)
    }
    pub fn target_path(&self) -> Option<PathBuf> {
        self.target.as_ref().map(PathBuf::from)
    }
    /// The user's name, else the folder's own directory name.
    pub fn display_name(&self) -> String {
        if let Some(name) = &self.name {
            return name.clone();
        }
        let target = self.target.as_deref().unwrap_or("");
        Path::new(target)
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| target.to_string())
    }
}

/// A stable id for a folder location without one: BLAKE3 of the target.
pub fn folder_location_id(target: &str) -> String {
    blake3::hash(format!("{}\n", target.trim()).as_bytes())
        .to_hex()
        .to_string()
}

impl<'de> Deserialize<'de> for LocationConfig {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let value = Value::deserialize(deserializer)?;
        let Value::Mapping(map) = value else {
            let mut wrapped = Mapping::new();
            wrapped.insert(Value::from("value"), value);
            return Ok(Self::Other(wrapped));
        };
        let is_folder = map
            .get("type")
            .and_then(scalar_text)
            .is_some_and(|kind| kind.trim() == "folder");
        if is_folder
            && let Ok(mut folder) =
                serde_yaml::from_value::<FolderLocationConfig>(Value::Mapping(map.clone()))
            && folder.target.is_some()
        {
            folder.extra.remove("type");
            return Ok(Self::Folder(folder));
        }
        Ok(Self::Other(map))
    }
}

impl Serialize for LocationConfig {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Self::Other(map) => map.serialize(serializer),
            Self::Folder(folder) => {
                let mut map = Mapping::new();
                map.insert("id".into(), folder.resolved_id().into());
                map.insert("type".into(), "folder".into());
                if let Some(target) = &folder.target {
                    map.insert("target".into(), target.clone().into());
                }
                map.insert("enabled".into(), folder.is_enabled().into());
                if let Some(name) = &folder.name {
                    map.insert("name".into(), name.clone().into());
                }
                for (key, value) in &folder.extra {
                    if !map.contains_key(key) {
                        map.insert(key.clone(), value.clone());
                    }
                }
                map.serialize(serializer)
            }
        }
    }
}

/// `channels.<key>`: one trace channel's display settings. Unset values
/// take the product defaults ([`ChannelStyle::defaults`]); colours come
/// from the theme unless set here.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct ChannelConfig {
    #[serde(
        default,
        deserialize_with = "opt_bool",
        skip_serializing_if = "Option::is_none"
    )]
    pub visible: Option<bool>,
    #[serde(
        default,
        deserialize_with = "opt_text",
        skip_serializing_if = "Option::is_none"
    )]
    pub color: Option<String>,
    #[serde(
        default,
        deserialize_with = "opt_text",
        skip_serializing_if = "Option::is_none"
    )]
    pub reference_color: Option<String>,
    #[serde(
        default,
        deserialize_with = "opt_f64",
        skip_serializing_if = "Option::is_none"
    )]
    pub stroke_width: Option<f64>,
    #[serde(
        default,
        deserialize_with = "opt_f64",
        skip_serializing_if = "Option::is_none"
    )]
    pub fill_opacity: Option<f64>,
    #[serde(
        default,
        deserialize_with = "opt_f64",
        skip_serializing_if = "Option::is_none"
    )]
    pub height_percent: Option<f64>,
    #[serde(
        default,
        deserialize_with = "opt_f64",
        skip_serializing_if = "Option::is_none"
    )]
    pub weight: Option<f64>,
    #[serde(
        default,
        deserialize_with = "opt_bool",
        skip_serializing_if = "Option::is_none"
    )]
    pub combine_with_previous: Option<bool>,
    #[serde(flatten)]
    pub extra: Mapping,
}

/// A channel's effective display settings: configuration over defaults,
/// clamped to sane ranges. Colours stay `None` when the theme decides.
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct ChannelStyle {
    pub visible: bool,
    pub color: Option<String>,
    pub reference_color: Option<String>,
    /// Logical pixels, independent of zoom.
    pub stroke_width: f64,
    /// Peak fill alpha; fades to zero at the baseline.
    pub fill_opacity: f64,
    pub height_percent: f64,
    pub weight: f64,
    pub combine_with_previous: bool,
}

impl ChannelStyle {
    /// Product defaults for a channel key (the Qt channel defaults).
    pub fn defaults(key: &str) -> Self {
        let visible = !matches!(
            key,
            "delta" | "clutch" | "driver_throttle" | "gps_lat" | "gps_lon"
        ) && !key.starts_with("raw:");
        // Light pedal fills so the reference outline reads through them; the
        // Δ gain/loss fill is that lane's message. Mirrors
        // `omatrack_trace::scene::{PEDAL_FILL, DELTA_FILL}`.
        let fill_opacity = match key {
            "throttle" | "brake" | "clutch" => 0.16,
            "delta" => 0.42,
            _ => 0.0,
        };
        // Relative lane heights (FIT weights). Mirrors
        // `omatrack_trace::layout::default_height_percent`.
        let height_percent = match key {
            "speed" => 34.0,
            "throttle" | "brake" => 16.0,
            "delta" => 20.0,
            "steering" => 16.0,
            "gear" => 12.0,
            _ if key.to_ascii_lowercase().contains("rpm") => 14.0,
            _ => 12.0,
        };
        Self {
            visible,
            color: None,
            reference_color: None,
            stroke_width: 1.25,
            fill_opacity,
            height_percent,
            weight: 1.0,
            // Brake has its own lane: the pedals read side by side in
            // one column, never on two scales in one lane.
            combine_with_previous: false,
        }
    }

    fn apply(mut self, config: &ChannelConfig) -> Self {
        if let Some(visible) = config.visible {
            self.visible = visible;
        }
        self.color = config.color.clone();
        self.reference_color = config.reference_color.clone();
        if let Some(width) = config.stroke_width {
            self.stroke_width = width.clamp(0.5, 4.0);
        }
        if let Some(fill) = config.fill_opacity {
            self.fill_opacity = fill.clamp(0.0, 1.0);
        }
        if let Some(height) = config.height_percent {
            self.height_percent = height.clamp(1.0, 100.0);
        }
        if let Some(weight) = config.weight.filter(|w| *w > 0.0) {
            self.weight = weight.clamp(0.05, 20.0);
        }
        if let Some(combine) = config.combine_with_previous {
            self.combine_with_previous = combine;
        }
        self
    }
}

/// Trace x-axis.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum XAxis {
    #[default]
    Distance,
    Time,
}

/// `trace`: trace workspace settings.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct TraceConfig {
    /// Fit every visible lane into the workspace (default on).
    #[serde(
        default,
        deserialize_with = "opt_bool",
        skip_serializing_if = "Option::is_none"
    )]
    pub fit_channels: Option<bool>,
    #[serde(
        default,
        deserialize_with = "lenient",
        skip_serializing_if = "Option::is_none"
    )]
    pub x_axis: Option<XAxis>,
    #[serde(flatten)]
    pub extra: Mapping,
}

impl TraceConfig {
    pub fn is_fitting_channels(&self) -> bool {
        self.fit_channels.unwrap_or(true)
    }
    pub fn x_axis(&self) -> XAxis {
        self.x_axis.unwrap_or_default()
    }
}

/// `video.hud_position`: the HUD's normalized position in the video.
#[derive(Debug, Clone, Copy, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct HudPosition {
    #[serde(default = "nan", deserialize_with = "f64_or_nan")]
    pub x: f64,
    #[serde(default = "nan", deserialize_with = "f64_or_nan")]
    pub y: f64,
}

fn nan() -> f64 {
    f64::NAN
}

fn hud_position<'de, D: Deserializer<'de>>(
    deserializer: D,
) -> Result<Option<HudPosition>, D::Error> {
    let position: Option<HudPosition> = lenient(deserializer)?;
    Ok(position.filter(HudPosition::is_valid))
}

impl HudPosition {
    pub fn new(x: f64, y: f64) -> Self {
        Self { x, y }
    }
    /// Both coordinates inside [0, 1].
    pub fn is_valid(&self) -> bool {
        (0.0..=1.0).contains(&self.x) && (0.0..=1.0).contains(&self.y)
    }
}

/// `video`: onboard video settings.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct VideoConfig {
    #[serde(
        default,
        deserialize_with = "opt_bool",
        skip_serializing_if = "Option::is_none"
    )]
    pub muted: Option<bool>,
    /// Requested alignment strategy key (`gps`, `pre-corner-dampers`,
    /// `manual-dampers`, `lap-percentage`).
    #[serde(
        default,
        deserialize_with = "opt_text",
        skip_serializing_if = "Option::is_none"
    )]
    pub reference_sync: Option<String>,
    /// `corners`, `gps` or `recording`.
    #[serde(
        default,
        deserialize_with = "opt_text",
        skip_serializing_if = "Option::is_none"
    )]
    pub reference_playback: Option<String>,
    #[serde(
        default,
        deserialize_with = "opt_bool",
        skip_serializing_if = "Option::is_none"
    )]
    pub continuous_playback: Option<bool>,
    /// Dropped when either coordinate is missing or outside [0, 1].
    #[serde(
        default,
        deserialize_with = "hud_position",
        skip_serializing_if = "Option::is_none"
    )]
    pub hud_position: Option<HudPosition>,
    #[serde(flatten)]
    pub extra: Mapping,
}

impl VideoConfig {
    pub fn is_muted(&self) -> bool {
        self.muted.unwrap_or(false)
    }
    pub fn is_continuous_playback(&self) -> bool {
        self.continuous_playback.unwrap_or(false)
    }
    /// The requested strategy, when it is one this build knows.
    pub fn reference_sync(&self) -> Option<Strategy> {
        self.reference_sync.as_deref().and_then(Strategy::from_key)
    }
    /// Reference playback policy (corners by default).
    pub fn reference_playback(&self) -> ReferencePlayback {
        self.reference_playback
            .as_deref()
            .and_then(ReferencePlayback::from_key)
            .unwrap_or_default()
    }
    /// The saved HUD position, when valid.
    pub fn hud_position(&self) -> Option<HudPosition> {
        self.hud_position.filter(HudPosition::is_valid)
    }

    /// Save a HUD position (`None`, or an invalid one, clears it).
    pub fn set_hud_position(&mut self, position: Option<HudPosition>) {
        self.hud_position = position.filter(HudPosition::is_valid);
    }
}

/// One user corner zone in `tracks.<key>.corners` (lap fractions).
/// Like every typed key, an unreadable value falls back (here: the zone is
/// skipped) and is not written back on the next save.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct CornerOverride {
    #[serde(
        default,
        deserialize_with = "opt_text",
        skip_serializing_if = "Option::is_none"
    )]
    pub id: Option<String>,
    #[serde(
        default,
        deserialize_with = "opt_text",
        skip_serializing_if = "Option::is_none"
    )]
    pub name: Option<String>,
    /// Lap fraction; `None` when missing or unreadable.
    #[serde(
        default,
        deserialize_with = "opt_f64",
        skip_serializing_if = "Option::is_none"
    )]
    pub start: Option<f64>,
    #[serde(
        default,
        deserialize_with = "opt_f64",
        skip_serializing_if = "Option::is_none"
    )]
    pub end: Option<f64>,
    #[serde(flatten)]
    pub extra: Mapping,
}

/// `tracks.<key>`: per-track settings.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct TrackConfig {
    /// The user's corner zones; they win over Track Atlas on load.
    #[serde(
        default,
        deserialize_with = "lenient",
        skip_serializing_if = "Vec::is_empty"
    )]
    pub corners: Vec<CornerOverride>,
    #[serde(flatten)]
    pub extra: Mapping,
}

impl TrackConfig {
    /// The valid corner overrides as zones, in document order.
    pub fn corner_zones(&self) -> Vec<CornerZone> {
        self.corners
            .iter()
            .enumerate()
            .filter_map(|(i, zone)| {
                let (start, end) = (zone.start?, zone.end?);
                if end <= start {
                    return None;
                }
                let id = zone.id.clone().unwrap_or_else(|| format!("t{}", i + 1));
                Some(CornerZone {
                    name: zone.name.clone().unwrap_or_else(|| id.clone()),
                    id,
                    start: start.clamp(0.0, 1.0),
                    end: end.clamp(0.0, 1.0),
                    source: ZoneSource::User,
                })
            })
            .collect()
    }
}

/// `selection`: the last primary/reference selection (restored at startup).
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct SelectionConfig {
    #[serde(
        default,
        deserialize_with = "opt_text",
        skip_serializing_if = "Option::is_none"
    )]
    pub primary_key: Option<String>,
    #[serde(
        default,
        deserialize_with = "opt_i32",
        skip_serializing_if = "Option::is_none"
    )]
    pub primary_lap: Option<i32>,
    #[serde(
        default,
        deserialize_with = "opt_text",
        skip_serializing_if = "Option::is_none"
    )]
    pub compare_key: Option<String>,
    #[serde(
        default,
        deserialize_with = "opt_i32",
        skip_serializing_if = "Option::is_none"
    )]
    pub compare_lap: Option<i32>,
    #[serde(flatten)]
    pub extra: Mapping,
}

impl SelectionConfig {
    /// The primary recording key and lap, when both are set (`-1` is none).
    pub fn primary(&self) -> Option<(&str, i32)> {
        Some((
            self.primary_key.as_deref()?,
            self.primary_lap.filter(|l| *l >= 0)?,
        ))
    }
    pub fn reference(&self) -> Option<(&str, i32)> {
        Some((
            self.compare_key.as_deref()?,
            self.compare_lap.filter(|l| *l >= 0)?,
        ))
    }
}

/// `workspace`: the dock layout.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct WorkspaceConfig {
    /// Opaque, versioned dock layout owned by the app.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub layout: Option<serde_json::Value>,
    #[serde(flatten)]
    pub extra: Mapping,
}

/// The typed `omatrack.yml`. Serialized schema: public fields, every
/// section keeps its unknown keys in `extra`.
#[derive(Debug, Clone, PartialEq, Default, Serialize, Deserialize)]
#[non_exhaustive]
pub struct Config {
    /// `None` until the library was configured (a fresh install).
    #[serde(
        default,
        deserialize_with = "lenient",
        skip_serializing_if = "Option::is_none"
    )]
    pub locations: Option<Vec<LocationConfig>>,
    #[serde(
        default,
        deserialize_with = "lenient",
        skip_serializing_if = "Vec::is_empty"
    )]
    pub recent_files: Vec<String>,
    #[serde(
        default,
        deserialize_with = "keyed",
        skip_serializing_if = "BTreeMap::is_empty"
    )]
    pub channels: BTreeMap<String, ChannelConfig>,
    #[serde(
        default,
        deserialize_with = "lenient",
        skip_serializing_if = "is_default"
    )]
    pub trace: TraceConfig,
    #[serde(
        default,
        deserialize_with = "lenient",
        skip_serializing_if = "is_default"
    )]
    pub video: VideoConfig,
    /// Driver id -> name (preference layer of the metadata precedence).
    #[serde(
        default,
        deserialize_with = "text_map",
        skip_serializing_if = "BTreeMap::is_empty"
    )]
    pub driver_mappings: BTreeMap<String, String>,
    /// Event date (`yyyy-mm-dd`) or folder -> Track Atlas slug.
    #[serde(
        default,
        deserialize_with = "text_map",
        skip_serializing_if = "BTreeMap::is_empty"
    )]
    pub track_assignments: BTreeMap<String, String>,
    /// Recording path -> per-recording metadata override (TRACK.yml shape).
    #[serde(
        default,
        deserialize_with = "keyed",
        skip_serializing_if = "BTreeMap::is_empty"
    )]
    pub recording_metadata: BTreeMap<String, Mapping>,
    #[serde(
        default,
        deserialize_with = "keyed",
        skip_serializing_if = "BTreeMap::is_empty"
    )]
    pub tracks: BTreeMap<String, TrackConfig>,
    #[serde(
        default,
        deserialize_with = "lenient",
        skip_serializing_if = "is_default"
    )]
    pub selection: SelectionConfig,
    #[serde(
        default,
        deserialize_with = "lenient",
        skip_serializing_if = "is_default"
    )]
    pub workspace: WorkspaceConfig,
    #[serde(flatten)]
    pub extra: Mapping,
}

fn is_default<T: Default + PartialEq>(value: &T) -> bool {
    *value == T::default()
}

/// The `tracks.<key>` key for a track name (lowercase, spaces and dashes
/// to `_`, other punctuation dropped; `unknown` when nothing is left).
pub fn track_key(track: &str) -> String {
    let key: String = track
        .to_lowercase()
        .chars()
        .map(|c| if c == ' ' || c == '-' { '_' } else { c })
        .filter(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || *c == '_')
        .collect();
    if key.is_empty() {
        "unknown".to_string()
    } else {
        key
    }
}

impl Config {
    /// Parse a document. An empty document is the default configuration.
    pub fn from_yaml_str(text: &str) -> Result<Self, serde_yaml::Error> {
        if text.trim().is_empty() {
            return Ok(Self::default());
        }
        let value: Value = serde_yaml::from_str(text)?;
        match value {
            Value::Null => Ok(Self::default()),
            value => serde_yaml::from_value(value),
        }
    }

    pub fn to_yaml_string(&self) -> Result<String, serde_yaml::Error> {
        serde_yaml::to_string(self)
    }

    /// Read `path`; a missing file is the default configuration.
    pub fn load(path: &Path) -> Result<Self, ConfigError> {
        let text = match std::fs::read_to_string(path) {
            Ok(text) => text,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(Self::default()),
            Err(source) => {
                return Err(ConfigError::Io {
                    path: path.to_path_buf(),
                    source,
                });
            }
        };
        Self::from_yaml_str(&text).map_err(|error| ConfigError::Parse {
            path: path.to_path_buf(),
            message: error.to_string(),
        })
    }

    /// Write `path` atomically (temporary file, then rename).
    pub fn save(&self, path: &Path) -> Result<(), ConfigError> {
        let text = self.to_yaml_string().map_err(|error| ConfigError::Parse {
            path: path.to_path_buf(),
            message: error.to_string(),
        })?;
        write_atomic(path, text.as_bytes()).map_err(|source| ConfigError::Io {
            path: path.to_path_buf(),
            source,
        })
    }

    /// Enabled and disabled folder locations, in library order.
    pub fn folder_locations(&self) -> impl Iterator<Item = &FolderLocationConfig> {
        self.locations
            .iter()
            .flatten()
            .filter_map(|location| match location {
                LocationConfig::Folder(folder) => Some(folder),
                LocationConfig::Other(_) => None,
            })
    }

    /// On a fresh install (no `locations` key), make `default_dir` the only
    /// location. Returns true when the document changed.
    pub fn ensure_default_location(&mut self, default_dir: &Path) -> bool {
        if self.locations.is_some() {
            return false;
        }
        self.locations = Some(vec![LocationConfig::Folder(FolderLocationConfig::new(
            default_dir,
        ))]);
        true
    }

    /// Add a folder location unless one already targets it. Returns true
    /// when added.
    pub fn add_folder_location(&mut self, directory: &Path) -> bool {
        let target = directory.to_string_lossy();
        if self
            .folder_locations()
            .any(|folder| folder.target.as_deref() == Some(&*target))
        {
            return false;
        }
        self.locations
            .get_or_insert_with(Vec::new)
            .push(LocationConfig::Folder(FolderLocationConfig::new(directory)));
        true
    }

    /// Remove the folder location with `id`. Returns true when removed.
    pub fn remove_location(&mut self, id: &str) -> bool {
        let Some(locations) = self.locations.as_mut() else {
            return false;
        };
        let before = locations.len();
        locations.retain(|location| match location {
            LocationConfig::Folder(folder) => folder.resolved_id() != id,
            LocationConfig::Other(_) => true,
        });
        before != locations.len()
    }

    /// Effective display settings of one channel.
    pub fn channel_style(&self, key: &str) -> ChannelStyle {
        let defaults = ChannelStyle::defaults(key);
        match self.channels.get(key) {
            Some(config) => defaults.apply(config),
            None => defaults,
        }
    }

    /// Record a successful open (most recent first, deduplicated, capped).
    pub fn push_recent_file(&mut self, path: &str) {
        crate::recent::push_recent(&mut self.recent_files, path);
    }

    /// The user's corner zones for a track, if any.
    pub fn track_corners(&self, track: &str) -> Option<Vec<CornerZone>> {
        let zones = self.tracks.get(&track_key(track))?.corner_zones();
        (!zones.is_empty()).then_some(zones)
    }

    /// Replace (or with `None`, drop) the user's corner zones for a track.
    pub fn set_track_corners(&mut self, track: &str, zones: Option<&[CornerZone]>) {
        let key = track_key(track);
        match zones.filter(|zones| !zones.is_empty()) {
            Some(zones) => {
                let entry = self.tracks.entry(key).or_default();
                entry.corners = zones
                    .iter()
                    .map(|zone| CornerOverride {
                        id: Some(zone.id.clone()),
                        name: Some(zone.name.clone()),
                        start: Some(zone.start),
                        end: Some(zone.end),
                        extra: Mapping::new(),
                    })
                    .collect();
            }
            None => {
                if let Some(entry) = self.tracks.get_mut(&key) {
                    entry.corners.clear();
                    if entry.extra.is_empty() {
                        self.tracks.remove(&key);
                    }
                }
            }
        }
    }

    /// Per-recording metadata override (layer 1).
    pub fn recording_override(&self, path: &Path) -> Option<&Mapping> {
        self.recording_metadata.get(&*path.to_string_lossy())
    }
}

/// `omatrack.yml` on disk. A document that exists but cannot be read is
/// never overwritten: the configuration falls back to defaults for this
/// session and [`ConfigFile::save`] refuses.
#[derive(Debug)]
pub struct ConfigFile {
    path: PathBuf,
    config: Config,
    load_error: Option<ConfigError>,
}

impl ConfigFile {
    pub fn open(path: impl Into<PathBuf>) -> Self {
        let path = path.into();
        match Config::load(&path) {
            Ok(config) => Self {
                path,
                config,
                load_error: None,
            },
            Err(error) => {
                log::warn!("{error}");
                Self {
                    path,
                    config: Config::default(),
                    load_error: Some(error),
                }
            }
        }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }
    pub fn config(&self) -> &Config {
        &self.config
    }
    pub fn config_mut(&mut self) -> &mut Config {
        &mut self.config
    }
    /// Why the document could not be read, if it could not.
    pub fn load_error(&self) -> Option<&ConfigError> {
        self.load_error.as_ref()
    }
    /// False when the document on disk could not be read.
    pub fn is_writable(&self) -> bool {
        self.load_error.is_none()
    }

    /// Write the configuration atomically, unless the document on disk
    /// could not be read.
    pub fn save(&self) -> Result<(), ConfigError> {
        if !self.is_writable() {
            return Err(ConfigError::Readonly {
                path: self.path.clone(),
            });
        }
        self.config.save(&self.path)
    }
}
