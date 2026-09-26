//! The interface fonts through `theme::install`: the bundled families are
//! named on the theme whatever palette applies, and a desktop choice that is
//! not installed never replaces them.

use std::fs;

use gpui_kit::TestAppContext;
use gpui_kit::component::{ActiveTheme as _, Theme};
use omatrack_ui::theme::{
    self, BUNDLED_MONO_FAMILY, BUNDLED_UI_FAMILY, FontOrigin, FontSource, ThemeFonts, ThemeSource,
};

fn families(cx: &mut TestAppContext) -> (String, String) {
    cx.update(|cx| {
        let theme = cx.theme();
        (
            theme.font_family.to_string(),
            theme.mono_font_family.to_string(),
        )
    })
}

#[gpui_kit::test]
fn the_built_in_theme_uses_the_bundled_families(cx: &mut TestAppContext) {
    cx.update(|cx| {
        gpui_kit::init(cx);
        theme::install(ThemeSource::none(), cx);
        let fonts = ThemeFonts::global(cx).expect("installed");
        assert_eq!(fonts.ui().name().as_ref(), BUNDLED_UI_FAMILY);
        assert_eq!(fonts.ui().origin(), FontOrigin::Bundled);
        assert_eq!(fonts.mono().name().as_ref(), BUNDLED_MONO_FAMILY);
        assert_eq!(fonts.label().as_ref(), "Inter · Geist Mono");
        // The Base layer's typography follows the component theme.
        let tokens = Theme::global(cx).semantic_tokens();
        assert_eq!(tokens.typography.sans.as_ref(), BUNDLED_UI_FAMILY);
        assert_eq!(tokens.typography.mono.as_ref(), BUNDLED_MONO_FAMILY);
    });
    assert_eq!(
        families(cx),
        (BUNDLED_UI_FAMILY.to_owned(), BUNDLED_MONO_FAMILY.to_owned())
    );
}

#[gpui_kit::test]
fn an_omarchy_palette_keeps_the_chosen_families(cx: &mut TestAppContext) {
    let home = tempfile::tempdir().unwrap();
    let current = home.path().join(".local/state/omarchy/current/theme");
    fs::create_dir_all(&current).unwrap();
    fs::write(
        current.join("colors.toml"),
        "background = '#1a1b26'\nforeground = '#a9b1d6'\naccent = '#7aa2f7'\n",
    )
    .unwrap();
    cx.update(|cx| {
        gpui_kit::init(cx);
        theme::install(ThemeSource::system_from_home(home.path()), cx);
    });
    assert_eq!(
        families(cx),
        (BUNDLED_UI_FAMILY.to_owned(), BUNDLED_MONO_FAMILY.to_owned())
    );
}

#[gpui_kit::test]
fn a_configured_family_that_is_not_installed_falls_back(cx: &mut TestAppContext) {
    let dir = tempfile::tempdir().unwrap();
    let conf = dir.path().join("fonts.conf");
    fs::write(
        &conf,
        "<fontconfig><alias><family>monospace</family>\
         <prefer><family>No Such Mono</family></prefer></alias></fontconfig>",
    )
    .unwrap();
    cx.update(|cx| {
        gpui_kit::init(cx);
        theme::install(ThemeSource::none().fonts(FontSource::files([conf])), cx);
        let fonts = ThemeFonts::global(cx).expect("installed");
        assert_eq!(fonts.mono().name().as_ref(), BUNDLED_MONO_FAMILY);
        assert_eq!(fonts.mono().origin(), FontOrigin::Bundled);
    });
    assert_eq!(families(cx).1, BUNDLED_MONO_FAMILY);
}
