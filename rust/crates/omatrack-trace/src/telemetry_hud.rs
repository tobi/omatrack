//! `TelemetryHud`: the broadcast-style telemetry overlay of fullscreen video
//! (port of the Qt `VideoTelemetryHud`).
//!
//! One band, 1000:210, drawn in design units scaled by `width / 1000`:
//!
//! - a scrolling track-progress window over 10% of the lap, the playhead at
//!   86% (33% in continuous playback, where the traces lead); throttle and
//!   brake, the primary solid in the channel hues, the reference thin in
//!   the reference role, placed through the shared alignment map exactly
//!   like the traces;
//! - brake and throttle pedal bars (the reference level as a tick);
//! - a steering dial with primary and reference ring notches;
//! - the gear, coloured by the gear delta against the reference, with a
//!   shift arrow; the speed, tinted by the speed delta;
//! - the ±8 m gap bar, only when the caller supplies a gap (both GPS fixes
//!   better than 1 m, `alignment::relative_along_track_meters`).
//!
//! Performance: the static scale scans (brake maximum, throttle scale) run
//! once per selection in [`TelemetryHudData::new`]. A frame samples the
//! cursor, decimates the window into retained buffers ([`crate::decimate`],
//! NaN a pen-up) and strokes it with [`crate::mesh::stroke`], never
//! `PathBuilder`; value labels are formatted and shaped only when their
//! value changes. Colours are theme tokens ([`TelemetryHudColors`]).

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::component::Theme;
use gpui_kit::{
    App, Bounds, Element, ElementId, FontWeight, GlobalElementId, Hsla, InspectorElementId,
    IntoElement, LayoutId, Pixels, ShapedLine, SharedString, Style, Window, fill, point, px,
    relative, size,
};
use omatrack_core::Comparison;
use omatrack_core::monotonic::interpolate_fraction;
use omatrack_core::unify::UnifiedLap;
use omatrack_ui::{MINUS, TypeStep, format_gap, gap_position};

use crate::decimate::{DecimateParams, PathPoint, PlotRect, decimate};
use crate::label;
use crate::lanes::PathBuffer;
use crate::mesh::{TriangleSink as _, stroke};
use crate::palette::TracePalette;
use crate::scene::FractionMap as _;

/// Height / width of the band.
pub const HUD_ASPECT: f32 = 0.21;
/// The share of the lap the progress window shows.
pub const WINDOW_FRACTION: f64 = 0.10;
/// Where the playhead sits in the window.
pub const MARKER: f64 = 0.86;
/// Where the playhead sits in continuous playback (the viewport's anchor).
pub const CONTINUOUS_MARKER: f64 = 0.33;
/// Share of the stage width the band takes before [`HUD_SCALE`].
const WIDTH_SHARE: f32 = 0.72;
/// The Qt overlay's scale factor.
const HUD_SCALE: f32 = 0.65;
/// Steering beyond this (degrees) pins the notch.
const STEERING_LIMIT: f64 = 180.0;
/// Speed deltas below this (km/h) leave the speed untinted.
const SPEED_TINT_FROM: f64 = 1.0;
/// Speed deltas beyond `SPEED_TINT_FROM + SPEED_TINT_SPAN` tint fully.
const SPEED_TINT_SPAN: f64 = 4.0;

// Design units (the band is 1000 x 210).
const BAND_RIGHT: f32 = 902.;
const BAND_TOP: f32 = 35.;
const BAND_BOTTOM: f32 = 175.;
const GRAPH: [f32; 4] = [52., 39., 630., 132.];
const PEDAL_X: f32 = 714.;
const PEDAL_STEP: f32 = 30.;
const PEDAL_WIDTH: f32 = 24.;
const DIAL: [f32; 2] = [902., 105.];
const DIAL_RADIUS: f32 = 70.;
const DIAL_RING: f32 = 10.;
const GAP_TRACK: [f32; 4] = [826., 184., 152., 10.];

/// The band's size on a stage `stage_width` wide (the Qt rule:
/// `0.65 * min(W - 16, 1000, max(520, 0.72 W))`, 1000:210).
pub fn hud_size(stage_width: f32) -> (f32, f32) {
    let unscaled = (stage_width - 16.)
        .min(1000.)
        .min((stage_width * WIDTH_SHARE).max(520.))
        .max(0.);
    let width = unscaled * HUD_SCALE;
    (width, width * HUD_ASPECT)
}

/// The playhead's share of the progress window.
pub fn marker(continuous: bool) -> f64 {
    if continuous {
        CONTINUOUS_MARKER
    } else {
        MARKER
    }
}

/// What the HUD reads: the primary lap, the comparison of the pair (its
/// reference and the one map), and the per-selection scales.
pub struct TelemetryHudData {
    primary: Arc<UnifiedLap>,
    comparison: Option<Arc<Comparison>>,
    brake_max: f64,
    throttle_scale: f64,
}

impl TelemetryHudData {
    /// `comparison` must be the pair's comparison for `primary` (or `None`
    /// for a single lap). Scans the scales once.
    pub fn new(primary: Arc<UnifiedLap>, comparison: Option<Arc<Comparison>>) -> Self {
        let finite_max = |values: &[f64]| {
            values
                .iter()
                .copied()
                .filter(|v| v.is_finite())
                .fold(f64::NEG_INFINITY, f64::max)
        };
        let reference = comparison.as_ref().map(|c| c.reference().clone());
        let brake_max = [
            1.0,
            finite_max(&primary.brake),
            reference
                .as_ref()
                .map_or(f64::NEG_INFINITY, |r| finite_max(&r.brake)),
        ]
        .into_iter()
        .fold(f64::NEG_INFINITY, f64::max);
        let throttle_scale = if finite_max(&primary.throttle) > 1.5 {
            100.0
        } else {
            1.0
        };
        Self {
            primary,
            comparison,
            brake_max,
            throttle_scale,
        }
    }

    /// Whether this data was built from exactly these laps.
    pub fn is_for(&self, primary: &Arc<UnifiedLap>, comparison: Option<&Arc<Comparison>>) -> bool {
        Arc::ptr_eq(&self.primary, primary)
            && match (&self.comparison, comparison) {
                (Some(a), Some(b)) => Arc::ptr_eq(a, b),
                (None, None) => true,
                _ => false,
            }
    }

    pub fn brake_max(&self) -> f64 {
        self.brake_max
    }

    pub fn throttle_scale(&self) -> f64 {
        self.throttle_scale
    }

    fn reference(&self) -> Option<&UnifiedLap> {
        self.comparison.as_ref().map(|c| c.reference().as_ref())
    }

    fn reference_fraction(&self, primary: f64) -> f64 {
        self.comparison
            .as_ref()
            .map_or(primary, |c| c.reference_fraction(primary))
    }

    /// Every readout at primary lap `fraction`.
    pub fn sample(&self, fraction: f64) -> HudSample {
        let fraction = if fraction.is_finite() {
            fraction.clamp(0.0, 1.0)
        } else {
            0.0
        };
        let primary = LapSample::at(&self.primary, fraction, self);
        let reference = self
            .reference()
            .map(|lap| LapSample::at(lap, self.reference_fraction(fraction), self));
        HudSample { primary, reference }
    }
}

/// One lap's readouts at one place; pedals normalized to `0..=1`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct LapSample {
    pub throttle: Option<f64>,
    pub brake: Option<f64>,
    pub steering: Option<f64>,
    pub speed: Option<f64>,
    pub gear: Option<i32>,
}

impl LapSample {
    fn at(lap: &UnifiedLap, fraction: f64, data: &TelemetryHudData) -> Self {
        let value = |values: &[f64]| {
            (values.len() >= 2)
                .then(|| interpolate_fraction(values, fraction))
                .filter(|v| v.is_finite())
        };
        let gear = (!lap.gear.is_empty())
            .then(|| {
                let ix = (fraction * (lap.gear.len() - 1) as f64).round() as usize;
                lap.gear[ix.min(lap.gear.len() - 1)]
            })
            .filter(|gear| *gear >= -1);
        Self {
            throttle: value(&lap.throttle).map(|v| (v / data.throttle_scale).clamp(0.0, 1.0)),
            brake: value(&lap.brake).map(|v| (v / data.brake_max).clamp(0.0, 1.0)),
            steering: value(&lap.steering),
            speed: value(&lap.speed),
            gear,
        }
    }
}

/// Both laps' readouts at the cursor.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct HudSample {
    pub primary: LapSample,
    pub reference: Option<LapSample>,
}

impl HudSample {
    /// Primary minus reference gear.
    pub fn gear_delta(&self) -> Option<i32> {
        Some(self.primary.gear? - self.reference?.gear?)
    }

    /// Primary minus reference speed, km/h.
    pub fn speed_delta(&self) -> Option<f64> {
        Some(self.primary.speed? - self.reference?.speed?)
    }
}

/// The colours of one HUD frame, from theme tokens.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct TelemetryHudColors {
    /// The band behind everything: the theme surface, translucent.
    pub scrim: Hsla,
    /// Hairline around the band and the dial.
    pub rim: Hsla,
    /// The steering ring.
    pub ring: Hsla,
    /// The graph and pedal wells.
    pub well: Hsla,
    pub grid: Hsla,
    pub foreground: Hsla,
    pub muted: Hsla,
    pub throttle: Hsla,
    pub brake: Hsla,
    pub reference: Hsla,
    pub gain: Hsla,
    pub loss: Hsla,
}

impl TelemetryHudColors {
    pub fn from_theme(theme: &Theme) -> Self {
        let palette = TracePalette::from_theme(theme);
        Self {
            // The band reads as a panel both over a picture and on the
            // black letterbox: the theme's surface, with darker wells.
            scrim: theme.background.opacity(0.9),
            well: gpui_kit::black().opacity(0.5),
            grid: theme.foreground.opacity(0.08),
            rim: theme.foreground.opacity(0.12),
            ring: theme.foreground.opacity(0.07),
            foreground: theme.foreground,
            muted: theme.muted_foreground,
            // Pedals read go/stop, the broadcast convention.
            throttle: theme.success,
            brake: theme.danger,
            reference: palette.reference,
            gain: palette.gain,
            loss: palette.loss,
        }
    }
}

/// A shaped label and the value it was shaped for.
#[derive(Default)]
struct CachedLabel {
    key: Option<(i64, u32, Hsla)>,
    line: ShapedLine,
}

impl CachedLabel {
    /// The line for `key` (a value identity), shaping `text()` only when the
    /// value, size or colour changed.
    fn get(
        &mut self,
        key: i64,
        size: Pixels,
        weight: FontWeight,
        color: Hsla,
        text: impl FnOnce() -> SharedString,
        window: &mut Window,
    ) -> &ShapedLine {
        let full = (key, size.as_f32().to_bits(), color);
        if self.key != Some(full) {
            self.line = label::shape(text(), size, weight, color, window);
            self.key = Some(full);
        }
        &self.line
    }
}

/// Retained buffers of one HUD (hold them in the owning view): nothing
/// allocates after warm-up.
#[derive(Default)]
pub struct TelemetryHudBuffers {
    points: Vec<PathPoint>,
    primary_throttle: PathBuffer,
    primary_brake: PathBuffer,
    reference_throttle: PathBuffer,
    reference_brake: PathBuffer,
    primary_notch: PathBuffer,
    reference_notch: PathBuffer,
    arrow: PathBuffer,
    notch: Vec<PathPoint>,
    gear: CachedLabel,
    speed: CachedLabel,
    unit: CachedLabel,
    legend_throttle: CachedLabel,
    legend_brake: CachedLabel,
    gap: CachedLabel,
}

/// One HUD frame. Fills the bounds it is laid out in (size it to
/// [`hud_size`]); the owner positions and drags it.
pub struct TelemetryHud {
    data: Arc<TelemetryHudData>,
    fraction: f64,
    marker: f64,
    gap: Option<f64>,
    colors: TelemetryHudColors,
    buffers: Rc<RefCell<TelemetryHudBuffers>>,
}

impl TelemetryHud {
    /// The HUD for `data` at primary lap `fraction`.
    pub fn new(
        data: Arc<TelemetryHudData>,
        fraction: f64,
        colors: TelemetryHudColors,
        buffers: Rc<RefCell<TelemetryHudBuffers>>,
    ) -> Self {
        Self {
            data,
            fraction: if fraction.is_finite() { fraction } else { 0.0 },
            marker: MARKER,
            gap: None,
            colors,
            buffers,
        }
    }

    /// The playhead's share of the window ([`marker`]).
    pub fn marker(mut self, marker: f64) -> Self {
        self.marker = marker.clamp(0.0, 1.0);
        self
    }

    /// Signed along-track metres to the reference car; `Some` only when both
    /// GPS fixes are better than 1 m.
    pub fn gap(mut self, metres: Option<f64>) -> Self {
        self.gap = metres.filter(|m| m.is_finite());
        self
    }
}

impl IntoElement for TelemetryHud {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for TelemetryHud {
    type RequestLayoutState = ();
    type PrepaintState = HudSample;

    fn id(&self) -> Option<ElementId> {
        None
    }

    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }

    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        let mut style = Style::default();
        style.size.width = relative(1.).into();
        style.size.height = relative(1.).into();
        (window.request_layout(style, [], cx), ())
    }

    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        _: &mut App,
    ) -> HudSample {
        let sample = self.data.sample(self.fraction);
        self.build(bounds, window.scale_factor() as f64, &sample);
        sample
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        sample: &mut HudSample,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.paint_frame(bounds, sample, window, cx);
    }
}

impl TelemetryHud {
    /// Decimate and mesh the window and the notches into the buffers.
    fn build(&self, bounds: Bounds<Pixels>, dpr: f64, sample: &HudSample) {
        let s = bounds.size.width.as_f32() as f64 / 1000.0;
        let mut buffers = self.buffers.borrow_mut();
        let TelemetryHudBuffers {
            points,
            primary_throttle,
            primary_brake,
            reference_throttle,
            reference_brake,
            primary_notch,
            reference_notch,
            arrow,
            notch,
            ..
        } = &mut *buffers;
        for buffer in [
            &mut *primary_throttle,
            &mut *primary_brake,
            &mut *reference_throttle,
            &mut *reference_brake,
            &mut *primary_notch,
            &mut *reference_notch,
            &mut *arrow,
        ] {
            buffer.clear();
        }
        if s <= 0.0 {
            return;
        }
        let data = &self.data;
        let inset = 4.0 * s;
        let rect = PlotRect::new(
            GRAPH[0] as f64 * s,
            GRAPH[1] as f64 * s + inset,
            GRAPH[2] as f64 * s,
            GRAPH[3] as f64 * s - 2.0 * inset,
        );
        let start = self.fraction - self.marker * WINDOW_FRACTION;
        let params = |y_span: f64| DecimateParams {
            x_start: start,
            x_span: WINDOW_FRACTION,
            rect,
            y_min: 0.0,
            y_span,
            dpr,
            ..DecimateParams::default()
        };
        let thin = (1.5 * s).max(1.0);
        let bold = (2.5 * s).max(1.5);
        if let Some(reference) = data.reference() {
            let map = |f: f64| data.reference_fraction(f);
            decimate(
                &reference.throttle,
                &map,
                &params(data.throttle_scale),
                points,
            );
            stroke(points, thin, reference_throttle);
            decimate(&reference.brake, &map, &params(data.brake_max), points);
            stroke(points, thin, reference_brake);
        }
        let identity = |f: f64| f;
        decimate(
            &data.primary.throttle,
            &identity,
            &params(data.throttle_scale),
            points,
        );
        stroke(points, bold, primary_throttle);
        decimate(
            &data.primary.brake,
            &identity,
            &params(data.brake_max),
            points,
        );
        stroke(points, bold, primary_brake);

        // Ring notches: from the rim inwards, at the steering angle.
        let centre = (DIAL[0] as f64 * s, DIAL[1] as f64 * s);
        let mut draw_notch = |steering: Option<f64>, width: f64, sink: &mut PathBuffer| {
            let Some(steering) = steering else {
                return;
            };
            let angle = steering.clamp(-STEERING_LIMIT, STEERING_LIMIT).to_radians();
            let (dx, dy) = (angle.sin(), -angle.cos());
            let at = |radius: f32| {
                let r = radius as f64 * s;
                PathPoint::new(centre.0 + dx * r, centre.1 + dy * r)
            };
            notch.clear();
            notch.push(at(DIAL_RADIUS));
            notch.push(at(DIAL_RADIUS - DIAL_RING));
            stroke(notch, width, sink);
        };
        draw_notch(
            sample.reference.and_then(|r| r.steering),
            3.0 * s,
            reference_notch,
        );
        draw_notch(sample.primary.steering, 5.0 * s, primary_notch);

        // The shift arrow beside the gear: up when the primary is a gear
        // higher than the reference, down when lower.
        if let Some(delta) = sample.gear_delta().filter(|d| *d != 0) {
            let (x, y) = (928.0 * s as f32, 89.0 * s as f32);
            let (half, rise) = (5.0 * s as f32, 4.5 * s as f32);
            let dir = if delta > 0 { -1.0 } else { 1.0 };
            arrow.triangle(
                [x - half, y - dir * rise],
                [x + half, y - dir * rise],
                [x, y + dir * rise],
            );
        }

        for buffer in [
            primary_throttle,
            primary_brake,
            reference_throttle,
            reference_brake,
            primary_notch,
            reference_notch,
            arrow,
        ] {
            buffer.finish();
        }
    }

    fn paint_frame(
        &self,
        bounds: Bounds<Pixels>,
        sample: &HudSample,
        window: &mut Window,
        cx: &mut App,
    ) {
        let s = bounds.size.width.as_f32() / 1000.0;
        if s <= 0.0 {
            return;
        }
        let c = self.colors;
        let origin = bounds.origin;
        let rect = |x: f32, y: f32, w: f32, h: f32| {
            Bounds::new(
                origin + point(px(x * s), px(y * s)),
                size(px(w * s), px(h * s)),
            )
        };
        let radius = px(4.0 * s);

        // Band and dial disc.
        window.paint_quad(
            fill(
                rect(0., BAND_TOP, BAND_RIGHT, BAND_BOTTOM - BAND_TOP),
                c.scrim,
            )
            .corner_radii(gpui_kit::Corners {
                top_left: radius,
                bottom_left: radius,
                top_right: px(0.),
                bottom_right: px(0.),
            })
            .border_widths(gpui_kit::Edges {
                top: px(1.),
                bottom: px(1.),
                left: px(1.),
                right: px(0.),
            })
            .border_color(c.rim),
        );
        let disc = |r: f32| rect(DIAL[0] - r, DIAL[1] - r, 2. * r, 2. * r);
        window.paint_quad(
            fill(disc(DIAL_RADIUS), c.scrim)
                .corner_radii(px(DIAL_RADIUS * s))
                .border_widths(px(1.))
                .border_color(c.rim),
        );
        // The steering ring, a shade above the band.
        window.paint_quad(
            fill(disc(DIAL_RADIUS - 1.), c.ring).corner_radii(px((DIAL_RADIUS - 1.) * s)),
        );
        window.paint_quad(
            fill(disc(DIAL_RADIUS - DIAL_RING), c.well)
                .corner_radii(px((DIAL_RADIUS - DIAL_RING) * s)),
        );

        // Graph well, quarter rules, traces, playhead.
        let graph = rect(GRAPH[0], GRAPH[1], GRAPH[2], GRAPH[3]);
        window.paint_quad(fill(graph, c.well).corner_radii(px(2.0 * s)));
        for quarter in 1..4 {
            let y = GRAPH[1] + GRAPH[3] * quarter as f32 / 4.0;
            window.paint_quad(fill(
                Bounds::new(
                    origin + point(px(GRAPH[0] * s), px((y * s).round())),
                    size(px(GRAPH[2] * s), px(1.)),
                ),
                c.grid,
            ));
        }
        {
            let buffers = self.buffers.borrow();
            for (buffer, color) in [
                (&buffers.reference_throttle, c.reference),
                (&buffers.reference_brake, c.reference.opacity(0.8)),
                (&buffers.primary_throttle, c.throttle),
                (&buffers.primary_brake, c.brake),
            ] {
                for path in buffer.translated(origin) {
                    window.paint_path(path, color);
                }
            }
        }
        let marker_x = ((GRAPH[0] + GRAPH[2] * self.marker as f32) * s).round();
        window.paint_quad(fill(
            Bounds::new(
                origin + point(px(marker_x), px(GRAPH[1] * s)),
                size(px(1.), px(GRAPH[3] * s)),
            ),
            c.foreground.opacity(0.6),
        ));

        // Legend in the gutter.
        let small = TypeStep::Caption.size(window).max(px(12.0 * s));
        let legend_x = |line: &ShapedLine| origin.x + px(26.0 * s) - line.width * 0.5;
        {
            let mut buffers = self.buffers.borrow_mut();
            let line = buffers.legend_throttle.get(
                0,
                small,
                FontWeight::MEDIUM,
                c.throttle,
                || "THR".into(),
                window,
            );
            let at = point(legend_x(line), origin.y + px(72.0 * s) - small * 0.6);
            label::paint(line, at, small * 1.2, window, cx);
            let line = buffers.legend_brake.get(
                0,
                small,
                FontWeight::MEDIUM,
                c.brake,
                || "BRK".into(),
                window,
            );
            let at = point(legend_x(line), origin.y + px(138.0 * s) - small * 0.6);
            label::paint(line, at, small * 1.2, window, cx);
        }

        // Pedals: brake then throttle, filled from the bottom.
        let pedals = [
            (
                sample.primary.brake,
                sample.reference.and_then(|r| r.brake),
                c.brake,
            ),
            (
                sample.primary.throttle,
                sample.reference.and_then(|r| r.throttle),
                c.throttle,
            ),
        ];
        for (i, (value, reference, color)) in pedals.into_iter().enumerate() {
            let x = PEDAL_X + i as f32 * PEDAL_STEP;
            let well = rect(x, GRAPH[1], PEDAL_WIDTH, GRAPH[3]);
            window.paint_quad(fill(well, c.well).corner_radii(px(2.0 * s)));
            let inner_h = GRAPH[3] - 4.0;
            if let Some(value) = value.filter(|v| *v > 0.0) {
                let h = inner_h * value as f32;
                window.paint_quad(fill(
                    rect(x + 2., GRAPH[1] + 2. + inner_h - h, PEDAL_WIDTH - 4., h),
                    color.opacity(0.9),
                ));
            }
            if let Some(reference) = reference {
                let y = GRAPH[1] + 2. + inner_h * (1.0 - reference as f32);
                window.paint_quad(fill(
                    Bounds::new(
                        origin + point(px(x * s), px(y * s - 1.)),
                        size(px(PEDAL_WIDTH * s), px(2.)),
                    ),
                    c.reference,
                ));
            }
        }

        // Steering notches.
        {
            let buffers = self.buffers.borrow();
            for path in buffers.reference_notch.translated(origin) {
                window.paint_path(path, c.reference);
            }
            for path in buffers.primary_notch.translated(origin) {
                window.paint_path(path, c.foreground);
            }
        }

        // Gear, shift arrow, speed.
        let gear_delta = sample.gear_delta().unwrap_or(0);
        let gear_color = match gear_delta.signum() {
            -1 => c.brake,
            1 => c.throttle,
            _ => c.foreground,
        };
        let speed_color = match sample.speed_delta() {
            Some(delta) if delta.abs() > SPEED_TINT_FROM => {
                let t = ((delta.abs() - SPEED_TINT_FROM) / SPEED_TINT_SPAN).min(1.0) as f32;
                let target = if delta > 0.0 { c.throttle } else { c.brake };
                mix(c.foreground, target, t)
            }
            _ => c.foreground,
        };
        let centre_x = origin.x + px(DIAL[0] * s);
        let mut buffers = self.buffers.borrow_mut();
        let gear_size = px(46.0 * s);
        let gear = sample.primary.gear;
        let line = buffers.gear.get(
            gear.map_or(i64::MIN, i64::from),
            gear_size,
            FontWeight::MEDIUM,
            gear_color,
            || omatrack_ui::format_gear(gear),
            window,
        );
        let at = point(
            centre_x - line.width * 0.5,
            origin.y + px(89.0 * s) - gear_size * 0.62,
        );
        label::paint(line, at, gear_size * 1.24, window, cx);
        for path in buffers.arrow.translated(origin) {
            window.paint_path(path, gear_color);
        }

        let unit_size = TypeStep::Caption.size(window).max(px(13.0 * s));
        let line = buffers.unit.get(
            0,
            unit_size,
            FontWeight::NORMAL,
            c.muted,
            || "km/h".into(),
            window,
        );
        let at = point(
            centre_x - line.width * 0.5,
            origin.y + px(144.0 * s) - unit_size * 0.6,
        );
        label::paint(line, at, unit_size * 1.2, window, cx);

        let speed_size = px(24.0 * s).max(TypeStep::Title.size(window));
        let speed = sample.primary.speed;
        let line = buffers.speed.get(
            speed.map_or(i64::MIN, |v| v.round() as i64),
            speed_size,
            FontWeight::NORMAL,
            speed_color,
            || match speed {
                Some(v) => SharedString::from(format!("{:.0}", v.max(0.0))),
                None => omatrack_ui::MISSING_VALUE.into(),
            },
            window,
        );
        let at = point(
            centre_x - line.width * 0.5,
            origin.y + px(120.0 * s) - speed_size * 0.6,
        );
        label::paint(line, at, speed_size * 1.2, window, cx);

        // Gap bar.
        if let Some(gap) = self.gap {
            let [x, y, w, h] = GAP_TRACK;
            window.paint_quad(
                fill(rect(x - 72., y - 4., w + 76., h + 8.), c.scrim).corner_radii(radius),
            );
            window.paint_quad(fill(rect(x, y, w, h), c.well).corner_radii(px(h * 0.5 * s)));
            let centre = x + w * 0.5;
            let end = centre + gap_position(gap) as f32 * w * 0.5;
            let (from, to) = (centre.min(end), centre.max(end));
            if to - from > 0.1 {
                window.paint_quad(fill(rect(from, y, to - from, h), c.reference.opacity(0.45)));
            }
            window.paint_quad(fill(
                Bounds::new(
                    origin + point(px((centre * s).round()), px(y * s)),
                    size(px(1.), px(h * s)),
                ),
                c.foreground,
            ));
            window.paint_quad(fill(
                Bounds::new(
                    origin + point(px(end * s - 2.), px((y - 2.) * s)),
                    size(px(4.), px((h + 4.) * s)),
                ),
                c.reference,
            ));
            // Positive: the reference is ahead, time is being lost.
            let color = if gap.abs() < 0.05 {
                c.foreground
            } else if gap > 0.0 {
                c.loss
            } else {
                c.gain
            };
            let gap_size = TypeStep::Caption.size(window).max(px(13.0 * s));
            let line = buffers.gap.get(
                (gap * 10.0).round() as i64,
                gap_size,
                FontWeight::NORMAL,
                color,
                || format_gap(gap),
                window,
            );
            let at = point(
                origin.x + px(820.0 * s) - line.width,
                origin.y + px(189.0 * s) - gap_size * 0.6,
            );
            label::paint(line, at, gap_size * 1.2, window, cx);
        }
    }
}

/// Linear mix in RGB (never a hue interpolation, which crosses hues).
fn mix(from: Hsla, to: Hsla, t: f32) -> Hsla {
    let (a, b) = (from.to_rgb(), to.to_rgb());
    let lerp = |x: f32, y: f32| x + (y - x) * t;
    gpui_kit::Rgba {
        r: lerp(a.r, b.r),
        g: lerp(a.g, b.g),
        b: lerp(a.b, b.b),
        a: lerp(a.a, b.a),
    }
    .into()
}

/// `+0.23` / `−0.23` (two decimals, the broadcast delta).
pub fn format_live_delta(seconds: f64) -> SharedString {
    if !seconds.is_finite() {
        return omatrack_ui::MISSING_VALUE.into();
    }
    let magnitude = format!("{:.2}", seconds.abs());
    if magnitude.bytes().all(|b| b == b'0' || b == b'.') {
        return magnitude.into();
    }
    let sign = if seconds < 0.0 { MINUS } else { '+' };
    format!("{sign}{magnitude}").into()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lap(throttle: f64, brake: f64, speed: f64, gear: i32, steering: f64) -> Arc<UnifiedLap> {
        let n = 101;
        let time: Vec<f64> = (0..n).map(|i| i as f64 * 0.02).collect();
        Arc::new(UnifiedLap {
            throttle: vec![throttle; n],
            brake: (0..n).map(|i| brake * i as f64 / 100.0).collect(),
            speed: vec![speed; n],
            gear: vec![gear; n],
            steering: vec![steering; n],
            distance: (0..n).map(|i| i as f64).collect(),
            time,
            ..UnifiedLap::default()
        })
    }

    #[test]
    fn the_band_follows_the_qt_size_rule() {
        let (w, h) = hud_size(1920.);
        assert!((w - 650.).abs() < 1e-3, "{w}");
        assert!((h - 650. * 0.21).abs() < 1e-3, "{h}");
        // Narrow stages: at least 520 before scaling, never wider than W-16.
        let (w, _) = hud_size(600.);
        assert!((w - 520. * 0.65).abs() < 1e-3, "{w}");
        let (w, _) = hud_size(400.);
        assert!((w - 384. * 0.65).abs() < 1e-3, "{w}");
        assert_eq!(marker(false), MARKER);
        assert_eq!(marker(true), CONTINUOUS_MARKER);
    }

    #[test]
    fn scales_are_scanned_once_and_pedals_normalize() {
        // Throttle in percent, brake in bar.
        let data = TelemetryHudData::new(lap(80.0, 60.0, 200.0, 5, 30.0), None);
        assert_eq!(data.throttle_scale(), 100.0);
        assert_eq!(data.brake_max(), 60.0);
        let sample = data.sample(0.5);
        assert!((sample.primary.throttle.unwrap() - 0.8).abs() < 1e-9);
        assert!((sample.primary.brake.unwrap() - 0.5).abs() < 1e-9);
        assert_eq!(sample.primary.gear, Some(5));
        assert_eq!(sample.reference, None);
        assert_eq!(sample.gear_delta(), None);
        // Fractions of one and a brake that never exceeds one bar.
        let data = TelemetryHudData::new(lap(0.5, 0.2, 100.0, 3, 0.0), None);
        assert_eq!(data.throttle_scale(), 1.0);
        assert_eq!(data.brake_max(), 1.0);
    }

    #[test]
    fn deltas_compare_with_the_reference_through_the_map() {
        let primary = lap(1.0, 10.0, 210.0, 5, 0.0);
        let reference = lap(1.0, 40.0, 204.0, 4, -90.0);
        let comparison = Arc::new(Comparison::new(
            primary.clone(),
            reference.clone(),
            omatrack_core::alignment::Strategy::LapPercentage,
            Vec::new(),
            0.0,
        ));
        let data = TelemetryHudData::new(primary.clone(), Some(comparison.clone()));
        // The brake scale is shared by both laps.
        assert_eq!(data.brake_max(), 40.0);
        assert!(data.is_for(&primary, Some(&comparison)));
        assert!(!data.is_for(&primary, None));
        let sample = data.sample(0.5);
        assert_eq!(sample.gear_delta(), Some(1));
        assert!((sample.speed_delta().unwrap() - 6.0).abs() < 1e-9);
        assert_eq!(sample.reference.unwrap().steering, Some(-90.0));
    }

    #[test]
    fn the_live_delta_reads_signed_with_two_decimals() {
        assert_eq!(format_live_delta(0.234).as_ref(), "+0.23");
        assert_eq!(format_live_delta(-1.5).as_ref(), "\u{2212}1.50");
        assert_eq!(format_live_delta(0.001).as_ref(), "0.00");
        assert_eq!(
            format_live_delta(f64::NAN).as_ref(),
            omatrack_ui::MISSING_VALUE
        );
    }
}
