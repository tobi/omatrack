//! The source abstraction for opening files: a [`Location`] discovers
//! recordings and opens them. [`FolderLocation`] (a local folder scanned
//! recursively) is the only implementation; the trait is the seam another
//! source would plug into.

use crate::index_cache::FileIdentity;
use omatrack_core::recording::{OpenError, Recording};
use omatrack_core::session::{is_recording_path, is_video_path};
use std::io;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

/// Stable identity of a configured location.
#[derive(Debug, Clone, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct LocationId(String);

impl LocationId {
    pub fn new(id: impl Into<String>) -> Self {
        Self(id.into())
    }
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl std::fmt::Display for LocationId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

/// A shared cancellation flag for long library jobs. Cloning shares the
/// flag; [`Cancel::flag`] hands it to core APIs that take `&AtomicBool`.
#[derive(Debug, Clone, Default)]
pub struct Cancel(Arc<AtomicBool>);

impl Cancel {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn cancel(&self) {
        self.0.store(true, Ordering::Relaxed);
    }
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Relaxed)
    }
    pub fn flag(&self) -> &AtomicBool {
        &self.0
    }
}

/// How much of a recording to open.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum OpenMode {
    /// A bounded metadata view for the library (laps, driver, GPS).
    Index,
    /// Every channel decoded, for analysis.
    Full,
}

/// What kind of file a discovered recording is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FileKind {
    /// The telemetry lives inside the onboard video (AiM MP4).
    Video,
    /// A telemetry-only file (`.pds`, `.ld`, `.vbo`, `.telemetry`, MTJ).
    Telemetry,
}

/// One recording a location found.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct DiscoveredFile {
    location: LocationId,
    path: PathBuf,
    identity: FileIdentity,
}

impl DiscoveredFile {
    pub fn new(location: LocationId, path: PathBuf, identity: FileIdentity) -> Self {
        Self {
            location,
            path,
            identity,
        }
    }
    pub fn location(&self) -> &LocationId {
        &self.location
    }
    pub fn path(&self) -> &Path {
        &self.path
    }
    /// POSIX identity the index cache is keyed by.
    pub fn identity(&self) -> &FileIdentity {
        &self.identity
    }
    pub fn kind(&self) -> FileKind {
        if is_video_path(&self.path) {
            FileKind::Video
        } else {
            FileKind::Telemetry
        }
    }
}

/// A place recordings come from.
pub trait Location: Send + Sync {
    fn id(&self) -> &LocationId;
    /// User-facing name.
    fn name(&self) -> &str;
    /// Report every supported recording through `sink`, checking `cancel`
    /// between entries. Never writes anything.
    fn scan(&self, cancel: &Cancel, sink: &mut dyn FnMut(DiscoveredFile)) -> io::Result<()>;
    /// Open a discovered recording.
    fn open(&self, file: &DiscoveredFile, mode: OpenMode) -> Result<Recording, OpenError>;
    /// The onboard video to play for this recording, when the location
    /// knows it without opening the recording.
    fn media_path(&self, file: &DiscoveredFile) -> Option<PathBuf>;
}

/// A local folder, scanned recursively. Hidden directories are skipped;
/// symbolic links are followed (link loops are reported and skipped).
/// Nothing is ever written beside a source.
#[derive(Debug, Clone)]
pub struct FolderLocation {
    id: LocationId,
    name: String,
    root: PathBuf,
}

impl FolderLocation {
    pub fn new(id: LocationId, name: impl Into<String>, root: impl Into<PathBuf>) -> Self {
        Self {
            id,
            name: name.into(),
            root: root.into(),
        }
    }

    /// The folder location a config entry describes (`None` without target).
    pub fn from_config(config: &crate::config::FolderLocationConfig) -> Option<Self> {
        Some(Self::new(
            LocationId::new(config.resolved_id()),
            config.display_name(),
            config.target_path()?,
        ))
    }

    pub fn root(&self) -> &Path {
        &self.root
    }
}

fn is_hidden(entry: &walkdir::DirEntry) -> bool {
    entry.depth() > 0 && entry.file_name().to_string_lossy().starts_with('.')
}

impl Location for FolderLocation {
    fn id(&self) -> &LocationId {
        &self.id
    }

    fn name(&self) -> &str {
        &self.name
    }

    fn scan(&self, cancel: &Cancel, sink: &mut dyn FnMut(DiscoveredFile)) -> io::Result<()> {
        let metadata = std::fs::metadata(&self.root)?;
        if !metadata.is_dir() {
            return Err(io::Error::new(
                io::ErrorKind::NotADirectory,
                format!("{} is not a folder", self.root.display()),
            ));
        }
        let walker = walkdir::WalkDir::new(&self.root)
            .follow_links(true)
            .sort_by_file_name()
            .into_iter()
            .filter_entry(|entry| !is_hidden(entry));
        for entry in walker {
            if cancel.is_cancelled() {
                return Err(io::Error::new(io::ErrorKind::Interrupted, "scan cancelled"));
            }
            let entry = match entry {
                Ok(entry) => entry,
                Err(error) => {
                    log::warn!("scan {}: {error}", self.root.display());
                    continue;
                }
            };
            if !entry.file_type().is_file() || !is_recording_path(entry.path()) {
                continue;
            }
            let identity = match entry.metadata() {
                Ok(metadata) => FileIdentity::from_metadata(&metadata),
                Err(error) => {
                    log::warn!("scan {}: {error}", entry.path().display());
                    continue;
                }
            };
            sink(DiscoveredFile::new(
                self.id.clone(),
                entry.into_path(),
                identity,
            ));
        }
        Ok(())
    }

    fn open(&self, file: &DiscoveredFile, mode: OpenMode) -> Result<Recording, OpenError> {
        match mode {
            OpenMode::Index => Recording::open_index(file.path()),
            OpenMode::Full => Recording::open(file.path()),
        }
    }

    fn media_path(&self, file: &DiscoveredFile) -> Option<PathBuf> {
        (file.kind() == FileKind::Video).then(|| file.path().to_path_buf())
    }
}
