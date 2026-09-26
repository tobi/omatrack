//! Interface fonts: the desktop's configured families, else bundled Inter
//! (interface text) and Geist Mono (numerics).
//!
//! gpui-component names the virtual `.SystemUIFont`, which on Linux lands on
//! whatever fontconfig happens to resolve (often Liberation or DejaVu Sans)
//! and a monospace family that is frequently missing. Omatrack instead ships
//! two families embedded in the binary and registers them at startup, so the
//! interface looks the same on every machine that has not chosen otherwise.
//!
//! "Chosen otherwise" means a user fontconfig file (the place Omarchy and
//! most desktops write their font choice) that prefers a family for
//! `system-ui` / `sans-serif` (interface) or `monospace` (numerics), and that
//! family is installed. Distribution-wide defaults under `/etc` are not a
//! choice and are ignored. A preferred family that is not installed falls
//! back to the bundled one, never to a family GPUI cannot find.

use std::borrow::Cow;
use std::path::{Path, PathBuf};

use gpui_kit::{App, Global, SharedString};

/// The bundled interface family.
pub const BUNDLED_UI_FAMILY: &str = "Inter";
/// The bundled monospace family, for numerics and identifiers.
pub const BUNDLED_MONO_FAMILY: &str = "Geist Mono";

/// The embedded faces: the weights the interface uses (regular, medium,
/// semibold, and bold for rich text), static builds so every renderer draws
/// them without variation support. OFL 1.1; see `assets/fonts/*-OFL.txt`.
const BUNDLED_FACES: &[&[u8]] = &[
    include_bytes!("../../assets/fonts/Inter-Regular.ttf"),
    include_bytes!("../../assets/fonts/Inter-Medium.ttf"),
    include_bytes!("../../assets/fonts/Inter-SemiBold.ttf"),
    include_bytes!("../../assets/fonts/Inter-Bold.ttf"),
    include_bytes!("../../assets/fonts/GeistMono-Regular.ttf"),
    include_bytes!("../../assets/fonts/GeistMono-Medium.ttf"),
    include_bytes!("../../assets/fonts/GeistMono-SemiBold.ttf"),
];

/// Where the desktop's font choice is read from: user fontconfig files, in
/// the order fontconfig loads them (a later file wins).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct FontSource {
    files: Vec<PathBuf>,
}

impl FontSource {
    /// The user's fontconfig, from `$HOME` and `$XDG_CONFIG_HOME`; see
    /// [`FontSource::system_from`].
    pub fn system() -> Self {
        let home = std::env::var_os("HOME").map(PathBuf::from);
        let config_home = std::env::var_os("XDG_CONFIG_HOME").map(PathBuf::from);
        Self::system_from(home, config_home)
    }

    /// The user fontconfig files for a given `home` and `$XDG_CONFIG_HOME`
    /// (default `~/.config`): `fontconfig/conf.d/*.conf` (listed when read),
    /// then `fontconfig/fonts.conf`, then the legacy `~/.fonts.conf`. Empty
    /// or relative paths are ignored.
    pub fn system_from(home: Option<PathBuf>, config_home: Option<PathBuf>) -> Self {
        let home = home.filter(|home| home.is_absolute());
        let config_home = config_home
            .filter(|path| path.is_absolute())
            .or_else(|| home.as_ref().map(|home| home.join(".config")));
        let mut files = Vec::new();
        if let Some(config_home) = config_home {
            files.push(config_home.join("fontconfig/conf.d"));
            files.push(config_home.join("fontconfig/fonts.conf"));
        }
        if let Some(home) = home {
            files.push(home.join(".fonts.conf"));
        }
        Self { files }
    }

    /// Exactly these fontconfig files (a directory stands for its `*.conf`).
    pub fn files(files: impl IntoIterator<Item = PathBuf>) -> Self {
        Self {
            files: files.into_iter().collect(),
        }
    }

    /// No desktop choice: always the bundled families.
    pub fn none() -> Self {
        Self::default()
    }

    /// Read the configured families now. Blocking file I/O on a few small
    /// files: startup only.
    pub fn configured(&self) -> ConfiguredFonts {
        let mut configured = ConfiguredFonts::default();
        for path in &self.files {
            for file in conf_files(path) {
                if let Ok(contents) = std::fs::read_to_string(&file) {
                    configured.merge(parse_fontconfig(&contents));
                }
            }
        }
        configured
    }
}

/// `path` itself, or a directory's `*.conf` files in name order.
fn conf_files(path: &Path) -> Vec<PathBuf> {
    let Ok(entries) = std::fs::read_dir(path) else {
        return vec![path.to_path_buf()];
    };
    let mut files: Vec<PathBuf> = entries
        .filter_map(|entry| entry.ok().map(|entry| entry.path()))
        .filter(|file| file.extension().is_some_and(|ext| ext == "conf"))
        .collect();
    files.sort();
    files
}

/// Families a fontconfig configuration prefers for the generic names.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ConfiguredFonts {
    system_ui: Option<String>,
    sans: Option<String>,
    mono: Option<String>,
}

impl ConfiguredFonts {
    /// The configured interface family: `system-ui`, else `sans-serif`.
    pub fn ui(&self) -> Option<&str> {
        self.system_ui.as_deref().or(self.sans.as_deref())
    }

    /// The configured `monospace` family.
    pub fn mono(&self) -> Option<&str> {
        self.mono.as_deref()
    }

    /// Later configuration wins, slot by slot.
    fn merge(&mut self, later: ConfiguredFonts) {
        if later.system_ui.is_some() {
            self.system_ui = later.system_ui;
        }
        if later.sans.is_some() {
            self.sans = later.sans;
        }
        if later.mono.is_some() {
            self.mono = later.mono;
        }
    }

    fn set(&mut self, generic: &str, family: String) {
        match generic.trim().to_ascii_lowercase().as_str() {
            "system-ui" => self.system_ui = Some(family),
            "sans-serif" | "sans" => self.sans = Some(family),
            "monospace" | "mono" => self.mono = Some(family),
            _ => {}
        }
    }
}

/// The generic-family preferences in one fontconfig file. Understands the
/// two forms desktops write:
///
/// - `<alias><family>monospace</family><prefer><family>X</family>…`
/// - `<match target="pattern"><test name="family"><string>monospace</string>
///   </test><edit name="family" …><string>X</string></edit></match>`
///
/// Anything else is ignored; a malformed file yields what it could read.
pub fn parse_fontconfig(xml: &str) -> ConfiguredFonts {
    let xml = strip_comments(xml);
    let mut configured = ConfiguredFonts::default();
    // Document order, so a later rule in the file wins.
    let mut rules: Vec<(usize, &str, &str)> = Vec::new();
    for body in elements(&xml, "alias") {
        rules.push((offset(&xml, body), "alias", body));
    }
    for body in elements(&xml, "match") {
        rules.push((offset(&xml, body), "match", body));
    }
    rules.sort_by_key(|(at, _, _)| *at);
    for (_, kind, body) in rules {
        let rule = match kind {
            "alias" => alias_rule(body),
            _ => match_rule(body),
        };
        if let Some((generic, family)) = rule {
            configured.set(&generic, family);
        }
    }
    configured
}

/// `(generic, preferred)` of an `<alias>` body.
fn alias_rule(body: &str) -> Option<(String, String)> {
    let generic = elements(body, "family").next()?;
    let prefer = elements(body, "prefer").next()?;
    let family = elements(prefer, "family").next()?;
    Some((unescape(generic), unescape(family)))
}

/// `(generic, family)` of a `<match>` body that edits the family.
fn match_rule(body: &str) -> Option<(String, String)> {
    let test = tagged(body, "test").find(|(open, _)| names_family(open))?.1;
    let edit = tagged(body, "edit").find(|(open, _)| names_family(open))?.1;
    let generic = elements(test, "string").next()?;
    let family = elements(edit, "string").next()?;
    Some((unescape(generic), unescape(family)))
}

fn names_family(open_tag: &str) -> bool {
    open_tag.contains("name=\"family\"") || open_tag.contains("name='family'")
}

/// The bodies of every `<tag …>…</tag>` in `xml` (not nested in itself).
fn elements<'a>(xml: &'a str, tag: &'a str) -> impl Iterator<Item = &'a str> + 'a {
    tagged(xml, tag).map(|(_, body)| body)
}

/// `(open tag, body)` of every `<tag …>…</tag>` in `xml`.
fn tagged<'a>(xml: &'a str, tag: &'a str) -> impl Iterator<Item = (&'a str, &'a str)> + 'a {
    let open = format!("<{tag}");
    let close = format!("</{tag}>");
    let mut rest = xml;
    std::iter::from_fn(move || {
        loop {
            let start = rest.find(&open)?;
            let after = &rest[start + open.len()..];
            // `<family>` must not match `<familyx>`.
            let boundary = after.chars().next()?;
            if !(boundary == '>' || boundary.is_whitespace() || boundary == '/') {
                rest = after;
                continue;
            }
            let head_end = after.find('>')?;
            let open_tag = &after[..head_end];
            if open_tag.ends_with('/') {
                rest = &after[head_end + 1..];
                continue;
            }
            let body_start = &after[head_end + 1..];
            let body_end = body_start.find(&close)?;
            rest = &body_start[body_end + close.len()..];
            return Some((open_tag, &body_start[..body_end]));
        }
    })
}

fn offset(outer: &str, inner: &str) -> usize {
    inner.as_ptr() as usize - outer.as_ptr() as usize
}

fn strip_comments(xml: &str) -> String {
    let mut out = String::with_capacity(xml.len());
    let mut rest = xml;
    while let Some(start) = rest.find("<!--") {
        out.push_str(&rest[..start]);
        match rest[start..].find("-->") {
            Some(end) => rest = &rest[start + end + 3..],
            None => return out,
        }
    }
    out.push_str(rest);
    out
}

fn unescape(text: &str) -> String {
    text.trim()
        .replace("&lt;", "<")
        .replace("&gt;", ">")
        .replace("&quot;", "\"")
        .replace("&apos;", "'")
        .replace("&amp;", "&")
}

/// Where an active family came from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FontOrigin {
    /// The desktop's fontconfig preference.
    Desktop,
    /// Embedded in Omatrack.
    Bundled,
}

/// One active family.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FontFamily {
    name: SharedString,
    origin: FontOrigin,
}

impl FontFamily {
    pub fn name(&self) -> &SharedString {
        &self.name
    }

    pub fn origin(&self) -> FontOrigin {
        self.origin
    }
}

/// The active interface and monospace families; set by
/// [`install`](super::install) before the first theme change.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ThemeFonts {
    ui: FontFamily,
    mono: FontFamily,
}

impl Global for ThemeFonts {}

impl ThemeFonts {
    /// Bundled Inter and Geist Mono.
    pub fn bundled() -> Self {
        Self {
            ui: FontFamily {
                name: BUNDLED_UI_FAMILY.into(),
                origin: FontOrigin::Bundled,
            },
            mono: FontFamily {
                name: BUNDLED_MONO_FAMILY.into(),
                origin: FontOrigin::Bundled,
            },
        }
    }

    /// The configured families that are installed, bundled ones otherwise.
    /// `installed` is the text system's family list; names match without
    /// regard to ASCII case and resolve to the installed spelling.
    pub fn select(configured: &ConfiguredFonts, installed: &[String]) -> Self {
        let pick = |wanted: Option<&str>, bundled: &str| {
            wanted
                .and_then(|wanted| {
                    installed
                        .iter()
                        .find(|name| name.eq_ignore_ascii_case(wanted))
                })
                .map(|name| FontFamily {
                    name: name.clone().into(),
                    origin: FontOrigin::Desktop,
                })
                .unwrap_or_else(|| FontFamily {
                    name: bundled.to_owned().into(),
                    origin: FontOrigin::Bundled,
                })
        };
        Self {
            ui: pick(configured.ui(), BUNDLED_UI_FAMILY),
            mono: pick(configured.mono(), BUNDLED_MONO_FAMILY),
        }
    }

    pub fn ui(&self) -> &FontFamily {
        &self.ui
    }

    pub fn mono(&self) -> &FontFamily {
        &self.mono
    }

    /// `Inter · Geist Mono`.
    pub fn label(&self) -> SharedString {
        format!("{} · {}", self.ui.name, self.mono.name).into()
    }

    /// One sentence on where the families came from, for a tooltip.
    pub fn description(&self) -> SharedString {
        let origin = |family: &FontFamily| match family.origin {
            FontOrigin::Desktop => "from the desktop font settings",
            FontOrigin::Bundled => "bundled",
        };
        format!(
            "Interface font {} ({}); numbers in {} ({}).",
            self.ui.name,
            origin(&self.ui),
            self.mono.name,
            origin(&self.mono)
        )
        .into()
    }

    /// The fonts of the running application; `None` before `install`.
    pub fn global(cx: &App) -> Option<&Self> {
        cx.try_global::<Self>()
    }
}

/// Add the embedded Inter and Geist Mono faces to GPUI's text system (a
/// failure is logged; the theme then falls back to the platform default).
pub(super) fn register_bundled(cx: &App) {
    let faces = BUNDLED_FACES
        .iter()
        .map(|face| Cow::Borrowed(*face))
        .collect();
    if let Err(error) = cx.text_system().add_fonts(faces) {
        log::warn!("Cannot register the bundled fonts: {error:#}");
    }
}

/// Register the bundled faces (once per application) and choose the
/// families. The result is also the [`ThemeFonts`] global.
pub(super) fn install_fonts(source: &FontSource, cx: &mut App) -> ThemeFonts {
    struct Registered;
    impl Global for Registered {}
    if !cx.has_global::<Registered>() {
        register_bundled(cx);
        cx.set_global(Registered);
    }
    let configured = source.configured();
    let fonts = if configured.ui().is_none() && configured.mono().is_none() {
        ThemeFonts::bundled()
    } else {
        ThemeFonts::select(&configured, &cx.text_system().all_font_names())
    };
    cx.set_global(fonts.clone());
    fonts
}

#[cfg(test)]
mod tests {
    use super::*;

    const ALIAS_CONF: &str = r#"<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <!-- <alias><family>monospace</family><prefer><family>Commented</family></prefer></alias> -->
  <alias>
    <family>sans-serif</family>
    <prefer><family>Liberation Sans</family></prefer>
  </alias>
  <alias binding="same">
    <family>monospace</family>
    <prefer>
      <family>JetBrainsMono Nerd Font</family>
      <family>Noto Sans Mono</family>
    </prefer>
  </alias>
  <alias><family>serif</family><prefer><family>Liberation Serif</family></prefer></alias>
</fontconfig>"#;

    const MATCH_CONF: &str = r#"<fontconfig>
  <match target="pattern">
    <test qual="any" name="family"><string>monospace</string></test>
    <edit name="family" mode="assign" binding="same"><string>Berkeley Mono</string></edit>
  </match>
  <match target="pattern">
    <test name="family"><string>system-ui</string></test>
    <edit name='family' mode="prepend"><string>IBM Plex Sans &amp; Co</string></edit>
  </match>
</fontconfig>"#;

    #[test]
    fn alias_preferences_name_the_interface_and_mono_families() {
        let configured = parse_fontconfig(ALIAS_CONF);
        assert_eq!(configured.ui(), Some("Liberation Sans"));
        assert_eq!(configured.mono(), Some("JetBrainsMono Nerd Font"));
    }

    #[test]
    fn match_edits_are_read_and_system_ui_beats_sans_serif() {
        let mut configured = parse_fontconfig(ALIAS_CONF);
        configured.merge(parse_fontconfig(MATCH_CONF));
        assert_eq!(configured.ui(), Some("IBM Plex Sans & Co"));
        assert_eq!(configured.mono(), Some("Berkeley Mono"));
    }

    #[test]
    fn nothing_configured_reads_as_empty() {
        assert_eq!(parse_fontconfig(""), ConfiguredFonts::default());
        assert_eq!(
            parse_fontconfig("<fontconfig><dir>~/fonts</dir>"),
            ConfiguredFonts::default()
        );
        assert_eq!(
            parse_fontconfig("not xml at all <alias>"),
            ConfiguredFonts::default()
        );
    }

    #[test]
    fn installed_configured_families_win_and_missing_ones_fall_back() {
        let configured = parse_fontconfig(ALIAS_CONF);
        let installed = ["liberation sans".to_owned(), "Adwaita Mono".to_owned()];
        let fonts = ThemeFonts::select(&configured, &installed);
        assert_eq!(fonts.ui().name().as_ref(), "liberation sans");
        assert_eq!(fonts.ui().origin(), FontOrigin::Desktop);
        // The Nerd Font is configured but not installed.
        assert_eq!(fonts.mono().name().as_ref(), BUNDLED_MONO_FAMILY);
        assert_eq!(fonts.mono().origin(), FontOrigin::Bundled);
        assert_eq!(fonts.label().as_ref(), "liberation sans · Geist Mono");
    }

    #[test]
    fn system_files_follow_fontconfig_load_order() {
        let source = FontSource::system_from(Some("/home/u".into()), None);
        assert_eq!(
            source,
            FontSource::files([
                PathBuf::from("/home/u/.config/fontconfig/conf.d"),
                PathBuf::from("/home/u/.config/fontconfig/fonts.conf"),
                PathBuf::from("/home/u/.fonts.conf"),
            ])
        );
        let xdg = FontSource::system_from(Some("/home/u".into()), Some("/cfg".into()));
        assert_eq!(xdg.files[1], PathBuf::from("/cfg/fontconfig/fonts.conf"));
        assert_eq!(
            FontSource::system_from(Some("rel".into()), Some("rel".into())),
            FontSource::none()
        );
    }

    #[test]
    fn a_user_conf_d_and_fonts_conf_are_read_in_order() {
        let home = tempfile::tempdir().unwrap();
        let config = home.path().join(".config/fontconfig");
        std::fs::create_dir_all(config.join("conf.d")).unwrap();
        std::fs::write(config.join("conf.d/10-mono.conf"), MATCH_CONF).unwrap();
        std::fs::write(config.join("fonts.conf"), ALIAS_CONF).unwrap();
        let configured = FontSource::system_from_home_for_tests(home.path()).configured();
        // fonts.conf loads after conf.d, so its monospace alias wins; its
        // sans-serif never beats conf.d's system-ui.
        assert_eq!(configured.mono(), Some("JetBrainsMono Nerd Font"));
        assert_eq!(configured.ui(), Some("IBM Plex Sans & Co"));
    }

    #[test]
    fn the_bundled_faces_carry_the_bundled_family_names() {
        let utf16 =
            |name: &str| -> Vec<u8> { name.encode_utf16().flat_map(u16::to_be_bytes).collect() };
        let contains = |face: &[u8], needle: &[u8]| face.windows(needle.len()).any(|w| w == needle);
        let (inter, geist) = BUNDLED_FACES.split_at(4);
        assert!(
            inter
                .iter()
                .all(|face| contains(face, &utf16(BUNDLED_UI_FAMILY)))
        );
        assert!(
            geist
                .iter()
                .all(|face| contains(face, &utf16(BUNDLED_MONO_FAMILY)))
        );
    }

    impl FontSource {
        fn system_from_home_for_tests(home: &Path) -> Self {
            Self::system_from(Some(home.to_path_buf()), None)
        }
    }
}
