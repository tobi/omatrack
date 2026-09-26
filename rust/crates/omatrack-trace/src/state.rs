//! The shared horizontal state of the trace workspace.
//!
//! [`ViewportState`] and [`CursorState`] are separate entities on purpose:
//! a cursor move notifies only `CursorState` observers (the overlay, lane
//! readouts, video), while the static trace layer observes `ViewportState`
//! alone. The application creates both and shares them with every view that
//! follows the lap (traces, track map, damper strip, video).

use std::time::{Duration, Instant};

use gpui_kit::{Context, Task};

use crate::scale::{Viewport, XAxis};

/// Duration of the corner-focus viewport animation.
pub const FOCUS_ANIMATION: Duration = Duration::from_millis(140);
const FRAME: Duration = Duration::from_millis(8);

/// `OutCubic` easing: fast start, gentle landing.
pub fn out_cubic(t: f64) -> f64 {
    let t = t.clamp(0.0, 1.0);
    1.0 - (1.0 - t).powi(3)
}

struct ViewportAnimation {
    from: Viewport,
    to: Viewport,
    started: Instant,
}

/// The visible lap window and the x-axis unit. Owned by the application,
/// shared by reference.
pub struct ViewportState {
    viewport: Viewport,
    axis: XAxis,
    animation: Option<ViewportAnimation>,
    animation_task: Option<Task<()>>,
}

impl Default for ViewportState {
    fn default() -> Self {
        Self::new()
    }
}

impl ViewportState {
    pub fn new() -> Self {
        Self {
            viewport: Viewport::FULL,
            axis: XAxis::Distance,
            animation: None,
            animation_task: None,
        }
    }

    pub fn viewport(&self) -> Viewport {
        self.viewport
    }

    pub fn axis(&self) -> XAxis {
        self.axis
    }

    pub fn is_animating(&self) -> bool {
        self.animation.is_some()
    }

    /// Replace the viewport, cancelling any animation.
    pub fn set_viewport(&mut self, viewport: Viewport, cx: &mut Context<'_, Self>) {
        self.stop_animation();
        self.apply(viewport, cx);
    }

    fn apply(&mut self, viewport: Viewport, cx: &mut Context<'_, Self>) {
        if viewport != self.viewport {
            self.viewport = viewport;
            cx.notify();
        }
    }

    fn stop_animation(&mut self) {
        self.animation = None;
        self.animation_task = None;
    }

    pub fn set_axis(&mut self, axis: XAxis, cx: &mut Context<'_, Self>) {
        if axis != self.axis {
            self.axis = axis;
            cx.notify();
        }
    }

    pub fn toggle_axis(&mut self, cx: &mut Context<'_, Self>) {
        self.set_axis(self.axis.toggled(), cx);
    }

    /// Zoom by `factor` (<1 zooms in) about a lap fraction.
    pub fn zoom_about(&mut self, anchor: f64, factor: f64, cx: &mut Context<'_, Self>) {
        let next = self.viewport.zoom_about(anchor, factor);
        self.set_viewport(next, cx);
    }

    /// Keyboard zoom step about `anchor` (the cursor), or the view centre.
    pub fn zoom_in(&mut self, anchor: Option<f64>, cx: &mut Context<'_, Self>) {
        let anchor = anchor.unwrap_or((self.viewport.start + self.viewport.end) * 0.5);
        self.zoom_about(anchor, 0.5, cx);
    }

    pub fn zoom_out(&mut self, anchor: Option<f64>, cx: &mut Context<'_, Self>) {
        let anchor = anchor.unwrap_or((self.viewport.start + self.viewport.end) * 0.5);
        self.zoom_about(anchor, 2.0, cx);
    }

    /// Pan by a lap-fraction distance.
    pub fn pan_by(&mut self, delta: f64, cx: &mut Context<'_, Self>) {
        let next = self.viewport.pan_by(delta);
        self.set_viewport(next, cx);
    }

    /// Back to the whole lap.
    pub fn reset(&mut self, cx: &mut Context<'_, Self>) {
        self.set_viewport(Viewport::FULL, cx);
    }

    /// Place a corner zone in the left half of the workspace. With `animate`
    /// the viewport eases there over 140 ms (`OutCubic`) unless the platform
    /// asks for reduced motion; an interrupted animation restarts from the
    /// currently shown viewport, never from its old endpoint.
    pub fn focus(&mut self, start: f64, end: f64, animate: bool, cx: &mut Context<'_, Self>) {
        self.move_to(Viewport::focus_on(start, end), animate, cx);
    }

    /// Frame a corner zone with its approach and exit (the Corners view,
    /// [`Viewport::frame_corner`]), with the same motion as [`Self::focus`].
    pub fn frame_corner(
        &mut self,
        start: f64,
        end: f64,
        animate: bool,
        cx: &mut Context<'_, Self>,
    ) {
        self.move_to(Viewport::frame_corner(start, end), animate, cx);
    }

    /// Show `target`, easing there over 140 ms with `animate`.
    fn move_to(&mut self, target: Viewport, animate: bool, cx: &mut Context<'_, Self>) {
        if !animate || cx.reduce_motion() || target == self.viewport {
            self.set_viewport(target, cx);
            return;
        }
        self.animation = Some(ViewportAnimation {
            from: self.viewport,
            to: target,
            started: cx.background_executor().now(),
        });
        self.animation_task = Some(cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor().timer(FRAME).await;
                let Ok(running) = this.update(cx, Self::step_animation) else {
                    break;
                };
                if !running {
                    break;
                }
            }
        }));
    }

    /// Advance the animation; false when it has landed.
    fn step_animation(&mut self, cx: &mut Context<'_, Self>) -> bool {
        let Some(animation) = &self.animation else {
            return false;
        };
        let elapsed = cx
            .background_executor()
            .now()
            .saturating_duration_since(animation.started);
        let t = elapsed.as_secs_f64() / FOCUS_ANIMATION.as_secs_f64();
        let (from, to) = (animation.from, animation.to);
        if t >= 1.0 {
            self.animation = None;
            self.apply(to, cx);
            return false;
        }
        self.apply(from.lerp(&to, out_cubic(t)), cx);
        true
    }
}

/// A range selection in primary lap fraction.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct Selection {
    pub start: f64,
    pub end: f64,
}

impl Selection {
    pub fn new(a: f64, b: f64) -> Self {
        Self {
            start: a.min(b),
            end: a.max(b),
        }
    }
}

/// The shared cursor, pointer hover, range selection and focused corner
/// zone. Cursor motion notifies only this entity.
#[derive(Default)]
pub struct CursorState {
    fraction: Option<f64>,
    hover: Option<f64>,
    selection: Option<Selection>,
    focus: Option<Selection>,
}

impl CursorState {
    pub fn new() -> Self {
        Self::default()
    }

    /// The shared cursor, in primary lap fraction (`0..=1`).
    pub fn fraction(&self) -> Option<f64> {
        self.fraction
    }

    /// Pointer position over a trace, while hovering.
    pub fn hover(&self) -> Option<f64> {
        self.hover
    }

    /// Where readouts sample: the hover position, else the cursor.
    pub fn readout_fraction(&self) -> Option<f64> {
        self.hover.or(self.fraction)
    }

    pub fn selection(&self) -> Option<Selection> {
        self.selection
    }

    pub fn has_selection(&self) -> bool {
        self.selection.is_some()
    }

    /// The focused corner zone, which the overlay keeps bright.
    pub fn focus(&self) -> Option<Selection> {
        self.focus
    }

    pub fn set_fraction(&mut self, fraction: Option<f64>, cx: &mut Context<'_, Self>) {
        let fraction = fraction
            .filter(|f| f.is_finite())
            .map(|f| f.clamp(0.0, 1.0));
        if fraction != self.fraction {
            self.fraction = fraction;
            cx.notify();
        }
    }

    pub fn set_hover(&mut self, hover: Option<f64>, cx: &mut Context<'_, Self>) {
        let hover = hover.filter(|f| f.is_finite());
        if hover != self.hover {
            self.hover = hover;
            cx.notify();
        }
    }

    pub fn set_selection(&mut self, selection: Option<Selection>, cx: &mut Context<'_, Self>) {
        if selection != self.selection {
            self.selection = selection;
            cx.notify();
        }
    }

    pub fn set_focus(&mut self, focus: Option<Selection>, cx: &mut Context<'_, Self>) {
        if focus != self.focus {
            self.focus = focus;
            cx.notify();
        }
    }

    /// Move the cursor by `steps` samples of an `n`-sample lap.
    #[expect(
        clippy::cast_precision_loss,
        reason = "Cursor motion converts sample counts and signed steps to lap fractions; extreme steps clamp to the lap endpoints."
    )]
    pub fn step(&mut self, steps: i64, samples: usize, cx: &mut Context<'_, Self>) {
        if samples < 2 {
            return;
        }
        let step = 1.0 / (samples - 1) as f64;
        let current = self.fraction.unwrap_or(0.0);
        self.set_fraction(Some(current + steps as f64 * step), cx);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui_kit::{AppContext as _, TestAppContext};

    #[test]
    #[expect(
        clippy::float_cmp,
        reason = "Assert exact stored, clamped or unchanged values; an epsilon would weaken this regression check."
    )]
    fn out_cubic_shape() {
        assert_eq!(out_cubic(0.0), 0.0);
        assert_eq!(out_cubic(1.0), 1.0);
        assert!(out_cubic(0.5) > 0.8);
    }

    #[gpui_kit::test]
    fn focus_animates_and_lands(cx: &mut TestAppContext) {
        let state = cx.new(|_| ViewportState::new());
        state.update(cx, |s, cx| s.focus(0.40, 0.46, true, cx));
        assert!(state.read_with(cx, |s, _| s.is_animating()));
        cx.executor().advance_clock(Duration::from_millis(40));
        cx.run_until_parked();
        let mid = state.read_with(cx, |s, _| s.viewport());
        assert!(mid != Viewport::FULL && mid.span() > 0.2, "{mid:?}");
        cx.executor().advance_clock(Duration::from_millis(200));
        cx.run_until_parked();
        let landed = state.read_with(cx, |s, _| s.viewport());
        assert_eq!(landed, Viewport::focus_on(0.40, 0.46));
        assert!(!state.read_with(cx, |s, _| s.is_animating()));
    }

    #[gpui_kit::test]
    fn reduced_motion_jumps(cx: &mut TestAppContext) {
        cx.update(|cx| cx.set_reduce_motion(true));
        let state = cx.new(|_| ViewportState::new());
        state.update(cx, |s, cx| s.focus(0.40, 0.46, true, cx));
        assert!(!state.read_with(cx, |s, _| s.is_animating()));
        assert_eq!(
            state.read_with(cx, |s, _| s.viewport()),
            Viewport::focus_on(0.40, 0.46)
        );
    }

    #[gpui_kit::test]
    fn cursor_clamps_and_notifies_on_change_only(cx: &mut TestAppContext) {
        let cursor = cx.new(|_| CursorState::new());
        let notified = std::rc::Rc::new(std::cell::Cell::new(0));
        let counter = notified.clone();
        let _subscription =
            cx.update(|cx| cx.observe(&cursor, move |_, _| counter.set(counter.get() + 1)));
        cursor.update(cx, |c, cx| c.set_fraction(Some(1.4), cx));
        cursor.update(cx, |c, cx| c.set_fraction(Some(1.0), cx));
        cx.run_until_parked();
        assert_eq!(cursor.read_with(cx, |c, _| c.fraction()), Some(1.0));
        assert_eq!(notified.get(), 1);
    }
}
