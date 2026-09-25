//! One library scan: discover recordings in every enabled location,
//! summarize them through the index cache, resolve their metadata and
//! build the tree. Blocking and cancellable; run it on a background
//! executor and apply the result on the UI thread.

use crate::catalog::{CatalogRecord, LibrarySnapshot};
use crate::config::Config;
use crate::index_cache::{CacheOutcome, IndexCache};
use crate::location::{Cancel, DiscoveredFile, FolderLocation, Location, LocationId};
use crate::metadata::{MetadataSources, effective_metadata};
use crate::summary::RecordingSummary;
use crate::track_yml::FolderMetadataCache;
use jiff::Timestamp;
use jiff::tz::TimeZone;
use std::collections::HashSet;
use std::io;
use std::path::PathBuf;
use std::sync::Arc;

/// The enabled folder locations of a configuration, in library order.
pub fn locations_from_config(config: &Config) -> Vec<Arc<dyn Location>> {
    config
        .folder_locations()
        .filter(|folder| folder.is_enabled())
        .filter_map(FolderLocation::from_config)
        .map(|location| Arc::new(location) as Arc<dyn Location>)
        .collect()
}

/// Progress of a running scan.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[non_exhaustive]
pub struct ScanProgress {
    /// Recordings found so far.
    pub discovered: usize,
    /// Recordings summarized so far.
    pub summarized: usize,
}

/// A recording that could not be summarized (plain data).
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub struct ScanFailure {
    pub path: PathBuf,
    pub message: String,
}

/// A location that could not be scanned (plain data).
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub struct LocationFailure {
    pub location: LocationId,
    pub message: String,
}

/// The result of a scan.
#[derive(Debug, Clone, Default)]
#[non_exhaustive]
pub struct ScanOutcome {
    pub snapshot: LibrarySnapshot,
    pub cache_hits: usize,
    pub cache_misses: usize,
    pub failures: Vec<ScanFailure>,
    pub location_failures: Vec<LocationFailure>,
}

/// The scan was cancelled.
#[derive(Debug, Clone, Copy, PartialEq, Eq, thiserror::Error)]
#[error("library scan cancelled")]
pub struct ScanCancelled;

/// The event-date key used for `track_assignments`: the recording's clock
/// in its declared (else local) timezone.
fn event_date_key(summary: &RecordingSummary) -> Option<String> {
    if summary.utc_start_ns <= 0 {
        return None;
    }
    let zone = TimeZone::get(&summary.timezone).unwrap_or_else(|_| TimeZone::system());
    let timestamp = Timestamp::from_nanosecond(i128::from(summary.utc_start_ns)).ok()?;
    Some(timestamp.to_zoned(zone).date().to_string())
}

/// Scan `locations`: prune other cache generations, discover, summarize
/// (cache hit or index open), resolve metadata, build the snapshot. A
/// location or file that fails is reported and skipped; cancellation stops
/// between files.
pub fn scan_library(
    locations: &[Arc<dyn Location>],
    cache: &IndexCache,
    config: &Config,
    cancel: &Cancel,
    progress: &mut dyn FnMut(ScanProgress),
) -> Result<ScanOutcome, ScanCancelled> {
    if let Err(error) = cache.prune_other_generations() {
        log::warn!("index cache prune: {error}");
    }
    let mut outcome = ScanOutcome::default();
    let mut state = ScanProgress::default();

    let mut discovered: Vec<(Arc<dyn Location>, DiscoveredFile)> = Vec::new();
    let mut seen = HashSet::new();
    for location in locations {
        let mut found = Vec::new();
        let result = location.scan(cancel, &mut |file| found.push(file));
        if cancel.is_cancelled() {
            return Err(ScanCancelled);
        }
        if let Err(error) = result {
            outcome.location_failures.push(LocationFailure {
                location: location.id().clone(),
                message: error.to_string(),
            });
            if error.kind() == io::ErrorKind::Interrupted {
                return Err(ScanCancelled);
            }
        }
        for file in found {
            // Overlapping locations list a file once.
            let identity = file.identity();
            if seen.insert((identity.dev, identity.ino, file.path().to_path_buf())) {
                discovered.push((location.clone(), file));
            }
        }
        state.discovered = discovered.len();
        progress(state);
    }

    let mut folders = FolderMetadataCache::new();
    let mut records = Vec::with_capacity(discovered.len());
    for (location, file) in discovered {
        if cancel.is_cancelled() {
            return Err(ScanCancelled);
        }
        match cache.summarize(location.as_ref(), &file) {
            Ok((summary, hit)) => {
                match hit {
                    CacheOutcome::Hit => outcome.cache_hits += 1,
                    CacheOutcome::Miss => outcome.cache_misses += 1,
                }
                let folder = file
                    .path()
                    .parent()
                    .map(|dir| folders.metadata(dir))
                    .unwrap_or_default();
                let event_date = event_date_key(&summary);
                let metadata = effective_metadata(
                    file.path(),
                    MetadataSources::new(&folder, config)
                        .with_summary(Some(&summary))
                        .with_event_date(event_date.as_deref()),
                );
                records.push(CatalogRecord::new(file, summary, metadata));
            }
            Err(error) => outcome.failures.push(ScanFailure {
                path: file.path().to_path_buf(),
                message: error.to_string(),
            }),
        }
        state.summarized += 1;
        progress(state);
    }
    if cancel.is_cancelled() {
        return Err(ScanCancelled);
    }
    outcome.snapshot = LibrarySnapshot::build(records);
    Ok(outcome)
}
