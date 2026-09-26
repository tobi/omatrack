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
//! A channel that shares a lane with another (brake under throttle) stays
//! in the role colours too, one step quieter: the primary lap in a deeper
//! shade of the primary hue, the reference in a quieter reference. Colour
//! therefore always answers "which lap"; which channel is told by the
//! lane's legend and the line's shade, never by a third hue that means
//! neither lap (and never by a gain/loss colour: a red brake would read as
//! Δ loss). [`TracePalette::channel_hue`] (the chart tokens) is kept for
//! user-coloured overlays. `channels.<key>.color` and `reference_color`
//! overrides (user data, carried by [`LaneStyle`]) win over both.
//!
//! Geometry caches never key on colour: a theme change only repaints.

use gpui_kit::Hsla;
use gpui_kit::component::Theme;

use crate::scene::LaneStyle;

/// Strength of a shared-lane channel's reference line against the lane
/// root's (full) reference colour.
const SHARED_REFERENCE_ALPHA: f32 = 0.6;
/// Strength of a shared-lane channel's primary line against the lane root's
/// (full) primary colour.
const SHARED_PRIMARY_ALPHA: f32 = 0.6;

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
    /// The quiet end of the heat ramp ([`Self::heat`]).
    pub heat_quiet: Hsla,
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
            // The cursor is chrome, not data: never as loud as a lap.
            cursor: opaque(theme.foreground.opacity(0.7)),
            hover: theme.muted_foreground.opacity(0.9),
            selection: theme.primary.opacity(0.14),
            corner_band: theme.foreground.opacity(0.035),
            corner_edge: theme.muted_foreground.opacity(0.2),
            mask: background.opacity(0.62),
            dim: background.opacity(0.6),
            // Neutral: the Δ is neither lap; gain and loss colour its fill.
            delta_line: opaque(theme.muted_foreground),
            heat_quiet: opaque(theme.muted_foreground.opacity(0.28)),
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
    /// The loss ramp at `t` in `[0, 1]`: the quiet muted tone at 0, the
    /// loss role (`danger`) at 1, opaque in between.
    pub fn heat(&self, t: f32) -> Hsla {
        self.heat_quiet.blend(self.loss.opacity(t.clamp(0.0, 1.0)))
    }

    pub fn channel_hue(&self, key: &str) -> Hsla {
        match key {
            "speed" | "throttle" | "driver_throttle" => self.chart[1],
            "brake" => self.chart[3],
            "steering" => self.chart[4],
            _ => {
                let hash = key
                    .bytes()
                    .fold(0u32, |h, b| h.wrapping_mul(31).wrapping_add(b as u32));
                self.chart[hash as usize % self.chart.len()]
            }
        }
    }

    /// Primary and reference stroke colours of a channel. `root` is whether
    /// the channel owns its lane (see the module docs); the key is kept for
    /// callers that colour by channel.
    pub fn channel_colors(&self, _key: &str, root: bool, style: &LaneStyle) -> (Hsla, Hsla) {
        let (primary, reference) = if root {
            (self.primary, self.reference)
        } else {
            // Both laps keep their role hue, a step quieter than the lane
            // root's so the two lines of one lap remain tellable apart. Never
            // a hue mix: it interpolates hue (blue and amber made green).
            (
                self.background
                    .blend(self.primary.opacity(SHARED_PRIMARY_ALPHA)),
                self.background
                    .blend(self.reference.opacity(SHARED_REFERENCE_ALPHA)),
            )
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
}
