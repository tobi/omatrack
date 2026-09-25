//! Watch directories rather than files so atomic saves and symlink swaps
//! survive.
//!
//! Adapted from `src/system_theme/watch.rs` of the MIT-licensed Omarchy theme
//! crate for GPUI Kit, version 0.1.3, by Jason Lee (huacnlee):
//!
//! MIT License
//!
//! Copyright (c) 2026 Jason Lee (huacnlee)
//!
//! Permission is hereby granted, free of charge, to any person obtaining a copy
//! of this software and associated documentation files (the "Software"), to deal
//! in the Software without restriction, including without limitation the rights
//! to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//! copies of the Software, and to permit persons to whom the Software is
//! furnished to do so, subject to the following conditions:
//!
//! The above copyright notice and this permission notice shall be included in all
//! copies or substantial portions of the Software.
//!
//! THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//! IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//! FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//! AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//! LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//! OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//! SOFTWARE.
//!
//! Changes: the watched files and the ancestor boundary are supplied by the
//! caller instead of being derived from `$HOME`, and errors go to `log`.

use notify::{RecommendedWatcher, RecursiveMode, Watcher};
use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
};

/// One consumed file and the highest ancestor worth watching for it.
#[derive(Debug, Clone)]
pub(super) struct WatchedFile {
    pub(super) path: PathBuf,
    pub(super) boundary: PathBuf,
}

pub(super) struct PaletteWatcher {
    watcher: RecommendedWatcher,
    files: Vec<WatchedFile>,
    paths: BTreeMap<PathBuf, Identity>,
}

// A deleted directory can be recreated at the same path before events are
// read. Its old OS watch then refers to a different inode and must be
// replaced.
#[cfg(unix)]
type Identity = (u64, u64);
#[cfg(not(unix))]
type Identity = Option<std::time::SystemTime>;

fn identity(metadata: &fs::Metadata) -> Identity {
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        (metadata.dev(), metadata.ino())
    }
    #[cfg(not(unix))]
    metadata.created().ok()
}

impl PaletteWatcher {
    pub(super) fn new(
        files: Vec<WatchedFile>,
    ) -> notify::Result<(Self, async_channel::Receiver<()>)> {
        // Coalesce bursts without blocking the OS callback or growing a queue.
        let (sender, receiver) = async_channel::bounded(1);
        let watcher = notify::recommended_watcher(move |event: notify::Result<notify::Event>| {
            // Reading the palette must not trigger another reload on Linux.
            if !matches!(&event, Ok(event) if event.kind.is_access()) {
                if let Err(error) = event {
                    log::warn!("Omarchy theme watcher error: {error}");
                }
                let _ = sender.try_send(());
            }
        })?;
        let mut watcher = Self {
            watcher,
            files,
            paths: BTreeMap::new(),
        };
        watcher.refresh();
        Ok((watcher, receiver))
    }

    /// Re-derive the watched directories: follows replaced symlinks and
    /// recreated directories.
    pub(super) fn refresh(&mut self) {
        let mut desired = BTreeMap::new();
        for file in &self.files {
            // Start at the consumed file so file symlinks are followed too;
            // retaining its lexical ancestors also catches link replacement.
            for ancestor in file.path.ancestors() {
                add_directory(ancestor, &mut desired);
                // Also watch a dangling link's target parents for later creation.
                if let Ok(target) = fs::read_link(ancestor) {
                    let target = ancestor.parent().unwrap_or(&file.boundary).join(target);
                    for parent in target.ancestors() {
                        if parent.is_dir() {
                            add_directory(parent, &mut desired);
                            if let Some(grandparent) = parent.parent() {
                                add_directory(grandparent, &mut desired);
                            }
                            break;
                        }
                    }
                }
                if ancestor == file.boundary {
                    break;
                }
            }
        }
        self.paths.retain(|path, old| {
            if desired.get(path) == Some(old) {
                true
            } else {
                let _ = self.watcher.unwatch(path);
                false
            }
        });
        for (path, identity) in desired {
            if !self.paths.contains_key(&path) {
                match self.watcher.watch(&path, RecursiveMode::NonRecursive) {
                    Ok(()) => {
                        self.paths.insert(path, identity);
                    }
                    Err(error) => {
                        log::warn!("Cannot watch Omarchy path {}: {error}", path.display())
                    }
                }
            }
        }
    }
}

fn add_directory(path: &Path, paths: &mut BTreeMap<PathBuf, Identity>) {
    if let Ok(path) = fs::canonicalize(path)
        && let Ok(metadata) = fs::metadata(&path)
        && metadata.is_dir()
    {
        paths.insert(path, identity(&metadata));
    }
}
