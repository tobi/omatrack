//! The library index cache: one JSON [`RecordingSummary`] per recording
//! under `$XDG_CACHE_HOME/omatrack/index/rs1/{generation}/`, keyed by the
//! file's POSIX identity `(dev, ino, size, mtime)`.
//!
//! The generation is `omatrack_core::converter_generation()`, so a parser
//! upgrade re-indexes everything; other generations are pruned. Only
//! successful summaries are stored: a file that failed to open is retried
//! on the next scan. Nothing is ever written beside a source recording.

use crate::fsutil::write_atomic;
use crate::location::{DiscoveredFile, Location, OpenMode};
use crate::summary::RecordingSummary;
use omatrack_core::recording::OpenError;
use serde::{Deserialize, Serialize};
use std::fs::Metadata;
use std::io;
use std::path::{Path, PathBuf};

const DOCUMENT_VERSION: u32 = 1;

/// A file's identity for cache purposes. Any change (a copy, an edit, a
/// touch) is a different identity.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
#[non_exhaustive]
pub struct FileIdentity {
    pub dev: u64,
    pub ino: u64,
    pub size: u64,
    /// Modification time, nanoseconds since the Unix epoch.
    pub mtime_ns: i64,
}

impl FileIdentity {
    pub fn new(dev: u64, ino: u64, size: u64, mtime_ns: i64) -> Self {
        Self {
            dev,
            ino,
            size,
            mtime_ns,
        }
    }

    #[cfg(unix)]
    pub fn from_metadata(metadata: &Metadata) -> Self {
        use std::os::unix::fs::MetadataExt;
        Self {
            dev: metadata.dev(),
            ino: metadata.ino(),
            size: metadata.size(),
            mtime_ns: metadata
                .mtime()
                .saturating_mul(1_000_000_000)
                .saturating_add(metadata.mtime_nsec()),
        }
    }

    #[cfg(not(unix))]
    pub fn from_metadata(metadata: &Metadata) -> Self {
        let mtime_ns = metadata
            .modified()
            .ok()
            .and_then(|time| time.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|d| i64::try_from(d.as_nanos()).unwrap_or(i64::MAX))
            .unwrap_or(0);
        Self {
            dev: 0,
            ino: 0,
            size: metadata.len(),
            mtime_ns,
        }
    }

    /// The identity of the file at `path` now.
    ///
    /// # Errors
    /// Returns the filesystem error if the source metadata cannot be read.
    pub fn of(path: &Path) -> io::Result<Self> {
        std::fs::metadata(path).map(|metadata| Self::from_metadata(&metadata))
    }

    fn file_name(&self) -> String {
        format!(
            "{}-{}-{}-{}.json",
            self.dev, self.ino, self.size, self.mtime_ns
        )
    }
}

#[derive(Serialize, Deserialize)]
struct CacheDocument {
    version: u32,
    generation: String,
    identity: FileIdentity,
    summary: RecordingSummary,
}

/// Whether a summary came from the cache.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum CacheOutcome {
    Hit,
    /// Opened and summarized; the summary was stored.
    Miss,
}

/// One converter generation's index cache.
#[derive(Debug, Clone)]
pub struct IndexCache {
    root: PathBuf,
    generation: String,
}

impl IndexCache {
    /// The cache under `root` (`Paths::index_cache_root`) for this build's
    /// converter generation.
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self::with_generation(root, omatrack_core::converter_generation())
    }

    /// A cache for an explicit generation (tests).
    pub fn with_generation(root: impl Into<PathBuf>, generation: impl Into<String>) -> Self {
        Self {
            root: root.into(),
            generation: generation.into(),
        }
    }

    pub fn generation(&self) -> &str {
        &self.generation
    }

    /// This generation's directory.
    pub fn directory(&self) -> PathBuf {
        self.root.join(&self.generation)
    }

    fn entry_path(&self, identity: &FileIdentity) -> PathBuf {
        self.directory().join(identity.file_name())
    }

    /// Remove every other generation's directory. Returns how many were
    /// removed.
    ///
    /// # Errors
    /// Returns an error if the cache root cannot be listed. A missing root is empty;
    /// individual removal failures are logged.
    pub fn prune_other_generations(&self) -> io::Result<usize> {
        let entries = match std::fs::read_dir(&self.root) {
            Ok(entries) => entries,
            Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(0),
            Err(error) => return Err(error),
        };
        let mut removed = 0;
        for entry in entries.flatten() {
            if entry.file_name().to_string_lossy() == self.generation {
                continue;
            }
            let path = entry.path();
            let result = if path.is_dir() {
                std::fs::remove_dir_all(&path)
            } else {
                std::fs::remove_file(&path)
            };
            match result {
                Ok(()) => removed += 1,
                Err(error) => log::warn!("index cache prune {}: {error}", path.display()),
            }
        }
        Ok(removed)
    }

    /// The cached summary for `identity`, when present and written by this
    /// generation for exactly this identity.
    pub fn load(&self, identity: &FileIdentity) -> Option<RecordingSummary> {
        let bytes = std::fs::read(self.entry_path(identity)).ok()?;
        let document: CacheDocument = serde_json::from_slice(&bytes).ok()?;
        (document.version == DOCUMENT_VERSION
            && document.generation == self.generation
            && document.identity == *identity)
            .then_some(document.summary)
    }

    /// Store a successful summary atomically.
    ///
    /// # Errors
    /// Returns an error if serialization or the atomic cache-file write fails.
    pub fn store(&self, identity: &FileIdentity, summary: &RecordingSummary) -> io::Result<()> {
        let document = CacheDocument {
            version: DOCUMENT_VERSION,
            generation: self.generation.clone(),
            identity: *identity,
            summary: summary.clone(),
        };
        let bytes = serde_json::to_vec(&document).map_err(io::Error::other)?;
        write_atomic(&self.entry_path(identity), &bytes)
    }

    /// The summary of a discovered file: from the cache, else an index open
    /// through its location (stored on success; a failure is not cached).
    ///
    /// # Errors
    /// Returns `OpenError` if the location cannot open the recording. Cache-write
    /// failures are logged without discarding a successful summary.
    pub fn summarize(
        &self,
        location: &dyn Location,
        file: &DiscoveredFile,
    ) -> Result<(RecordingSummary, CacheOutcome), OpenError> {
        if let Some(summary) = self.load(file.identity()) {
            return Ok((summary, CacheOutcome::Hit));
        }
        let recording = location.open(file, OpenMode::Index)?;
        let summary = RecordingSummary::from_recording(&recording);
        if let Err(error) = self.store(file.identity(), &summary) {
            log::warn!("index cache store {}: {error}", file.path().display());
        }
        Ok((summary, CacheOutcome::Miss))
    }
}
