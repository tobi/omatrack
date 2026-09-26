//! The lap strip: one session's laps as a proportional row of cells (port of
//! `src/app/FilmstripLayout.h` and `LapFilmstrip.qml`).
//!
//! Every driven interval (out, in, flying, fragment) shares the row by
//! driving time, with a 12 px selectable floor so a fully stopped segment
//! stays clickable; a pit stop is one fixed 36 px cell however long the car
//! stood. Stopped time never earns space. The strip carries no tooltips:
//! the cells are the readout.
//!
//! Each cell is a gpui-component [`Button`], so hover, pressed, keyboard
//! focus, activation and accessibility come from the component system. The
//! primary lap is filled with the primary role colour, the reference lap is
//! outlined in the reference (warning) role colour, the session's best lap
//! carries a `success` underline.
//!
//! Selection is controlled: the owner passes the current primary and
//! reference lap ids and receives requests through [`LapStrip::on_select`].
//! A plain click (or Enter / Space) asks for the strip's own role
//! ([`LapStrip::role`], primary by default); a right click or Alt+click
//! asks for the reference role.
//!
//! Layout: [`lap_strip_layout`] owns the pixel budget. The row is a custom
//! element that resolves its width, runs the layout and places the cell
//! buttons at the resulting x and width (measured runtime geometry, the
//! documented `px` exception), so rendering and the pure function can never
//! disagree.

use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::base::TestSupportExt as _;
use gpui_kit::component::button::{Button, ButtonRounded, ButtonVariants as _};
use gpui_kit::component::{ActiveTheme as _, Selectable as _, Sizable as _, StyledExt as _};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, AvailableSpace, Bounds, ClickEvent, Element, ElementId, GlobalElementId,
    InspectorElementId, InteractiveElement as _, IntoElement, LayoutId, ParentElement as _, Pixels,
    RenderOnce, Role, SharedString, StatefulInteractiveElement as _, Style, Styled as _, Window,
    div, point, px, relative, size,
};
use omatrack_core::session::{LapStripCell, LapStripKind};

use crate::{LapRole, TypeScale as _};

/// Width of a pit-stop cell, logical pixels.
pub const PIT_STOP_CELL: f32 = 36.0;
/// Selectable floor of a driven cell, logical pixels.
pub const MIN_CELL: f32 = 12.0;

/// Horizontal room a cell keeps around its text, logical pixels.
const CELL_TEXT_INSET: f32 = 6.0;

/// Advance of one tabular-figure `text_xs` character, as a share of the rem
/// (0.75 rem × a 0.6 em advance, rounded up for wider desktop faces).
const FIGURE_XS_ADVANCE: f32 = 0.47;
/// Widest gap between cells, logical pixels.
pub const MAX_GAP: f32 = 3.0;

/// The pixel budget of one strip (port of `omatrack::FilmstripCells`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct StripCells {
    /// Gap between neighbouring cells.
    pub spacing: f32,
    /// Width of each fixed (pit-stop) cell.
    pub fixed: f32,
    /// Floor of each variable cell.
    pub minimum: f32,
    /// Width shared by the variable cells in proportion to driven time.
    pub flexible: f32,
}

/// Split `width` among `count` cells of which `fixed` are pit stops. Fixed
/// cells keep their size before any pixel goes to gaps; dense sessions
/// tighten spacing rather than shrinking pit-stop cells.
pub fn strip_cells(width: f32, count: usize, fixed: usize) -> StripCells {
    let width = width as f64;
    if !width.is_finite() || width <= 0.0 || count == 0 {
        return StripCells::default();
    }
    let fixed = fixed.min(count);
    let variable = (count - fixed) as f64;
    let (count_f, fixed_f) = (count as f64, fixed as f64);
    let selectable = (width / count_f).min(1.0);
    let fixed_width = if fixed > 0 {
        ((width - variable * selectable) / fixed_f).clamp(0.0, PIT_STOP_CELL as f64)
    } else {
        0.0
    };
    let gap = if count > 1 {
        ((width - fixed_f * fixed_width - variable * selectable) / (count_f - 1.0))
            .clamp(0.0, MAX_GAP as f64)
    } else {
        0.0
    };
    let usable = (width - gap * (count_f - 1.0)).max(0.0);
    let remaining = (usable - fixed_f * fixed_width).max(0.0);
    let minimum = if variable > 0.0 {
        (remaining / variable).min(MIN_CELL as f64)
    } else {
        0.0
    };
    StripCells {
        spacing: gap as f32,
        fixed: fixed_width as f32,
        minimum: minimum as f32,
        flexible: (remaining - variable * minimum).max(0.0) as f32,
    }
}

/// Horizontal placement of one cell.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[non_exhaustive]
pub struct CellSpan {
    pub x: f32,
    pub width: f32,
}

/// Place every item of a strip `width` logical pixels wide: pit stops at
/// their fixed width, driven intervals at the floor plus their share of
/// driven time (equal shares when no time is known).
pub fn lap_strip_layout(width: f32, items: &[LapStripItem]) -> Vec<CellSpan> {
    let fixed = items.iter().filter(|item| item.pit_stop).count();
    let cells = strip_cells(width, items.len(), fixed);
    let driven = |item: &LapStripItem| {
        if item.driven_s.is_finite() {
            item.driven_s.max(0.0)
        } else {
            0.0
        }
    };
    let variable = items.len() - fixed;
    let total: f64 = items.iter().filter(|i| !i.pit_stop).map(driven).sum();
    let weight = |item: &LapStripItem| -> f64 {
        if total > 0.0 {
            driven(item) / total
        } else if variable > 0 {
            1.0 / variable as f64
        } else {
            0.0
        }
    };
    let mut spans = Vec::with_capacity(items.len());
    let (mut fixed_before, mut offset) = (0usize, 0.0f64);
    for (index, item) in items.iter().enumerate() {
        let x = fixed_before as f32 * cells.fixed
            + index as f32 * cells.spacing
            + (index - fixed_before) as f32 * cells.minimum
            + cells.flexible * offset.clamp(0.0, 1.0) as f32;
        let width = if item.pit_stop {
            fixed_before += 1;
            cells.fixed
        } else {
            let share = weight(item);
            offset += share;
            cells.minimum + cells.flexible * share.clamp(0.0, 1.0) as f32
        };
        spans.push(CellSpan { x, width });
    }
    spans
}

/// One cell of the strip.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub struct LapStripItem {
    /// Stable lap id (the session's lap index).
    pub lap_id: i32,
    /// `L8`, `Out`, `In`, `Pit`, `Frag`.
    pub label: SharedString,
    /// Formatted lap time of a complete lap (`1:13.644`).
    pub time: Option<SharedString>,
    /// Driving time, seconds: lap time minus clearly stopped time.
    pub driven_s: f64,
    /// A stationary pit stop: one fixed cell.
    pub pit_stop: bool,
    /// The session's representative fastest lap.
    pub best: bool,
}

impl LapStripItem {
    pub fn new(lap_id: i32, label: impl Into<SharedString>, driven_s: f64) -> Self {
        Self {
            lap_id,
            label: label.into(),
            time: None,
            driven_s,
            pit_stop: false,
            best: false,
        }
    }

    pub fn time(mut self, time: impl Into<SharedString>) -> Self {
        self.time = Some(time.into());
        self
    }

    pub fn pit_stop(mut self, pit_stop: bool) -> Self {
        self.pit_stop = pit_stop;
        self
    }

    pub fn best(mut self, best: bool) -> Self {
        self.best = best;
        self
    }

    /// What the cell shows: the time of a complete lap, else its label.
    pub fn text(&self) -> SharedString {
        self.time.clone().unwrap_or_else(|| self.label.clone())
    }

    /// The full description of a cell, for its tooltip.
    pub fn tooltip(&self) -> SharedString {
        match &self.time {
            Some(time) => format!("{} · {time}", self.label).into(),
            None => self.label.clone(),
        }
    }

    /// What fits a cell `width` logical pixels wide when one tabular
    /// character takes `char_width`: the label and time (`L3  1:21.004`),
    /// else the time, else the label (`In`, `L3`),
    /// else nothing (the cell keeps its spoken label and tooltip). Never a
    /// clipped or ellipsized fragment.
    pub fn text_for_width(&self, width: f32, char_width: f32) -> Option<SharedString> {
        let fits = |text: &str| text.chars().count() as f32 * char_width + CELL_TEXT_INSET <= width;
        let labelled = self
            .time
            .as_ref()
            .map(|time| SharedString::from(format!("{}  {time}", self.label)));
        [labelled.as_ref(), self.time.as_ref(), Some(&self.label)]
            .into_iter()
            .flatten()
            .find(|text| fits(text))
            .cloned()
    }
}

impl From<&LapStripCell> for LapStripItem {
    fn from(cell: &LapStripCell) -> Self {
        let item = Self::new(cell.lap_id, cell.label.clone(), cell.driven_s)
            .pit_stop(cell.kind == LapStripKind::PitStop)
            .best(cell.best);
        if cell.complete && cell.time_ms.is_finite() && cell.time_ms > 0.0 {
            item.time(omatrack_core::format_lap_time(cell.time_ms))
        } else {
            item
        }
    }
}

/// A selection request from the strip.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub struct LapSelect {
    pub lap_id: i32,
    /// [`LapRole::Reference`] for a right click or Alt+click, else the
    /// strip's own role.
    pub role: LapRole,
    /// A right click or Alt+click ("compare against this lap"), not the
    /// plain activation.
    pub secondary: bool,
}

impl LapSelect {
    pub fn new(lap_id: i32, role: LapRole) -> Self {
        Self {
            lap_id,
            role,
            secondary: false,
        }
    }

    /// The request of a right click or Alt+click: the reference role.
    pub fn compare(lap_id: i32) -> Self {
        Self {
            lap_id,
            role: LapRole::Reference,
            secondary: true,
        }
    }
}

type SelectHandler = Rc<dyn Fn(&LapSelect, &mut Window, &mut App)>;

/// A session's laps as a proportional strip of cells. See the module docs.
#[derive(IntoElement)]
pub struct LapStrip {
    id: ElementId,
    items: Arc<[LapStripItem]>,
    role: LapRole,
    primary: Option<i32>,
    reference: Option<i32>,
    primary_playhead: Option<f64>,
    reference_playhead: Option<f64>,
    on_select: Option<SelectHandler>,
}

impl LapStrip {
    /// `id` should name the session (for example `("lap-strip", session)`);
    /// cells are identified by lap id inside it.
    pub fn new(id: impl Into<ElementId>, items: impl Into<Arc<[LapStripItem]>>) -> Self {
        Self {
            id: id.into(),
            items: items.into(),
            role: LapRole::Primary,
            primary: None,
            reference: None,
            primary_playhead: None,
            reference_playhead: None,
            on_select: None,
        }
    }

    /// The role a plain click (or Enter / Space) asks for: the role this
    /// strip's recording plays. Primary by default.
    pub fn role(mut self, role: LapRole) -> Self {
        self.role = role;
        self
    }

    /// The primary lap of this session, if it is one of these laps.
    pub fn primary(mut self, lap_id: Option<i32>) -> Self {
        self.primary = lap_id;
        self
    }

    /// The reference lap of this session, if it is one of these laps.
    pub fn reference(mut self, lap_id: Option<i32>) -> Self {
        self.reference = lap_id;
        self
    }

    /// Playhead inside the primary cell, lap fraction.
    pub fn primary_playhead(mut self, fraction: Option<f64>) -> Self {
        self.primary_playhead = fraction.filter(|f| f.is_finite());
        self
    }

    /// Playhead inside the reference cell (reference lap fraction, through
    /// the shared map).
    pub fn reference_playhead(mut self, fraction: Option<f64>) -> Self {
        self.reference_playhead = fraction.filter(|f| f.is_finite());
        self
    }

    /// Requested selection: a plain click asks for the strip's role, a
    /// right click or Alt+click for the reference role. Runs after the
    /// click; the owner updates its model and renders the strip again (a
    /// request for the lap a role already holds is the owner's to
    /// interpret, e.g. as "back to the lap start").
    pub fn on_select(
        mut self,
        handler: impl Fn(&LapSelect, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_select = Some(Rc::new(handler));
        self
    }
}

impl RenderOnce for LapStrip {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        let spoken: SharedString = match self.items.len() {
            0 => "Laps: none".into(),
            1 => "Laps: 1 lap".into(),
            n => format!("Laps: {n} laps").into(),
        };
        let empty = self.items.is_empty();
        let muted = theme.muted_foreground;
        div()
            .id(self.id)
            .role(Role::Group)
            .aria_label(spoken)
            .test_support()
            .relative()
            .w_full()
            .h_6()
            .flex_shrink_0()
            .when(empty, |el| {
                el.flex()
                    .items_center()
                    .px_2()
                    .text_label()
                    .text_color(muted)
                    .child("No laps")
            })
            .when(!empty, |el| {
                el.child(StripCellsElement {
                    items: self.items,
                    role: self.role,
                    primary: self.primary,
                    reference: self.reference,
                    primary_playhead: self.primary_playhead,
                    reference_playhead: self.reference_playhead,
                    on_select: self.on_select,
                })
            })
    }
}

/// The custom element that places the cell buttons at their computed spans.
struct StripCellsElement {
    items: Arc<[LapStripItem]>,
    role: LapRole,
    primary: Option<i32>,
    reference: Option<i32>,
    primary_playhead: Option<f64>,
    reference_playhead: Option<f64>,
    on_select: Option<SelectHandler>,
}

impl IntoElement for StripCellsElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for StripCellsElement {
    type RequestLayoutState = ();
    type PrepaintState = Vec<AnyElement>;

    fn id(&self) -> Option<ElementId> {
        Some("lap-strip-cells".into())
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
        cx: &mut App,
    ) -> Vec<AnyElement> {
        let width = bounds.size.width.as_f32();
        let height = bounds.size.height.as_f32();
        let spans = lap_strip_layout(width, &self.items);
        let char_width = window.rem_size().as_f32() * FIGURE_XS_ADVANCE;
        let mut cells = Vec::with_capacity(self.items.len());
        for (item, span) in self.items.iter().zip(spans) {
            if span.width <= 0.0 {
                continue;
            }
            let mut cell = self.cell(item, span.width, height, char_width, cx);
            cell.layout_as_root(
                size(
                    AvailableSpace::Definite(px(span.width)),
                    AvailableSpace::Definite(px(height)),
                ),
                window,
                cx,
            );
            cell.prepaint_at(bounds.origin + point(px(span.x), px(0.)), window, cx);
            cells.push(cell);
        }
        cells
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        cells: &mut Vec<AnyElement>,
        window: &mut Window,
        cx: &mut App,
    ) {
        for cell in cells {
            cell.paint(window, cx);
        }
    }
}

impl StripCellsElement {
    fn cell(
        &self,
        item: &LapStripItem,
        width: f32,
        height: f32,
        char_width: f32,
        cx: &App,
    ) -> AnyElement {
        let theme = cx.theme();
        let is_primary = self.primary == Some(item.lap_id);
        let is_reference = !is_primary && self.reference == Some(item.lap_id);
        let playhead = if is_primary {
            self.primary_playhead
        } else if is_reference {
            self.reference_playhead
        } else {
            None
        };
        // Role colours: primary fills, reference outlines; a quiet surface
        // for the rest. Text on a filled cell follows the variant.
        let text_color = if is_primary {
            theme.primary_foreground
        } else if is_reference {
            theme.warning
        } else if item.best {
            theme.success
        } else if item.pit_stop {
            theme.muted_foreground
        } else {
            theme.secondary_foreground
        };
        let playhead_color = if is_primary {
            theme.primary_foreground
        } else {
            theme.warning
        };

        let mut spoken = format!("{} {}", item.label, item.time.as_deref().unwrap_or(""));
        spoken = spoken.trim_end().to_string();
        if item.best {
            spoken.push_str(", best lap");
        }
        if is_primary {
            spoken.push_str(", primary");
        } else if is_reference {
            spoken.push_str(", reference");
        }

        let lap_id = item.lap_id;
        let row_role = self.role;
        let on_select = self.on_select.clone();
        let mut button = Button::new(("lap-strip-cell", lap_id as u32))
            .xsmall()
            .rounded(ButtonRounded::Small)
            .map(|b| {
                if is_primary {
                    b.primary()
                } else if is_reference {
                    b.warning().outline()
                } else {
                    b.secondary()
                }
            })
            .selected(is_primary || is_reference)
            .accessibility_label(SharedString::from(spoken))
            .w(px(width))
            .h(px(height))
            .when(width < 28.0, |b| b.px_0())
            .when_some(item.text_for_width(width, char_width), |b, text| {
                b.child(
                    div()
                        .min_w_0()
                        .overflow_hidden()
                        .whitespace_nowrap()
                        .text_label()
                        .numeric()
                        .text_color(text_color)
                        .when(is_primary || is_reference, |d| d.font_semibold())
                        .child(text),
                )
            })
            .tooltip(item.tooltip())
            .when(item.best, |b| {
                b.child(
                    div()
                        .absolute()
                        .left_0()
                        .right_0()
                        .bottom_0()
                        .h_0p5()
                        .bg(theme.success),
                )
            })
            // The playhead is progress along the cell's top edge, never a
            // line through the text.
            .when_some(playhead, |b, fraction| {
                b.child(
                    div()
                        .absolute()
                        .top_0()
                        .left_0()
                        .h_0p5()
                        .w(relative(fraction.clamp(0.0, 1.0) as f32))
                        .bg(playhead_color),
                )
            })
            .when_some(on_select.clone(), |b, handler| {
                b.on_click(move |event: &ClickEvent, window, cx| {
                    let request = if event.modifiers().alt {
                        LapSelect::compare(lap_id)
                    } else {
                        LapSelect::new(lap_id, row_role)
                    };
                    handler(&request, window, cx);
                })
            });
        // A right click compares against the lap (the Qt filmstrip's
        // "set comparison").
        if let Some(handler) = on_select {
            button
                .interactivity()
                .on_aux_click(move |event: &ClickEvent, window, cx| {
                    if event.is_right_click() {
                        handler(&LapSelect::compare(lap_id), window, cx);
                    }
                });
        }
        button.into_any_element()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lap(id: i32, driven: f64) -> LapStripItem {
        LapStripItem::new(id, format!("L{id}"), driven)
    }

    fn pit(id: i32) -> LapStripItem {
        LapStripItem::new(id, "Pit", 0.0).pit_stop(true)
    }

    #[test]
    fn cell_text_is_the_time_the_label_or_nothing_never_a_fragment() {
        let lap = lap(3, 90.0).time("1:21.004");
        assert_eq!(
            lap.text_for_width(120.0, 7.0).as_deref(),
            Some("L3  1:21.004")
        );
        assert_eq!(lap.text_for_width(80.0, 7.0).as_deref(), Some("1:21.004"));
        assert_eq!(lap.text_for_width(30.0, 7.0).as_deref(), Some("L3"));
        assert_eq!(lap.text_for_width(12.0, 7.0), None);
        // A trailing in-lap at the 12 px floor shows nothing, not "I…".
        let in_lap = LapStripItem::new(9, "In", 5.0);
        assert_eq!(in_lap.text_for_width(MIN_CELL, 7.0), None);
        assert_eq!(in_lap.text_for_width(24.0, 7.0).as_deref(), Some("In"));
        assert_eq!(lap.tooltip().as_ref(), "L3 · 1:21.004");
    }

    fn right(span: &CellSpan) -> f32 {
        span.x + span.width
    }

    #[test]
    fn pit_stops_take_one_fixed_cell() {
        let items = [lap(0, 100.0), pit(1), lap(2, 90.0), lap(3, 88.0)];
        let spans = lap_strip_layout(800.0, &items);
        assert_eq!(spans[1].width, PIT_STOP_CELL);
        // Full gaps at this width.
        assert!((spans[1].x - right(&spans[0]) - MAX_GAP).abs() < 1e-3);
    }

    #[test]
    fn driven_cells_are_proportional_above_a_12px_floor() {
        let items = [lap(0, 0.0), lap(1, 60.0), lap(2, 120.0)];
        let spans = lap_strip_layout(600.0, &items);
        // A fully stopped interval still gets the selectable floor.
        assert!((spans[0].width - MIN_CELL).abs() < 1e-3);
        // Above the floor, widths follow driven time 1:2.
        let (a, b) = (spans[1].width - MIN_CELL, spans[2].width - MIN_CELL);
        assert!((b / a - 2.0).abs() < 1e-4, "{a} {b}");
    }

    #[test]
    fn cells_fill_the_width_without_overlap() {
        let items = [
            lap(0, 40.0),
            lap(1, 92.4),
            pit(2),
            lap(3, 20.0),
            lap(4, 91.9),
        ];
        for width in [80.0, 240.0, 1234.5] {
            let spans = lap_strip_layout(width, &items);
            assert!(
                (right(spans.last().unwrap()) - width).abs() < 1e-2,
                "{width}"
            );
            for pair in spans.windows(2) {
                assert!(pair[1].x >= right(&pair[0]) - 1e-3, "{width}: {pair:?}");
            }
            assert!(spans.iter().all(|s| s.width >= 0.0));
        }
    }

    #[test]
    fn dense_strips_shrink_floors_then_gaps_then_pit_cells() {
        // 20 laps + 2 pit stops.
        let mut items: Vec<LapStripItem> = (0..20).map(|i| lap(i, 90.0)).collect();
        items.insert(5, pit(100));
        items.push(pit(101));
        // 300 px: full gaps and pit cells, the driven floor gives way.
        let cells = strip_cells(300.0, items.len(), 2);
        assert_eq!((cells.fixed, cells.spacing), (PIT_STOP_CELL, MAX_GAP));
        assert!((cells.minimum - 8.25).abs() < 1e-4, "{cells:?}");
        let spans = lap_strip_layout(300.0, &items);
        assert!((right(spans.last().unwrap()) - 300.0).abs() < 1e-2);
        // 100 px: pit cells still whole, gaps tighten.
        let cells = strip_cells(100.0, 22, 2);
        assert_eq!(cells.fixed, PIT_STOP_CELL);
        assert!(cells.spacing > 0.0 && cells.spacing < MAX_GAP, "{cells:?}");
        // 30 px: no gaps, pit cells shrink last, one pixel per driven lap.
        let cells = strip_cells(30.0, 22, 2);
        assert_eq!(cells.spacing, 0.0);
        assert!((cells.fixed - 5.0).abs() < 1e-4, "{cells:?}");
        assert!((cells.minimum - 1.0).abs() < 1e-4, "{cells:?}");
    }

    #[test]
    fn unknown_driving_time_shares_equally() {
        let items = [lap(0, f64::NAN), lap(1, 0.0)];
        let spans = lap_strip_layout(203.0, &items);
        assert!((spans[0].width - spans[1].width).abs() < 1e-3);
        assert!(strip_cells(0.0, 3, 0) == StripCells::default());
        assert!(lap_strip_layout(100.0, &[]).is_empty());
    }

    #[test]
    fn items_come_from_core_cells() {
        let item = LapStripItem::new(3, "L3", 70.0).time("1:13.644").best(true);
        assert_eq!(item.text().as_ref(), "1:13.644");
        assert_eq!(LapStripItem::new(4, "Out", 30.0).text().as_ref(), "Out");
    }
}
