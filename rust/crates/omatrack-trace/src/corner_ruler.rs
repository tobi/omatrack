//! `CornerRuler`: the corner row above the trace lanes.
//!
//! Every corner zone is a label on the shared x mapping (the application's
//! [`ViewportState`]), centred over its zone on one of two staggered rows:
//! corners alternate rows by index (T1 T3 T5 above, T2 T4 below), so tight
//! neighbours never fight for one line; where every label fits on one row
//! (few corners, or zoomed in) they share the lower row
//! ([`place_on_one_row`]). A thin rule along the ruler's foot marks each
//! zone under its label (the chip corner's in the foreground). The zones'
//! shading runs through the lanes (the static layer). Corner complexes
//! that group two or more corners are a quiet bracket line above the rows
//! (a single-corner complex only repeats its corner and is not drawn).
//! The chip corner, the focused one or else the one under the shared cursor,
//! is a filled chip in the foreground; a hovered corner brightens.
//!
//! Labels never overlap and are never ellipsized. The ruler uses one form,
//! the short one (`Turn 10A` → `T10A`, as on the map, the table and the
//! palette) at every width and zoom; a corner without a short form keeps
//! its name. When two labels of a row would collide the lower-priority one
//! is dropped. Priority: the chip corner
//! (always labelled), then the hovered one, then wider bands
//! ([`place_labels`]).
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
//! the plot column (beside a spacer as wide as the lane chrome, [`crate::CHROME_REMS`]) for
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
    fill, point, px, relative, rems, size,
};

use omatrack_ui::TypeStep;

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
    cursor_corner: Option<u32>,
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
            cursor_corner: None,
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
        let complexes = grouping_complexes(&corners, complexes);
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

    /// The complexes drawn: those grouping two or more corners.
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

    /// The corner under the shared cursor (controlled by the owner, which
    /// calls this on every cursor move; it repaints only when the corner
    /// changes). It shows as the chip unless another corner is focused.
    pub fn set_cursor_corner(&mut self, id: Option<u32>, cx: &mut Context<Self>) {
        if self.cursor_corner != id {
            self.cursor_corner = id;
            cx.notify();
        }
    }

    pub fn cursor_corner(&self) -> Option<u32> {
        self.cursor_corner
    }

    /// The corner drawn as the filled chip: the focused one, else the one
    /// under the cursor.
    pub fn chip_corner(&self) -> Option<u32> {
        self.focused.or(self.cursor_corner)
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

/// Complexes that group at least two corners (by corner midpoint). The atlas
/// also lists every lone corner as a one-member complex; drawing those would
/// only repeat the corner row.
fn grouping_complexes(corners: &[CornerBand], complexes: Vec<ComplexBand>) -> Vec<ComplexBand> {
    complexes
        .into_iter()
        .filter(|complex| {
            corners
                .iter()
                .filter(|corner| {
                    let middle = (corner.start + corner.end) * 0.5;
                    middle >= complex.start && middle <= complex.end
                })
                .nth(1)
                .is_some()
        })
        .collect()
}

/// The short form of a corner label: `Turn 10A` → `T10A`, `Turn 1` → `T1`.
/// Labels that are already short, or have no turn number, stay as they are.
pub fn short_label(label: &str) -> Option<SharedString> {
    let trimmed = label.trim();
    let rest = ["turn ", "turn", "t "].iter().find_map(|prefix| {
        trimmed
            .get(..prefix.len())
            .filter(|head| head.eq_ignore_ascii_case(prefix))
            .map(|_| trimmed[prefix.len()..].trim_start())
    })?;
    if rest.is_empty() || !rest.starts_with(|c: char| c.is_ascii_digit()) {
        return None;
    }
    let short = format!("T{rest}");
    (short != trimmed).then(|| short.into())
}

/// A corner label to place: its band on screen, its label row (corners
/// alternate between the two rows by index) and the widths of its full and
/// short forms (`short` is `None` without a distinct short form).
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct LabelCandidate {
    pub left: f32,
    pub right: f32,
    pub full: f32,
    pub short: Option<f32>,
    /// 0 for the upper row, 1 for the lower.
    pub row: u8,
    /// The chip corner (focused or under the cursor, always labelled) is 2,
    /// a hovered one 1: placed first.
    pub priority: u8,
}

/// Which form of a label was placed, and where (left edge).
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum LabelForm {
    Full,
    Short,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct Placement {
    pub form: LabelForm,
    pub x: f32,
}

/// A zone rule's distance above the ruler's foot, logical pixels.
const ZONE_RULE_INSET: f32 = 2.0;
/// Emphasis of a zone rule under an unfocused label.
const ZONE_RULE_ALPHA: f32 = 0.45;

/// Minimum clear space between two labels of one row, logical pixels.
const LABEL_GAP: f32 = 6.0;

/// Collision-free label placement over a ruler `width` pixels wide, on two
/// staggered rows.
///
/// Corners alternate rows by index (T1 T3 T5 above, T2 T4 below), so a
/// label may span its neighbours' bands: it only has to clear the labels of
/// its own row. One form for the whole ruler, the short one (`Turn 10A` →
/// `T10A`): the same names the map and the tables use, at every width.
/// Within a row,
/// candidates go in priority order (higher `priority`, then wider bands),
/// centred on their visible band and clamped into the ruler; a label that
/// would come within [`LABEL_GAP`] of one already placed is dropped. The
/// chip corner (`priority >= 2`) is always placed.
pub(crate) fn place_labels(candidates: &[LabelCandidate], width: f32) -> Vec<Option<Placement>> {
    place_in_rows(candidates, width, true)
}

/// The labels on one row, where every visible label fits there without
/// coming within [`LABEL_GAP`] of another: a sparse ruler (few corners, or
/// zoomed in) reads as one line. `None` when any would collide, and the
/// ruler staggers ([`place_labels`]).
pub(crate) fn place_on_one_row(
    candidates: &[LabelCandidate],
    width: f32,
) -> Option<Vec<Option<Placement>>> {
    let one_row: Vec<LabelCandidate> = candidates
        .iter()
        .map(|c| LabelCandidate {
            row: 0,
            priority: c.priority.min(1),
            ..*c
        })
        .collect();
    let placed = place_in_rows(&one_row, width, true);
    let every = one_row.iter().zip(&placed).all(|(c, p)| {
        let visible = c.full.is_finite() && c.right.min(width) > c.left.max(0.0);
        !visible || p.is_some()
    });
    every.then_some(placed)
}

fn place_in_rows(candidates: &[LabelCandidate], width: f32, short: bool) -> Vec<Option<Placement>> {
    let mut order: Vec<usize> = (0..candidates.len()).collect();
    order.sort_by(|&a, &b| {
        let (a, b) = (&candidates[a], &candidates[b]);
        b.priority
            .cmp(&a.priority)
            .then((b.right - b.left).total_cmp(&(a.right - a.left)))
    });
    let mut placed: Vec<Option<Placement>> = vec![None; candidates.len()];
    let mut taken: Vec<(u8, f32, f32)> = Vec::with_capacity(candidates.len());
    for index in order {
        let c = &candidates[index];
        let left = c.left.max(0.0);
        let right = c.right.min(width);
        if right <= left {
            continue;
        }
        // In a short row a corner without a short form keeps its name.
        let (form, w) = match c.short {
            Some(w) if short => (LabelForm::Short, w),
            _ => (LabelForm::Full, c.full),
        };
        if !w.is_finite() {
            continue;
        }
        let x = ((left + right - w) * 0.5).clamp(0.0, (width - w).max(0.0));
        let free = taken
            .iter()
            .filter(|(row, _, _)| *row == c.row)
            .all(|&(_, l, r)| x + w + LABEL_GAP <= l || x >= r + LABEL_GAP);
        if free || c.priority >= 2 {
            taken.push((c.row, x, x + w));
            placed[index] = Some(Placement { form, x });
        }
    }
    placed
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
            chip: self.chip_corner(),
            hovered: self.hovered,
            editing: self.editing,
            dragging: self.interaction.is_dragging(),
            palette,
            label: theme.muted_foreground,
            strong: theme.foreground,
            chip_text: theme.background,
            radius: theme.radius,
        };
        let has_complexes = !self.complexes.is_empty();
        div()
            .id("corner-ruler")
            .role(Role::Group)
            .aria_label(self.spoken())
            .test_support()
            .relative()
            .w_full()
            .h(rems(ruler_height_rems(has_complexes)))
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
    chip: Option<u32>,
    hovered: Option<(u32, CornerPart)>,
    editing: bool,
    dragging: bool,
    palette: TracePalette,
    label: Hsla,
    strong: Hsla,
    chip_text: Hsla,
    radius: Pixels,
}

impl IntoElement for RulerElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

/// The ruler's height: two staggered label rows, plus a bracket line when
/// complexes are drawn.
pub fn ruler_height_rems(has_complexes: bool) -> f32 {
    if has_complexes { 3.25 } else { 2.25 }
}

/// Vertical split of the ruler: complex bracket tier above the two corner
/// label rows.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct RulerTiers {
    pub complex_top: f32,
    pub complex_height: f32,
    /// Top of the upper and lower label rows.
    pub row_top: [f32; 2],
    pub row_height: f32,
}

/// Tiers for a ruler `height` logical pixels tall. The bracket tier holds one
/// `text_height` label line plus the bracket; without complexes the two
/// label rows share everything.
pub(crate) fn ruler_tiers(height: f32, text_height: f32, has_complexes: bool) -> RulerTiers {
    let complex_height = if has_complexes {
        (text_height + 4.0).min(height * 0.34)
    } else {
        0.0
    };
    let row_height = ((height - complex_height) * 0.5).max(0.0);
    RulerTiers {
        complex_top: 0.0,
        complex_height,
        row_top: [complex_height, complex_height + row_height],
        row_height,
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
        let text_size = TypeStep::Label.size(window);
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

        // Complex brackets: a dimension line spanning the member corners,
        // end ticks and the name centred in a break of the line. Quiet: it
        // groups, the corners below carry the content.
        let bracket = self.label.opacity(0.45);
        for complex in self.complexes.iter() {
            let (x1, x2) = (x_for(complex.start), x_for(complex.end));
            if x2 <= 0.0 || x1 >= width || x2 - x1 < 4.0 {
                continue;
            }
            let line_y = (tiers.complex_top + tiers.complex_height * 0.5).round();
            let (left, right) = (x1.max(0.0), x2.min(width));
            rect(window, x1 + 0.5, line_y - 2.0, 1.0, 5.0, bracket);
            rect(window, x2 - 1.5, line_y - 2.0, 1.0, 5.0, bracket);
            let line = label::shape(
                complex.name.clone(),
                text_size,
                FontWeight::NORMAL,
                self.label,
                window,
            );
            let text_width = line.width().as_f32();
            if text_width + 2.0 * LABEL_GAP <= right - left {
                let text_left = ((left + right - text_width) * 0.5).round();
                rect(
                    window,
                    x1,
                    line_y,
                    text_left - LABEL_GAP * 0.5 - x1,
                    1.0,
                    bracket,
                );
                let text_right = text_left + text_width + LABEL_GAP * 0.5;
                rect(window, text_right, line_y, x2 - text_right, 1.0, bracket);
                label::paint(
                    &line,
                    bounds.origin + point(px(text_left), px(line_y - (text_height * 0.5).round())),
                    px(text_height),
                    window,
                    cx,
                );
            } else {
                rect(window, x1, line_y, x2 - x1, 1.0, bracket);
            }
        }

        // Hover and edit tints: a quiet column over the zone. At rest the
        // ruler is only labels; the zones' own shading runs through the
        // lanes below.
        let hovered_id = self.hovered.map(|(id, _)| id);
        for corner in self.corners.iter() {
            let (x1, x2) = (x_for(corner.start), x_for(corner.end));
            if x2 <= 0.0 || x1 >= width {
                continue;
            }
            let hovered = hovered_id == Some(corner.id);
            let tint = if hovered {
                palette.foreground.opacity(0.08)
            } else if self.editing {
                palette.foreground.opacity(0.045)
            } else {
                continue;
            };
            let band_width = (x2 - x1 - 1.0).max(1.0);
            rect(
                window,
                x1,
                tiers.row_top[0],
                band_width,
                tiers.row_height * 2.0,
                tint,
            );
            if self.editing {
                // Grips: full-height edges in the reference role colour with
                // a wider handle in the middle, stronger when hovered.
                let (top, h) = (tiers.row_top[0], tiers.row_height * 2.0);
                for (edge_x, part) in [(x1, CornerPart::Start), (x2, CornerPart::End)] {
                    let active = self.hovered == Some((corner.id, part));
                    let color = if active {
                        palette.reference
                    } else {
                        palette.reference.opacity(0.7)
                    };
                    rect(window, edge_x - 0.5, top, 1.0, h, color);
                    let handle = (h * 0.5).max(4.0);
                    rect(
                        window,
                        edge_x - 1.5,
                        top + (h - handle) * 0.5,
                        3.0,
                        handle,
                        color,
                    );
                }
            }
        }

        // A rule under each zone along the ruler's foot: where the corner
        // runs, under its label; the chip corner's in the foreground.
        if !self.editing {
            let rule_y = height - ZONE_RULE_INSET;
            for corner in self.corners.iter() {
                let (x1, x2) = (x_for(corner.start), x_for(corner.end));
                if x2 <= 0.0 || x1 >= width || x2 - x1 < 3.0 {
                    continue;
                }
                let color = if self.chip == Some(corner.id) {
                    self.strong
                } else {
                    self.label.opacity(ZONE_RULE_ALPHA)
                };
                rect(window, x1 + 1.0, rule_y, x2 - x1 - 2.0, 1.0, color);
            }
        }

        // Labels on two staggered rows by corner index; the chip corner
        // (focused, else under the cursor) is a filled chip in the
        // foreground with its label in the background colour.
        let chip_pad = (text_size.as_f32() * 0.5).round();
        let style = |corner: &CornerBand| {
            if self.chip == Some(corner.id) {
                (self.chip_text, FontWeight::MEDIUM, 2)
            } else if hovered_id == Some(corner.id) || self.editing {
                (self.strong, FontWeight::NORMAL, 1)
            } else {
                (self.label, FontWeight::NORMAL, 0)
            }
        };
        let mut shaped = Vec::with_capacity(self.corners.len());
        let mut candidates = Vec::with_capacity(self.corners.len());
        for (index, corner) in self.corners.iter().enumerate() {
            let (x1, x2) = (x_for(corner.start), x_for(corner.end));
            let (color, weight, priority) = style(corner);
            let visible = x2 > 0.0 && x1 < width;
            let pad = if priority >= 2 { 2.0 * chip_pad } else { 0.0 };
            let full = visible
                .then(|| label::shape(corner.label.clone(), text_size, weight, color, window));
            let short = visible
                .then(|| short_label(&corner.label))
                .flatten()
                .map(|text| label::shape(text, text_size, weight, color, window));
            candidates.push(LabelCandidate {
                left: x1,
                right: x2,
                full: full
                    .as_ref()
                    .map_or(f32::INFINITY, |l| l.width().as_f32() + pad),
                short: short.as_ref().map(|l| l.width().as_f32() + pad),
                row: (index % 2) as u8,
                priority: if visible { priority } else { 0 },
            });
            shaped.push((full, short));
        }
        // One row where every label fits on it, else staggered.
        let single = place_on_one_row(&candidates, width);
        let one_row = single.is_some();
        let placements = single.unwrap_or_else(|| place_labels(&candidates, width));
        for ((placement, (full, short)), candidate) in
            placements.into_iter().zip(&shaped).zip(&candidates)
        {
            let Some(placement) = placement else {
                continue;
            };
            let line = match placement.form {
                LabelForm::Full => full.as_ref(),
                LabelForm::Short => short.as_ref(),
            };
            let Some(line) = line else {
                continue;
            };
            // A single row sits in the lower row, nearest its zone rule.
            let row_top = tiers.row_top[if one_row { 1 } else { candidate.row as usize }];
            let top = (row_top + (tiers.row_height - text_height) * 0.5).round();
            let mut x = placement.x.round();
            if candidate.priority >= 2 {
                let chip_width = line.width().as_f32() + 2.0 * chip_pad;
                let chip_top = row_top + 1.0;
                let chip_height = (tiers.row_height - 2.0).max(text_height);
                window.paint_quad(
                    fill(
                        Bounds::new(
                            bounds.origin + point(px(x), px(chip_top)),
                            size(px(chip_width), px(chip_height)),
                        ),
                        self.strong,
                    )
                    .corner_radii(self.radius.min(px(chip_height * 0.5))),
                );
                x += chip_pad;
            }
            label::paint(
                line,
                bounds.origin + point(px(x), px(top)),
                px(text_height),
                window,
                cx,
            );
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

    fn candidate(left: f32, right: f32, full: f32, short: Option<f32>, row: u8) -> LabelCandidate {
        LabelCandidate {
            left,
            right,
            full,
            short,
            row,
            priority: 0,
        }
    }

    #[test]
    fn short_labels() {
        assert_eq!(short_label("Turn 10A").as_deref(), Some("T10A"));
        assert_eq!(short_label("turn 1").as_deref(), Some("T1"));
        assert_eq!(short_label("T5"), None);
        assert_eq!(short_label("Bus Stop"), None);
        assert_eq!(short_label("Turn"), None);
    }

    #[test]
    fn staggered_rows_only_collide_within_a_row() {
        // Three narrow neighbours: on one row the middle label would
        // collide, staggered it clears both (they are on the other row).
        let placed = place_labels(
            &[
                candidate(100.0, 130.0, 40.0, Some(16.0), 0),
                candidate(130.0, 160.0, 40.0, Some(16.0), 1),
                candidate(160.0, 190.0, 40.0, Some(16.0), 0),
            ],
            1000.0,
        );
        // Short forms even with room for full names, centred.
        assert_eq!(
            placed[0],
            Some(Placement {
                form: LabelForm::Short,
                x: 107.0
            })
        );
        assert!(
            placed
                .iter()
                .all(|p| p.map(|p| p.form) == Some(LabelForm::Short))
        );

        // Tight: short forms throughout.
        let placed = place_labels(
            &[
                candidate(100.0, 120.0, 40.0, Some(16.0), 0),
                candidate(120.0, 140.0, 40.0, Some(16.0), 1),
                candidate(140.0, 160.0, 40.0, Some(16.0), 0),
            ],
            1000.0,
        );
        assert!(
            placed
                .iter()
                .all(|p| p.map(|p| p.form) == Some(LabelForm::Short))
        );

        // Same row and overlapping: the wider band wins, the other drops.
        let placed = place_labels(
            &[
                candidate(100.0, 160.0, 26.0, None, 0),
                candidate(120.0, 190.0, 26.0, None, 0),
            ],
            1000.0,
        );
        assert_eq!(placed[0], None);
        assert!(placed[1].is_some());
    }

    #[test]
    fn a_sparse_ruler_reads_on_one_row() {
        // Wide apart: one row.
        let sparse = [
            candidate(100.0, 160.0, 40.0, Some(16.0), 0),
            candidate(300.0, 360.0, 40.0, Some(16.0), 1),
        ];
        let placed = place_on_one_row(&sparse, 1000.0).expect("fits on one row");
        assert!(placed.iter().all(Option::is_some));
        // Neighbours whose labels would touch: staggered.
        let dense = [
            candidate(100.0, 120.0, 40.0, Some(16.0), 0),
            candidate(120.0, 140.0, 40.0, Some(16.0), 1),
        ];
        assert_eq!(place_on_one_row(&dense, 1000.0), None);
    }

    #[test]
    fn the_chip_label_is_always_placed_first() {
        let mut chip = candidate(100.0, 110.0, 50.0, None, 0);
        chip.priority = 2;
        let placed = place_labels(&[candidate(60.0, 100.0, 30.0, None, 0), chip], 1000.0);
        let label = placed[1].unwrap();
        assert_eq!(label.x, 80.0);
        // Its neighbour on the same row now collides and gives way.
        assert_eq!(placed[0], None);
        // Clamped into the ruler at the edge.
        let mut edge = candidate(-50.0, 4.0, 50.0, Some(24.0), 1);
        edge.priority = 2;
        assert_eq!(place_labels(&[edge], 1000.0)[0].unwrap().x, 0.0);
    }

    #[test]
    fn only_grouping_complexes_are_drawn() {
        let corners = [
            CornerBand::new(1, "T1", 0.10, 0.20),
            CornerBand::new(2, "T2", 0.30, 0.40),
            CornerBand::new(3, "T3", 0.42, 0.50),
        ];
        let kept = grouping_complexes(
            &corners,
            vec![
                ComplexBand::new("T1", 0.10, 0.20),
                ComplexBand::new("Esses", 0.30, 0.50),
            ],
        );
        assert_eq!(kept.len(), 1);
        assert_eq!(kept[0].name.as_ref(), "Esses");
    }

    #[test]
    fn tiers_split_two_label_rows_under_an_optional_bracket_line() {
        let plain = ruler_tiers(36.0, 15.0, false);
        assert_eq!(plain.complex_height, 0.0);
        assert_eq!(plain.row_top, [0.0, 18.0]);
        assert_eq!(plain.row_height, 18.0);
        let with = ruler_tiers(52.0, 15.0, true);
        assert_eq!(with.complex_height, 17.68);
        assert_eq!(with.row_top[0], with.complex_height);
        assert!((with.row_height - 17.16).abs() < 1e-4);
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
