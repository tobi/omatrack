//! The trace view modes (`trace.view_mode`): what the trace area frames.
//!
//! - **Lap**: the whole-lap viewport; `h` / `j` focus a corner in the left
//!   half and Escape returns to the lap.
//! - **Corners**: one corner at a time. Entering frames the focused corner
//!   (else the first) with its approach and
//!   exit ([`omatrack_trace::Viewport::frame_corner`]) through the same
//!   140 ms motion; `h` / `j` step; a new analysis keeps the same corner.
//!   Leaving (another mode, or Escape) returns to the viewport and cursor
//!   from before the mode.
//! - **Consistency** and **Events** keep the lap framing. Consistency draws
//!   the primary's session laps behind it (loaded lazily by
//!   [`TraceView`](crate::state::TraceView)); Events marks brake onsets,
//!   lifts, shifts and corner notes on the lanes (see the traces panel).
//!
//! The colour mode (`trace.color_mode`) is a preference flip; the lanes
//! repaint from it and never rebuild geometry.

use gpui_kit::Context;
use omatrack_library::config::{TraceColorMode, TraceViewMode};
use omatrack_trace::Viewport;

use super::Workspace;

impl Workspace {
    /// The trace view mode (the [`TraceView`](crate::state::TraceView)
    /// entity holds it and persists it as `trace.view_mode`).
    pub fn view_mode(&self, cx: &gpui_kit::App) -> TraceViewMode {
        self.app.trace_view.read(cx).mode()
    }

    /// Switch the trace area to `mode` and persist it. Choosing the current
    /// mode again changes nothing.
    pub(super) fn set_view_mode(&mut self, mode: TraceViewMode, cx: &mut Context<Self>) {
        let current = self.view_mode(cx);
        if current == mode {
            return;
        }
        if current == TraceViewMode::Corners {
            self.leave_corners_view(cx);
        }
        // The TraceView persists the mode and starts what it needs to draw
        // (the Consistency view's session load).
        self.app
            .trace_view
            .update(cx, |trace_view, cx| trace_view.set_mode(mode, cx));
        if mode == TraceViewMode::Corners {
            self.enter_corners_view(cx);
        }
        cx.notify();
    }

    /// Remember the view to return to and frame the corner to start from.
    fn enter_corners_view(&mut self, cx: &mut Context<Self>) {
        // A corner focused in the lap view already remembers the lap view.
        let viewport = self
            .pre_focus_viewport
            .unwrap_or_else(|| self.app.viewport.read(cx).viewport());
        let cursor = if self.pre_focus_viewport.is_some() {
            self.pre_focus_cursor
        } else {
            self.app.cursor.read(cx).fraction()
        };
        self.corners_return = Some((viewport, cursor));
        self.frame_start_corner(None, cx);
    }

    /// Frame `id`'s corner if the analysis has it, else the focused corner,
    /// else the first.
    fn frame_start_corner(&mut self, id: Option<String>, cx: &mut Context<Self>) {
        let Some(ix) = self.app.session.read(cx).analysis().and_then(|analysis| {
            let corners = analysis.corners();
            id.and_then(|id| corners.iter().position(|zone| zone.id == id))
                .or(self.focused_corner.filter(|ix| *ix < corners.len()))
                .or((!corners.is_empty()).then_some(0))
        }) else {
            return;
        };
        self.focus_corner(ix, cx);
    }

    /// Drop the corner focus and return to the view from before the mode.
    fn leave_corners_view(&mut self, cx: &mut Context<Self>) {
        let (viewport, cursor) = self.corners_return.take().unwrap_or((Viewport::FULL, None));
        self.forget_corner_focus(cx);
        self.app
            .viewport
            .update(cx, |state, cx| state.set_viewport(viewport, cx));
        if cursor.is_some() {
            self.app
                .cursor
                .update(cx, |state, cx| state.set_fraction(cursor, cx));
        }
    }

    /// A new analysis arrived: in the Corners view, frame the same corner
    /// (by zone id) in it, or its first corner.
    pub(super) fn refit_view_mode(&mut self, previous: Option<String>, cx: &mut Context<Self>) {
        if self.view_mode(cx) != TraceViewMode::Corners {
            return;
        }
        if self.corners_return.is_none() {
            self.corners_return = Some((Viewport::FULL, None));
        }
        self.frame_start_corner(previous, cx);
    }

    /// The zone id of the focused corner in the current analysis.
    pub(super) fn focused_zone_id(&self, cx: &gpui_kit::App) -> Option<String> {
        let ix = self.focused_corner?;
        let analysis = self.app.session.read(cx).analysis()?;
        analysis.corners().get(ix).map(|zone| zone.id.clone())
    }

    /// Whether corners are framed with their approach and exit (the
    /// Corners view) rather than centred in the left half.
    pub(super) fn frames_corners(&self, cx: &gpui_kit::App) -> bool {
        self.view_mode(cx) == TraceViewMode::Corners
    }

    /// Escape in the Corners view returns to the lap view. False outside it.
    pub(super) fn escape_view_mode(&mut self, cx: &mut Context<Self>) -> bool {
        if self.view_mode(cx) != TraceViewMode::Corners {
            return false;
        }
        self.set_view_mode(TraceViewMode::Lap, cx);
        true
    }

    /// Flip the lanes between lap colours and channel colours.
    pub(super) fn toggle_trace_color_mode(&mut self, cx: &mut Context<Self>) {
        self.app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| {
                let next = match config.trace.color_mode() {
                    TraceColorMode::Lap => TraceColorMode::Channel,
                    TraceColorMode::Channel => TraceColorMode::Lap,
                };
                config.trace.color_mode = Some(next);
            });
        });
    }
}
