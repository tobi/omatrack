//! The session library: configured locations, the scan pipeline and the
//! Track > Date > Session > Laps snapshot with its search and facets.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use gpui_kit::{
    AppContext as _, Context, Entity, EventEmitter, PathPromptOptions, SharedString, Task,
};
use omatrack_library::location::{DiscoveredFile, Location};
use omatrack_library::{
    Cancel, Facets, IndexCache, LibraryFilter, LibrarySnapshot, ScanOutcome, ScanProgress,
    SessionNode, locations_from_config, scan_library,
};

use crate::state::jobs::Jobs;
use crate::state::preferences::Preferences;

/// Where the library is in its scan cycle. A scan cannot fail as a
/// whole: unreadable recordings and locations are reported by
/// [`LibraryEvent::ScanFinished`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ScanStatus {
    /// No scan is running.
    Idle,
    Scanning {
        discovered: usize,
        summarized: usize,
    },
}

/// What changed in the [`Library`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LibraryEvent {
    /// A new snapshot (scan result or filter change).
    SnapshotChanged,
    /// A scan finished; recordings or locations that could not be read.
    ScanFinished {
        unreadable: usize,
        location_errors: Vec<String>,
    },
}

/// Everything needed to open one recording of the snapshot.
#[derive(Clone)]
#[non_exhaustive]
pub struct RecordingSource {
    pub location: Arc<dyn Location>,
    pub file: DiscoveredFile,
    pub node: SessionNode,
}

/// The library entity. Scans run on the background executor with one task
/// slot (a new scan cancels the running one); results are applied here and
/// the whole snapshot is swapped. Selection elsewhere is keyed by the
/// snapshot's stable ids, so a rescan never resets it.
pub struct Library {
    preferences: Entity<Preferences>,
    jobs: Entity<Jobs>,
    cache: IndexCache,
    default_dir: Option<PathBuf>,
    locations: Vec<Arc<dyn Location>>,
    snapshot: Arc<LibrarySnapshot>,
    filtered: Arc<LibrarySnapshot>,
    facets: Facets,
    query: String,
    track: Option<String>,
    year: Option<i16>,
    driver: Option<String>,
    status: ScanStatus,
    scanned: bool,
    scan_generation: u64,
    scan_cancel: Option<Cancel>,
    scan_task: Option<Task<()>>,
    progress_task: Option<Task<()>>,
    prompt_task: Option<Task<()>>,
}

impl EventEmitter<LibraryEvent> for Library {}

impl Library {
    /// `default_dir` becomes the only location on a fresh install (no
    /// `locations` key); `None` leaves a fresh library empty.
    pub fn new(
        preferences: Entity<Preferences>,
        jobs: Entity<Jobs>,
        default_dir: Option<PathBuf>,
        cx: &mut Context<Self>,
    ) -> Self {
        let cache = IndexCache::new(preferences.read(cx).paths().index_cache_root());
        let locations = locations_from_config(preferences.read(cx).config());
        Self {
            preferences,
            jobs,
            cache,
            default_dir,
            locations,
            snapshot: Arc::default(),
            filtered: Arc::default(),
            facets: Facets::default(),
            query: String::new(),
            track: None,
            year: None,
            driver: None,
            status: ScanStatus::Idle,
            scanned: false,
            scan_generation: 0,
            scan_cancel: None,
            scan_task: None,
            progress_task: None,
            prompt_task: None,
        }
    }

    /// The whole library.
    pub fn snapshot(&self) -> &Arc<LibrarySnapshot> {
        &self.snapshot
    }

    /// The library restricted to the search text and facets.
    pub fn filtered(&self) -> &Arc<LibrarySnapshot> {
        &self.filtered
    }

    pub fn facets(&self) -> &Facets {
        &self.facets
    }

    pub fn status(&self) -> &ScanStatus {
        &self.status
    }

    pub fn is_scanning(&self) -> bool {
        matches!(self.status, ScanStatus::Scanning { .. })
    }

    /// True once a scan has completed (the snapshot is not just "not yet").
    pub fn has_scanned(&self) -> bool {
        self.scanned
    }

    pub fn has_locations(&self) -> bool {
        !self.locations.is_empty()
    }

    pub fn query(&self) -> &str {
        &self.query
    }

    pub fn track_facet(&self) -> Option<&str> {
        self.track.as_deref()
    }

    pub fn year_facet(&self) -> Option<i16> {
        self.year
    }

    pub fn driver_facet(&self) -> Option<&str> {
        self.driver.as_deref()
    }

    pub fn has_filter(&self) -> bool {
        !self.filter().is_empty()
    }

    fn filter(&self) -> LibraryFilter {
        LibraryFilter::new()
            .query(self.query.clone())
            .track(self.track.clone())
            .year(self.year)
            .driver(self.driver.clone())
    }

    fn refilter(&mut self, cx: &mut Context<Self>) {
        self.filtered = if self.filter().is_empty() {
            self.snapshot.clone()
        } else {
            Arc::new(self.snapshot.filtered(&self.filter()))
        };
        cx.emit(LibraryEvent::SnapshotChanged);
        cx.notify();
    }

    pub fn set_query(&mut self, query: impl Into<String>, cx: &mut Context<Self>) {
        let query = query.into();
        if query != self.query {
            self.query = query;
            self.refilter(cx);
        }
    }

    pub fn set_track_facet(&mut self, slug: Option<String>, cx: &mut Context<Self>) {
        if slug != self.track {
            self.track = slug;
            self.refilter(cx);
        }
    }

    pub fn set_year_facet(&mut self, year: Option<i16>, cx: &mut Context<Self>) {
        if year != self.year {
            self.year = year;
            self.refilter(cx);
        }
    }

    pub fn set_driver_facet(&mut self, driver: Option<String>, cx: &mut Context<Self>) {
        if driver != self.driver {
            self.driver = driver;
            self.refilter(cx);
        }
    }

    /// Clear the search text and every facet.
    pub fn clear_filter(&mut self, cx: &mut Context<Self>) {
        if self.has_filter() {
            self.query.clear();
            self.track = None;
            self.year = None;
            self.driver = None;
            self.refilter(cx);
        }
    }

    /// Replace the snapshot (a scan result, or a prepared catalog in tests).
    pub fn set_snapshot(&mut self, snapshot: LibrarySnapshot, cx: &mut Context<Self>) {
        self.facets = snapshot.facets();
        self.snapshot = Arc::new(snapshot);
        self.scanned = true;
        self.refilter(cx);
    }

    /// The recording behind a session id, with the location that opens it.
    pub fn source(&self, session_id: &str) -> Option<RecordingSource> {
        let node = self.snapshot.session(session_id)?;
        let location = self
            .locations
            .iter()
            .find(|location| location.id() == node.file.location())?
            .clone();
        Some(RecordingSource {
            location,
            file: node.file.clone(),
            node: node.clone(),
        })
    }

    /// Scan every enabled location again. A running scan is cancelled; the
    /// current snapshot stays usable until the new one arrives.
    pub fn rescan(&mut self, cx: &mut Context<Self>) {
        if let Some(cancel) = self.scan_cancel.take() {
            cancel.cancel();
        }
        self.scan_task = None;
        self.progress_task = None;

        let default_dir = self.default_dir.clone();
        if let Some(dir) = default_dir.as_deref() {
            self.preferences.update(cx, |preferences, cx| {
                preferences.update(cx, |config| {
                    config.ensure_default_location(dir);
                });
            });
        }
        let config = self.preferences.read(cx).config().clone();
        self.locations = locations_from_config(&config);
        let locations = self.locations.clone();
        let cache = self.cache.clone();
        let cancel = Cancel::new();
        self.scan_cancel = Some(cancel.clone());
        self.scan_generation += 1;
        let generation = self.scan_generation;
        self.status = ScanStatus::Scanning {
            discovered: 0,
            summarized: 0,
        };
        let job = self
            .jobs
            .update(cx, |jobs, cx| jobs.start("Scanning library", cx));
        cx.notify();

        let job_id = job.id();
        let (progress_tx, progress_rx) = async_channel::unbounded::<ScanProgress>();
        let scan = cx.background_spawn(async move {
            if let Some(dir) = default_dir
                && let Err(error) = std::fs::create_dir_all(&dir)
            {
                log::warn!("cannot create {}: {error}", dir.display());
            }
            scan_library(&locations, &cache, &config, &cancel, &mut |progress| {
                let _ = progress_tx.try_send(progress);
            })
        });

        self.progress_task = Some(cx.spawn(async move |this, cx| {
            while let Ok(progress) = progress_rx.recv().await {
                let Ok(()) = this.update(cx, |this, cx| {
                    // Progress can still be queued when the scan's result
                    // lands (two tasks, no ordering): never un-finish it.
                    if this.scan_generation != generation || !this.is_scanning() {
                        return;
                    }
                    this.status = ScanStatus::Scanning {
                        discovered: progress.discovered,
                        summarized: progress.summarized,
                    };
                    let jobs = this.jobs.clone();
                    jobs.update(cx, |jobs, cx| {
                        jobs.set_progress(job_id, progress.summarized, progress.discovered, cx)
                    });
                    cx.notify();
                }) else {
                    break;
                };
            }
        }));

        let jobs = self.jobs.clone();
        self.scan_task = Some(cx.spawn(async move |this, cx| {
            let result = scan.await;
            jobs.update(cx, |jobs, cx| jobs.finish(job, cx));
            let _ = this.update(cx, |this, cx| {
                if this.scan_generation != generation {
                    return;
                }
                this.scan_cancel = None;
                this.progress_task = None;
                match result {
                    Ok(outcome) => this.apply_outcome(outcome, cx),
                    // Cancelled by a newer scan or by shutdown: keep the
                    // previous snapshot.
                    Err(_) => this.status = ScanStatus::Idle,
                }
                cx.notify();
            });
        }));
    }

    fn apply_outcome(&mut self, outcome: ScanOutcome, cx: &mut Context<Self>) {
        self.status = ScanStatus::Idle;
        let unreadable = outcome.failures.len();
        let location_errors = outcome
            .location_failures
            .iter()
            .map(|failure| format!("{}: {}", failure.location, failure.message))
            .collect::<Vec<_>>();
        for failure in &outcome.failures {
            log::warn!("{}: {}", failure.path.display(), failure.message);
        }
        self.set_snapshot(outcome.snapshot, cx);
        cx.emit(LibraryEvent::ScanFinished {
            unreadable,
            location_errors,
        });
    }

    /// Make `directory` a library location and rescan.
    pub fn add_folder(&mut self, directory: &Path, cx: &mut Context<Self>) {
        let directory = directory.to_path_buf();
        self.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| {
                config.add_folder_location(&directory);
            });
        });
        self.rescan(cx);
    }

    /// Ask for a folder with the platform picker, then add it.
    pub fn prompt_add_folder(&mut self, cx: &mut Context<Self>) {
        let receiver = cx.prompt_for_paths(PathPromptOptions {
            files: false,
            directories: true,
            multiple: false,
            prompt: Some(SharedString::from("Add folder")),
        });
        self.prompt_task = Some(cx.spawn(async move |this, cx| {
            let Ok(Ok(Some(paths))) = receiver.await else {
                return;
            };
            let Some(directory) = paths.into_iter().next() else {
                return;
            };
            let _ = this.update(cx, |this, cx| this.add_folder(&directory, cx));
        }));
    }
}
