//! The Omarchy bridge against real files in temporary homes: installing,
//! following edits, symlink swaps and corruption, and state-over-legacy
//! precedence. Filesystem events arrive on notify's thread in real time,
//! while the 100 ms debounce runs on GPUI's test clock, so the wait loop
//! advances both.

#![cfg(test)]

use std::{fs, path::Path, time::Duration};

use gpui_kit::component::{ActiveTheme as _, Theme, ThemeMode, ThemeRegistry};
use gpui_kit::{Hsla, TestAppContext, rgb};
use omatrack_ui::theme::{self, ThemeOrigin, ThemeSource, ThemeStatus};

const TOKYO: &str = "background = '#1a1b26'\nforeground = '#a9b1d6'\naccent = '#7aa2f7'\n\
    color1 = '#f7768e'\ncolor2 = '#9ece6a'\ncolor3 = '#e0af68'\ncolor8 = '#414868'\n";

fn accent(hex: u32) -> Hsla {
    rgb(hex).into()
}

fn write_theme(dir: &Path, palette: &str) {
    fs::create_dir_all(dir).unwrap();
    fs::write(dir.join("colors.toml"), palette).unwrap();
}

fn primary(cx: &mut TestAppContext) -> Hsla {
    cx.update(|cx| cx.theme().primary)
}

fn status(cx: &mut TestAppContext) -> ThemeStatus {
    cx.update(|cx| ThemeStatus::global(cx).expect("installed").clone())
}

fn wait_until(cx: &mut TestAppContext, what: &str, done: impl Fn(&mut TestAppContext) -> bool) {
    let deadline = std::time::Instant::now() + Duration::from_secs(5);
    loop {
        cx.executor().advance_clock(Duration::from_millis(150));
        cx.run_until_parked();
        if done(cx) {
            return;
        }
        assert!(std::time::Instant::now() < deadline, "timed out: {what}");
        std::thread::sleep(Duration::from_millis(10));
    }
}

/// Creates the directories the watcher's boundary expects on a real system.
fn home_with_xdg_dirs() -> tempfile::TempDir {
    let home = tempfile::tempdir().unwrap();
    fs::create_dir_all(home.path().join(".local/state")).unwrap();
    fs::create_dir_all(home.path().join(".config")).unwrap();
    home
}

#[gpui_kit::test]
fn no_palette_installs_the_built_in_dark_theme(cx: &mut TestAppContext) {
    cx.update(|cx| {
        gpui_kit::init(cx);
        theme::install(ThemeSource::none(), cx);
        let expected = ThemeRegistry::global(cx).default_dark_theme().clone();
        let theme = Theme::global(cx);
        assert_eq!(theme.mode, ThemeMode::Dark);
        assert_eq!(theme.theme_name(), &expected.name);
        let status = ThemeStatus::global(cx).unwrap();
        assert_eq!(status.origin(), &ThemeOrigin::BuiltIn);
        assert_eq!(status.label().as_ref(), "Built-in dark");
    });
}

#[gpui_kit::test]
fn an_existing_palette_applies_before_the_first_frame(cx: &mut TestAppContext) {
    cx.background_executor.allow_parking();
    let home = home_with_xdg_dirs();
    let current = home.path().join(".local/state/omarchy/current");
    write_theme(&current.join("theme"), TOKYO);
    fs::write(current.join("theme.name"), "Tokyo Night\n").unwrap();
    cx.update(|cx| {
        gpui_kit::init(cx);
        theme::install(ThemeSource::system_from_home(home.path()), cx);
        assert_eq!(cx.theme().primary, accent(0x007a_a2f7));
        assert_eq!(cx.theme().chart_1, accent(0x007a_a2f7));
        assert_eq!(cx.theme().background, accent(0x001a_1b26));
        assert!(cx.theme().is_dark());
        // The Base layer (scrollbars, resize handles) was reprojected too.
        assert_eq!(
            gpui_kit::base::Theme::global(cx).tokens.colors.primary,
            accent(0x007a_a2f7)
        );
    });
    let status = status(cx);
    assert_eq!(status.label().as_ref(), "Omarchy · Tokyo Night");
    assert_eq!(status.origin(), &ThemeOrigin::Omarchy(current));
}

#[cfg(unix)]
#[gpui_kit::test]
fn follows_writes_symlink_swaps_and_ignores_corruption(cx: &mut TestAppContext) {
    cx.background_executor.allow_parking();
    let home = home_with_xdg_dirs();
    let themes = tempfile::tempdir().unwrap();
    let current = home.path().join(".local/state/omarchy/current");
    fs::create_dir_all(&current).unwrap();

    cx.update(|cx| {
        gpui_kit::init(cx);
        theme::install(ThemeSource::system_from_home(home.path()), cx);
    });
    assert_eq!(status(cx).origin(), &ThemeOrigin::BuiltIn);

    // Writing a palette switches to it.
    let first = themes.path().join("first");
    write_theme(&first, TOKYO);
    std::os::unix::fs::symlink(&first, current.join("theme")).unwrap();
    wait_until(cx, "first palette", |cx| primary(cx) == accent(0x007a_a2f7));
    assert!(matches!(status(cx).origin(), ThemeOrigin::Omarchy(_)));

    // An Omarchy theme switch replaces the `theme` symlink atomically.
    let second = themes.path().join("second");
    write_theme(&second, &TOKYO.replace("#7aa2f7", "#bb9af7"));
    std::os::unix::fs::symlink(&second, current.join("next")).unwrap();
    fs::rename(current.join("next"), current.join("theme")).unwrap();
    wait_until(cx, "symlink swap", |cx| primary(cx) == accent(0x00bb_9af7));

    // A corrupted palette keeps the theme that is showing.
    fs::write(second.join("colors.toml"), "background = 'broken").unwrap();
    for _ in 0..5 {
        cx.executor().advance_clock(Duration::from_millis(150));
        cx.run_until_parked();
        std::thread::sleep(Duration::from_millis(20));
    }
    assert_eq!(primary(cx), accent(0x00bb_9af7));
    assert!(matches!(status(cx).origin(), ThemeOrigin::Omarchy(_)));

    // An atomic save of a fixed palette recovers, and the name follows.
    let staged = second.join("colors.toml.tmp");
    fs::write(&staged, TOKYO.replace("#7aa2f7", "#123456")).unwrap();
    fs::rename(&staged, second.join("colors.toml")).unwrap();
    fs::write(current.join("theme.name"), "Custom").unwrap();
    wait_until(cx, "atomic save", |cx| {
        primary(cx) == accent(0x0012_3456) && status(cx).label().as_ref() == "Omarchy · Custom"
    });

    // Removing the theme returns to the built-in look.
    fs::remove_file(current.join("theme")).unwrap();
    wait_until(cx, "removal", |cx| {
        status(cx).origin() == &ThemeOrigin::BuiltIn
    });
}

#[gpui_kit::test]
fn the_state_directory_beats_legacy_while_it_exists(cx: &mut TestAppContext) {
    cx.background_executor.allow_parking();
    let home = home_with_xdg_dirs();
    let legacy = home.path().join(".config/omarchy/current");
    write_theme(&legacy.join("theme"), TOKYO);
    fs::write(legacy.join("theme.name"), "Legacy").unwrap();

    cx.update(|cx| {
        gpui_kit::init(cx);
        theme::install(ThemeSource::system_from_home(home.path()), cx);
    });
    assert_eq!(status(cx).label().as_ref(), "Omarchy · Legacy");

    let state = home.path().join(".local/state/omarchy/current");
    write_theme(&state.join("theme"), &TOKYO.replace("#7aa2f7", "#9ece6a"));
    fs::write(state.join("theme.name"), "Current").unwrap();
    wait_until(cx, "state wins", |cx| {
        status(cx).label().as_ref() == "Omarchy · Current" && primary(cx) == accent(0x009e_ce6a)
    });

    fs::remove_dir_all(&state).unwrap();
    wait_until(cx, "legacy again", |cx| {
        status(cx).label().as_ref() == "Omarchy · Legacy" && primary(cx) == accent(0x007a_a2f7)
    });
}
