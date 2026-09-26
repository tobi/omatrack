//! Trace colours, resolved from the gpui-component `Theme` tokens, in one of
//! two [`ColorMode`]s.
//!
//! **Lap colours** ([`ColorMode::Lap`], the default): roles, not palette
//! positions. The primary lap is `primary` (the Omarchy accent under
//! omatrack's theme bridge), the reference lap is `warning`, in every lane.
//! A channel that shares a lane with another (brake under throttle) stays in
//! the role colours too, one step quieter, so colour always answers "which
//! lap" (never a gain/loss colour: a red brake would read as Δ loss).
//!
//! **Channel colours** ([`ColorMode::Channel`]): each channel in its own hue
//! from the theme's named palette (speed `blue`, throttle `green`, brake
//! `red`, steering `yellow`, gear the foreground, anything else a chart
//! hue). The primary lap is the solid line and the fill; the reference is the
//! same hue, quieter ([`CHANNEL_REFERENCE_ALPHA`] over the background), so
//! the laps stay tellable apart by weight rather than hue.
//!
//! In both modes Δ gain is `success` and Δ loss is `danger`, and only Δ.
//! Grids come from `border`, labels from `muted_foreground`. Translucent
//! tokens are pre-mixed against `background` so strokes stay opaque: GPUI
//! composites path triangles with premultiplied blending, and an opaque
//! stroke cannot double-blend at a folded join. `channels.<key>.color` and
//! `reference_color` overrides (user data, carried by [`LaneStyle`]) win over
//! both modes.
//!
//! Geometry caches never key on colour: a theme or mode change only repaints.

use gpui_kit::Hsla;
use gpui_kit::component::Theme;

use crate::scene::LaneStyle;

/// Strength of a shared-lane channel's reference line against the lane
/// root's (full) reference colour.
const SHARED_REFERENCE_ALPHA: f32 = 0.6;
/// Strength of a shared-lane channel's primary line against the lane root's
/// (full) primary colour.
const SHARED_PRIMARY_ALPHA: f32 = 0.6;
/// Strength of the reference lap's line against its channel hue in
/// [`ColorMode::Channel`]: the same hue, clearly the quieter of the two.
pub const CHANNEL_REFERENCE_ALPHA: f32 = 0.4;
/// Alpha of the corner zone columns (the `muted` token) behind the traces.
pub const CORNER_BAND_ALPHA: f32 = 0.45;
/// Opacity of the gain/loss colours on an approximate (LOW confidence)
/// Δ: still a reading, not a verdict.
pub(crate) const APPROXIMATE_DELTA_EMPHASIS: f32 = 0.6;

/// Loss-role opacity at the quiet end of the heat ramp.
const HEAT_RAMP_FLOOR: f32 = 0.22;

/// What trace colour says: which lap, or which channel.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum ColorMode {
    /// Primary and reference lap roles in every lane.
    #[default]
    Lap,
    /// Each channel in its own hue; the reference quieter.
    Channel,
}

impl ColorMode {
    /// The other mode.
    pub fn toggled(self) -> Self {
        match self {
            Self::Lap => Self::Channel,
            Self::Channel => Self::Lap,
        }
    }

    /// "Lap colours" / "Channel colours".
    pub fn label(self) -> &'static str {
        match self {
            Self::Lap => "Lap colours",
            Self::Channel => "Channel colours",
        }
    }
}

/// Resolved colours of one trace frame.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct TracePalette {
    pub mode: ColorMode,
    pub background: Hsla,
    pub foreground: Hsla,
    pub primary: Hsla,
    pub reference: Hsla,
    pub gain: Hsla,
    pub loss: Hsla,
    /// Lane grid and dividers.
    pub grid: Hsla,
    /// Lane separators and the axis line.
    pub grid_strong: Hsla,
    /// The Δ lane's zero line (level with the reference lap).
    pub zero: Hsla,
    /// Axis and chrome labels.
    pub label: Hsla,
    /// The shared cursor.
    pub cursor: Hsla,
    /// The pointer hover line.
    pub hover: Hsla,
    /// Range selection band.
    pub selection: Hsla,
    /// Corner zone tint, through every lane.
    pub corner_band: Hsla,
    /// Out-of-lap mask over neighbouring laps.
    pub mask: Hsla,
    /// Dimming outside a focused corner.
    pub dim: Hsla,
    /// Δ line.
    pub delta_line: Hsla,
    /// The quiet end of the heat ramp ([`Self::heat`]).
    pub heat_quiet: Hsla,
    /// Floating labels (event hover).
    pub popover: Hsla,
    pub chart: [Hsla; 5],
    /// Channel hues of [`ColorMode::Channel`], from the theme's named
    /// palette: speed, throttle, brake, steering.
    pub blue: Hsla,
    pub green: Hsla,
    pub red: Hsla,
    pub yellow: Hsla,
}

/// Whether two colours read as one hue (both saturated, hues within
/// [`SAME_HUE`]).
fn same_hue(a: Hsla, b: Hsla) -> bool {
    let distance = (a.h - b.h).abs();
    a.s > 0.2 && b.s > 0.2 && distance.min(1.0 - distance) < SAME_HUE
}

/// Hue distance (0–1 turn) under which two colours read as the same hue.
const SAME_HUE: f32 = 0.04;

impl TracePalette {
    /// Lap colours from `theme`; see [`Self::with_mode`].
    pub fn from_theme(theme: &Theme) -> Self {
        let background = theme.background;
        let opaque = |color: Hsla| background.blend(color);
        let grid = opaque(theme.border.opacity(0.55));
        Self {
            mode: ColorMode::Lap,
            background,
            foreground: theme.foreground,
            primary: opaque(theme.primary),
            reference: opaque(theme.warning),
            gain: opaque(theme.success),
            loss: opaque(theme.danger),
            grid,
            grid_strong: opaque(theme.border),
            zero: opaque(theme.muted_foreground.opacity(0.55)),
            label: theme.muted_foreground,
            // The cursor is chrome, not data: never as loud as a lap.
            cursor: opaque(theme.foreground.opacity(0.7)),
            hover: theme.muted_foreground.opacity(0.9),
            selection: theme.primary.opacity(0.14),
            // The muted surface token at low alpha, behind the traces: a
            // zone reads as a quiet column, never as data.
            corner_band: theme.muted.opacity(CORNER_BAND_ALPHA),
            mask: background.opacity(0.62),
            dim: background.opacity(0.6),
            // Neutral: the Δ is neither lap; gain and loss colour its fill.
            delta_line: opaque(theme.muted_foreground),
            heat_quiet: opaque(theme.muted_foreground.opacity(0.28)),
            popover: theme.popover,
            chart: [
                opaque(theme.chart_1),
                opaque(theme.chart_2),
                opaque(theme.chart_3),
                opaque(theme.chart_4),
                opaque(theme.chart_5),
            ],
            // Speed's hue must not read as the primary role: where the
            // theme's blue is the accent, speed takes cyan.
            blue: opaque(if same_hue(theme.blue, theme.primary) {
                theme.cyan
            } else {
                theme.blue
            }),
            green: opaque(theme.green),
            red: opaque(theme.red),
            yellow: opaque(theme.yellow),
        }
    }

    /// The same colours in `mode`.
    pub fn with_mode(mut self, mode: ColorMode) -> Self {
        self.mode = mode;
        self
    }

    /// The loss ramp at `t` in `[0, 1]`: a dim tint of the loss role over
    /// the quiet muted tone at 0 (low loss still reads as the ramp, not as
    /// grey), the loss role (`danger`) at 1, opaque in between.
    pub fn heat(&self, t: f32) -> Hsla {
        let t = t.clamp(0.0, 1.0);
        self.heat_quiet.blend(
            self.loss
                .opacity(HEAT_RAMP_FLOOR + (1.0 - HEAT_RAMP_FLOOR) * t),
        )
    }

    /// A channel's own hue ([`ColorMode::Channel`]): speed blue, the
    /// throttles green, brake red, steering yellow, gear the foreground;
    /// any other channel a chart hue chosen by its key.
    pub fn channel_hue(&self, key: &str) -> Hsla {
        match key {
            "speed" => self.blue,
            "throttle" | "driver_throttle" => self.green,
            "brake" => self.red,
            "steering" => self.yellow,
            "gear" => self.background.blend(self.foreground.opacity(0.85)),
            _ => {
                let hash = key
                    .bytes()
                    .fold(0u32, |h, b| h.wrapping_mul(31).wrapping_add(b as u32));
                self.chart[hash as usize % self.chart.len()]
            }
        }
    }

    /// Primary and reference stroke colours of a channel. `root` is whether
    /// the channel owns its lane (see the module docs).
    pub fn channel_colors(&self, key: &str, root: bool, style: &LaneStyle) -> (Hsla, Hsla) {
        let (primary, reference) = match self.mode {
            ColorMode::Lap if root => (self.primary, self.reference),
            // Both laps keep their role hue, a step quieter than the lane
            // root's so the two lines of one lap remain tellable apart. Never
            // a hue mix: it interpolates hue (blue and amber made green).
            ColorMode::Lap => (
                self.background
                    .blend(self.primary.opacity(SHARED_PRIMARY_ALPHA)),
                self.background
                    .blend(self.reference.opacity(SHARED_REFERENCE_ALPHA)),
            ),
            // The hue names the channel, so a shared lane needs no shade.
            ColorMode::Channel => {
                let hue = self.channel_hue(key);
                (
                    hue,
                    self.background.blend(hue.opacity(CHANNEL_REFERENCE_ALPHA)),
                )
            }
        };
        (
            style
                .color
                .map(|c| self.background.blend(c))
                .unwrap_or(primary),
            style
                .reference_color
                .map(|c| self.background.blend(c))
                .unwrap_or(reference),
        )
    }

    /// Colour of the reference lap's value in a lane legend: its role in lap
    /// colours; in channel colours the hue is the channel's, so the value
    /// reads plainly beside its muted lap label.
    pub fn reference_value(&self, key: &str, root: bool, style: &LaneStyle) -> Hsla {
        match self.mode {
            ColorMode::Lap => {
                if root {
                    self.channel_colors(key, root, style).1
                } else {
                    self.reference
                }
            }
            ColorMode::Channel => self.foreground,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui_kit::TestAppContext;

    #[gpui_kit::test]
    fn roles_follow_theme_tokens(cx: &mut TestAppContext) {
        cx.update(gpui_kit::init);
        let palette = cx.update(|cx| TracePalette::from_theme(Theme::global(cx)));
        assert_eq!(palette.mode, ColorMode::Lap);
        assert_eq!(palette.primary.a, 1.0);
        assert_eq!(palette.reference.a, 1.0);
        assert_ne!(palette.primary, palette.reference);
        let style = LaneStyle::default();
        assert_eq!(
            palette.channel_colors("speed", true, &style),
            (palette.primary, palette.reference)
        );
        // A shared-lane channel keeps the primary hue, quieter, and never
        // takes a gain/loss colour.
        let brake = palette.channel_colors("brake", false, &style).0;
        assert!((brake.h - palette.primary.h).abs() < 0.02 || palette.primary.s < 0.05);
        for role in [
            palette.gain,
            palette.loss,
            palette.primary,
            palette.reference,
        ] {
            assert_ne!(brake, role);
        }
        // Its reference line stays in the reference role: the reference
        // hue, quieter than the root's, never a blend towards another hue.
        let brake_reference = palette.channel_colors("brake", false, &style).1;
        assert!((brake_reference.h - palette.reference.h).abs() < 0.02);
        assert_ne!(brake_reference, palette.reference);
        assert_ne!(brake_reference, brake);
        // Overrides are user data and win.
        let custom_color = gpui_kit::hsla(0.5, 0.5, 0.5, 1.0);
        let custom = LaneStyle::default().with_color(Some(custom_color));
        assert_eq!(
            palette.channel_colors("speed", true, &custom).0,
            custom_color
        );
    }

    #[gpui_kit::test]
    fn channel_colours_give_each_channel_its_own_hue(cx: &mut TestAppContext) {
        cx.update(gpui_kit::init);
        let theme = cx.update(|cx| Theme::global(cx).clone());
        let palette = TracePalette::from_theme(&theme).with_mode(ColorMode::Channel);
        let style = LaneStyle::default();
        let speed = palette.channel_colors("speed", true, &style);
        let throttle = palette.channel_colors("throttle", true, &style);
        let brake = palette.channel_colors("brake", true, &style);
        let steering = palette.channel_colors("steering", true, &style);
        // The theme's named hues, opaque.
        let speed_hue = if same_hue(theme.blue, theme.primary) {
            theme.cyan
        } else {
            theme.blue
        };
        assert_eq!(speed.0, theme.background.blend(speed_hue));
        // Speed never takes the primary role's colour.
        assert!(!same_hue(speed.0, palette.primary));
        assert_eq!(throttle.0, theme.background.blend(theme.green));
        assert_eq!(brake.0, theme.background.blend(theme.red));
        assert_eq!(steering.0, theme.background.blend(theme.yellow));
        for (primary, reference) in [speed, throttle, brake, steering] {
            assert_eq!(primary.a, 1.0);
            // The reference is the same hue, quieter: never the reference
            // role, never the primary's own colour.
            assert_ne!(reference, primary);
            assert_ne!(reference, palette.reference);
            assert!((reference.h - primary.h).abs() < 0.02 || primary.s < 0.05);
        }
        // A shared lane's channel keeps its own hue at full strength.
        assert_eq!(palette.channel_colors("brake", false, &style), brake);
        // The mode changes colour only; overrides still win.
        let custom_color = gpui_kit::hsla(0.5, 0.5, 0.5, 1.0);
        let custom = LaneStyle::default().with_color(Some(custom_color));
        assert_eq!(
            palette.channel_colors("speed", true, &custom).0,
            custom_color
        );
        assert_eq!(ColorMode::Lap.toggled(), ColorMode::Channel);
        assert_eq!(ColorMode::Channel.toggled(), ColorMode::Lap);
    }

    #[gpui_kit::test]
    fn legend_values_follow_the_colour_mode(cx: &mut TestAppContext) {
        cx.update(gpui_kit::init);
        let theme = cx.update(|cx| Theme::global(cx).clone());
        let style = LaneStyle::default();
        // Lap colours: each lap's value in its role.
        let lap = TracePalette::from_theme(&theme);
        assert_eq!(lap.channel_colors("speed", true, &style).0, lap.primary);
        assert_eq!(lap.reference_value("speed", true, &style), lap.reference);
        // Channel colours: the primary's in the channel hue, the
        // reference's plainly in the foreground (its lap label says whose).
        let channel = lap.with_mode(ColorMode::Channel);
        assert_eq!(
            channel.channel_colors("brake", true, &style).0,
            channel.channel_hue("brake")
        );
        assert_eq!(
            channel.reference_value("brake", true, &style),
            channel.foreground
        );
    }
}
