//! The trace workspace gesture state machine: a port of the pointer half of
//! `src/app/TraceInteraction.cpp`, free of GPUI so it can be tested as math.
//!
//! - left-drag selects a range (a click just moves the cursor);
//! - middle-drag pans, keeping the grabbed sample under the pointer;
//! - horizontal trackpad motion pans;
//! - the wheel (and Shift/Ctrl + wheel) zooms about the pointer;
//! - with manual-height overflow, unmodified vertical wheel scrolls lanes;
//! - double-click resets the viewport;
//! - in corner editing, corner edges and bodies drag;
//! - in lane resize mode, lane dividers drag with neighbour borrowing.
//!
//! Inputs are logical pixels relative to the trace area (x includes the plot
//! left inset). The machine returns [`Effect`]s; the owner applies them to
//! the shared viewport/cursor entities and emits events.

use smallvec::SmallVec;

use crate::layout::resize_lane_boundary;
use crate::scale::Viewport;

/// Mouse button of a press or release.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PointerButton {
    Left,
    Middle,
    Right,
}

/// Modifier state of a wheel event.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[non_exhaustive]
pub struct KeyModifiers {
    pub shift: bool,
    pub control: bool,
    pub alt: bool,
    pub platform: bool,
}

impl KeyModifiers {
    pub fn new(shift: bool, control: bool, alt: bool, platform: bool) -> Self {
        Self {
            shift,
            control,
            alt,
            platform,
        }
    }
    pub fn none(&self) -> bool {
        !(self.shift || self.control || self.alt || self.platform)
    }
}

/// A wheel delta. `precise` deltas are trackpad pixels; line deltas are
/// notches (one notch = 120 units, as Qt's angle delta). Positive `y` is
/// "away from the user" (scroll up), which zooms in.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct WheelDelta {
    pub x: f64,
    pub y: f64,
    pub precise: bool,
}

impl WheelDelta {
    pub fn pixels(x: f64, y: f64) -> Self {
        Self {
            x,
            y,
            precise: true,
        }
    }
    pub fn lines(x: f64, y: f64) -> Self {
        Self {
            x: x * 120.0,
            y: y * 120.0,
            precise: false,
        }
    }
}

/// A corner zone, in primary lap fraction.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct CornerSpan {
    pub start: f64,
    pub end: f64,
}

impl CornerSpan {
    pub const fn new(start: f64, end: f64) -> Self {
        Self { start, end }
    }
}

/// Everything the machine needs to know about the current frame.
///
/// Start from [`InteractionContext::default`] (full lap, no plot, no data)
/// and set the fields that apply.
#[derive(Clone, Copy, Debug, Default)]
#[non_exhaustive]
pub struct InteractionContext<'a> {
    pub viewport: Viewport,
    /// Plot left edge and width (the data area, excluding lane chrome).
    pub plot_left: f64,
    pub plot_width: f64,
    /// Manual lanes overflow the scroll region.
    pub lanes_overflow: bool,
    /// FIT mode (no vertical scrolling).
    pub fit: bool,
    pub has_data: bool,
    pub editing_corners: bool,
    pub corners: &'a [CornerSpan],
    pub focused_corner: Option<usize>,
    /// Lane resize editing.
    pub resizing: bool,
    /// Bottom edges of the resizable lanes (trace-area y) and their heights.
    pub lane_bottoms: &'a [f64],
    pub lane_heights: &'a [f64],
}

impl InteractionContext<'_> {
    fn fraction_for_x(&self, x: f64) -> f64 {
        self.viewport
            .fraction_for_x(x, self.plot_left, self.plot_width)
    }
    fn x_for_fraction(&self, fraction: f64) -> f64 {
        self.viewport
            .x_for_fraction(fraction, self.plot_left, self.plot_width)
    }
}

/// A requested state change.
#[derive(Clone, Debug, PartialEq)]
pub enum Effect {
    /// Move the shared cursor to a lap fraction.
    MoveCursor(f64),
    /// Pointer hover (None when it leaves).
    Hover(Option<f64>),
    /// A range selection, in lap fraction; `finished` on release.
    Select {
        start: f64,
        end: f64,
        finished: bool,
    },
    ClearSelection,
    /// Replace the viewport (pan or zoom).
    SetViewport(Viewport),
    /// Double-click: back to the whole lap.
    ResetViewport,
    /// Scroll the lane region by this many logical pixels (positive: down).
    ScrollLanes(f64),
    /// A corner edge or body was dragged.
    EditCorner {
        index: usize,
        start: f64,
        end: f64,
    },
    /// Lane resize draft (heights of the resizable lanes, top to bottom).
    ResizeLanes {
        heights: Vec<f64>,
    },
    /// Secondary click inside the plot.
    ContextMenu {
        x: f64,
        y: f64,
    },
}

pub type Effects = SmallVec<[Effect; 3]>;

/// What the pointer should look like.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum GestureCursor {
    #[default]
    Default,
    Crosshair,
    Grabbing,
    ResizeColumn,
    ResizeRow,
}

/// Which part of a corner zone a pointer grabs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CornerPart {
    /// The zone's start edge.
    Start,
    /// The zone's end edge.
    End,
    /// Inside the zone, away from both edges.
    Body,
}

/// A corner zone under the pointer: which zone (index into
/// [`InteractionContext::corners`]), which part, and for a body grab the
/// lap-fraction distance from the zone start to the pointer.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
pub struct CornerHit {
    pub index: usize,
    pub part: CornerPart,
    pub grab: f64,
}

/// Grab distance of the focused corner's edges, logical pixels.
pub const FOCUSED_CORNER_EDGE_TOLERANCE: f64 = 8.0;

/// The corner edge or body under logical `x`, preferring the focused
/// corner's grips (within [`FOCUSED_CORNER_EDGE_TOLERANCE`]), then any
/// corner edge within [`CORNER_EDGE_TOLERANCE`] or body, in zone order.
///
/// The trace stack's corner editing and the corner ruler share this test so
/// a grip grabs at the same distance everywhere.
pub fn hit_corner(x: f64, ctx: &InteractionContext) -> Option<CornerHit> {
    let fraction = ctx.fraction_for_x(x);
    let test = |index: usize, tolerance: f64| -> Option<CornerHit> {
        let corner = ctx.corners.get(index)?;
        let x1 = ctx.x_for_fraction(corner.start);
        let x2 = ctx.x_for_fraction(corner.end);
        let hit = |part, grab| CornerHit { index, part, grab };
        if (x - x1).abs() <= tolerance {
            return Some(hit(CornerPart::Start, 0.0));
        }
        if (x - x2).abs() <= tolerance {
            return Some(hit(CornerPart::End, 0.0));
        }
        if corner.start <= fraction && fraction <= corner.end {
            return Some(hit(CornerPart::Body, fraction - corner.start));
        }
        None
    };
    if let Some(focused) = ctx.focused_corner
        && let Some(hit) = test(focused, FOCUSED_CORNER_EDGE_TOLERANCE)
    {
        return Some(hit);
    }
    (0..ctx.corners.len()).find_map(|index| test(index, CORNER_EDGE_TOLERANCE))
}

#[derive(Clone, Debug, Default, PartialEq)]
enum Gesture {
    #[default]
    Idle,
    Selecting {
        press_x: f64,
        anchor: f64,
    },
    Panning {
        anchor: f64,
    },
    DraggingCorner {
        index: usize,
        drag: CornerPart,
        grab: f64,
    },
    ResizingLanes {
        boundary: usize,
        origin_y: f64,
        heights: Vec<f64>,
    },
}

/// Distance in logical pixels within which a corner edge grabs.
pub const CORNER_EDGE_TOLERANCE: f64 = 5.0;
/// Distance in logical pixels within which a lane divider grabs.
pub const DIVIDER_TOLERANCE: f64 = 6.0;
/// A press that moves less than this is a click, not a selection.
pub const CLICK_SLOP: f64 = 3.0;
/// Lines scrolled per wheel notch in manual overflow.
pub const SCROLL_PER_NOTCH: f64 = 48.0;

/// The gesture state machine. One per trace stack.
#[derive(Clone, Debug, Default)]
pub struct Interaction {
    gesture: Gesture,
    hovered_divider: Option<usize>,
}

impl Interaction {
    pub fn new() -> Self {
        Self::default()
    }

    /// Whether a drag gesture is in progress (pointer moves outside the plot
    /// still belong to it).
    pub fn is_dragging(&self) -> bool {
        !matches!(self.gesture, Gesture::Idle)
    }

    pub fn is_selecting(&self) -> bool {
        matches!(self.gesture, Gesture::Selecting { .. })
    }

    pub fn is_panning(&self) -> bool {
        matches!(self.gesture, Gesture::Panning { .. })
    }

    pub fn cursor(&self) -> GestureCursor {
        match self.gesture {
            Gesture::Idle if self.hovered_divider.is_some() => GestureCursor::ResizeRow,
            Gesture::Idle => GestureCursor::Crosshair,
            Gesture::Selecting { .. } => GestureCursor::Crosshair,
            Gesture::Panning { .. } => GestureCursor::Grabbing,
            Gesture::DraggingCorner {
                drag: CornerPart::Body,
                ..
            } => GestureCursor::Grabbing,
            Gesture::DraggingCorner { .. } => GestureCursor::ResizeColumn,
            Gesture::ResizingLanes { .. } => GestureCursor::ResizeRow,
        }
    }

    /// Abandon any gesture (focus loss, data change).
    pub fn cancel(&mut self) {
        self.gesture = Gesture::Idle;
        self.hovered_divider = None;
    }

    /// Corner edge or body under `x` (see [`hit_corner`]).
    fn corner_at(&self, x: f64, ctx: &InteractionContext) -> Option<(usize, CornerPart, f64)> {
        hit_corner(x, ctx).map(|hit| (hit.index, hit.part, hit.grab))
    }

    /// Lane divider under `y` in resize mode (the last lane has none).
    pub fn divider_at(&self, y: f64, ctx: &InteractionContext) -> Option<usize> {
        if !ctx.resizing || ctx.lane_bottoms.len() < 2 {
            return None;
        }
        let mut best = None;
        let mut distance = DIVIDER_TOLERANCE;
        for (index, bottom) in ctx.lane_bottoms[..ctx.lane_bottoms.len() - 1]
            .iter()
            .enumerate()
        {
            let d = (y - bottom).abs();
            if d < distance {
                distance = d;
                best = Some(index);
            }
        }
        best
    }

    pub fn pointer_down(
        &mut self,
        x: f64,
        y: f64,
        button: PointerButton,
        click_count: usize,
        ctx: &InteractionContext,
    ) -> Effects {
        let mut effects = Effects::new();
        if !ctx.has_data {
            return effects;
        }
        if ctx.resizing {
            if button == PointerButton::Left
                && let Some(boundary) = self.divider_at(y, ctx)
            {
                self.gesture = Gesture::ResizingLanes {
                    boundary,
                    origin_y: y,
                    heights: ctx.lane_heights.to_vec(),
                };
            }
            return effects;
        }
        let fraction = ctx.fraction_for_x(x);
        match button {
            PointerButton::Right => {
                effects.push(Effect::ContextMenu { x, y });
            }
            PointerButton::Middle => {
                self.gesture = Gesture::Panning { anchor: fraction };
            }
            PointerButton::Left if click_count >= 2 => {
                self.gesture = Gesture::Idle;
                effects.push(Effect::ClearSelection);
                effects.push(Effect::ResetViewport);
            }
            PointerButton::Left => {
                if ctx.editing_corners
                    && let Some((index, drag, grab)) = self.corner_at(x, ctx)
                {
                    self.gesture = Gesture::DraggingCorner { index, drag, grab };
                    return effects;
                }
                self.gesture = Gesture::Selecting {
                    press_x: x,
                    anchor: fraction,
                };
                effects.push(Effect::MoveCursor(fraction));
                effects.push(Effect::ClearSelection);
            }
        }
        effects
    }

    pub fn pointer_move(&mut self, x: f64, y: f64, ctx: &InteractionContext) -> Effects {
        let mut effects = Effects::new();
        if !ctx.has_data {
            return effects;
        }
        let fraction = ctx.fraction_for_x(x);
        match &self.gesture {
            Gesture::Idle => {
                self.hovered_divider = self.divider_at(y, ctx);
                if !ctx.resizing {
                    effects.push(Effect::Hover(Some(fraction)));
                }
            }
            Gesture::Selecting { anchor, .. } => {
                effects.push(Effect::MoveCursor(fraction));
                effects.push(Effect::Hover(Some(fraction)));
                effects.push(Effect::Select {
                    start: anchor.min(fraction),
                    end: anchor.max(fraction),
                    finished: false,
                });
            }
            Gesture::Panning { anchor } => {
                // Unclamped pointer fraction: the grabbed sample follows the
                // pointer even past the plot edges.
                let under = ctx.viewport.start
                    + (x - ctx.plot_left) / ctx.plot_width.max(1.0) * ctx.viewport.span();
                let next = ctx.viewport.pan_by(anchor - under);
                if next != ctx.viewport {
                    effects.push(Effect::SetViewport(next));
                }
            }
            Gesture::DraggingCorner { index, drag, grab } => {
                if let Some(corner) = ctx.corners.get(*index) {
                    let (start, end) = match drag {
                        CornerPart::Start => (fraction.clamp(0.0, corner.end), corner.end),
                        CornerPart::End => (corner.start, fraction.clamp(corner.start, 1.0)),
                        CornerPart::Body => {
                            let width = corner.end - corner.start;
                            let start = (fraction - grab).clamp(0.0, 1.0 - width);
                            (start, start + width)
                        }
                    };
                    effects.push(Effect::EditCorner {
                        index: *index,
                        start,
                        end,
                    });
                } else {
                    self.gesture = Gesture::Idle;
                }
            }
            Gesture::ResizingLanes {
                boundary,
                origin_y,
                heights,
            } => {
                effects.push(Effect::ResizeLanes {
                    heights: resize_lane_boundary(heights, *boundary, y - origin_y),
                });
            }
        }
        effects
    }

    pub fn pointer_up(
        &mut self,
        x: f64,
        _button: PointerButton,
        ctx: &InteractionContext,
    ) -> Effects {
        let mut effects = Effects::new();
        let gesture = std::mem::take(&mut self.gesture);
        if let Gesture::Selecting { press_x, anchor } = gesture {
            if (x - press_x).abs() >= CLICK_SLOP && ctx.has_data {
                let fraction = ctx.fraction_for_x(x);
                effects.push(Effect::Select {
                    start: anchor.min(fraction),
                    end: anchor.max(fraction),
                    finished: true,
                });
            } else {
                effects.push(Effect::ClearSelection);
            }
        }
        effects
    }

    pub fn pointer_leave(&mut self) -> Effects {
        let mut effects = Effects::new();
        self.hovered_divider = None;
        if matches!(self.gesture, Gesture::Idle) {
            effects.push(Effect::Hover(None));
        }
        effects
    }

    pub fn wheel(
        &mut self,
        x: f64,
        delta: WheelDelta,
        modifiers: KeyModifiers,
        ctx: &InteractionContext,
    ) -> Effects {
        let mut effects = Effects::new();
        if !ctx.has_data || ctx.resizing {
            return effects;
        }
        let (dx, dy) = (delta.x, delta.y);
        if dx == 0.0 && dy == 0.0 {
            return effects;
        }
        let modified = modifiers.shift || modifiers.control;
        // Some platforms turn Shift+wheel into horizontal motion; a modified
        // wheel always zooms, using whichever axis carries the motion.
        let zoom_delta = if modified && dx.abs() > dy.abs() {
            dx
        } else {
            dy
        };
        if !modified && dx.abs() > dy.abs() {
            let width = ctx.plot_width.max(1.0);
            let next = ctx.viewport.pan_by(-dx / width * ctx.viewport.span());
            if next != ctx.viewport {
                effects.push(Effect::SetViewport(next));
            }
            return effects;
        }
        if !ctx.fit && modifiers.none() && ctx.lanes_overflow {
            let scroll = if delta.precise {
                -dy
            } else {
                -dy / 120.0 * SCROLL_PER_NOTCH
            };
            effects.push(Effect::ScrollLanes(scroll));
            return effects;
        }
        let anchor = ctx.fraction_for_x(x);
        let next = ctx
            .viewport
            .zoom_about(anchor, 0.8f64.powf(zoom_delta / 120.0));
        if next != ctx.viewport {
            effects.push(Effect::SetViewport(next));
        }
        effects
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ctx(viewport: Viewport) -> InteractionContext<'static> {
        InteractionContext {
            viewport,
            plot_left: 100.0,
            plot_width: 1000.0,
            lanes_overflow: false,
            fit: true,
            has_data: true,
            editing_corners: false,
            corners: &[],
            focused_corner: None,
            resizing: false,
            lane_bottoms: &[],
            lane_heights: &[],
        }
    }

    #[test]
    fn click_moves_cursor_and_drag_selects() {
        let mut machine = Interaction::new();
        let c = ctx(Viewport::FULL);
        let down = machine.pointer_down(350.0, 10.0, PointerButton::Left, 1, &c);
        assert_eq!(down[0], Effect::MoveCursor(0.25));
        let moved = machine.pointer_move(600.0, 10.0, &c);
        assert!(moved.contains(&Effect::Select {
            start: 0.25,
            end: 0.5,
            finished: false
        }));
        let up = machine.pointer_up(600.0, PointerButton::Left, &c);
        assert_eq!(
            up.as_slice(),
            &[Effect::Select {
                start: 0.25,
                end: 0.5,
                finished: true
            }]
        );
        // A click without motion clears instead.
        machine.pointer_down(350.0, 10.0, PointerButton::Left, 1, &c);
        let up = machine.pointer_up(351.0, PointerButton::Left, &c);
        assert_eq!(up.as_slice(), &[Effect::ClearSelection]);
    }

    #[test]
    fn middle_drag_keeps_grabbed_sample_under_pointer() {
        let mut machine = Interaction::new();
        let mut view = Viewport::new(0.4, 0.5);
        let grabbed = ctx(view).fraction_for_x(600.0);
        machine.pointer_down(600.0, 10.0, PointerButton::Middle, 1, &ctx(view));
        for x in [550.0, 500.0, 420.0] {
            for effect in machine.pointer_move(x, 10.0, &ctx(view)) {
                if let Effect::SetViewport(next) = effect {
                    view = next;
                }
            }
            let under = ctx(view).fraction_for_x(x);
            assert!((under - grabbed).abs() < 1e-12, "{under} vs {grabbed}");
        }
        assert!(view.start > 0.4);
        assert_eq!(machine.cursor(), GestureCursor::Grabbing);
        machine.pointer_up(420.0, PointerButton::Middle, &ctx(view));
        assert!(!machine.is_dragging());
    }

    #[test]
    fn wheel_zooms_about_pointer() {
        let mut machine = Interaction::new();
        let c = ctx(Viewport::FULL);
        let effects = machine.wheel(
            350.0,
            WheelDelta::lines(0.0, 1.0),
            KeyModifiers::default(),
            &c,
        );
        let Effect::SetViewport(view) = effects[0] else {
            panic!("{effects:?}")
        };
        assert!((view.span() - 0.8).abs() < 1e-12);
        assert!(((0.25 - view.start) / view.span() - 0.25).abs() < 1e-12);
        // Wheel down zooms back out.
        let effects = machine.wheel(
            350.0,
            WheelDelta::lines(0.0, -1.0),
            KeyModifiers::default(),
            &ctx(view),
        );
        let Effect::SetViewport(back) = effects[0] else {
            panic!()
        };
        assert!((back.span() - 1.0).abs() < 1e-12);
        // Shift+wheel delivered as horizontal motion still zooms.
        let shift = KeyModifiers::new(true, false, false, false);
        let effects = machine.wheel(350.0, WheelDelta::lines(1.0, 0.0), shift, &c);
        let Effect::SetViewport(view) = effects[0] else {
            panic!()
        };
        assert!((view.span() - 0.8).abs() < 1e-12);
    }

    #[test]
    fn horizontal_motion_pans_and_overflow_scrolls() {
        let mut machine = Interaction::new();
        let view = Viewport::new(0.2, 0.4);
        let effects = machine.wheel(
            350.0,
            WheelDelta::pixels(-100.0, 0.0),
            KeyModifiers::default(),
            &ctx(view),
        );
        let Effect::SetViewport(next) = effects[0] else {
            panic!()
        };
        assert!((next.start - 0.22).abs() < 1e-12);
        let manual = InteractionContext {
            fit: false,
            lanes_overflow: true,
            ..ctx(view)
        };
        let effects = machine.wheel(
            350.0,
            WheelDelta::lines(0.0, -1.0),
            KeyModifiers::default(),
            &manual,
        );
        assert_eq!(effects.as_slice(), &[Effect::ScrollLanes(48.0)]);
        // A modified wheel still zooms while overflowing.
        let ctrl = KeyModifiers::new(false, true, false, false);
        let effects = machine.wheel(350.0, WheelDelta::lines(0.0, 1.0), ctrl, &manual);
        assert!(matches!(effects[0], Effect::SetViewport(_)));
    }

    #[test]
    fn double_click_resets() {
        let mut machine = Interaction::new();
        let effects = machine.pointer_down(
            350.0,
            10.0,
            PointerButton::Left,
            2,
            &ctx(Viewport::new(0.1, 0.2)),
        );
        assert!(effects.contains(&Effect::ResetViewport));
        assert!(!machine.is_dragging());
    }

    #[test]
    fn corner_edges_hit_and_drag_in_edit_mode() {
        let corners = [CornerSpan {
            start: 0.2,
            end: 0.3,
        }];
        let c = InteractionContext {
            editing_corners: true,
            corners: &corners,
            ..ctx(Viewport::FULL)
        };
        let mut machine = Interaction::new();
        // 3 px from the start edge at x = 300.
        machine.pointer_down(303.0, 10.0, PointerButton::Left, 1, &c);
        assert_eq!(machine.cursor(), GestureCursor::ResizeColumn);
        let effects = machine.pointer_move(250.0, 10.0, &c);
        assert_eq!(
            effects.as_slice(),
            &[Effect::EditCorner {
                index: 0,
                start: 0.15,
                end: 0.3
            }]
        );
        machine.pointer_up(250.0, PointerButton::Left, &c);
        // Body drag keeps the width.
        machine.pointer_down(350.0, 10.0, PointerButton::Left, 1, &c);
        let effects = machine.pointer_move(450.0, 10.0, &c);
        let Effect::EditCorner { start, end, .. } = effects[0] else {
            panic!()
        };
        assert!((start - 0.3).abs() < 1e-12 && (end - 0.4).abs() < 1e-12);
        // Outside edit mode a press never grabs a corner.
        let mut plain = Interaction::new();
        let effects = plain.pointer_down(303.0, 10.0, PointerButton::Left, 1, &ctx(Viewport::FULL));
        assert!(matches!(effects[0], Effect::MoveCursor(_)));
    }

    #[test]
    fn lane_dividers_resize_with_borrowing() {
        let bottoms = [100.0, 200.0, 300.0, 400.0];
        let heights = [100.0, 100.0, 100.0, 100.0];
        let c = InteractionContext {
            resizing: true,
            lane_bottoms: &bottoms,
            lane_heights: &heights,
            ..ctx(Viewport::FULL)
        };
        let mut machine = Interaction::new();
        assert_eq!(
            machine.divider_at(398.0, &c),
            None,
            "last lane has no divider"
        );
        machine.pointer_down(500.0, 102.0, PointerButton::Left, 1, &c);
        let effects = machine.pointer_move(500.0, 302.0, &c);
        assert_eq!(
            effects.as_slice(),
            &[Effect::ResizeLanes {
                heights: vec![300.0, 20.0, 20.0, 60.0]
            }]
        );
        // Wheel is inert while resizing.
        assert!(
            machine
                .wheel(
                    500.0,
                    WheelDelta::lines(0.0, 1.0),
                    KeyModifiers::default(),
                    &c
                )
                .is_empty()
        );
    }
}
