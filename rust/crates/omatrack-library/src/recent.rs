//! The recent-files list: successful opens of individual files, most
//! recent first. Individual files never become library locations.

use std::path::Path;

/// `recent_files` never holds more than this many entries.
pub const MAX_RECENT_FILES: usize = 6;

/// Put `path` first, drop an older copy of it, and keep at most
/// [`MAX_RECENT_FILES`] entries.
pub fn push_recent(list: &mut Vec<String>, path: &str) {
    let path = path.trim();
    if path.is_empty() {
        return;
    }
    list.retain(|entry| entry != path);
    list.insert(0, path.to_string());
    list.truncate(MAX_RECENT_FILES);
}

/// Drop entries that are no longer files (deleted recordings) and
/// duplicates, keeping order and the cap. Returns true when anything was
/// removed. Touches the filesystem: call it off the UI thread.
pub fn prune_recent(list: &mut Vec<String>) -> bool {
    let before = list.clone();
    let mut kept: Vec<String> = Vec::with_capacity(list.len());
    for entry in list.drain(..) {
        if !entry.is_empty() && !kept.contains(&entry) && Path::new(&entry).is_file() {
            kept.push(entry);
        }
    }
    kept.truncate(MAX_RECENT_FILES);
    *list = kept;
    *list != before
}
