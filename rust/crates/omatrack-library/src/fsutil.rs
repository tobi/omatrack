//! Small filesystem helpers shared by the writers in this crate.

use std::fs::{self, File};
use std::io::{self, Write};
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};

/// Write `bytes` to `path` atomically: a temporary file in the same
/// directory, fsynced, then renamed over the target. A reader sees either
/// the old document or the new one, never a torn write. The directory is
/// created when missing.
pub(crate) fn write_atomic(path: &Path, bytes: &[u8]) -> io::Result<()> {
    static COUNTER: AtomicU64 = AtomicU64::new(0);
    let directory = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    fs::create_dir_all(directory)?;
    let name = path
        .file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "file".to_string());
    let temporary = directory.join(format!(
        ".{name}.tmp-{}-{}",
        std::process::id(),
        COUNTER.fetch_add(1, Ordering::Relaxed)
    ));
    let result = (|| {
        let mut file = File::create(&temporary)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        fs::rename(&temporary, path)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

/// A YAML scalar as text (`"7"`, `7`, `true`); `None` for maps, sequences
/// and null.
pub(crate) fn scalar_text(value: &serde_yaml::Value) -> Option<String> {
    match value {
        serde_yaml::Value::String(text) => Some(text.clone()),
        serde_yaml::Value::Number(number) => Some(number.to_string()),
        serde_yaml::Value::Bool(flag) => Some(flag.to_string()),
        serde_yaml::Value::Tagged(tagged) => scalar_text(&tagged.value),
        _ => None,
    }
}

/// Nested text at `path` in a YAML mapping, trimmed; `None` when missing
/// or blank.
pub(crate) fn nested_text(map: &serde_yaml::Mapping, path: &[&str]) -> Option<String> {
    let (last, parents) = path.split_last()?;
    let mut current = map;
    for key in parents {
        current = current.get(*key)?.as_mapping()?;
    }
    scalar_text(current.get(*last)?)
        .map(|text| text.trim().to_string())
        .filter(|text| !text.is_empty())
}
