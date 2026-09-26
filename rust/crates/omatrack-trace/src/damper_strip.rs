//! `DamperStrip`: the manual damper alignment tool (port of
//! `src/app/DamperStripView`).
//!
//! Both laps' front-damper traces in a window centred on the shared cursor
//! (±3 s by default; the wheel zooms from 1 s to the whole lap), each
//! autoscaled to what the window shows. Static ride height and fuel load
//! differ between laps, so a shared or whole-lap scale would flatten the
//! bumps the eye has to line up. The reference is drawn through the shared
//! primary → reference map ([`FractionMap`]), manual offset included, exactly
//! like the traces, delta and video: lining up the bumps here lines up
//! everything.
//!
//! Interaction contract:
//!
//! - left-drag slides the reference; every step previews locally and emits
//!   [`DamperStripEvent::OffsetChanged`] with the new absolute offset;
//! - ‹ / › nudge the reference one 20 ms sample earlier / later;
//! - double-click (or **Reset**) returns the offset to zero;
//! - the wheel zooms the window about the cursor.
//!
//! State ownership: the application owns the offset and the map. It applies
//! an [`DamperStripEvent::OffsetChanged`] by rebuilding the comparison
//! (`Analysis::with_manual_offset`, which takes the offset in primary lap
//! fraction) and hands the strip the new data through
//! [`DamperStrip::set_data`]. Until then the strip previews the requested
//! offset by shifting the lookup into the committed map, so a drag stays
//! fluid however long the rebuild takes.
//!
//! The plot is one custom element; its geometry is decimated
//! ([`crate::decimate`]) and meshed ([`crate::mesh`]) into reusable
//! [`PathBuffer`]s, never `PathBuilder`. Its placement is measured runtime
//! geometry (the documented `px` exception); colours come from
//! [`TracePalette`].

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::base::TestSupportExt as _;
use gpui_kit::component::button::{Button, ButtonVariants as _};
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, StyledExt as _, h_flex, v_flex,
};
use gpui_kit::{
    App, Bounds, Context, CursorStyle, DispatchPhase, Element, ElementId, Entity, EventEmitter,
    GlobalElementId, Hitbox, HitboxBehavior, InspectorElementId, InteractiveElement as _,
    IntoElement, LayoutId, MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent,
    ParentElement as _, Pixels, Render, Role, ScrollDelta, ScrollWheelEvent, SharedString,
    StatefulInteractiveElement as _, Style, Styled as _, Subscription, WeakEntity, Window, div,
    fill, point, prelude::FluentBuilder as _, px, relative, rems, size,
};

use omatrack_ui::TypeScale as _;

use crate::decimate::{DecimateParams, PathPoint, PlotRect, decimate};
use crate::interaction::{CLICK_SLOP, WheelDelta};
use crate::lanes::PathBuffer;
use crate::mesh::stroke;
use crate::palette::TracePalette;
use crate::scene::FractionMap;
use crate::stack::CHROME_REMS;
use crate::state::CursorState;

/// Narrowest window, seconds of the primary lap.
pub const MIN_WINDOW_SECONDS: f64 = 1.0;
/// Default window: ±3 s around the cursor.
pub const DEFAULT_WINDOW_SECONDS: f64 = 6.0;
/// One 50 Hz sample: the ‹ › nudge step.
pub const NUDGE_SECONDS: f64 = 0.02;
/// Stroke width of both traces, logical pixels.
const STROKE_WIDTH: f64 = 1.4;

/// What the strip draws: one front-damper series per lap, the lap length
/// and the committed alignment.
#[derive(Clone)]
pub struct DamperStripData {
    primary: Arc<[f64]>,
    reference: Arc<[f64]>,
    lap_seconds: f64,
    map: Option<Arc<dyn FractionMap>>,
    offset: f64,
}

impl DamperStripData {
    /// `primary` on the primary lap's 50 Hz grid, `reference` on the
    /// reference lap's grid; `lap_seconds` is the primary lap's duration.
    pub fn new(primary: Arc<[f64]>, reference: Arc<[f64]>, lap_seconds: f64) -> Self {
        Self {
            primary,
            reference,
            lap_seconds: if lap_seconds.is_finite() {
                lap_seconds.max(0.0)
            } else {
                0.0
            },
            map: None,
            offset: 0.0,
        }
    }

    /// The shared primary → reference map, with `offset` (primary lap
    /// fraction, as `Comparison::manual_offset`) already applied inside it.
    #[must_use]
    pub fn with_map(mut self, map: Option<Arc<dyn FractionMap>>, offset: f64) -> Self {
        self.map = map;
        self.offset = if offset.is_finite() { offset } else { 0.0 };
        self
    }

    pub fn lap_seconds(&self) -> f64 {
        self.lap_seconds
    }

    /// The committed manual offset, primary lap fraction.
    pub fn offset(&self) -> f64 {
        self.offset
    }

    fn reference_fraction(&self, primary: f64) -> f64 {
        let primary = primary.clamp(0.0, 1.0);
        self.map
            .as_ref()
            .map_or(primary, |map| map.reference_fraction(primary))
    }

    fn seconds_to_fraction(&self, seconds: f64) -> f64 {
        if self.lap_seconds > 0.0 {
            seconds / self.lap_seconds
        } else {
            0.0
        }
    }
}

/// User intent reported by a [`DamperStrip`].
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub enum DamperStripEvent {
    /// The requested manual offset, in seconds of the primary lap and in
    /// primary lap fraction (the unit `Analysis::with_manual_offset` takes).
    /// Positive moves the reference later.
    OffsetChanged { seconds: f64, fraction: f64 },
}

/// Window span in primary lap fraction for a window of `seconds`, clamped
/// to `[MIN_WINDOW_SECONDS, lap]` (the whole lap when the lap is unknown).
pub fn window_fraction(seconds: f64, lap_seconds: f64) -> f64 {
    if lap_seconds <= 0.0 {
        return 1.0;
    }
    (clamp_window(seconds, lap_seconds) / lap_seconds).min(1.0)
}

/// Clamp a window length to `[MIN_WINDOW_SECONDS, lap_seconds]`.
pub fn clamp_window(seconds: f64, lap_seconds: f64) -> f64 {
    let seconds = if seconds.is_finite() {
        seconds
    } else {
        DEFAULT_WINDOW_SECONDS
    };
    let upper = if lap_seconds > 0.0 {
        lap_seconds.max(MIN_WINDOW_SECONDS)
    } else {
        f64::INFINITY
    };
    seconds.clamp(MIN_WINDOW_SECONDS, upper)
}

/// Autoscale of one trace over the window: 8% padding, a unit span around a
/// flat signal (port of the Qt `range` lambda). Returns `(low, span)`.
#[expect(
    clippy::cast_possible_truncation,
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "Clamped lap fractions map to indices in resident sample buffers; interpolation intentionally uses f64."
)]
#[expect(
    clippy::neg_cmp_op_on_partial_ord,
    reason = "Negated ordered comparisons deliberately include unordered (NaN) values; preserve that behavior."
)]
pub fn window_range(values: &[f64], from: f64, to: f64) -> (f64, f64) {
    let mut lo = f64::INFINITY;
    let mut hi = f64::NEG_INFINITY;
    if values.len() >= 2 {
        let last = (values.len() - 1) as f64;
        let a = (from.clamp(0.0, 1.0) * last) as usize;
        let b = (to.clamp(0.0, 1.0) * last).ceil() as usize;
        for value in values.iter().take(b + 1).skip(a) {
            if value.is_finite() {
                lo = lo.min(*value);
                hi = hi.max(*value);
            }
        }
    }
    if !(hi > lo) {
        return (if lo.is_finite() { lo - 0.5 } else { 0.0 }, 1.0);
    }
    let pad = 0.08 * (hi - lo);
    (lo - pad, hi - lo + 2.0 * pad)
}

/// A drag of the reference in progress.
#[derive(Clone, Copy)]
struct Drag {
    press_x: f64,
    from_offset: f64,
    moved: bool,
}

/// Reusable decimation and mesh buffers (no allocation after warm-up).
#[derive(Default)]
struct Buffers {
    points: Vec<PathPoint>,
    primary: PathBuffer,
    reference: PathBuffer,
}

/// The damper strip row. See the module docs.
pub struct DamperStrip {
    cursor: Entity<CursorState>,
    data: Option<Arc<DamperStripData>>,
    window_seconds: f64,
    /// Requested offset (primary lap fraction) not yet in the committed map.
    draft: Option<f64>,
    drag: Option<Drag>,
    buffers: Rc<RefCell<Buffers>>,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<DamperStripEvent> for DamperStrip {}

impl DamperStrip {
    pub fn new(cursor: Entity<CursorState>, cx: &mut Context<'_, Self>) -> Self {
        let subscriptions = vec![cx.observe(&cursor, |_, _, cx| cx.notify())];
        Self {
            cursor,
            data: None,
            window_seconds: DEFAULT_WINDOW_SECONDS,
            draft: None,
            drag: None,
            buffers: Rc::default(),
            _subscriptions: subscriptions,
        }
    }

    /// Replace the traces and the committed alignment. A pending preview is
    /// dropped unless a drag is still in progress.
    pub fn set_data(&mut self, data: Option<Arc<DamperStripData>>, cx: &mut Context<'_, Self>) {
        self.data = data;
        if self.drag.is_none() {
            self.draft = None;
        }
        let lap = self.lap_seconds();
        self.window_seconds = clamp_window(self.window_seconds, lap);
        cx.notify();
    }

    pub fn data(&self) -> Option<&Arc<DamperStripData>> {
        self.data.as_ref()
    }

    fn lap_seconds(&self) -> f64 {
        self.data.as_ref().map_or(0.0, |d| d.lap_seconds)
    }

    /// Visible span in primary lap seconds.
    pub fn window_seconds(&self) -> f64 {
        self.window_seconds
    }

    pub fn set_window_seconds(&mut self, seconds: f64, cx: &mut Context<'_, Self>) {
        let seconds = clamp_window(seconds, self.lap_seconds());
        if (seconds - self.window_seconds).abs() > 1e-9 {
            self.window_seconds = seconds;
            cx.notify();
        }
    }

    /// Multiply the window by `factor` (<1 zooms in), clamped to 1 s … lap.
    pub fn zoom(&mut self, factor: f64, cx: &mut Context<'_, Self>) {
        if factor.is_finite() && factor > 0.0 {
            self.set_window_seconds(self.window_seconds * factor, cx);
        }
    }

    /// The offset shown (a pending request, else the committed one), in
    /// primary lap fraction.
    pub fn offset(&self) -> f64 {
        self.draft
            .or_else(|| self.data.as_ref().map(|d| d.offset))
            .unwrap_or(0.0)
    }

    /// [`Self::offset`] in seconds of the primary lap.
    pub fn offset_seconds(&self) -> f64 {
        self.offset() * self.lap_seconds()
    }

    /// Request an absolute offset in seconds.
    pub fn set_offset_seconds(&mut self, seconds: f64, cx: &mut Context<'_, Self>) {
        let Some(data) = &self.data else {
            return;
        };
        let fraction = data.seconds_to_fraction(seconds);
        self.request(fraction, cx);
    }

    /// Move the reference by `steps` 20 ms samples (negative: earlier).
    pub fn nudge(&mut self, steps: i32, cx: &mut Context<'_, Self>) {
        let seconds = self.offset_seconds() + f64::from(steps) * NUDGE_SECONDS;
        self.set_offset_seconds(seconds, cx);
    }

    /// Back to no manual offset.
    pub fn reset_offset(&mut self, cx: &mut Context<'_, Self>) {
        self.request(0.0, cx);
    }

    fn request(&mut self, fraction: f64, cx: &mut Context<'_, Self>) {
        if !fraction.is_finite() || self.data.is_none() {
            return;
        }
        if (fraction - self.offset()).abs() < 1e-12 {
            return;
        }
        self.draft = Some(fraction);
        let seconds = fraction * self.lap_seconds();
        cx.emit(DamperStripEvent::OffsetChanged { seconds, fraction });
        cx.notify();
    }

    /// Lap fraction the window is centred on.
    fn centre(&self, cx: &App) -> f64 {
        self.cursor.read(cx).fraction().unwrap_or(0.0)
    }

    fn window_span(&self) -> f64 {
        window_fraction(self.window_seconds, self.lap_seconds())
    }

    fn pointer_down(&mut self, x: f64, click_count: usize, cx: &mut Context<'_, Self>) {
        if self.data.is_none() {
            return;
        }
        if click_count >= 2 {
            self.drag = None;
            self.reset_offset(cx);
            return;
        }
        self.drag = Some(Drag {
            press_x: x,
            from_offset: self.offset(),
            moved: false,
        });
        cx.notify();
    }

    fn pointer_move(&mut self, x: f64, width: f64, cx: &mut Context<'_, Self>) {
        let Some(mut drag) = self.drag else {
            return;
        };
        let dx = x - drag.press_x;
        if !drag.moved && dx.abs() < CLICK_SLOP {
            return;
        }
        drag.moved = true;
        self.drag = Some(drag);
        let shift = dx / width.max(1.0) * self.window_span();
        self.request(drag.from_offset + shift, cx);
    }

    fn pointer_up(&mut self, cx: &mut Context<'_, Self>) {
        if self.drag.take().is_some() {
            cx.notify();
        }
    }

    fn wheel(&mut self, delta: WheelDelta, cx: &mut Context<'_, Self>) {
        let motion = if delta.y.abs() >= delta.x.abs() {
            delta.y
        } else {
            delta.x
        };
        if motion != 0.0 {
            self.zoom(0.8f64.powf(motion / 120.0), cx);
        }
    }

    fn spoken_offset(&self) -> SharedString {
        format_offset(self.offset_seconds()).into()
    }
}

/// `+0.120 s`, `−0.040 s`, `±0.000 s`.
pub fn format_offset(seconds: f64) -> String {
    if !seconds.is_finite() || seconds.abs() < 0.0005 {
        return "±0.000 s".into();
    }
    format!(
        "{} s",
        omatrack_ui::format_delta(Some(seconds), 3, omatrack_ui::DeltaSense::default()).0
    )
}

impl Render for DamperStrip {
    #[expect(
        clippy::too_many_lines,
        reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
    )]
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let theme = cx.theme();
        let palette = TracePalette::from_theme(theme);
        let (muted, foreground, border) = (theme.muted_foreground, theme.foreground, theme.border);
        let has_data = self.data.is_some();
        let offset = self.offset();
        let committed = self.data.as_ref().map_or(0.0, |d| d.offset);
        let plot = DamperPlot {
            strip: cx.entity().downgrade(),
            data: self.data.clone(),
            centre: self.centre(cx),
            span: self.window_span(),
            preview: offset - committed,
            dragging: self.drag.is_some_and(|d| d.moved),
            palette,
            buffers: self.buffers.clone(),
        };
        let label: SharedString = format!("Reference offset {}", self.spoken_offset()).into();
        h_flex()
            .id("damper-strip")
            .w_full()
            .h_16()
            .flex_shrink_0()
            .items_stretch()
            .border_b_1()
            .border_color(border)
            .child(
                v_flex()
                    .w(rems(CHROME_REMS))
                    .flex_shrink_0()
                    .px_2()
                    .py_1()
                    .gap_0p5()
                    .justify_center()
                    .border_r_1()
                    .border_color(border)
                    .child(
                        h_flex()
                            .gap_1()
                            .child(
                                div()
                                    .flex_1()
                                    .min_w_0()
                                    .truncate()
                                    .text_label()
                                    .font_medium()
                                    .text_color(foreground)
                                    .child("Front dampers"),
                            )
                            .child(
                                Button::new("damper-reset")
                                    .ghost()
                                    .xsmall()
                                    .label("Reset")
                                    .accessibility_label("Reset reference offset")
                                    .disabled(!has_data || offset.abs() < 1e-12)
                                    .on_click(cx.listener(|this, _, _, cx| this.reset_offset(cx))),
                            ),
                    )
                    .child(
                        h_flex()
                            .gap_1()
                            .child(
                                div()
                                    .id("damper-offset")
                                    .role(Role::Status)
                                    .aria_label(label.clone())
                                    .test_support()
                                    .flex_1()
                                    .min_w_0()
                                    .truncate()
                                    .text_label()
                                    .numeric()
                                    .text_color(if has_data { foreground } else { muted })
                                    .child(self.spoken_offset()),
                            )
                            .child(
                                Button::new("damper-nudge-earlier")
                                    .ghost()
                                    .xsmall()
                                    .label("‹")
                                    .accessibility_label("Nudge reference earlier")
                                    .tooltip("Nudge reference 20 ms earlier")
                                    .disabled(!has_data)
                                    .on_click(cx.listener(|this, _, _, cx| this.nudge(-1, cx))),
                            )
                            .child(
                                Button::new("damper-nudge-later")
                                    .ghost()
                                    .xsmall()
                                    .label("›")
                                    .accessibility_label("Nudge reference later")
                                    .tooltip("Nudge reference 20 ms later")
                                    .disabled(!has_data)
                                    .on_click(cx.listener(|this, _, _, cx| this.nudge(1, cx))),
                            ),
                    ),
            )
            .child(
                div()
                    .id("damper-strip-plot")
                    .role(Role::Figure)
                    .aria_label(label)
                    .test_support()
                    .relative()
                    .flex_1()
                    .min_w_0()
                    .overflow_hidden()
                    .child(plot)
                    .when(!has_data, |el| {
                        el.child(
                            div()
                                .absolute()
                                .inset_0()
                                .flex()
                                .items_center()
                                .justify_center()
                                .text_caption()
                                .text_color(muted)
                                .child("Both laps need front damper data"),
                        )
                    }),
            )
    }
}

/// One strip frame, snapshotted by [`DamperStrip::render`].
struct DamperPlot {
    strip: WeakEntity<DamperStrip>,
    data: Option<Arc<DamperStripData>>,
    centre: f64,
    span: f64,
    /// Requested minus committed offset, primary lap fraction.
    preview: f64,
    dragging: bool,
    palette: TracePalette,
    buffers: Rc<RefCell<Buffers>>,
}

impl IntoElement for DamperPlot {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for DamperPlot {
    type RequestLayoutState = ();
    type PrepaintState = Hitbox;

    fn id(&self) -> Option<ElementId> {
        Some("damper-plot".into())
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
        (): &mut (),
        window: &mut Window,
        _: &mut App,
    ) -> Hitbox {
        self.build(bounds, f64::from(window.scale_factor()));
        window.insert_hitbox(bounds, HitboxBehavior::Normal)
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        (): &mut (),
        hitbox: &mut Hitbox,
        window: &mut Window,
        _: &mut App,
    ) {
        let palette = self.palette;
        {
            let buffers = self.buffers.borrow();
            for path in buffers.reference.translated(bounds.origin) {
                window.paint_path(path, palette.reference);
            }
            for path in buffers.primary.translated(bounds.origin) {
                window.paint_path(path, palette.primary);
            }
        }
        // The cursor sits at the centre of the window.
        let x = (bounds.size.width.as_f32() * 0.5).round() - 0.5;
        window.paint_quad(fill(
            Bounds::new(
                bounds.origin + point(px(x), px(0.)),
                size(px(1.), bounds.size.height),
            ),
            palette.cursor.opacity(0.5),
        ));
        let style = if self.dragging {
            CursorStyle::ResizeLeftRight
        } else if self.data.is_some() {
            CursorStyle::OpenHand
        } else {
            CursorStyle::Arrow
        };
        window.set_cursor_style(style, hitbox);
        self.register_input(bounds, hitbox.clone(), window);
    }
}

impl DamperPlot {
    fn build(&self, bounds: Bounds<Pixels>, dpr: f64) {
        let mut buffers = self.buffers.borrow_mut();
        let Buffers {
            points,
            primary,
            reference,
        } = &mut *buffers;
        primary.clear();
        reference.clear();
        let Some(data) = &self.data else {
            return;
        };
        let width = f64::from(bounds.size.width.as_f32());
        let height = f64::from(bounds.size.height.as_f32());
        if width < 2.0 || height < 4.0 {
            return;
        }
        let start = self.centre - 0.5 * self.span;
        let end = start + self.span;
        let rect = PlotRect::new(0.0, 2.0, width, height - 4.0);
        let preview = self.preview;
        let reference_at = |f: f64| data.reference_fraction(f - preview);

        let params = |y_min: f64, y_span: f64| DecimateParams {
            x_start: start,
            x_span: self.span,
            rect,
            y_min,
            y_span,
            dpr,
            ..DecimateParams::default()
        };

        // Reference first so the primary stroke sits on top.
        let (low, span) = window_range(&data.reference, reference_at(start), reference_at(end));
        decimate(&data.reference, &reference_at, &params(low, span), points);
        stroke(points, STROKE_WIDTH, reference);

        let (low, span) = window_range(&data.primary, start, end);
        decimate(&data.primary, &|f| f, &params(low, span), points);
        stroke(points, STROKE_WIDTH, primary);
        // Paths are clipped to their bounds: without these they paint nothing.
        reference.finish();
        primary.finish();
    }

    fn register_input(&self, bounds: Bounds<Pixels>, hitbox: Hitbox, window: &mut Window) {
        let origin = bounds.origin;
        let width = f64::from(bounds.size.width.as_f32());
        let local_x =
            move |position: gpui_kit::Point<Pixels>| f64::from((position.x - origin.x).as_f32());

        let strip = self.strip.clone();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseDownEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble
                || event.button != MouseButton::Left
                || !hit.is_hovered(window)
            {
                return;
            }
            let x = local_x(event.position);
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no input/deferred update; the weak entity handle may already be gone.")]
            let _ = strip
                .update(cx, |strip, cx| strip.pointer_down(x, event.click_count, cx));
            cx.stop_propagation();
        });

        let strip = self.strip.clone();
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, _, cx| {
            if phase != DispatchPhase::Bubble {
                return;
            }
            let x = local_x(event.position);
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no input/deferred update; the weak entity handle may already be gone.")]
            let _ = strip
                .update(cx, |strip, cx| strip.pointer_move(x, width, cx));
        });

        let strip = self.strip.clone();
        window.on_mouse_event(move |event: &MouseUpEvent, phase, _, cx| {
            if phase != DispatchPhase::Bubble || event.button != MouseButton::Left {
                return;
            }
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no input/deferred update; the weak entity handle may already be gone.")]
            let _ = strip.update(cx, DamperStrip::pointer_up);
        });

        let strip = self.strip.clone();
        window.on_mouse_event(move |event: &ScrollWheelEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble || !hitbox.should_handle_scroll(window) {
                return;
            }
            let delta = match event.delta {
                ScrollDelta::Pixels(p) => {
                    WheelDelta::pixels(f64::from(p.x.as_f32()), f64::from(p.y.as_f32()))
                }
                ScrollDelta::Lines(p) => WheelDelta::lines(f64::from(p.x), f64::from(p.y)),
            };
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no input/deferred update; the weak entity handle may already be gone.")]
            let _ = strip.update(cx, |strip, cx| strip.wheel(delta, cx));
            cx.stop_propagation();
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn window_clamps_from_one_second_to_the_lap() {
        assert_eq!(clamp_window(6.0, 90.0), 6.0);
        assert_eq!(clamp_window(0.2, 90.0), MIN_WINDOW_SECONDS);
        assert_eq!(clamp_window(500.0, 90.0), 90.0);
        // Unknown lap: only the floor applies.
        assert_eq!(clamp_window(500.0, 0.0), 500.0);
        // A lap shorter than the floor keeps the floor.
        assert_eq!(clamp_window(0.1, 0.5), MIN_WINDOW_SECONDS);
        assert!((window_fraction(6.0, 90.0) - 6.0 / 90.0).abs() < 1e-12);
        assert_eq!(window_fraction(500.0, 90.0), 1.0);
        assert_eq!(window_fraction(6.0, 0.0), 1.0);
    }

    #[test]
    fn each_trace_autoscales_to_its_window() {
        let values: Vec<f64> = (0..=100)
            .map(|i| if i < 50 { 10.0 } else { f64::from(i) })
            .collect();
        // First half is flat at 10: a unit span around it.
        assert_eq!(window_range(&values, 0.0, 0.4), (9.5, 1.0));
        // Second half: 8% padding each side.
        let (low, span) = window_range(&values, 0.6, 1.0);
        assert!((low - (60.0 - 3.2)).abs() < 1e-9, "{low}");
        assert!((span - 46.4).abs() < 1e-9, "{span}");
        // NaN samples are skipped, an empty window falls back.
        assert_eq!(window_range(&[f64::NAN, f64::NAN], 0.0, 1.0), (0.0, 1.0));
    }

    #[test]
    fn offsets_read_with_an_explicit_sign() {
        assert_eq!(format_offset(0.12), "+0.120 s");
        assert_eq!(format_offset(-0.04), "\u{2212}0.040 s");
        assert_eq!(format_offset(0.0001), "±0.000 s");
    }
}
