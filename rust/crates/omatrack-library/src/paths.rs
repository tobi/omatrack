//! Where Omatrack keeps its files: XDG config, cache and state directories.

use std::env;
use std::path::{Path, PathBuf};

const APP_DIR: &str = "omatrack";

/// The application's configuration, cache and state directories. Built
/// from the environment in the app, or from explicit roots in tests so
/// nothing touches the real home directory.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Paths {
    config_dir: PathBuf,
    cache_dir: PathBuf,
    state_dir: PathBuf,
}

/// An XDG base directory variable: only an absolute path counts.
fn xdg_home(variable: &str, fallback: &[&str]) -> PathBuf {
    if let Some(value) = env::var_os(variable) {
        let path = PathBuf::from(value);
        if path.is_absolute() {
            return path;
        }
    }
    let mut path = home_dir().unwrap_or_else(env::temp_dir);
    for part in fallback {
        path.push(part);
    }
    path
}

/// `$HOME`, when set.
pub fn home_dir() -> Option<PathBuf> {
    env::var_os("HOME")
        .map(PathBuf::from)
        .filter(|path| path.is_absolute())
}

/// `~/Documents/Telemetry`: the only library location on a fresh install.
pub fn default_telemetry_dir() -> Option<PathBuf> {
    home_dir().map(|home| home.join("Documents").join("Telemetry"))
}

impl Paths {
    /// `$XDG_CONFIG_HOME`, `$XDG_CACHE_HOME` and `$XDG_STATE_HOME` (with the
    /// standard `~/.config`, `~/.cache`, `~/.local/state` fallbacks), each
    /// followed by `omatrack`.
    pub fn from_env() -> Self {
        Self::with_roots(
            xdg_home("XDG_CONFIG_HOME", &[".config"]),
            xdg_home("XDG_CACHE_HOME", &[".cache"]),
            xdg_home("XDG_STATE_HOME", &[".local", "state"]),
        )
    }

    /// Explicit XDG roots; `omatrack` is appended to each.
    pub fn with_roots(
        config_home: impl Into<PathBuf>,
        cache_home: impl Into<PathBuf>,
        state_home: impl Into<PathBuf>,
    ) -> Self {
        Self {
            config_dir: config_home.into().join(APP_DIR),
            cache_dir: cache_home.into().join(APP_DIR),
            state_dir: state_home.into().join(APP_DIR),
        }
    }

    pub fn config_dir(&self) -> &Path {
        &self.config_dir
    }
    pub fn cache_dir(&self) -> &Path {
        &self.cache_dir
    }
    pub fn state_dir(&self) -> &Path {
        &self.state_dir
    }

    /// `omatrack.yml`, the single configuration document.
    pub fn config_file(&self) -> PathBuf {
        self.config_dir.join("omatrack.yml")
    }

    /// Root of the library index cache; one subdirectory per converter
    /// generation lives below it.
    pub fn index_cache_root(&self) -> PathBuf {
        self.cache_dir.join("index").join("rs1")
    }
}
