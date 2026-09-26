//! `CornerRuler`: the corner row above the trace lanes.
//!
//! Every corner zone is a band on the shared x mapping (the application's
//! [`ViewportState`]) with its driver-facing label (`T1`, `T5`, …); corner
//! complexes are brackets above the corners they span. The focused corner is
//! filled with the primary role colour, a hovered corner brightens.
//!
//! Interaction contract:
//!
//! - outside edit mode a click on a band emits
//!   [`CornerRulerEvent::CornerClicked`]; the owner decides what focusing
//!   means (usually `TraceStack::focus_corner`) and reports the result back
//!   through [`CornerRuler::set_focused_corner`] (controlled value);
//! - in edit mode ([`CornerRuler::set_editing`]) the zone edges become
//!   grips. Edge and body hits use [`crate::interaction::hit_corner`], the
//!   same test the trace stack uses, and drags run through the same
//!   [`Interaction`] machine; every drag step updates the displayed zone and
//!   emits [`CornerRulerEvent::CornerEdited`]. Clicks in edit mode never
//!   focus (corner editing and analysis focus are separate modes).
//!
//! Placement: the ruler maps lap fraction across its own width, so put it in
//! the plot column (beside a spacer as wide as the lane chrome, `w_40`) for
//! its bands to line up with the lanes below.
//!
//! State ownership: the application owns the viewport and the corner data;
//! the ruler owns only transient pointer state and the zones being dragged.
//! The ruler repaints on viewport changes (one element, a few quads and
//! labels per corner) and never touches the trace static layer.
//!
//! The element positions bands from the resolved bounds and the viewport
//! mapping: measured runtime geometry, the documented `px` exception.
//! Colours come from [`TracePalette`] and theme tokens only.

use std::sync::Arc;

use gpui_kit::base::TestSupportExt as _;
use gpui_kit::component::ActiveTheme as _;
use gpui_kit::{
    App, Bounds, Context, CursorStyle, DispatchPhase, Element, ElementId, Entity, EventEmitter,
    FontWeight, GlobalElementId, Hitbox, HitboxBehavior, Hsla, InspectorElementId,
    InteractiveElement as _, IntoElement, LayoutId, MouseButton, MouseDownEvent, MouseMoveEvent,
    MouseUpEvent, ParentElement as _, Pixels, Render, Role, SharedString,
    StatefulInteractiveElement as _, Style, Styled as _, Subscription, WeakEntity, Window, div,
    fill, point, prelude::FluentBuilder as _, px, relative, size,
};

use crate::interaction::{
    CLICK_SLOP, CornerPart, CornerSpan, Effect, Interaction, InteractionContext, PointerButton,
    hit_corner,
};
use crate::label;
use crate::palette::TracePalette;
use crate::scale::Viewport;
use crate::scene::{ComplexBand, CornerBand, TraceScene};
use crate::state::ViewportState;

/// User intent reported by a [`CornerRuler`].
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub enum CornerRulerEvent {
    /// A corner band was clicked outside edit mode (corner id).
    CornerClicked(u32),
    /// A zone edge or body was dragged in edit mode; the new zone, in
    /// primary lap fraction.
    CornerEdited { id: u32, start: f64, end: f64 },
}

/// A pointer press on a corner, outside edit mode.
#[derive(Clone, Copy)]
struct Press {
    x: f64,
    corner: u32,
}

/// The corner row. Create once per trace workspace and share the
/// application's [`ViewportState`]; see the module docs.
pub struct CornerRuler {
    viewport: Entity<ViewportState>,
    corners: Arc<[CornerBand]>,
    spans: Vec<CornerSpan>,
    complexes: Arc<[ComplexBand]>,
    focused: Option<u32>,
    editing: bool,
    hovered: Option<(u32, CornerPart)>,
    press: Option<Press>,
    interaction: Interaction,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<CornerRulerEvent> for CornerRuler {}

impl CornerRuler {
    pub fn new(viewport: Entity<ViewportState>, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![cx.observe(&viewport, |_, _, cx| cx.notify())];
        Self {
            viewport,
            corners: Arc::from(Vec::new()),
            spans: Vec::new(),
            complexes: Arc::from(Vec::new()),
            focused: None,
            editing: false,
            hovered: None,
            press: None,
            interaction: Interaction::new(),
            _subscriptions: subscriptions,
        }
    }

    /// The zones and complexes of `scene`.
    pub fn with_scene(mut self, scene: &TraceScene) -> Self {
        self.replace(scene.corners().to_vec(), scene.complexes().to_vec());
        self
    }

    fn replace(&mut self, corners: Vec<CornerBand>, complexes: Vec<ComplexBand>) {
        self.spans = corners
            .iter()
            .map(|c| CornerSpan::new(c.start, c.end))
            .collect();
        self.corners = corners.into();
        self.complexes = complexes.into();
        if let Some((id, _)) = self.hovered
            && !self.corners.iter().any(|c| c.id == id)
        {
            self.hovered = None;
        }
    }

    /// Replace the zones and complexes (a new lap, an edit committed or
    /// cancelled). Cancels any drag in progress.
    pub fn set_corners(
        &mut self,
        corners: Vec<CornerBand>,
        complexes: Vec<ComplexBand>,
        cx: &mut Context<Self>,
    ) {
        self.interaction.cancel();
        self.press = None;
        self.replace(corners, complexes);
        cx.notify();
    }

    /// The zones as displayed, including a drag in progress.
    pub fn corners(&self) -> &[CornerBand] {
        &self.corners
    }

    pub fn complexes(&self) -> &[ComplexBand] {
        &self.complexes
    }

    /// The focused corner (controlled by the owner).
    pub fn set_focused_corner(&mut self, id: Option<u32>, cx: &mut Context<Self>) {
        if self.focused != id {
            self.focused = id;
            cx.notify();
        }
    }

    pub fn focused_corner(&self) -> Option<u32> {
        self.focused
    }

    /// Corner edit mode: edges become grips and drags edit zones.
    pub fn set_editing(&mut self, editing: bool, cx: &mut Context<Self>) {
        if self.editing != editing {
            self.editing = editing;
            self.interaction.cancel();
            self.press = None;
            cx.notify();
        }
    }

    pub fn is_editing(&self) -> bool {
        self.editing
    }

    /// Whether an edge or body drag is in progress.
    pub fn is_dragging(&self) -> bool {
        self.interaction.is_dragging()
    }

    /// The corner under the pointer, if any.
    pub fn hovered_corner(&self) -> Option<u32> {
        self.hovered.map(|(id, _)| id)
    }

    fn context(&self, viewport: Viewport, width: f64) -> InteractionContext<'_> {
        ruler_context(
            &self.corners,
            &self.spans,
            self.focused,
            self.editing,
            viewport,
            width,
        )
    }

    /// Run the shared gesture machine against the current zones.
    fn with_interaction<R>(
        &mut self,
        width: f64,
        cx: &App,
        f: impl FnOnce(&mut Interaction, &InteractionContext) -> R,
    ) -> R {
        let viewport = self.viewport.read(cx).viewport();
        let ctx = ruler_context(
            &self.corners,
            &self.spans,
            self.focused,
            self.editing,
            viewport,
            width,
        );
        f(&mut self.interaction, &ctx)
    }

    fn hit(&self, x: f64, width: f64, cx: &App) -> Option<(u32, CornerPart)> {
        let viewport = self.viewport.read(cx).viewport();
        let ctx = self.context(viewport, width);
        let hit = hit_corner(x, &ctx)?;
        let corner = self.corners.get(hit.index)?;
        Some((corner.id, hit.part))
    }

    fn pointer_down(&mut self, x: f64, width: f64, cx: &mut Context<Self>) {
        let hit = self.hit(x, width, cx);
        if self.editing {
            if hit.is_some() {
                let effects = self.with_interaction(width, cx, |machine, ctx| {
                    machine.pointer_down(x, 0.0, PointerButton::Left, 1, ctx)
                });
                debug_assert!(effects.is_empty(), "{effects:?}");
                cx.notify();
            }
            return;
        }
        self.press = hit.map(|(corner, _)| Press { x, corner });
    }

    fn pointer_move(&mut self, x: f64, width: f64, inside: bool, cx: &mut Context<Self>) {
        if self.interaction.is_dragging() {
            let effects =
                self.with_interaction(width, cx, |machine, ctx| machine.pointer_move(x, 0.0, ctx));
            for effect in effects {
                if let Effect::EditCorner { index, start, end } = effect {
                    self.edit(index, start, end, cx);
                }
            }
            return;
        }
        let hovered = if inside { self.hit(x, width, cx) } else { None };
        if hovered != self.hovered {
            self.hovered = hovered;
            cx.notify();
        }
    }

    fn pointer_up(&mut self, x: f64, width: f64, cx: &mut Context<Self>) {
        if self.interaction.is_dragging() {
            self.with_interaction(width, cx, |machine, ctx| {
                machine.pointer_up(x, PointerButton::Left, ctx)
            });
            cx.notify();
            return;
        }
        let Some(press) = self.press.take() else {
            return;
        };
        if (x - press.x).abs() < CLICK_SLOP
            && self.hit(x, width, cx).map(|(id, _)| id) == Some(press.corner)
        {
            cx.emit(CornerRulerEvent::CornerClicked(press.corner));
        }
    }

    fn edit(&mut self, index: usize, start: f64, end: f64, cx: &mut Context<Self>) {
        let mut corners = self.corners.to_vec();
        let Some(corner) = corners.get_mut(index) else {
            return;
        };
        corner.start = start;
        corner.end = end;
        let id = corner.id;
        let complexes = self.complexes.to_vec();
        self.replace(corners, complexes);
        cx.emit(CornerRulerEvent::CornerEdited { id, start, end });
        cx.notify();
    }

    fn spoken(&self) -> SharedString {
        if self.corners.is_empty() {
            return "Corners: none".into();
        }
        let names: Vec<&str> = self.corners.iter().map(|c| c.label.as_ref()).collect();
        let mut text = format!("Corners: {}", names.join(", "));
        if let Some(focused) = self
            .focused
            .and_then(|id| self.corners.iter().find(|c| c.id == id))
        {
            text.push_str(&format!("; focused {}", focused.label));
        }
        if self.editing {
            text.push_str("; editing");
        }
        text.into()
    }
}

fn ruler_context<'a>(
    corners: &[CornerBand],
    spans: &'a [CornerSpan],
    focused: Option<u32>,
    editing: bool,
    viewport: Viewport,
    width: f64,
) -> InteractionContext<'a> {
    InteractionContext {
        viewport,
        plot_left: 0.0,
        plot_width: width,
        fit: true,
        has_data: !corners.is_empty(),
        editing_corners: editing,
        corners: spans,
        focused_corner: focused.and_then(|id| corners.iter().position(|c| c.id == id)),
        ..InteractionContext::default()
    }
}

impl Render for CornerRuler {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let palette = TracePalette::from_theme(theme);
        let element = RulerElement {
            ruler: cx.entity().downgrade(),
            corners: self.corners.clone(),
            complexes: self.complexes.clone(),
            viewport: self.viewport.read(cx).viewport(),
            focused: self.focused,
            hovered: self.hovered,
            editing: self.editing,
            dragging: self.interaction.is_dragging(),
            palette,
            label: theme.muted_foreground,
            strong: theme.foreground,
        };
        let has_complexes = !self.complexes.is_empty();
        div()
            .id("corner-ruler")
            .role(Role::Group)
            .aria_label(self.spoken())
            .test_support()
            .relative()
            .w_full()
            .map(|el| if has_complexes { el.h_10() } else { el.h_6() })
            .flex_shrink_0()
            .overflow_hidden()
            .child(element)
    }
}

/// One ruler frame, snapshotted by [`CornerRuler::render`].
struct RulerElement {
    ruler: WeakEntity<CornerRuler>,
    corners: Arc<[CornerBand]>,
    complexes: Arc<[ComplexBand]>,
    viewport: Viewport,
    focused: Option<u32>,
    hovered: Option<(u32, CornerPart)>,
    editing: bool,
    dragging: bool,
    palette: TracePalette,
    label: Hsla,
    strong: Hsla,
}

impl IntoElement for RulerElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

/// Vertical split of the ruler: complex bracket tier above the corner tier.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct RulerTiers {
    pub complex_top: f32,
    pub complex_height: f32,
    pub corner_top: f32,
    pub corner_height: f32,
}

/// Tiers for a ruler `height` logical pixels tall. The bracket tier holds one
/// `text_height` label line plus the bracket; without complexes the corner
/// tier takes everything.
pub(crate) fn ruler_tiers(height: f32, text_height: f32, has_complexes: bool) -> RulerTiers {
    let complex_height = if has_complexes {
        (text_height + 4.0).min(height * 0.5)
    } else {
        0.0
    };
    RulerTiers {
        complex_top: 0.0,
        complex_height,
        corner_top: complex_height,
        corner_height: (height - complex_height).max(0.0),
    }
}

impl Element for RulerElement {
    type RequestLayoutState = ();
    type PrepaintState = Hitbox;

    fn id(&self) -> Option<ElementId> {
        Some("corner-ruler-surface".into())
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
    ) -> Hitbox {
        window.insert_hitbox(bounds, HitboxBehavior::Normal)
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        hitbox: &mut Hitbox,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.paint_ruler(bounds, window, cx);
        let style = match (self.editing, self.hovered.map(|(_, part)| part)) {
            (true, Some(CornerPart::Start | CornerPart::End)) => CursorStyle::ResizeLeftRight,
            (true, Some(CornerPart::Body)) if self.dragging => CursorStyle::ClosedHand,
            (true, Some(CornerPart::Body)) => CursorStyle::OpenHand,
            _ => CursorStyle::Arrow,
        };
        window.set_cursor_style(style, hitbox);
        self.register_input(bounds, hitbox.clone(), window);
    }
}

impl RulerElement {
    fn paint_ruler(&self, bounds: Bounds<Pixels>, window: &mut Window, cx: &mut App) {
        let width = bounds.size.width.as_f32();
        let height = bounds.size.height.as_f32();
        if width <= 0.0 || height <= 0.0 {
            return;
        }
        let palette = &self.palette;
        let text_size = label::xs(window);
        let text_height = text_size.as_f32() * 1.25;
        let tiers = ruler_tiers(height, text_height, !self.complexes.is_empty());
        let x_for =
            |fraction: f64| self.viewport.x_for_fraction(fraction, 0.0, width as f64) as f32;
        let rect = |window: &mut Window, left: f32, top: f32, w: f32, h: f32, color: Hsla| {
            let left_clamped = left.max(0.0);
            let right = (left + w).min(width);
            if right > left_clamped && h > 0.0 {
                window.paint_quad(fill(
                    Bounds::new(
                        bounds.origin + point(px(left_clamped), px(top)),
                        size(px(right - left_clamped), px(h)),
                    ),
                    color,
                ));
            }
        };

        // Complex brackets: a hairline spanning the member corners with end
        // ticks, the name above its start.
        for complex in self.complexes.iter() {
            let (x1, x2) = (x_for(complex.start), x_for(complex.end));
            if x2 <= 0.0 || x1 >= width || x2 - x1 < 1.0 {
                continue;
            }
            let line_y = tiers.complex_top + tiers.complex_height - 2.0;
            let color = self.label.opacity(0.7);
            rect(window, x1, line_y, x2 - x1, 1.0, color);
            rect(window, x1, line_y - 3.0, 1.0, 5.0, color);
            rect(window, x2 - 1.0, line_y - 3.0, 1.0, 5.0, color);
            let visible_left = x1.max(0.0) + 3.0;
            let budget = x2.min(width) - visible_left - 2.0;
            if let Some(line) = label::shape_fitted(
                &complex.name,
                text_size,
                FontWeight::NORMAL,
                self.label,
                budget,
                window,
            ) {
                label::paint(
                    &line,
                    bounds.origin + point(px(visible_left), px(tiers.complex_top)),
                    px(text_height),
                    window,
                    cx,
                );
            }
        }

        // Corner bands.
        let band_top = tiers.corner_top + 2.0;
        let band_height = (tiers.corner_height - 4.0).max(1.0);
        for corner in self.corners.iter() {
            let (x1, x2) = (x_for(corner.start), x_for(corner.end));
            if x2 <= 0.0 || x1 >= width {
                continue;
            }
            let focused = self.focused == Some(corner.id);
            let hovered = self.hovered.map(|(id, _)| id) == Some(corner.id);
            let fill_color = if focused {
                palette.primary.opacity(0.24)
            } else if hovered {
                palette.foreground.opacity(0.12)
            } else {
                palette.foreground.opacity(0.06)
            };
            rect(
                window,
                x1,
                band_top,
                (x2 - x1).max(1.0),
                band_height,
                fill_color,
            );
            if focused {
                rect(window, x1, band_top, 1.0, band_height, palette.primary);
                rect(
                    window,
                    x2 - 1.0,
                    band_top,
                    1.0,
                    band_height,
                    palette.primary,
                );
            }
            if self.editing {
                // Grips: full-height edges in the reference role colour with
                // a wider handle in the middle, stronger when hovered.
                for (edge_x, part) in [(x1, CornerPart::Start), (x2, CornerPart::End)] {
                    let active = self.hovered == Some((corner.id, part));
                    let color = if active {
                        palette.reference
                    } else {
                        palette.reference.opacity(0.7)
                    };
                    rect(window, edge_x - 0.5, band_top, 1.0, band_height, color);
                    let handle = (band_height * 0.5).max(4.0);
                    rect(
                        window,
                        edge_x - 1.5,
                        band_top + (band_height - handle) * 0.5,
                        3.0,
                        handle,
                        color,
                    );
                }
            }
            let visible_left = x1.max(0.0) + 4.0;
            let budget = x2.min(width) - visible_left - 3.0;
            let (color, weight) = if focused {
                (self.strong, FontWeight::SEMIBOLD)
            } else if hovered || self.editing {
                (self.strong, FontWeight::NORMAL)
            } else {
                (self.label, FontWeight::NORMAL)
            };
            if let Some(line) =
                label::shape_fitted(&corner.label, text_size, weight, color, budget, window)
            {
                let top = band_top + (band_height - text_height) * 0.5;
                label::paint(
                    &line,
                    bounds.origin + point(px(visible_left), px(top)),
                    px(text_height),
                    window,
                    cx,
                );
            }
        }
    }

    fn register_input(&self, bounds: Bounds<Pixels>, hitbox: Hitbox, window: &mut Window) {
        let origin = bounds.origin;
        let width = bounds.size.width.as_f32() as f64;
        let local_x =
            move |position: gpui_kit::Point<Pixels>| (position.x - origin.x).as_f32() as f64;

        let ruler = self.ruler.clone();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseDownEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble
                || event.button != MouseButton::Left
                || !hit.is_hovered(window)
            {
                return;
            }
            let x = local_x(event.position);
            ruler
                .update(cx, |ruler, cx| ruler.pointer_down(x, width, cx))
                .ok();
            cx.stop_propagation();
        });

        let ruler = self.ruler.clone();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble {
                return;
            }
            let x = local_x(event.position);
            let inside = hit.is_hovered(window);
            ruler
                .update(cx, |ruler, cx| ruler.pointer_move(x, width, inside, cx))
                .ok();
        });

        let ruler = self.ruler.clone();
        window.on_mouse_event(move |event: &MouseUpEvent, phase, _, cx| {
            if phase != DispatchPhase::Bubble || event.button != MouseButton::Left {
                return;
            }
            let x = local_x(event.position);
            ruler
                .update(cx, |ruler, cx| ruler.pointer_up(x, width, cx))
                .ok();
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tiers_reserve_a_bracket_line_only_with_complexes() {
        let plain = ruler_tiers(24.0, 15.0, false);
        assert_eq!(plain.corner_top, 0.0);
        assert_eq!(plain.corner_height, 24.0);
        let with = ruler_tiers(40.0, 15.0, true);
        assert_eq!(with.complex_height, 19.0);
        assert_eq!(with.corner_top, 19.0);
        assert_eq!(with.corner_height, 21.0);
        // A squeezed ruler never gives brackets more than half.
        let squeezed = ruler_tiers(20.0, 15.0, true);
        assert_eq!(squeezed.complex_height, 10.0);
    }

    fn ctx<'a>(spans: &'a [CornerSpan], focused: Option<usize>) -> InteractionContext<'a> {
        InteractionContext {
            viewport: Viewport::FULL,
            plot_left: 0.0,
            plot_width: 1000.0,
            has_data: true,
            editing_corners: true,
            corners: spans,
            focused_corner: focused,
            ..InteractionContext::default()
        }
    }

    #[test]
    fn ruler_hit_tests_edges_bodies_and_misses() {
        let spans = [CornerSpan::new(0.20, 0.30), CornerSpan::new(0.31, 0.40)];
        let c = ctx(&spans, None);
        let start = hit_corner(203.0, &c).unwrap();
        assert_eq!((start.index, start.part), (0, CornerPart::Start));
        let end = hit_corner(297.0, &c).unwrap();
        assert_eq!((end.index, end.part), (0, CornerPart::End));
        let body = hit_corner(250.0, &c).unwrap();
        assert_eq!((body.index, body.part), (0, CornerPart::Body));
        assert!((body.grab - 0.05).abs() < 1e-12);
        // Between zones but beyond both tolerances: nothing.
        assert!(hit_corner(100.0, &c).is_none());
        // Two edges within tolerance: zone order wins without focus.
        let shared = hit_corner(305.0, &c).unwrap();
        assert_eq!((shared.index, shared.part), (0, CornerPart::End));
        // The focused corner's grip wins, with its wider tolerance.
        let c = ctx(&spans, Some(1));
        let focused = hit_corner(304.0, &c).unwrap();
        assert_eq!((focused.index, focused.part), (1, CornerPart::Start));
    }
}
