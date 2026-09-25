//! An Omarchy `colors.toml` palette and its projection onto the gpui-component
//! theme.
//!
//! Omarchy publishes the active desktop theme as a flat TOML table of hex
//! colors. Two shapes exist in the wild: the semantic one (`background`,
//! `foreground`, `accent`, `red`, `green`, ...) and the terminal one
//! (`color0` ... `color15` beside `background`, `foreground` and `accent`).
//! Both are accepted; a semantic key wins over its ANSI alias.
//!
//! The palette only carries colors the user chose. Every surface the theme
//! needs beyond them is a mix of those colors, so a switched desktop theme
//! reads as one system rather than a palette pasted onto another theme.
//! This file and its tests are the only place in the application where hex
//! literals are allowed.

use std::fmt;

use gpui_kit::component::{ThemeConfig, ThemeMode};
use gpui_kit::{Hsla, Rgba, SharedString};

/// Why a `colors.toml` could not become a palette.
#[derive(Debug, Clone, PartialEq, Eq)]
#[non_exhaustive]
pub enum ThemeLoadError {
    /// The file could not be read.
    Io { path: String, message: String },
    /// The file is not valid TOML.
    Toml(String),
    /// A key is present but is not a `#rrggbb` / `#rrggbbaa` string.
    InvalidColor { key: String },
    /// A required role is absent (named by its semantic key).
    MissingKey(&'static str),
    /// `mode` is present but is neither `dark` nor `light`.
    InvalidMode,
}

impl fmt::Display for ThemeLoadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Io { path, message } => write!(f, "{path}: {message}"),
            Self::Toml(message) => write!(f, "invalid TOML: {message}"),
            Self::InvalidColor { key } => write!(f, "{key} must be a #rrggbb or #rrggbbaa color"),
            Self::MissingKey(key) => write!(f, "missing required color `{key}`"),
            Self::InvalidMode => f.write_str("mode must be `dark` or `light`"),
        }
    }
}

impl std::error::Error for ThemeLoadError {}

/// An 8-bit sRGB color, kept as bytes so a palette compares and serializes
/// exactly.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct Srgb8 {
    r: u8,
    g: u8,
    b: u8,
    a: u8,
}

impl Srgb8 {
    const BLACK: Self = Self::opaque(0x00, 0x00, 0x00);
    const WHITE: Self = Self::opaque(0xff, 0xff, 0xff);

    const fn opaque(r: u8, g: u8, b: u8) -> Self {
        Self { r, g, b, a: 0xff }
    }

    /// Parse `#rrggbb` or `#rrggbbaa`.
    fn parse(text: &str) -> Option<Self> {
        let hex = text.trim().strip_prefix('#')?;
        if !(hex.len() == 6 || hex.len() == 8) || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
            return None;
        }
        let byte = |ix: usize| u8::from_str_radix(&hex[ix..ix + 2], 16).ok();
        Some(Self {
            r: byte(0)?,
            g: byte(2)?,
            b: byte(4)?,
            a: if hex.len() == 8 { byte(6)? } else { 0xff },
        })
    }

    /// `#rrggbb`, or `#rrggbbaa` when not opaque.
    pub(crate) fn hex(self) -> String {
        if self.a == 0xff {
            format!("#{:02x}{:02x}{:02x}", self.r, self.g, self.b)
        } else {
            format!("#{:02x}{:02x}{:02x}{:02x}", self.r, self.g, self.b, self.a)
        }
    }

    /// Linear interpolation in sRGB from `self` (amount 0) to `other` (1).
    fn mix(self, other: Self, amount: f32) -> Self {
        let lerp = |a: u8, b: u8| {
            let value = a as f32 + (b as f32 - a as f32) * amount;
            value.round().clamp(0., 255.) as u8
        };
        Self {
            r: lerp(self.r, other.r),
            g: lerp(self.g, other.g),
            b: lerp(self.b, other.b),
            a: lerp(self.a, other.a),
        }
    }

    /// WCAG relative luminance.
    fn luminance(self) -> f32 {
        let linear = |v: u8| {
            let v = v as f32 / 255.;
            if v <= 0.04045 {
                v / 12.92
            } else {
                ((v + 0.055) / 1.055).powf(2.4)
            }
        };
        0.2126 * linear(self.r) + 0.7152 * linear(self.g) + 0.0722 * linear(self.b)
    }

    /// WCAG contrast ratio.
    fn contrast(self, other: Self) -> f32 {
        let (a, b) = (self.luminance(), other.luminance());
        (a.max(b) + 0.05) / (a.min(b) + 0.05)
    }

    pub(crate) fn to_hsla(self) -> Hsla {
        Rgba {
            r: self.r as f32 / 255.,
            g: self.g as f32 / 255.,
            b: self.b as f32 / 255.,
            a: self.a as f32 / 255.,
        }
        .into()
    }
}

/// The text color for content drawn on `fill`: whichever of the palette's
/// own background and foreground contrasts more, or black/white when
/// neither reaches WCAG AA.
fn ink_on(fill: Srgb8, background: Srgb8, foreground: Srgb8) -> Srgb8 {
    let best = |a: Srgb8, b: Srgb8| {
        if fill.contrast(a) >= fill.contrast(b) {
            a
        } else {
            b
        }
    };
    let themed = best(background, foreground);
    if fill.contrast(themed) >= 4.5 {
        themed
    } else {
        best(Srgb8::BLACK, Srgb8::WHITE)
    }
}

/// An Omarchy palette: the user's colors plus the handful of surfaces derived
/// from them.
///
/// Built only through [`OmarchyPalette::from_colors_toml`], which validates
/// every required role; readers expose the resolved roles as GPUI colors.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OmarchyPalette {
    name: SharedString,
    mode: ThemeMode,
    background: Srgb8,
    foreground: Srgb8,
    accent: Srgb8,
    cursor: Srgb8,
    selection: Srgb8,
    surface: Srgb8,
    border: Srgb8,
    red: Srgb8,
    green: Srgb8,
    yellow: Srgb8,
    blue: Srgb8,
    magenta: Srgb8,
    cyan: Srgb8,
}

impl OmarchyPalette {
    /// Parse an Omarchy `colors.toml`.
    ///
    /// Required: `background`, `foreground`, `accent`, and the status roles
    /// `red`/`color1`, `yellow`/`color3`, `green`/`color2`. Optional roles
    /// (`blue`/`color4`, `magenta`/`color5`, `cyan`/`color6`,
    /// `muted`/`color8`, `selection_background`, `cursor`,
    /// `lighter_background`) are derived when absent. An explicit
    /// `mode = "dark" | "light"` wins; otherwise the palette is dark when its
    /// background is darker than its foreground.
    pub fn from_colors_toml(name: &str, contents: &str) -> Result<Self, ThemeLoadError> {
        let table: toml::Table =
            toml::from_str(contents).map_err(|error| ThemeLoadError::Toml(error.to_string()))?;
        let color = |keys: &[&str]| -> Result<Option<Srgb8>, ThemeLoadError> {
            for key in keys {
                if let Some(value) = table.get(*key) {
                    return value
                        .as_str()
                        .and_then(Srgb8::parse)
                        .map(Some)
                        .ok_or_else(|| ThemeLoadError::InvalidColor {
                            key: (*key).to_owned(),
                        });
                }
            }
            Ok(None)
        };
        let required = |keys: &[&'static str]| -> Result<Srgb8, ThemeLoadError> {
            color(keys)?.ok_or(ThemeLoadError::MissingKey(keys[0]))
        };

        let background = required(&["background"])?;
        let foreground = required(&["foreground"])?;
        let accent = required(&["accent"])?;
        let red = required(&["red", "color1"])?;
        let yellow = required(&["yellow", "color3"])?;
        let green = required(&["green", "color2"])?;

        let mode = match table.get("mode") {
            None => {
                if background.luminance() < foreground.luminance() {
                    ThemeMode::Dark
                } else {
                    ThemeMode::Light
                }
            }
            Some(toml::Value::String(mode)) if mode == "dark" => ThemeMode::Dark,
            Some(toml::Value::String(mode)) if mode == "light" => ThemeMode::Light,
            Some(_) => return Err(ThemeLoadError::InvalidMode),
        };

        let blue = color(&["blue", "color4"])?.unwrap_or(accent);
        Ok(Self {
            name: SharedString::from(name.trim().to_owned()),
            mode,
            background,
            foreground,
            accent,
            cursor: color(&["cursor"])?.unwrap_or(accent),
            selection: color(&["selection_background", "selection"])?
                .unwrap_or_else(|| background.mix(accent, 0.3)),
            surface: color(&["lighter_background"])?
                .unwrap_or_else(|| background.mix(foreground, 0.05)),
            border: color(&["muted", "color8"])?
                .unwrap_or_else(|| background.mix(foreground, 0.25)),
            red,
            green,
            yellow,
            blue,
            magenta: color(&["magenta", "color5"])?.unwrap_or_else(|| red.mix(blue, 0.5)),
            cyan: color(&["cyan", "color6"])?.unwrap_or_else(|| green.mix(blue, 0.5)),
        })
    }

    /// The theme name shown to the user (from `current/theme.name`).
    pub fn name(&self) -> &SharedString {
        &self.name
    }

    /// Dark or light, explicit or inferred from luminance.
    pub fn mode(&self) -> ThemeMode {
        self.mode
    }

    pub fn background(&self) -> Hsla {
        self.background.to_hsla()
    }

    pub fn foreground(&self) -> Hsla {
        self.foreground.to_hsla()
    }

    /// The Omarchy accent; it becomes the theme's `primary`.
    pub fn accent(&self) -> Hsla {
        self.accent.to_hsla()
    }

    /// The gpui-component theme this palette projects to.
    ///
    /// Colors the palette does not decide are left unset so gpui-component
    /// derives them from the ones it does (hover and active shades, button
    /// fills). Surfaces are quiet mixes of background and foreground, the
    /// radius is nearly square, and chart hues follow the palette.
    pub fn theme_config(&self) -> ThemeConfig {
        let bg = self.background;
        let fg = self.foreground;
        let tint = |amount: f32| bg.mix(fg, amount);
        let ink = |fill: Srgb8| ink_on(fill, bg, fg);

        let chrome = tint(0.035);
        let hover = tint(0.07);
        let muted = tint(0.08);
        let muted_foreground = tint(0.68);
        let subtle_border = tint(0.12);
        let active_border = bg.mix(self.accent, 0.6);

        let colors = [
            ("background", bg),
            ("foreground", fg),
            ("border", self.border),
            ("input.border", self.border),
            ("window.border", self.border),
            ("ring", self.accent),
            ("caret", self.cursor),
            ("link", self.accent),
            ("primary.background", self.accent),
            ("primary.foreground", ink(self.accent)),
            ("accent.background", bg.mix(self.accent, 0.18)),
            ("accent.foreground", fg),
            ("selection.background", self.selection),
            ("secondary.background", tint(0.06)),
            ("secondary.hover.background", hover),
            ("secondary.foreground", fg),
            ("muted.background", muted),
            ("muted.foreground", muted_foreground),
            ("popover.background", self.surface),
            ("popover.foreground", fg),
            ("group_box.background", tint(0.03)),
            ("description_list.label.background", chrome),
            ("danger.background", self.red),
            ("danger.foreground", ink(self.red)),
            ("warning.background", self.yellow),
            ("warning.foreground", ink(self.yellow)),
            ("success.background", self.green),
            ("success.foreground", ink(self.green)),
            ("info.background", self.blue),
            ("info.foreground", ink(self.blue)),
            ("chart.1", self.accent),
            ("chart.2", self.green),
            ("chart.3", self.yellow),
            ("chart.4", self.magenta),
            ("chart.5", self.cyan),
            ("chart.bullish", self.green),
            ("chart.bearish", self.red),
            ("list.background", bg),
            ("list.even.background", tint(0.02)),
            ("list.head.background", chrome),
            ("list.hover.background", hover),
            ("list.active.background", self.accent),
            ("list.active.border", active_border),
            ("table.background", bg),
            ("table.even.background", tint(0.02)),
            ("table.head.background", chrome),
            ("table.head.foreground", muted_foreground),
            ("table.hover.background", hover),
            ("table.active.background", self.accent),
            ("table.active.border", active_border),
            ("table.row.border", subtle_border),
            ("tab_bar.background", chrome),
            ("tab_bar.segmented.background", muted),
            ("tab.background", chrome),
            ("tab.foreground", muted_foreground),
            ("tab.active.background", bg),
            ("tab.active.foreground", fg),
            ("sidebar.background", chrome),
            ("sidebar.foreground", fg),
            ("sidebar.border", self.border),
            ("sidebar.accent.background", hover),
            ("sidebar.accent.foreground", fg),
            ("sidebar.primary.background", self.accent),
            ("sidebar.primary.foreground", ink(self.accent)),
            ("title_bar.background", chrome),
            ("title_bar.border", self.border),
            ("status_bar.background", chrome),
            ("status_bar.border", self.border),
            ("scrollbar.thumb.background", tint(0.25)),
            ("scrollbar.thumb.hover.background", tint(0.4)),
            ("base.red", self.red),
            ("base.green", self.green),
            ("base.yellow", self.yellow),
            ("base.blue", self.blue),
            ("base.magenta", self.magenta),
            ("base.cyan", self.cyan),
        ];

        let mut color_table = toml::Table::new();
        for (key, color) in colors {
            color_table.insert(key.to_owned(), toml::Value::String(color.hex()));
        }
        let mut table = toml::Table::new();
        table.insert("name".into(), toml::Value::String(self.name.to_string()));
        table.insert("mode".into(), toml::Value::String(self.mode.name().into()));
        table.insert("radius".into(), toml::Value::Integer(2));
        table.insert("radius.lg".into(), toml::Value::Integer(4));
        table.insert("colors".into(), toml::Value::Table(color_table));
        // Every key above is one of `ThemeConfig`'s serde names
        // (gpui-component theme/schema.rs); deserializing through serde is the
        // only way to reach its private `base.*` fields.
        toml::Value::Table(table)
            .try_into::<ThemeConfig>()
            .expect("the palette projection only writes known ThemeConfig keys")
    }

    #[cfg(test)]
    pub(crate) fn srgb(&self, role: &str) -> Srgb8 {
        match role {
            "background" => self.background,
            "foreground" => self.foreground,
            "accent" => self.accent,
            "selection" => self.selection,
            "surface" => self.surface,
            "border" => self.border,
            "red" => self.red,
            "green" => self.green,
            "yellow" => self.yellow,
            "blue" => self.blue,
            "magenta" => self.magenta,
            "cyan" => self.cyan,
            _ => panic!("unknown role {role}"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A terminal-style palette (Flexoki Light shape): ANSI keys only.
    const ANSI_LIGHT: &str = "background = '#fffcf0'\nforeground = '#100f0f'\naccent = '#205ea6'\n\
        color0 = '#100f0f'\ncolor1 = '#af3029'\ncolor2 = '#66800b'\ncolor3 = '#ad8301'\n\
        color4 = '#205ea6'\ncolor5 = '#a02f6f'\ncolor6 = '#24837b'\ncolor7 = '#6f6e69'\n\
        color8 = '#b7b5ac'\n";

    /// A semantic palette (Tokyo Night shape).
    const SEMANTIC_DARK: &str = "background = '#1a1b26'\nforeground = '#a9b1d6'\n\
        accent = '#7aa2f7'\ncursor = '#c0caf5'\nselection_background = '#33467c'\n\
        red = '#f7768e'\ngreen = '#9ece6a'\nyellow = '#e0af68'\nblue = '#7aa2f7'\n\
        magenta = '#bb9af7'\ncyan = '#7dcfff'\nmuted = '#414868'\n";

    fn hex(color: Srgb8) -> String {
        color.hex()
    }

    #[test]
    fn ansi_palette_reads_status_roles_from_color_keys() {
        let palette = OmarchyPalette::from_colors_toml("Flexoki Light", ANSI_LIGHT).unwrap();
        assert_eq!(palette.name().as_ref(), "Flexoki Light");
        assert_eq!(hex(palette.srgb("red")), "#af3029");
        assert_eq!(hex(palette.srgb("green")), "#66800b");
        assert_eq!(hex(palette.srgb("yellow")), "#ad8301");
        assert_eq!(hex(palette.srgb("blue")), "#205ea6");
        assert_eq!(hex(palette.srgb("magenta")), "#a02f6f");
        assert_eq!(hex(palette.srgb("cyan")), "#24837b");
        assert_eq!(hex(palette.srgb("border")), "#b7b5ac");
    }

    #[test]
    fn semantic_palette_preserves_explicit_roles() {
        let palette = OmarchyPalette::from_colors_toml("Tokyo Night", SEMANTIC_DARK).unwrap();
        assert_eq!(hex(palette.srgb("accent")), "#7aa2f7");
        assert_eq!(hex(palette.srgb("selection")), "#33467c");
        assert_eq!(hex(palette.srgb("border")), "#414868");
        assert_eq!(hex(palette.srgb("magenta")), "#bb9af7");
    }

    #[test]
    fn semantic_keys_win_over_ansi_aliases() {
        let source = format!("{ANSI_LIGHT}red = '#ff0000'\n");
        let palette = OmarchyPalette::from_colors_toml("Mixed", &source).unwrap();
        assert_eq!(hex(palette.srgb("red")), "#ff0000");
    }

    #[test]
    fn eight_digit_colors_keep_their_alpha() {
        let source = SEMANTIC_DARK.replace("#1a1b26", "#1a1b26cc");
        let palette = OmarchyPalette::from_colors_toml("Glass", &source).unwrap();
        assert_eq!(hex(palette.srgb("background")), "#1a1b26cc");
        let config = palette.theme_config();
        assert_eq!(config.colors.background.as_deref(), Some("#1a1b26cc"));
    }

    #[test]
    fn missing_required_roles_are_errors() {
        for (key, line) in [
            ("background", "background = '#1a1b26'\n"),
            ("foreground", "foreground = '#a9b1d6'\n"),
            ("accent", "accent = '#7aa2f7'\n"),
            ("red", "red = '#f7768e'\n"),
            ("green", "green = '#9ece6a'\n"),
            ("yellow", "yellow = '#e0af68'\n"),
        ] {
            let source = SEMANTIC_DARK.replace(line, "");
            assert_eq!(
                OmarchyPalette::from_colors_toml("Bad", &source),
                Err(ThemeLoadError::MissingKey(key)),
                "{key}"
            );
        }
    }

    #[test]
    fn malformed_palettes_are_rejected_without_partial_defaults() {
        for source in [
            "broken toml".to_owned(),
            "background = 42".to_owned(),
            SEMANTIC_DARK.replace("#7aa2f7", "#nope!!"),
            SEMANTIC_DARK.replace("#7aa2f7", "#7aa"),
            format!("{SEMANTIC_DARK}mode = 'automatic'\n"),
        ] {
            assert!(
                OmarchyPalette::from_colors_toml("Bad", &source).is_err(),
                "{source}"
            );
        }
    }

    #[test]
    fn mode_follows_luminance_unless_explicit() {
        let dark = OmarchyPalette::from_colors_toml("Dark", SEMANTIC_DARK).unwrap();
        assert_eq!(dark.mode(), ThemeMode::Dark);
        let light = OmarchyPalette::from_colors_toml("Light", ANSI_LIGHT).unwrap();
        assert_eq!(light.mode(), ThemeMode::Light);
        let forced = format!("{SEMANTIC_DARK}mode = 'light'\n");
        let forced = OmarchyPalette::from_colors_toml("Forced", &forced).unwrap();
        assert_eq!(forced.mode(), ThemeMode::Light);
        assert_eq!(forced.theme_config().mode, ThemeMode::Light);
    }

    #[test]
    fn accent_maps_to_primary_ring_and_first_chart_hue() {
        let config = OmarchyPalette::from_colors_toml("Tokyo Night", SEMANTIC_DARK)
            .unwrap()
            .theme_config();
        assert_eq!(config.name.as_ref(), "Tokyo Night");
        assert_eq!(config.mode, ThemeMode::Dark);
        let colors = &config.colors;
        assert_eq!(colors.primary.as_deref(), Some("#7aa2f7"));
        assert_eq!(colors.ring.as_deref(), Some("#7aa2f7"));
        assert_eq!(colors.chart_1.as_deref(), Some("#7aa2f7"));
        assert_eq!(colors.chart_2.as_deref(), Some("#9ece6a"));
        assert_eq!(colors.chart_3.as_deref(), Some("#e0af68"));
        assert_eq!(colors.chart_4.as_deref(), Some("#bb9af7"));
        assert_eq!(colors.chart_5.as_deref(), Some("#7dcfff"));
        assert_eq!(colors.danger.as_deref(), Some("#f7768e"));
        assert_eq!(colors.warning.as_deref(), Some("#e0af68"));
        assert_eq!(colors.success.as_deref(), Some("#9ece6a"));
        assert_eq!(colors.info.as_deref(), Some("#7aa2f7"));
        assert_eq!(colors.background.as_deref(), Some("#1a1b26"));
        assert_eq!(colors.border.as_deref(), Some("#414868"));
        assert_eq!(colors.selection.as_deref(), Some("#33467c"));
        assert_eq!(config.radius, Some(2));
    }

    #[test]
    fn text_on_status_fills_is_readable() {
        for source in [SEMANTIC_DARK, ANSI_LIGHT] {
            let palette = OmarchyPalette::from_colors_toml("Any", source).unwrap();
            let (bg, fg) = (palette.srgb("background"), palette.srgb("foreground"));
            for role in ["accent", "red", "green", "yellow", "blue"] {
                let fill = palette.srgb(role);
                assert!(ink_on(fill, bg, fg).contrast(fill) >= 4.5, "{role}");
            }
            // Supporting text stays legible on the main surface.
            let muted = bg.mix(fg, 0.68);
            assert!(muted.contrast(bg) >= 3.0, "muted text on background");
        }
    }

    #[test]
    fn mixing_and_hex_round_trip() {
        let black = Srgb8::parse("#000000").unwrap();
        let white = Srgb8::parse("#ffffff").unwrap();
        assert_eq!(black.mix(white, 0.5).hex(), "#808080");
        assert_eq!(Srgb8::parse("#12345678").unwrap().hex(), "#12345678");
        assert_eq!(Srgb8::parse("#123456ff").unwrap().hex(), "#123456");
        assert!(Srgb8::parse("123456").is_none());
    }
}
