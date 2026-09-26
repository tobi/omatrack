//! Portable folder metadata: `TRACK.yml` files inherited root to leaf.
//!
//! A recording inherits every `TRACK.yml` above it; a closer folder's file
//! overrides its parents key by key (nested maps merge, blank strings do
//! not erase). Editing a folder's metadata rewrites only the keys Omatrack
//! owns and keeps everything else (such as `files`), atomically. Port of
//! `TrackMetadata.cpp`.

use crate::fsutil::{scalar_text, write_atomic};
use serde_yaml::{Mapping, Value};
use std::collections::HashMap;
use std::io;
use std::path::{Path, PathBuf};

/// The folder metadata file name.
pub const FILE_NAME: &str = "TRACK.yml";

/// Top-level keys Omatrack owns in a `TRACK.yml`.
pub const OWNED_KEYS: &[&str] = &[
    "schema", "driver", "folder", "car", "event", "series", "track", "channels",
];

/// Reading or writing a `TRACK.yml` failed.
#[derive(Debug, thiserror::Error)]
#[non_exhaustive]
pub enum TrackYmlError {
    #[error("{path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: io::Error,
    },
    #[error("{path}: not a YAML mapping: {message}")]
    Parse { path: PathBuf, message: String },
    #[error("{0}: metadata folder does not exist")]
    MissingFolder(PathBuf),
}

fn canonical_directory(directory: &Path) -> Option<PathBuf> {
    let metadata = std::fs::metadata(directory).ok()?;
    if !metadata.is_dir() {
        return None;
    }
    std::fs::canonicalize(directory)
        .ok()
        .or_else(|| std::path::absolute(directory).ok())
}

/// The `TRACK.yml` path of an existing directory (the file need not exist).
pub fn file_path(directory: &Path) -> Option<PathBuf> {
    canonical_directory(directory).map(|dir| dir.join(FILE_NAME))
}

/// Existing `TRACK.yml` files from the filesystem root down to `directory`.
pub fn hierarchy_paths(directory: &Path, include_target: bool) -> Vec<PathBuf> {
    let Some(canonical) = canonical_directory(directory) else {
        return Vec::new();
    };
    let mut lineage: Vec<&Path> = canonical.ancestors().collect();
    lineage.reverse();
    lineage
        .into_iter()
        .filter(|dir| include_target || *dir != canonical.as_path())
        .map(|dir| dir.join(FILE_NAME))
        .filter(|path| path.is_file())
        .collect()
}

/// Read one document; an empty file is an empty mapping.
///
/// # Errors
/// Returns `TrackYmlError` for I/O errors other than a missing file, invalid YAML or a
/// non-mapping document.
pub fn read_document(path: &Path) -> Result<Mapping, TrackYmlError> {
    let text = std::fs::read_to_string(path).map_err(|source| TrackYmlError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if text.trim().is_empty() {
        return Ok(Mapping::new());
    }
    match serde_yaml::from_str::<Value>(&text) {
        Ok(Value::Mapping(map)) => Ok(map),
        Ok(Value::Null) => Ok(Mapping::new()),
        Ok(_) => Err(TrackYmlError::Parse {
            path: path.to_path_buf(),
            message: "the document is not a mapping".to_string(),
        }),
        Err(error) => Err(TrackYmlError::Parse {
            path: path.to_path_buf(),
            message: error.to_string(),
        }),
    }
}

/// Recursive merge: nested maps merge key by key (a map left empty is
/// removed), blank strings never erase an inherited value, anything else
/// replaces it.
pub fn merge(base: &mut Mapping, overlay: &Mapping) {
    for (key, value) in overlay {
        match value {
            Value::Mapping(nested_overlay) => {
                let mut nested = match base.get(key) {
                    Some(Value::Mapping(map)) => map.clone(),
                    _ => Mapping::new(),
                };
                merge(&mut nested, nested_overlay);
                if nested.is_empty() {
                    base.remove(key);
                } else {
                    base.insert(key.clone(), Value::Mapping(nested));
                }
            }
            Value::String(text) if text.trim().is_empty() => {}
            _ => {
                base.insert(key.clone(), value.clone());
            }
        }
    }
}

/// Every `TRACK.yml` above (and with `include_target`, in) `directory`,
/// merged root to leaf, and the files that contributed. Unreadable files
/// are skipped with a warning.
pub fn read_hierarchy(directory: &Path, include_target: bool) -> (Mapping, Vec<PathBuf>) {
    let paths = hierarchy_paths(directory, include_target);
    let mut merged = Mapping::new();
    for path in &paths {
        match read_document(path) {
            Ok(document) => merge(&mut merged, &document),
            Err(error) => log::warn!("{error}"),
        }
    }
    (merged, paths)
}

/// Replace the Omatrack-owned keys of `directory`'s `TRACK.yml` with those in `owned`
/// (other keys of `owned` are ignored), keeping every unrelated key.
///
/// Creates the file when missing; writes atomically. Returns the path.
///
/// # Errors
/// Returns `TrackYmlError` if the existing document cannot be read or parsed, or
/// serialization or atomic replacement fails.
pub fn update(directory: &Path, owned: &Mapping) -> Result<PathBuf, TrackYmlError> {
    let target = file_path(directory)
        .ok_or_else(|| TrackYmlError::MissingFolder(directory.to_path_buf()))?;
    let mut document = if target.is_file() {
        read_document(&target)?
    } else {
        Mapping::new()
    };
    for key in OWNED_KEYS {
        document.remove(*key);
    }
    for (key, value) in owned {
        if key.as_str().is_some_and(|key| OWNED_KEYS.contains(&key)) {
            document.insert(key.clone(), value.clone());
        }
    }
    let text =
        serde_yaml::to_string(&Value::Mapping(document)).map_err(|error| TrackYmlError::Parse {
            path: target.clone(),
            message: error.to_string(),
        })?;
    write_atomic(&target, text.as_bytes()).map_err(|source| TrackYmlError::Io {
        path: target.clone(),
        source,
    })?;
    Ok(target)
}

/// Canonical driver-mapping key: `*`, or a positive number printed with 15
/// significant digits (`02.500` -> `2.5`); `None` otherwise.
pub fn normalized_driver_mapping_key(text: &str) -> Option<String> {
    let text = text.trim();
    if text == "*" {
        return Some(text.to_string());
    }
    let id: f64 = text.parse().ok()?;
    (id.is_finite() && id > 0.0).then(|| driver_id_key(id))
}

/// The mapping key of a numeric driver id (`%.15g`).
pub fn driver_id_key(id: f64) -> String {
    omatrack_core::sprintf!("%.15g", id)
}

/// The driver name `metadata.driver.mappings` gives a detected driver id:
/// an exact mapping wins over the `*` fallback.
pub fn driver_name_for_id(metadata: &Mapping, driver_id: f64) -> Option<String> {
    if !(driver_id.is_finite() && driver_id > 0.0) {
        return None;
    }
    let mappings = metadata
        .get("driver")
        .and_then(Value::as_mapping)
        .and_then(|driver| driver.get("mappings"))
        .and_then(Value::as_mapping)?;
    let mut exact = None;
    let mut wildcard = None;
    let wanted = driver_id_key(driver_id);
    for (key, value) in mappings {
        let (Some(key), Some(name)) = (scalar_text(key), scalar_text(value)) else {
            continue;
        };
        let name = name.trim().to_string();
        if name.is_empty() {
            continue;
        }
        match normalized_driver_mapping_key(&key).as_deref() {
            Some("*") => wildcard = Some(name),
            Some(key) if key == wanted => exact = Some(name),
            _ => {}
        }
    }
    exact.or(wildcard)
}

/// Layer-2 snapshot: each folder's inherited metadata read once per scan
/// and memoized (a child reuses its parent's merged document).
#[derive(Debug, Default)]
pub struct FolderMetadataCache {
    merged: HashMap<PathBuf, Mapping>,
}

impl FolderMetadataCache {
    pub fn new() -> Self {
        Self::default()
    }

    /// Merged `TRACK.yml` chain for `directory` (a recording's folder).
    pub fn metadata(&mut self, directory: &Path) -> Mapping {
        match canonical_directory(directory) {
            Some(canonical) => self.merged_for(&canonical).clone(),
            None => Mapping::new(),
        }
    }

    fn merged_for(&mut self, canonical: &Path) -> &Mapping {
        if !self.merged.contains_key(canonical) {
            let mut merged = match canonical.parent() {
                Some(parent) if parent != canonical => self.merged_for(parent).clone(),
                _ => Mapping::new(),
            };
            let own = canonical.join(FILE_NAME);
            if own.is_file() {
                match read_document(&own) {
                    Ok(document) => merge(&mut merged, &document),
                    Err(error) => log::warn!("{error}"),
                }
            }
            self.merged.insert(canonical.to_path_buf(), merged);
        }
        &self.merged[canonical]
    }
}
