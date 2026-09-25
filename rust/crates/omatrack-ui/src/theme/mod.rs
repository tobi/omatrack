//! The Omarchy theme bridge: follow the desktop's active palette.
//!
//! Omarchy publishes the active theme as a directory,
//! `~/.local/state/omarchy/current` (older installations:
//! `~/.config/omarchy/current`), holding `theme/colors.toml` and an optional
//! `theme.name`. [`install`] reads it, projects it onto the gpui-component
//! [`Theme`] and keeps following it: a theme switch swaps a symlink, an
//! editor saves atomically, a directory is deleted and recreated — each
//! re-applies the palette about 100 ms later. Without a palette the built-in
//! gpui-component dark theme applies.
//!
//! Ownership: the component [`Theme`] global stays the single source of
//! colors. This module only writes it (through [`Theme::change`], which also
//! reprojects the Base layer) and publishes [`ThemeStatus`] so the interface
//! can say where the colors came from. Views that draw theme colors observe
//! `Theme` with `cx.observe_global::<Theme>`.
//!
//! Failure policy: a missing palette means "not on Omarchy" and restores the
//! built-in theme; a palette that exists but cannot be parsed keeps whatever
//! theme is showing and logs a warning, so a half-written file never flashes
//! the application to another look.

mod palette;
mod watch;

use std::{
    io,
    path::{Path, PathBuf},
    rc::Rc,
    time::Duration,
};

use gpui_kit::component::{Theme, ThemeMode, ThemeRegistry};
use gpui_kit::{App, Global, SharedString, Task};

pub use palette::{OmarchyPalette, ThemeLoadError};
use watch::{PaletteWatcher, WatchedFile};

/// How long a burst of file events settles before the palette is reread.
const DEBOUNCE: Duration = Duration::from_millis(100);

/// Where Omarchy's `current` theme directory is looked up.
///
/// Paths are injected, never read from the environment inside the loader, so
/// tests run against temporary homes. A state directory wins whenever it
/// exists, even when its palette is missing or broken: a stale legacy theme
/// left behind by an upgrade is never revived.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ThemeSource {
    /// State `current` directories, in priority order.
    state_current: Vec<PathBuf>,
    legacy_current: Option<PathBuf>,
}

impl ThemeSource {
    /// The real desktop, from `$HOME` and `$XDG_STATE_HOME`; see
    /// [`ThemeSource::system_from`].
    pub fn system() -> Self {
        let home = std::env::var_os("HOME").map(PathBuf::from);
        let state_home = std::env::var_os("XDG_STATE_HOME").map(PathBuf::from);
        Self::system_from(home, state_home)
    }

    /// The Omarchy locations for a given `home` and `$XDG_STATE_HOME`:
    /// `~/.local/state/omarchy/current` (where Omarchy writes, whatever
    /// `$XDG_STATE_HOME` says), then `$XDG_STATE_HOME/omarchy/current` when
    /// it names another absolute directory, then the legacy
    /// `~/.config/omarchy/current`. An empty or relative path is ignored.
    pub fn system_from(home: Option<PathBuf>, state_home: Option<PathBuf>) -> Self {
        let home = home.filter(|home| home.is_absolute());
        let state_home = state_home.filter(|path| path.is_absolute());
        let mut state_current = Vec::new();
        if let Some(home) = &home {
            state_current.push(home.join(".local/state/omarchy/current"));
        }
        if let Some(state_home) = state_home {
            let current = state_home.join("omarchy/current");
            if !state_current.contains(&current) {
                state_current.push(current);
            }
        }
        Self {
            state_current,
            legacy_current: home.map(|home| home.join(".config/omarchy/current")),
        }
    }

    /// The default Omarchy locations under `home`:
    /// `.local/state/omarchy/current`, then `.config/omarchy/current`.
    pub fn system_from_home(home: impl AsRef<Path>) -> Self {
        Self::system_from(Some(home.as_ref().to_path_buf()), None)
    }

    /// No palette at all: the built-in theme, and nothing is watched.
    pub fn none() -> Self {
        Self::default()
    }

    /// The `current` directory that decides the palette, if any exists.
    pub fn current_dir(&self) -> Option<PathBuf> {
        for state in &self.state_current {
            match std::fs::symlink_metadata(state) {
                Err(error) if error.kind() == io::ErrorKind::NotFound => {}
                _ => return Some(state.clone()),
            }
        }
        self.legacy_current
            .as_ref()
            .filter(|legacy| std::fs::symlink_metadata(legacy).is_ok())
            .cloned()
    }

    /// Read the palette now. Blocking file I/O: call it off the UI thread
    /// except at startup.
    pub fn load(&self) -> ThemeDiscovery {
        let Some(current) = self.current_dir() else {
            return ThemeDiscovery::Missing;
        };
        let file = current.join("theme/colors.toml");
        let contents = match std::fs::read_to_string(&file) {
            Ok(contents) => contents,
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                return ThemeDiscovery::Missing;
            }
            Err(error) => {
                return ThemeDiscovery::Invalid(ThemeLoadError::Io {
                    path: file.display().to_string(),
                    message: error.to_string(),
                });
            }
        };
        let name = std::fs::read_to_string(current.join("theme.name"))
            .ok()
            .map(|name| name.trim().to_owned())
            .filter(|name| !name.is_empty())
            .unwrap_or_else(|| "Omarchy".to_owned());
        match OmarchyPalette::from_colors_toml(&name, &contents) {
            Ok(palette) => ThemeDiscovery::Palette {
                palette,
                dir: current,
            },
            Err(error) => ThemeDiscovery::Invalid(error),
        }
    }

    fn watched_files(&self) -> Vec<WatchedFile> {
        let mut files = Vec::new();
        for current in self.state_current.iter().chain(&self.legacy_current) {
            // Watch up to the directory that holds `omarchy/`, so creating
            // the whole tree later is noticed without watching all of $HOME.
            let boundary = current
                .parent()
                .and_then(Path::parent)
                .unwrap_or(current)
                .to_path_buf();
            for relative in ["theme/colors.toml", "theme.name"] {
                files.push(WatchedFile {
                    path: current.join(relative),
                    boundary: boundary.clone(),
                });
            }
        }
        files
    }
}

/// The result of reading a [`ThemeSource`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ThemeDiscovery {
    /// A valid palette from the given `current` directory.
    Palette {
        palette: OmarchyPalette,
        dir: PathBuf,
    },
    /// No Omarchy palette: not on Omarchy, or the theme was removed.
    Missing,
    /// A palette exists but could not be read or parsed.
    Invalid(ThemeLoadError),
}

/// Where the active colors came from.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ThemeOrigin {
    /// An Omarchy `current` directory.
    Omarchy(PathBuf),
    /// gpui-component's built-in theme.
    BuiltIn,
}

/// The active theme's name and origin, for status and preferences surfaces.
///
/// Set by [`install`] before every theme change, so an observer of
/// [`Theme`] always reads a status that matches the colors.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ThemeStatus {
    name: SharedString,
    mode: ThemeMode,
    origin: ThemeOrigin,
}

impl Global for ThemeStatus {}

impl ThemeStatus {
    pub fn name(&self) -> &SharedString {
        &self.name
    }

    pub fn origin(&self) -> &ThemeOrigin {
        &self.origin
    }

    pub fn mode(&self) -> ThemeMode {
        self.mode
    }

    /// One short line: `Omarchy · Tokyo Night` or `Built-in dark`.
    pub fn label(&self) -> SharedString {
        match &self.origin {
            ThemeOrigin::Omarchy(_) => format!("Omarchy · {}", self.name).into(),
            ThemeOrigin::BuiltIn => format!("Built-in {}", self.mode.name()).into(),
        }
    }

    /// The status of the running application; `None` before [`install`].
    pub fn global(cx: &App) -> Option<&Self> {
        cx.try_global::<Self>()
    }
}

/// Keeps the watcher task alive; replaced by every [`install`].
struct ThemeWatcher {
    _task: Task<()>,
}

impl Global for ThemeWatcher {}

/// What the watcher last applied, to skip no-op reloads.
#[derive(Debug, Clone, PartialEq, Eq)]
enum Applied {
    BuiltIn,
    Palette(OmarchyPalette, PathBuf),
}

/// Apply the palette from `source` and follow it until the next `install`.
///
/// Requires `gpui_kit::init` first. The first palette is read synchronously
/// so the first frame already has the right colors; later reloads parse on
/// the background executor and apply on the foreground.
pub fn install(source: ThemeSource, cx: &mut App) {
    if cx.has_global::<ThemeWatcher>() {
        cx.remove_global::<ThemeWatcher>();
    }
    // Register before reading so a change during startup is not missed.
    let watcher = if source == ThemeSource::none() {
        None
    } else {
        match PaletteWatcher::new(source.watched_files()) {
            Ok(watcher) => Some(watcher),
            Err(error) => {
                log::warn!("Cannot watch the Omarchy theme: {error}");
                None
            }
        }
    };
    let mut applied = apply(None, source.load(), cx);
    let Some((mut watcher, events)) = watcher else {
        return;
    };
    let task = cx.spawn(async move |cx| {
        while events.recv().await.is_ok() {
            cx.background_executor().timer(DEBOUNCE).await;
            while events.try_recv().is_ok() {}
            let source = source.clone();
            let (next_watcher, discovery) = cx
                .background_executor()
                .spawn(async move {
                    watcher.refresh();
                    let discovery = source.load();
                    (watcher, discovery)
                })
                .await;
            watcher = next_watcher;
            applied = cx.update(|cx| apply(Some(applied), discovery, cx));
        }
    });
    cx.set_global(ThemeWatcher { _task: task });
}

/// Apply `discovery` over `previous` (`None` at startup) and return what is
/// now showing.
fn apply(previous: Option<Applied>, discovery: ThemeDiscovery, cx: &mut App) -> Applied {
    let next = match discovery {
        ThemeDiscovery::Palette { palette, dir } => Applied::Palette(palette, dir),
        ThemeDiscovery::Missing => Applied::BuiltIn,
        ThemeDiscovery::Invalid(error) => {
            log::warn!("Ignoring the Omarchy palette: {error}");
            match previous {
                Some(previous) => return previous,
                None => Applied::BuiltIn,
            }
        }
    };
    if previous.as_ref() == Some(&next) {
        return next;
    }
    match &next {
        Applied::Palette(palette, dir) => apply_palette(palette, dir, cx),
        Applied::BuiltIn => apply_built_in(cx),
    }
    next
}

fn apply_palette(palette: &OmarchyPalette, dir: &Path, cx: &mut App) {
    let config = Rc::new(palette.theme_config());
    let mode = palette.mode();
    cx.set_global(ThemeStatus {
        name: palette.name().clone(),
        mode,
        origin: ThemeOrigin::Omarchy(dir.to_path_buf()),
    });
    // `Theme::change` applies the mode's config and reprojects the Base
    // layer (scrollbars, resize handles) and text defaults in one step.
    let theme = Theme::global_mut(cx);
    if mode.is_dark() {
        theme.dark_theme = config;
    } else {
        theme.light_theme = config;
    }
    Theme::change(mode, None, cx);
    cx.refresh_windows();
}

fn apply_built_in(cx: &mut App) {
    let registry = ThemeRegistry::global(cx);
    let dark = registry.default_dark_theme().clone();
    let light = registry.default_light_theme().clone();
    cx.set_global(ThemeStatus {
        name: dark.name.clone(),
        mode: ThemeMode::Dark,
        origin: ThemeOrigin::BuiltIn,
    });
    let theme = Theme::global_mut(cx);
    theme.dark_theme = dark;
    theme.light_theme = light;
    Theme::change(ThemeMode::Dark, None, cx);
    cx.refresh_windows();
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    const PALETTE: &str = "background = '#1a1b26'\nforeground = '#a9b1d6'\naccent = '#7aa2f7'\n\
        color1 = '#f7768e'\ncolor2 = '#9ece6a'\ncolor3 = '#e0af68'\n";

    fn write_theme(current: &Path, name: &str) {
        fs::create_dir_all(current.join("theme")).unwrap();
        fs::write(current.join("theme/colors.toml"), PALETTE).unwrap();
        fs::write(current.join("theme.name"), format!("{name}\n")).unwrap();
    }

    fn loaded_name(source: &ThemeSource) -> Option<String> {
        match source.load() {
            ThemeDiscovery::Palette { palette, .. } => Some(palette.name().to_string()),
            _ => None,
        }
    }

    #[test]
    fn state_directory_beats_legacy_and_never_revives_it() {
        let home = tempfile::tempdir().unwrap();
        let state = home.path().join(".local/state/omarchy/current");
        let legacy = home.path().join(".config/omarchy/current");
        write_theme(&legacy, "Legacy");
        let source = ThemeSource::system_from_home(home.path());
        assert_eq!(loaded_name(&source).as_deref(), Some("Legacy"));

        write_theme(&state, "Current");
        assert_eq!(loaded_name(&source).as_deref(), Some("Current"));

        // A state directory without a palette means "no palette", not legacy.
        fs::remove_file(state.join("theme/colors.toml")).unwrap();
        assert_eq!(source.load(), ThemeDiscovery::Missing);

        fs::remove_dir_all(&state).unwrap();
        assert_eq!(loaded_name(&source).as_deref(), Some("Legacy"));
    }

    #[test]
    fn home_state_directory_is_found_when_xdg_state_home_points_elsewhere() {
        let home = tempfile::tempdir().unwrap();
        let elsewhere = tempfile::tempdir().unwrap();
        let source = ThemeSource::system_from(
            Some(home.path().to_path_buf()),
            Some(elsewhere.path().to_path_buf()),
        );
        write_theme(&home.path().join(".config/omarchy/current"), "Legacy");
        write_theme(&home.path().join(".local/state/omarchy/current"), "Home");
        assert_eq!(loaded_name(&source).as_deref(), Some("Home"));

        // Only $XDG_STATE_HOME has a state directory: it still beats legacy.
        fs::remove_dir_all(home.path().join(".local/state")).unwrap();
        write_theme(&elsewhere.path().join("omarchy/current"), "Xdg");
        assert_eq!(loaded_name(&source).as_deref(), Some("Xdg"));

        // Relative or empty variables are ignored, never resolved against cwd.
        let ignored = ThemeSource::system_from(Some(PathBuf::new()), Some("state".into()));
        assert_eq!(ignored, ThemeSource::none());
        // $XDG_STATE_HOME = ~/.local/state is one candidate, not two.
        let same = ThemeSource::system_from(
            Some(home.path().to_path_buf()),
            Some(home.path().join(".local/state")),
        );
        assert_eq!(same, ThemeSource::system_from_home(home.path()));
    }

    #[test]
    fn broken_palettes_are_invalid_and_absent_ones_missing() {
        let home = tempfile::tempdir().unwrap();
        let source = ThemeSource::system_from_home(home.path());
        assert_eq!(source.load(), ThemeDiscovery::Missing);
        let state = home.path().join(".local/state/omarchy/current");
        write_theme(&state, "Current");
        fs::write(state.join("theme/colors.toml"), "broken").unwrap();
        assert!(matches!(source.load(), ThemeDiscovery::Invalid(_)));
        assert_eq!(ThemeSource::none().load(), ThemeDiscovery::Missing);
    }

    #[test]
    fn a_blank_theme_name_falls_back_to_omarchy() {
        let home = tempfile::tempdir().unwrap();
        let state = home.path().join(".local/state/omarchy/current");
        write_theme(&state, "  ");
        assert_eq!(
            loaded_name(&ThemeSource::system_from_home(home.path())).as_deref(),
            Some("Omarchy")
        );
    }

    #[test]
    fn status_labels_name_the_origin() {
        let omarchy = ThemeStatus {
            name: "Tokyo Night".into(),
            mode: ThemeMode::Dark,
            origin: ThemeOrigin::Omarchy(PathBuf::from("/x")),
        };
        assert_eq!(omarchy.label().as_ref(), "Omarchy · Tokyo Night");
        let built_in = ThemeStatus {
            name: "Default Dark".into(),
            mode: ThemeMode::Dark,
            origin: ThemeOrigin::BuiltIn,
        };
        assert_eq!(built_in.label().as_ref(), "Built-in dark");
    }
}
