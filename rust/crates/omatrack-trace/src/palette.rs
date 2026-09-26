//! Trace colours, resolved from the gpui-component `Theme` tokens.
//!
//! Roles, not palette positions: the primary lap is `primary` (the Omarchy
//! accent under omatrack's theme bridge), the reference lap is `warning`,
//! Δ gain is `success` and Δ loss is `danger`. Grids come from `border`,
//! labels from `muted_foreground`. Translucent tokens are pre-mixed against
//! `background` so strokes stay opaque: GPUI composites path triangles with
//! premultiplied blending, and an opaque stroke cannot double-blend at a
//! folded join.
//!
//! Channel hues (`speed`/`throttle` success, `brake` danger, `steering`
//! warning, others `chart_1..5`) identify a channel that shares a lane with
//! another (throttle/brake); the lane's root channel keeps the role colours
//! so primary and reference read the same everywhere. `channels.<key>.color`
//! and `reference_color` overrides (user data, carried by [`LaneStyle`])
//! win over both.
//!
//! Geometry caches never key on colour: a theme change only repaints.

use gpui_kit::Hsla;
use gpui_kit::component::{Colorize as _, Theme};

use crate::scene::LaneStyle;

/// Resolved colours of one trace frame.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct TracePalette {
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
    /// Corner zone tint.
    pub corner_band: Hsla,
    /// Corner zone edges.
    pub corner_edge: Hsla,
    /// Out-of-lap mask over neighbouring laps.
    pub mask: Hsla,
    /// Dimming outside a focused corner.
    pub dim: Hsla,
    /// Δ line.
    pub delta_line: Hsla,
    pub chart: [Hsla; 5],
}

impl TracePalette {
    pub fn from_theme(theme: &Theme) -> Self {
        let background = theme.background;
        let opaque = |color: Hsla| background.blend(color);
        let grid = opaque(theme.border.opacity(0.55));
        Self {
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
            cursor: theme.foreground,
            hover: theme.muted_foreground.opacity(0.9),
            selection: theme.primary.opacity(0.14),
            corner_band: theme.foreground.opacity(0.035),
            corner_edge: theme.muted_foreground.opacity(0.2),
            mask: background.opacity(0.62),
            dim: background.opacity(0.6),
            delta_line: opaque(theme.foreground.opacity(0.85)),
            chart: [
                opaque(theme.chart_1),
                opaque(theme.chart_2),
                opaque(theme.chart_3),
                opaque(theme.chart_4),
                opaque(theme.chart_5),
            ],
        }
    }

    /// Default hue of a channel key.
    pub fn channel_hue(&self, key: &str) -> Hsla {
        match key {
            "speed" | "throttle" | "driver_throttle" => self.gain,
            "brake" => self.loss,
            "steering" => self.reference,
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
        let (primary, reference) = if root {
            (self.primary, self.reference)
        } else {
            let hue = self.channel_hue(key);
            (hue, hue.mix(self.reference, 0.45))
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
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui_kit::TestAppContext;

    #[gpui_kit::test]
    fn roles_follow_theme_tokens(cx: &mut TestAppContext) {
        cx.update(gpui_kit::init);
        let palette = cx.update(|cx| TracePalette::from_theme(Theme::global(cx)));
        assert_eq!(palette.primary.a, 1.0);
        assert_eq!(palette.reference.a, 1.0);
        assert_ne!(palette.primary, palette.reference);
        let style = LaneStyle::default();
        assert_eq!(
            palette.channel_colors("speed", true, &style),
            (palette.primary, palette.reference)
        );
        assert_eq!(
            palette.channel_colors("brake", false, &style).0,
            palette.loss
        );
        // Overrides are user data and win.
        let custom_color = gpui_kit::hsla(0.5, 0.5, 0.5, 1.0);
        let custom = LaneStyle::default().with_color(Some(custom_color));
        assert_eq!(
            palette.channel_colors("speed", true, &custom).0,
            custom_color
        );
    }
}
