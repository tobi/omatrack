//! The pure session model: one lap loaded for analysis ([`LoadedLap`]) and
//! the primary/reference [`Analysis`] every surface reads.
//!
//! This is the order `TelemetryStore::loadSessionLap` and
//! `buildCornerRows` ran in, without Qt and without a store:
//!
//! ```text
//! load_lap(recording, lap id)     laps -> unify -> track layout -> overlay
//!         -> LoadedLap            providers -> lap strip -> video binding
//! Analysis::build(primary, reference)
//!         -> corner zones (atlas through the station map, a user override,
//!            or brake-zone generation) -> Comparison (the one map) ->
//!            corner rows (metrics, deltas, notes from the check registry)
//! ```
//!
//! Everything here is `Send + Sync`, shares its large arrays through `Arc`,
//! and checks a caller-owned cancel flag between stages, so a UI can run it
//! on a background executor and drop stale results (latest wins).

use crate::alignment::Strategy;
use crate::comparison::{self, Comparison};
use crate::corners::zones::{
    ComplexZone, CornerZone, StationMapper, ZoneSource, atlas_complex_zones, atlas_corner_zones,
    auto_generate_corners,
};
use crate::corners::{CornerContext, CornerMetrics, CornerNote, NoteSeverity, checks};
use crate::laps::{Lap, LapKind, classify_laps};
use crate::mapping::ChannelOverrides;
use crate::monotonic::{interpolate_fraction, invert_fraction};
use crate::num::clamp;
use crate::overlay::{ChannelProvider, OverlayGroup, SourceChannels, StandardChannels};
use crate::recording::Recording;
use crate::track::{self, TrackLayout};
use crate::unify::UnifiedLap;
use crate::video_clock::{VideoClock, blake3_file};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

/// A session stage failed or was abandoned.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[non_exhaustive]
pub enum SessionError {
    /// The caller's cancel flag was raised between stages.
    #[error("cancelled")]
    Cancelled,
    /// The recording has no lap with this id.
    #[error("no lap {0} in the recording")]
    NoSuchLap(i32),
    /// The lap produced no unified samples.
    #[error("unable to normalize the selected lap")]
    EmptyLap,
}

fn check_cancel(cancel: &AtomicBool) -> Result<(), SessionError> {
    if cancel.load(Ordering::Relaxed) {
        Err(SessionError::Cancelled)
    } else {
        Ok(())
    }
}

/// Extensions (lowercase) of recordings the parsers open, besides MTJ
/// documents, which are recognised by their full suffix.
pub const RECORDING_EXTENSIONS: &[&str] = &["mp4", "pds", "ld", "vbo", "telemetry"];

/// True when `path` names a host recording the core can open (MTX sidecar
/// extensions are documents about a recording, not recordings).
pub fn is_recording_path(path: &Path) -> bool {
    if telemetry_format::is_jsonl_path(path) {
        return !telemetry_format::is_jsonl_ext_path(path);
    }
    path.extension()
        .and_then(|ext| ext.to_str())
        .map(str::to_ascii_lowercase)
        .is_some_and(|ext| RECORDING_EXTENSIONS.contains(&ext.as_str()))
}

/// True when the recording file is itself the onboard video (AiM MP4).
pub fn is_video_path(path: &Path) -> bool {
    path.extension()
        .and_then(|ext| ext.to_str())
        .is_some_and(|ext| ext.eq_ignore_ascii_case("mp4"))
}

// ── laps ────────────────────────────────────────────────────────────

/// Driver-facing labels for a classified lap list, in order: `L<n>` for a
/// complete lap (the source number when upstream supplied one, else the
/// sequential count), else the fragment role `Out`, `In`, `Pit` or `Frag`
/// (port of `SessionHandle::populateLaps`).
pub fn lap_labels(laps: &[Lap]) -> Vec<String> {
    let mut sequential = 0;
    laps.iter()
        .enumerate()
        .map(|(i, lap)| {
            if lap.complete {
                sequential += 1;
                return format!("L{}", lap.source_number.unwrap_or(sequential));
            }
            match lap.kind {
                LapKind::Pit => "Pit".to_string(),
                LapKind::Out => "Out".to_string(),
                LapKind::In => "In".to_string(),
                _ if i == 0 => "Out".to_string(),
                _ if i + 1 == laps.len() => "In".to_string(),
                _ => "Frag".to_string(),
            }
        })
        .collect()
}

/// The representative fastest lap: the shortest lap that counts for best.
/// `None` when no lap does (then nothing is marked best).
pub fn best_lap_id(laps: &[Lap]) -> Option<i32> {
    laps.iter()
        .filter(|lap| lap.counts_for_best() && lap.time_ms.is_finite())
        .min_by(|a, b| a.time_ms.total_cmp(&b.time_ms))
        .map(|lap| lap.id)
}

/// Role of one lap-strip cell.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum LapStripKind {
    /// A complete start/finish-to-start/finish lap.
    Flying,
    Out,
    In,
    /// An incomplete interval between two crossings.
    Fragment,
    /// The stationary interval upstream carved out of a pit stop: one fixed
    /// cell however long the car stood.
    PitStop,
}

/// One lap-strip cell (plain data). `driven_s` is the view-only width
/// projection: lap time minus clearly stopped time (`stopped_duration`),
/// never a catalog scalar.
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct LapStripCell {
    pub lap_id: i32,
    pub label: String,
    pub kind: LapStripKind,
    pub time_ms: f64,
    pub driven_s: f64,
    /// A pit in/out lap or a pit stop.
    pub pit: bool,
    /// The representative fastest lap.
    pub best: bool,
    pub complete: bool,
}

fn strip_kind(lap: &Lap, label: &str) -> LapStripKind {
    if lap.is_pit_stop() {
        return LapStripKind::PitStop;
    }
    if lap.complete {
        return LapStripKind::Flying;
    }
    match label {
        "Out" => LapStripKind::Out,
        "In" => LapStripKind::In,
        _ => LapStripKind::Fragment,
    }
}

fn lap_strip(
    recording: &Recording,
    laps: &[Lap],
    overrides: &ChannelOverrides,
    cancel: &AtomicBool,
) -> Result<Vec<LapStripCell>, SessionError> {
    let labels = lap_labels(laps);
    let best = best_lap_id(laps);
    let mut cells = Vec::with_capacity(laps.len());
    for (lap, label) in laps.iter().zip(labels) {
        check_cancel(cancel)?;
        let seconds = if lap.time_ms.is_finite() {
            lap.time_ms.max(0.0) / 1000.0
        } else {
            0.0
        };
        let stopped = recording
            .stopped_duration(lap.start_time, lap.end_time, overrides)
            .unwrap_or(0.0);
        cells.push(LapStripCell {
            lap_id: lap.id,
            kind: strip_kind(lap, &label),
            label,
            time_ms: lap.time_ms,
            driven_s: (seconds - stopped).max(0.0),
            pit: lap.is_pit_lap || lap.is_pit_stop(),
            best: best == Some(lap.id),
            complete: lap.complete,
        });
    }
    Ok(cells)
}

// ── video ───────────────────────────────────────────────────────────

/// How far the linked video is known to be the one the telemetry describes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum IdentityState {
    /// Hashing was not requested.
    #[default]
    NotChecked,
    /// The telemetry and the video are the same file (AiM MP4).
    ExactSource,
    /// The file's BLAKE3 matches the catalog.
    VerifiedHash,
    /// Nothing to verify against (no link or no hash).
    Unverified,
    /// A different file, or the file changed after conversion.
    Mismatch,
}

impl IdentityState {
    /// Video timing may be trusted.
    pub fn is_trusted(self) -> bool {
        matches!(self, Self::ExactSource | Self::VerifiedHash)
    }
}

/// The onboard video bound to a loaded lap (plain data).
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct VideoBinding {
    pub path: PathBuf,
    /// Telemetry <-> presentation mapping copied from the recording.
    pub clock: Arc<VideoClock>,
    pub identity: IdentityState,
    /// Catalog file index the clock offsets apply to.
    pub file_index: Option<u32>,
    /// User-facing explanation when the identity is not trusted.
    pub warning: Option<String>,
}

fn canonical(path: &Path) -> Option<PathBuf> {
    std::fs::canonicalize(path).ok()
}

/// Port of `verifyVideoIdentity`: which catalog video `video_path` is, and
/// whether it is provably that file. `hash` enables the BLAKE3 check of a
/// separate video (a full read of a multi-gigabyte file).
pub fn verify_video_identity(
    recording: &Recording,
    video_path: &Path,
    hash: bool,
) -> (IdentityState, Option<u32>, Option<String>) {
    let clock = recording.video_clock();
    let filename = video_path
        .file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_default();
    let expected = clock
        .file_named(&filename)
        .or_else(|| {
            clock
                .files
                .iter()
                .find(|file| file.filename.eq_ignore_ascii_case(&filename))
        })
        // Older single-video catalogs may carry a stale filename after the
        // recording was renamed; there is still only one clock.
        .or_else(|| (clock.files.len() == 1).then(|| &clock.files[0]));

    let video_size = std::fs::metadata(video_path).map(|m| m.len()).unwrap_or(0);
    let same_file = video_size > 0
        && canonical(Path::new(recording.path())).is_some_and(|a| Some(a) == canonical(video_path));
    if same_file {
        return (
            IdentityState::ExactSource,
            expected.map(|file| file.index),
            None,
        );
    }
    let Some(expected) = expected else {
        return if clock.files.is_empty() {
            (
                IdentityState::Unverified,
                None,
                Some("The telemetry has no linked video identity.".to_string()),
            )
        } else {
            (
                IdentityState::Mismatch,
                None,
                Some("The telemetry links a different video file.".to_string()),
            )
        };
    };
    let index = Some(expected.index);
    let Some(wanted) = expected.blake3 else {
        return (
            IdentityState::Unverified,
            index,
            Some("The telemetry has no video identity hash.".to_string()),
        );
    };
    if !hash {
        return (IdentityState::NotChecked, index, None);
    }
    match blake3_file(video_path) {
        None => (
            IdentityState::Unverified,
            index,
            Some("The video identity could not be verified.".to_string()),
        ),
        Some(actual) if actual != wanted => (
            IdentityState::Mismatch,
            index,
            Some("The video was changed after telemetry conversion.".to_string()),
        ),
        Some(_) => (IdentityState::VerifiedHash, index, None),
    }
}

/// The recording's own path when it is the video, else a linked video file
/// sitting next to it.
fn default_video_path(recording: &Recording) -> Option<PathBuf> {
    let path = Path::new(recording.path());
    if recording.path().is_empty() {
        return None;
    }
    if is_video_path(path) {
        return Some(path.to_path_buf());
    }
    let directory = path.parent()?;
    recording
        .video_clock()
        .files
        .iter()
        .filter(|file| !file.filename.is_empty())
        .map(|file| directory.join(&file.filename))
        .find(|candidate| candidate.is_file())
}

// ── loading ─────────────────────────────────────────────────────────

/// Inputs to [`load_lap`] beyond the recording and lap id.
#[derive(Clone)]
pub struct LoadOptions {
    overrides: ChannelOverrides,
    providers: Vec<Arc<dyn ChannelProvider>>,
    provider_keys: BTreeMap<String, Vec<String>>,
    track_hint: Option<String>,
    video_path: Option<PathBuf>,
    verify_video_hash: bool,
}

impl std::fmt::Debug for LoadOptions {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("LoadOptions")
            .field("overrides", &self.overrides)
            .field(
                "providers",
                &self.providers.iter().map(|p| p.id()).collect::<Vec<_>>(),
            )
            .field("provider_keys", &self.provider_keys)
            .field("track_hint", &self.track_hint)
            .field("video_path", &self.video_path)
            .field("verify_video_hash", &self.verify_video_hash)
            .finish()
    }
}

impl Default for LoadOptions {
    /// No channel overrides, the [`StandardChannels`] provider, no raw
    /// source channels, track resolved from the lap's own GPS, the
    /// recording's own (or sibling linked) video, no BLAKE3 hashing.
    fn default() -> Self {
        Self {
            overrides: ChannelOverrides::new(),
            providers: vec![Arc::new(StandardChannels)],
            provider_keys: BTreeMap::new(),
            track_hint: None,
            video_path: None,
            verify_video_hash: false,
        }
    }
}

impl LoadOptions {
    pub fn new() -> Self {
        Self::default()
    }

    /// Concept -> source channel overrides (effective recording metadata).
    pub fn with_overrides(mut self, overrides: ChannelOverrides) -> Self {
        self.overrides = overrides;
        self
    }

    /// Replace the provider list.
    pub fn with_providers(mut self, providers: Vec<Arc<dyn ChannelProvider>>) -> Self {
        self.providers = providers;
        self
    }

    /// Append one provider (ignored when one with the same id is present).
    pub fn with_provider(mut self, provider: Arc<dyn ChannelProvider>) -> Self {
        if !self.providers.iter().any(|p| p.id() == provider.id()) {
            self.providers.push(provider);
        }
        self
    }

    /// Resample only `keys` from provider `id`. A provider without explicit
    /// keys resamples its whole catalog.
    pub fn with_provider_keys(mut self, id: impl Into<String>, keys: Vec<String>) -> Self {
        self.provider_keys.insert(id.into(), keys);
        self
    }

    /// Opt in to raw source channels (`raw:<name>` keys) through
    /// [`SourceChannels`]. An empty list removes the opt-in.
    pub fn with_source_channel_keys(self, keys: Vec<String>) -> Self {
        let id = SourceChannels.id().to_string();
        if keys.is_empty() {
            let mut options = self;
            options.provider_keys.remove(&id);
            options.providers.retain(|p| p.id() != id);
            return options;
        }
        self.with_provider(Arc::new(SourceChannels))
            .with_provider_keys(id, keys)
    }

    /// Track name or slug from recording metadata; the lap's GPS resolves
    /// the facility when absent.
    pub fn with_track_hint(mut self, hint: Option<String>) -> Self {
        self.track_hint = hint.filter(|h| !h.trim().is_empty());
        self
    }

    /// The video file to bind (a location's media path).
    pub fn with_video_path(mut self, path: Option<PathBuf>) -> Self {
        self.video_path = path;
        self
    }

    /// Hash a separate linked video against the catalog BLAKE3.
    pub fn verify_video_hash(mut self, verify: bool) -> Self {
        self.verify_video_hash = verify;
        self
    }

    pub fn overrides(&self) -> &ChannelOverrides {
        &self.overrides
    }
    pub fn providers(&self) -> &[Arc<dyn ChannelProvider>] {
        &self.providers
    }
    pub fn track_hint(&self) -> Option<&str> {
        self.track_hint.as_deref()
    }
    pub fn video_path(&self) -> Option<&Path> {
        self.video_path.as_deref()
    }
    pub fn is_verifying_video_hash(&self) -> bool {
        self.verify_video_hash
    }
    /// Explicit keys for one provider, if configured.
    pub fn provider_keys(&self, id: &str) -> Option<&[String]> {
        self.provider_keys.get(id).map(Vec::as_slice)
    }
    /// The opted-in raw source channel keys.
    pub fn source_channel_keys(&self) -> &[String] {
        self.provider_keys(SourceChannels.id()).unwrap_or(&[])
    }
}

/// One lap of one recording, normalized and ready for analysis. Cheap to
/// clone: every large part is shared.
#[derive(Clone)]
pub struct LoadedLap {
    recording: Arc<Recording>,
    lap: Lap,
    laps: Arc<[Lap]>,
    unified: Arc<UnifiedLap>,
    video: Option<VideoBinding>,
    layout: Option<Arc<TrackLayout>>,
    overlays: Arc<[OverlayGroup]>,
    strip: Arc<[LapStripCell]>,
    overrides: ChannelOverrides,
}

impl std::fmt::Debug for LoadedLap {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("LoadedLap")
            .field("recording", &self.recording.path())
            .field("lap", &self.lap.id)
            .field("samples", &self.unified.len())
            .field("layout", &self.layout.as_ref().map(|l| l.layout_id))
            .field("video", &self.video.as_ref().map(|v| &v.path))
            .finish_non_exhaustive()
    }
}

impl LoadedLap {
    pub fn recording(&self) -> &Arc<Recording> {
        &self.recording
    }
    /// The selected lap (classified).
    pub fn lap(&self) -> &Lap {
        &self.lap
    }
    pub fn lap_id(&self) -> i32 {
        self.lap.id
    }
    /// Every lap of the recording, classified, in recording order.
    pub fn laps(&self) -> &Arc<[Lap]> {
        &self.laps
    }
    /// The canonical 50 Hz lap.
    pub fn unified(&self) -> &Arc<UnifiedLap> {
        &self.unified
    }
    pub fn video(&self) -> Option<&VideoBinding> {
        self.video.as_ref()
    }
    /// The Track Atlas layout this lap resolved to.
    pub fn layout(&self) -> Option<&Arc<TrackLayout>> {
        self.layout.as_ref()
    }
    /// Plottable channels, one group per provider.
    pub fn overlays(&self) -> &[OverlayGroup] {
        &self.overlays
    }
    /// Lap-strip cells for every lap of the recording.
    pub fn strip(&self) -> &[LapStripCell] {
        &self.strip
    }
    /// The channel overrides the lap was unified with.
    pub fn overrides(&self) -> &ChannelOverrides {
        &self.overrides
    }
    /// The lap with `id` in this recording.
    pub fn lap_by_id(&self, id: i32) -> Option<&Lap> {
        self.laps.iter().find(|lap| lap.id == id)
    }
    /// The neighbouring lap in recording order (`offset` -1 or +1), for
    /// the out-of-lap viewport mask.
    pub fn neighbour_lap(&self, offset: isize) -> Option<&Lap> {
        let index = self.laps.iter().position(|lap| lap.id == self.lap.id)?;
        let target = index.checked_add_signed(offset)?;
        self.laps.get(target)
    }
}

fn resample_overlays(
    recording: &Recording,
    unified: &UnifiedLap,
    options: &LoadOptions,
    cancel: &AtomicBool,
) -> Result<Vec<OverlayGroup>, SessionError> {
    let mut groups = Vec::with_capacity(options.providers.len());
    for provider in &options.providers {
        check_cancel(cancel)?;
        let catalog = provider.catalog(recording);
        let keys: Vec<String> = match options.provider_keys(provider.id()) {
            Some(keys) => keys
                .iter()
                .filter(|key| catalog.iter().any(|info| &info.key == *key))
                .cloned()
                .collect(),
            None => catalog.into_iter().map(|info| info.key).collect(),
        };
        if keys.is_empty() {
            continue;
        }
        // Optional channels degrade gracefully: a provider that fails is
        // left out, never fabricated.
        match provider.resample(recording, unified, &keys) {
            Ok(group) => groups.push(group),
            Err(error) => log::warn!("channel provider {} failed: {error}", provider.id()),
        }
    }
    Ok(groups)
}

/// Load one lap for analysis: classify the recording's laps, unify the
/// selected one, resolve its Track Atlas layout, resample every provider's
/// channels, build the lap strip, and bind the onboard video. The cancel
/// flag is checked between stages.
pub fn load_lap(
    recording: Arc<Recording>,
    lap_id: i32,
    options: &LoadOptions,
    cancel: &AtomicBool,
) -> Result<LoadedLap, SessionError> {
    check_cancel(cancel)?;
    let mut laps = recording.detect_laps();
    classify_laps(&mut laps);
    let lap = laps
        .iter()
        .find(|lap| lap.id == lap_id)
        .cloned()
        .ok_or(SessionError::NoSuchLap(lap_id))?;

    check_cancel(cancel)?;
    let unified = recording.unify_lap(lap.start_time, lap.end_time, &options.overrides);
    if unified.is_empty() {
        return Err(SessionError::EmptyLap);
    }

    check_cancel(cancel)?;
    let layout = track::resolve_for_lap(options.track_hint(), &unified).map(Arc::new);

    check_cancel(cancel)?;
    let overlays = resample_overlays(&recording, &unified, options, cancel)?;

    let strip = lap_strip(&recording, &laps, &options.overrides, cancel)?;

    check_cancel(cancel)?;
    let video_path = options
        .video_path
        .clone()
        .or_else(|| default_video_path(&recording));
    let video = video_path.map(|path| {
        let (identity, file_index, warning) =
            verify_video_identity(&recording, &path, options.verify_video_hash);
        VideoBinding {
            path,
            clock: Arc::new(recording.video_clock().clone()),
            identity,
            file_index,
            warning,
        }
    });

    Ok(LoadedLap {
        recording,
        lap,
        laps: Arc::from(laps),
        unified: Arc::new(unified),
        video,
        layout,
        overlays: Arc::from(overlays),
        strip: Arc::from(strip),
        overrides: options.overrides.clone(),
    })
}

// ── analysis ────────────────────────────────────────────────────────

/// Which alignment the user asked for.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum StrategyRequest {
    /// GPS, then pre-corner dampers over a lap-time base, then lap %.
    #[default]
    Auto,
    /// This strategy when both laps support it, else the automatic choice.
    Prefer(Strategy),
}

impl StrategyRequest {
    fn requested(self) -> Option<Strategy> {
        match self {
            Self::Auto => None,
            Self::Prefer(strategy) => Some(strategy),
        }
    }
}

/// Where the corner zones of an analysis came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CornerSource {
    /// Track Atlas ranges mapped through the lap's station map.
    Atlas,
    /// The user's per-track override.
    User,
    /// Brake-zone detection (no atlas data applies).
    Generated,
    /// Track Atlas ranges mapped on the reference lap (whose GPS matches
    /// the centerline) and carried onto the primary through the one
    /// comparison map, because the primary's own GPS does not match.
    Reference,
    /// The atlas layout is known but the lap's GPS does not match it (and no
    /// matching reference carries the ranges): corners are hidden rather
    /// than fabricated.
    Unmatched,
}

/// Speeds through one corner, km/h: at the zone start, the minimum, and at
/// the zone end.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
#[non_exhaustive]
pub struct CornerSpeeds {
    pub entry: f64,
    pub apex: f64,
    pub exit: f64,
}

/// A driver-facing event inside a corner.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum MarkerKind {
    Brake,
    TurnIn,
    Apex,
    Throttle,
}

impl MarkerKind {
    pub const ALL: [Self; 4] = [Self::Brake, Self::TurnIn, Self::Apex, Self::Throttle];

    /// Upper-case ruler label.
    pub fn label(self) -> &'static str {
        match self {
            Self::Brake => "BRAKE",
            Self::TurnIn => "TURN-IN",
            Self::Apex => "APEX",
            Self::Throttle => "THROTTLE",
        }
    }
}

/// One corner event on the primary lap's fraction axis; the reference
/// lap's own event is mapped through the shared station map onto the same
/// axis, so both land on one zoomed viewport (plain data).
#[derive(Debug, Clone, Copy, PartialEq)]
#[non_exhaustive]
pub struct CornerMarker {
    pub kind: MarkerKind,
    /// NaN when the primary lap has no such event.
    pub fraction: f64,
    /// `None` without a reference; NaN when the reference has no event.
    pub reference_fraction: Option<f64>,
}

/// One corner of an analysis (plain data). Distances are metres, times
/// seconds, speeds km/h. Every delta is primary minus reference, NaN when
/// unknown: + time means the primary lost time; + metres means the
/// primary's event is later along the track.
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct CornerRow {
    pub zone: CornerZone,
    pub primary: CornerMetrics,
    /// The reference measured over its mapped zone.
    pub reference: Option<CornerMetrics>,
    pub speeds: CornerSpeeds,
    pub reference_speeds: Option<CornerSpeeds>,
    /// Primary time through the zone.
    pub time: f64,
    /// Reference time through the zone (from the delta when it exists).
    pub reference_time: f64,
    /// Time lost (+) or gained through the zone, from the cached delta.
    pub dt: f64,
    /// Time lost from the zone start to the apex.
    pub entry_dt: f64,
    /// Time lost from the apex to the zone end.
    pub exit_dt: f64,
    pub brake_point_delta: f64,
    pub lift_point_delta: f64,
    pub turn_in_delta: f64,
    pub apex_point_delta: f64,
    pub throttle_point_delta: f64,
    /// 0-100 corner score (50 = matched); NaN without a reference.
    pub score: f64,
    /// Checks from the registry, in registration order.
    pub notes: Vec<CornerNote>,
    pub markers: Vec<CornerMarker>,
}

/// One corner complex (plain data).
#[derive(Debug, Clone, PartialEq)]
#[non_exhaustive]
pub struct ComplexRow {
    pub zone: ComplexZone,
    /// Time lost (+) across the complex; NaN without a delta.
    pub dt: f64,
    /// Indices into [`Analysis::rows`] of the member corners present.
    pub members: Vec<usize>,
}

/// Where the lap's time went: the final delta split into the part lost
/// inside corner zones and the part lost between them (seconds, + the
/// primary is slower). `corners + straights == total` by construction.
#[derive(Debug, Clone, Copy, PartialEq)]
#[non_exhaustive]
pub struct TimeSplit {
    /// Δt accumulated inside the union of the corner zones (overlapping
    /// zones count once).
    pub corners: f64,
    /// Δt accumulated outside every corner zone.
    pub straights: f64,
    /// The final delta (the lap-time difference through the shared map).
    pub total: f64,
}

/// Split the cumulative `delta` (primary grid) at `zones` (fractions).
/// `None` without a delta.
fn time_split(delta: &[f64], zones: &[CornerZone]) -> Option<TimeSplit> {
    let total = *delta.last()?;
    if delta.len() < 2 || !total.is_finite() {
        return None;
    }
    let mut spans: Vec<(f64, f64)> = zones
        .iter()
        .map(|zone| (clamp(zone.start, 0.0, 1.0), clamp(zone.end, 0.0, 1.0)))
        .filter(|(start, end)| end > start)
        .collect();
    spans.sort_by(|a, b| a.0.total_cmp(&b.0));
    let mut merged: Vec<(f64, f64)> = Vec::with_capacity(spans.len());
    for (start, end) in spans {
        match merged.last_mut() {
            Some(last) if start <= last.1 => last.1 = last.1.max(end),
            _ => merged.push((start, end)),
        }
    }
    let corners: f64 = merged
        .iter()
        .map(|(start, end)| interpolate_fraction(delta, *end) - interpolate_fraction(delta, *start))
        .filter(|dt| dt.is_finite())
        .sum();
    Some(TimeSplit {
        corners,
        straights: total - corners,
        total,
    })
}

/// The primary lap, the optional reference lap, the one comparison map
/// between them, and everything derived from it: corner zones and rows,
/// complexes. Cheap to clone.
#[derive(Debug, Clone)]
pub struct Analysis {
    primary: LoadedLap,
    reference: Option<LoadedLap>,
    request: StrategyRequest,
    corner_override: Option<Vec<CornerZone>>,
    corner_source: CornerSource,
    comparison: Option<Arc<Comparison>>,
    available: Vec<Strategy>,
    corners: Arc<[CornerZone]>,
    complexes: Arc<[ComplexZone]>,
    rows: Arc<[CornerRow]>,
    complex_rows: Arc<[ComplexRow]>,
    time_split: Option<TimeSplit>,
}

fn sorted_zones(mut zones: Vec<CornerZone>) -> Vec<CornerZone> {
    zones.sort_by(|a, b| a.start.total_cmp(&b.start));
    zones
}

/// Corner zones and complexes for a primary lap.
fn corner_zones(
    primary: &LoadedLap,
    corner_override: Option<&[CornerZone]>,
) -> (Vec<CornerZone>, Vec<ComplexZone>, CornerSource) {
    let mapper = primary
        .layout()
        .and_then(|layout| StationMapper::new(primary.unified(), layout).map(|m| (layout, m)));
    let complexes = mapper
        .as_ref()
        .map(|(layout, mapper)| atlas_complex_zones(layout, mapper))
        .unwrap_or_default();
    if let Some(zones) = corner_override {
        let zones = zones
            .iter()
            .cloned()
            .map(|mut zone| {
                zone.source = ZoneSource::User;
                zone
            })
            .collect();
        return (sorted_zones(zones), complexes, CornerSource::User);
    }
    match (primary.layout(), mapper) {
        (Some(_), None) => (Vec::new(), Vec::new(), CornerSource::Unmatched),
        (Some(_), Some((layout, mapper))) => {
            let zones = atlas_corner_zones(layout, &mapper);
            if zones.is_empty() {
                (
                    auto_generate_corners(primary.unified()),
                    complexes,
                    CornerSource::Generated,
                )
            } else {
                (zones, complexes, CornerSource::Atlas)
            }
        }
        (None, _) => (
            auto_generate_corners(primary.unified()),
            Vec::new(),
            CornerSource::Generated,
        ),
    }
}

/// Primary lap fraction at a primary-lap distance (metres).
fn fraction_at_distance(lap: &UnifiedLap, metres: f64) -> f64 {
    if !metres.is_finite() {
        return f64::NAN;
    }
    invert_fraction(&lap.distance, metres)
}

struct RowInputs<'a> {
    primary: &'a UnifiedLap,
    reference: Option<&'a UnifiedLap>,
    comparison: Option<&'a Comparison>,
}

impl RowInputs<'_> {
    fn delta_at(&self, fraction: f64) -> f64 {
        match self.comparison {
            Some(comparison) if !comparison.delta().is_empty() => {
                interpolate_fraction(comparison.delta(), fraction)
            }
            _ => f64::NAN,
        }
    }

    fn has_delta(&self) -> bool {
        self.comparison.is_some_and(|c| !c.delta().is_empty())
    }

    fn compare_fraction(&self, primary_fraction: f64) -> f64 {
        self.comparison
            .map(|c| c.compare_fraction_for_primary_fraction(primary_fraction))
            .unwrap_or(primary_fraction)
    }

    fn primary_fraction(&self, compare_fraction: f64) -> f64 {
        self.comparison
            .map(|c| c.primary_fraction_for_compare_fraction(compare_fraction))
            .unwrap_or(compare_fraction)
    }

    /// A reference event (metres from the reference zone start) on the
    /// primary lap's fraction axis, through the shared map.
    fn reference_event_fraction(&self, metrics: &CornerMetrics, point: f64) -> f64 {
        let Some(reference) = self.reference else {
            return f64::NAN;
        };
        if !point.is_finite() || reference.distance.len() < 2 {
            return f64::NAN;
        }
        let compare = invert_fraction(&reference.distance, metrics.start_distance + point);
        self.primary_fraction(compare)
    }

    fn row(&self, zone: &CornerZone) -> CornerRow {
        let primary = self.primary;
        let speeds_of = |lap: &UnifiedLap, start: f64, end: f64, apex: f64| CornerSpeeds {
            entry: interpolate_fraction(&lap.speed, start),
            apex,
            exit: interpolate_fraction(&lap.speed, end),
        };
        let time_of = |lap: &UnifiedLap, start: f64, end: f64| {
            interpolate_fraction(&lap.time, end) - interpolate_fraction(&lap.time, start)
        };

        let mut primary_metrics =
            crate::corners::measure_corner(primary, zone.start, zone.end, true);
        let mut context_reference = None;
        let mut reference_metrics = None;
        let (mut reference_start, mut reference_end) = (zone.start, zone.end);
        if let Some(reference) = self.reference.filter(|r| r.distance.len() >= 2) {
            reference_start = self.compare_fraction(zone.start);
            reference_end = self.compare_fraction(zone.end);
            let mut metrics =
                crate::corners::measure_corner(reference, reference_start, reference_end, true);
            // One turn-in definition on both laps: lateral G on only one of
            // them would invent a 20-30 m "late" the steering does not show.
            if primary_metrics.has_lateral_g != metrics.has_lateral_g {
                primary_metrics =
                    crate::corners::measure_corner(primary, zone.start, zone.end, false);
                metrics = crate::corners::measure_corner(
                    reference,
                    reference_start,
                    reference_end,
                    false,
                );
            }
            context_reference = Some(reference);
            reference_metrics = Some(metrics);
        }

        let last = primary.len().saturating_sub(1).max(1) as f64;
        let apex_fraction = if primary_metrics.valid {
            f64::from(primary_metrics.apex_index) / last
        } else {
            (zone.start + zone.end) * 0.5
        };
        let speeds = speeds_of(primary, zone.start, zone.end, primary_metrics.apex_speed);
        let time = time_of(primary, zone.start, zone.end);

        let point_delta = |primary_point: f64, reference_point: f64| -> f64 {
            let Some(reference) = reference_metrics.as_ref() else {
                return f64::NAN;
            };
            if !primary_point.is_finite() || !reference_point.is_finite() {
                return f64::NAN;
            }
            let fraction = self.reference_event_fraction(reference, reference_point);
            primary_metrics.start_distance + primary_point
                - interpolate_fraction(&primary.distance, fraction)
        };

        let mut row = CornerRow {
            zone: zone.clone(),
            primary: primary_metrics.clone(),
            reference: reference_metrics.clone(),
            speeds,
            reference_speeds: None,
            time,
            reference_time: f64::NAN,
            dt: f64::NAN,
            entry_dt: f64::NAN,
            exit_dt: f64::NAN,
            brake_point_delta: f64::NAN,
            lift_point_delta: f64::NAN,
            turn_in_delta: f64::NAN,
            apex_point_delta: f64::NAN,
            throttle_point_delta: f64::NAN,
            score: f64::NAN,
            notes: Vec::new(),
            markers: Vec::new(),
        };

        let mut context = CornerContext::new(primary, primary_metrics.clone());
        if let (Some(reference), Some(metrics)) = (context_reference, reference_metrics.as_ref()) {
            let reference_speeds = speeds_of(
                reference,
                reference_start,
                reference_end,
                metrics.apex_speed,
            );
            let reference_raw_time = time_of(reference, reference_start, reference_end);
            // Time comes from the one cached delta whenever it exists, so the
            // table, the delta lane and the cursor readout cannot disagree.
            let dt = if self.has_delta() {
                self.delta_at(zone.end) - self.delta_at(zone.start)
            } else {
                time - reference_raw_time
            };
            row.reference_time = if self.has_delta() {
                time - dt
            } else {
                reference_raw_time
            };
            row.dt = dt;
            row.entry_dt = self.delta_at(apex_fraction) - self.delta_at(zone.start);
            row.exit_dt = self.delta_at(zone.end) - self.delta_at(apex_fraction);
            row.brake_point_delta = point_delta(primary_metrics.brake_point, metrics.brake_point);
            row.lift_point_delta = point_delta(primary_metrics.lift_point, metrics.lift_point);
            row.turn_in_delta = point_delta(primary_metrics.turn_in_point, metrics.turn_in_point);
            row.apex_point_delta = point_delta(primary_metrics.apex_point, metrics.apex_point);
            row.throttle_point_delta =
                point_delta(primary_metrics.throttle_point, metrics.throttle_point);
            let exit_delta = speeds.exit - reference_speeds.exit;
            let apex_delta = speeds.apex - reference_speeds.apex;
            row.score = clamp(
                50.0 - dt * 40.0 + exit_delta * 0.8 + apex_delta * 0.35,
                0.0,
                100.0,
            );
            row.reference_speeds = Some(reference_speeds);

            context.reference = Some(reference);
            context.reference_metrics = metrics.clone();
            context.time_delta = dt;
            context.entry_time_delta = row.entry_dt;
            context.exit_time_delta = row.exit_dt;
            context.brake_point_delta = row.brake_point_delta;
            context.turn_in_delta = row.turn_in_delta;
            context.throttle_point_delta = row.throttle_point_delta;
        }

        row.notes = checks::run(&context);
        if row.notes.is_empty() && context.reference.is_some() {
            row.notes.push(CornerNote {
                id: "matched",
                text: "Closely matched".to_string(),
                severity: NoteSeverity::Info,
            });
        }

        let point_of = |metrics: &CornerMetrics, kind: MarkerKind| match kind {
            MarkerKind::Brake => metrics.brake_point,
            MarkerKind::TurnIn => metrics.turn_in_point,
            MarkerKind::Apex => metrics.apex_point,
            MarkerKind::Throttle => metrics.throttle_point,
        };
        if primary_metrics.valid {
            row.markers = MarkerKind::ALL
                .iter()
                .map(|&kind| CornerMarker {
                    kind,
                    fraction: fraction_at_distance(
                        primary,
                        primary_metrics.start_distance + point_of(&primary_metrics, kind),
                    ),
                    reference_fraction: reference_metrics.as_ref().map(|metrics| {
                        self.reference_event_fraction(metrics, point_of(metrics, kind))
                    }),
                })
                .collect();
        }
        row
    }
}

fn complex_rows(
    complexes: &[ComplexZone],
    rows: &[CornerRow],
    inputs: &RowInputs<'_>,
) -> Vec<ComplexRow> {
    complexes
        .iter()
        .map(|zone| ComplexRow {
            zone: zone.clone(),
            dt: inputs.delta_at(zone.end) - inputs.delta_at(zone.start),
            members: zone
                .members
                .iter()
                .filter_map(|id| rows.iter().position(|row| &row.zone.id == id))
                .collect(),
        })
        .collect()
}

/// The reference lap's own atlas ranges carried onto the primary lap
/// through `comparison` (reference fraction -> primary fraction). `None`
/// unless the reference maps the atlas through its own GPS.
fn transferred_zones(
    reference: &LoadedLap,
    comparison: &Comparison,
) -> Option<(Vec<CornerZone>, Vec<ComplexZone>)> {
    let (zones, complexes, source) = corner_zones(reference, None);
    if source != CornerSource::Atlas || zones.is_empty() {
        return None;
    }
    let map = |fraction: f64| comparison.primary_fraction_for_compare_fraction(fraction);
    let zones = zones
        .into_iter()
        .map(|zone| CornerZone {
            start: map(zone.start),
            end: map(zone.end),
            ..zone
        })
        .filter(|zone| zone.end > zone.start)
        .collect();
    let complexes = complexes
        .into_iter()
        .map(|zone| ComplexZone {
            start: map(zone.start),
            end: map(zone.end),
            ..zone
        })
        .filter(|zone| zone.end > zone.start)
        .collect();
    Some((sorted_zones(zones), complexes))
}

impl Analysis {
    /// Build the analysis of `primary` (optionally against `reference`).
    ///
    /// Corner zones come from `corner_override` when given, else Track
    /// Atlas through the primary lap's station map, else (when the primary's
    /// GPS misses the centerline) the reference's atlas ranges carried over
    /// through the comparison map, else brake-zone generation. The comparison uses the requested strategy when both
    /// laps support it (see [`comparison::effective_strategy`]); the manual
    /// offset only applies to manual damper alignment. Never call from a
    /// paint, cursor or playback path: this is the static per-pair work.
    pub fn build(
        primary: &LoadedLap,
        reference: Option<&LoadedLap>,
        request: StrategyRequest,
        manual_offset: f64,
        corner_override: Option<Vec<CornerZone>>,
        cancel: &AtomicBool,
    ) -> Result<Self, SessionError> {
        check_cancel(cancel)?;
        let (mut corners, mut complexes, mut source) =
            corner_zones(primary, corner_override.as_deref());
        check_cancel(cancel)?;
        let mut available = Vec::new();
        let mut comparison = None;
        if let Some(reference) = reference {
            let requested = request.requested();
            let (p, r) = (primary.unified(), reference.unified());
            let mut provisional = None;
            if source == CornerSource::Unmatched {
                // The primary's GPS misses the centerline: carry the
                // reference's atlas ranges over through a map built without
                // corners (only pre-corner dampers need corner starts).
                let strategy = comparison::effective_strategy(requested, p, r, false);
                let map =
                    Comparison::new(p.clone(), r.clone(), strategy, Vec::new(), manual_offset);
                if let Some((zones, groups)) = transferred_zones(reference, &map) {
                    (corners, complexes, source) = (zones, groups, CornerSource::Reference);
                }
                provisional = Some(map);
            }
            let has_corners = !corners.is_empty();
            available = comparison::available_strategies(p, r, has_corners);
            let strategy = comparison::effective_strategy(requested, p, r, has_corners);
            let map = match provisional {
                Some(map) if map.strategy() == strategy => map,
                _ => Comparison::new(
                    p.clone(),
                    r.clone(),
                    strategy,
                    corners.iter().map(|zone| zone.start).collect(),
                    manual_offset,
                ),
            };
            if source == CornerSource::Reference
                && let Some((zones, groups)) = transferred_zones(reference, &map)
            {
                (corners, complexes) = (zones, groups);
            }
            comparison = Some(Arc::new(map));
        }
        Self::assemble(
            primary.clone(),
            reference.cloned(),
            request,
            corner_override,
            (corners, complexes, source),
            comparison,
            available,
            cancel,
        )
    }

    fn assemble(
        primary: LoadedLap,
        reference: Option<LoadedLap>,
        request: StrategyRequest,
        corner_override: Option<Vec<CornerZone>>,
        zones: (Vec<CornerZone>, Vec<ComplexZone>, CornerSource),
        comparison: Option<Arc<Comparison>>,
        available: Vec<Strategy>,
        cancel: &AtomicBool,
    ) -> Result<Self, SessionError> {
        let (corners, complexes, corner_source) = zones;
        check_cancel(cancel)?;
        let inputs = RowInputs {
            primary: primary.unified(),
            reference: reference.as_ref().map(|r| r.unified().as_ref()),
            comparison: comparison.as_deref(),
        };
        let mut rows = Vec::with_capacity(corners.len());
        for zone in &corners {
            check_cancel(cancel)?;
            rows.push(inputs.row(zone));
        }
        let complex_rows = complex_rows(&complexes, &rows, &inputs);
        let time_split = comparison
            .as_deref()
            .and_then(|comparison| time_split(comparison.delta(), &corners));
        Ok(Self {
            primary,
            reference,
            request,
            corner_override,
            corner_source,
            comparison,
            available,
            corners: Arc::from(corners),
            complexes: Arc::from(complexes),
            rows: Arc::from(rows),
            complex_rows: Arc::from(complex_rows),
            time_split,
        })
    }

    /// The same pair with roles exchanged: the reference becomes primary,
    /// the manual offset inverts ([`Comparison::swapped`]) and corners are
    /// re-mapped onto the new primary. Cursor and viewport fractions are
    /// the caller's and stay where they are. Without a reference this is a
    /// clone.
    pub fn swapped(&self, cancel: &AtomicBool) -> Result<Self, SessionError> {
        let Some(reference) = self.reference.clone() else {
            return Ok(self.clone());
        };
        check_cancel(cancel)?;
        let primary = reference;
        let reference = self.primary.clone();
        let mut zones = corner_zones(&primary, self.corner_override.as_deref());
        let comparison = self.comparison.as_ref().map(|comparison| {
            // Corner starts only steer pre-corner damper alignment; a
            // primary without its own atlas match keeps the old ones mapped.
            let starts = if zones.2 == CornerSource::Unmatched {
                self.corners
                    .iter()
                    .map(|zone| comparison.compare_fraction_for_primary_fraction(zone.start))
                    .collect()
            } else {
                zones.0.iter().map(|zone| zone.start).collect()
            };
            Arc::new(comparison.swapped(starts))
        });
        if zones.2 == CornerSource::Unmatched
            && let Some(map) = comparison.as_deref()
            && let Some((corners, complexes)) = transferred_zones(&reference, map)
        {
            zones = (corners, complexes, CornerSource::Reference);
        }
        let available = comparison::available_strategies(
            primary.unified(),
            reference.unified(),
            !zones.0.is_empty(),
        );
        Self::assemble(
            primary,
            Some(reference),
            self.request,
            self.corner_override.clone(),
            zones,
            comparison,
            available,
            cancel,
        )
    }

    /// The same analysis with a new manual damper offset (primary lap
    /// fraction). Only the delta and the rows are rebuilt; a no-op for
    /// other strategies.
    pub fn with_manual_offset(
        &self,
        offset: f64,
        cancel: &AtomicBool,
    ) -> Result<Self, SessionError> {
        let Some(comparison) = self.comparison.as_ref() else {
            return Ok(self.clone());
        };
        if comparison.strategy() != Strategy::ManualDampers {
            return Ok(self.clone());
        }
        let mut updated = Comparison::clone(comparison);
        updated.set_manual_offset(offset);
        let mut zones = (
            self.corners.to_vec(),
            self.complexes.to_vec(),
            self.corner_source,
        );
        // Carried-over ranges follow the map they were carried through.
        if self.corner_source == CornerSource::Reference
            && let Some(reference) = self.reference.as_ref()
            && let Some((corners, complexes)) = transferred_zones(reference, &updated)
        {
            zones = (corners, complexes, CornerSource::Reference);
        }
        Self::assemble(
            self.primary.clone(),
            self.reference.clone(),
            self.request,
            self.corner_override.clone(),
            zones,
            Some(Arc::new(updated)),
            self.available.clone(),
            cancel,
        )
    }

    pub fn primary(&self) -> &LoadedLap {
        &self.primary
    }
    pub fn reference(&self) -> Option<&LoadedLap> {
        self.reference.as_ref()
    }
    /// The one primary -> reference map (traces, delta, cursor, video).
    pub fn comparison(&self) -> Option<&Arc<Comparison>> {
        self.comparison.as_ref()
    }
    /// What the user asked for.
    pub fn request(&self) -> StrategyRequest {
        self.request
    }
    /// The strategy in effect, when comparing.
    pub fn strategy(&self) -> Option<Strategy> {
        self.comparison.as_ref().map(|c| c.strategy())
    }
    /// Strategies both laps support, in menu order.
    pub fn available_strategies(&self) -> &[Strategy] {
        &self.available
    }
    /// Primary lap time minus reference lap time (s), from the laps' own
    /// times: exact whatever the alignment confidence, since no station
    /// map is involved. Positive means the primary is slower. `None`
    /// without a reference or when either time is unknown.
    pub fn lap_time_delta(&self) -> Option<f64> {
        let reference = self.reference.as_ref()?;
        let delta = (self.primary.lap().time_ms - reference.lap().time_ms) / 1000.0;
        delta.is_finite().then_some(delta)
    }
    /// Cumulative delta (s) on the primary grid; empty without one.
    pub fn delta(&self) -> &[f64] {
        self.comparison.as_ref().map(|c| c.delta()).unwrap_or(&[])
    }
    pub fn corner_source(&self) -> CornerSource {
        self.corner_source
    }
    /// True when the zones are the user's override.
    pub fn has_corner_override(&self) -> bool {
        self.corner_override.is_some()
    }
    /// Corner zones on the primary lap, sorted by start.
    pub fn corners(&self) -> &[CornerZone] {
        &self.corners
    }
    /// Corner complexes on the primary lap, sorted by start.
    pub fn complexes(&self) -> &[ComplexZone] {
        &self.complexes
    }
    /// One row per corner, in corner order.
    pub fn rows(&self) -> &[CornerRow] {
        &self.rows
    }
    pub fn complex_rows(&self) -> &[ComplexRow] {
        &self.complex_rows
    }
    /// Time lost (s/m) at every primary sample, from the one delta
    /// ([`Comparison::loss_rate`]); empty without a delta.
    pub fn loss_rate(&self) -> &[f64] {
        self.comparison
            .as_ref()
            .map(|c| c.loss_rate())
            .unwrap_or(&[])
    }
    /// The final delta split into corners and straights; `None` without a
    /// delta.
    pub fn time_split(&self) -> Option<TimeSplit> {
        self.time_split
    }
    /// The row for a corner id.
    pub fn row(&self, corner_id: &str) -> Option<&CornerRow> {
        self.rows.iter().find(|row| row.zone.id == corner_id)
    }
}
