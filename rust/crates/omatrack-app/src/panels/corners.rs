//! Corners: where the lap gains and loses time, corner by corner.
//!
//! A [`DataTable`] over the analysis' corner rows, sorted by Δt descending
//! by default ("where am I losing time"), with the selected corner's notes
//! below it. The notes are the `CornerCheck` registry's output
//! (`omatrack_core::corners::checks::run`, already in each row); this view
//! only presents them and never decides what a corner says.
//!
//! Ownership:
//! - [`Session`](crate::state::Session) owns the analysis; the panel keeps a
//!   presentation snapshot ([`CornerTable`]) rebuilt only when the analysis
//!   changes (by identity).
//! - Selection is kept by corner id, so sorting or a new analysis of the
//!   same track keeps the selected corner.
//! - Brake-point consistency over the session's fastest quarter is measured
//!   on the background executor (one task slot, latest wins).
//! - The panel never re-renders on cursor motion: it observes
//!   [`CursorState`](omatrack_trace::CursorState) only to follow a corner
//!   focus, and notifies only when that focus changes.
//!
//! Keyboard: the table owns arrow/Home/End/Page navigation; Enter (or a
//! double-click) dispatches [`FocusCorner`] for the selected row.

use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName, Sizable as _, StyledExt as _, h_flex,
    menu::PopupMenu,
    table::{Column, ColumnSort, DataTable, TableDelegate, TableEvent, TableState},
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, AppContext as _, Context, Div, ElementId, Entity, FocusHandle, InteractiveElement as _,
    IntoElement, KeyBinding, ParentElement as _, Pixels, Render, SharedString, Stateful,
    StatefulInteractiveElement as _, Styled as _, Subscription, Task, TestSupportExt as _, Window,
    div, rems,
};
use omatrack_core::consistency::{CornerConsistency, SessionLaps};
use omatrack_core::corners::checks::NoteSeverity;
use omatrack_core::session::{Analysis, CornerRow, CornerSource};
use omatrack_trace::Selection;
use omatrack_ui::{DeltaSense, DeltaText, MISSING_VALUE, Readout, format_delta};

use crate::actions::FocusCorner;
use crate::panels::{PanelKind, analysis_body, empty_state, panel_body};
use crate::state::AppState;

/// The panel's key context: Enter focuses the selected corner.
pub const CORNERS_CONTEXT: &str = "Corners";

gpui_kit::actions!(
    omatrack,
    [
        /// Focus the corner selected in the corners table (Enter).
        FocusSelectedCorner
    ]
);

/// One corner as the table shows it (plain data, no entity handles).
#[derive(Debug, Clone, PartialEq)]
pub struct CornerLine {
    /// Position in lap order, 0-based (the `#` column shows it 1-based).
    order: usize,
    id: SharedString,
    name: SharedString,
    zone: (f64, f64),
    dt: f64,
    entry_dt: f64,
    exit_dt: f64,
    /// Entry, minimum and exit speed: primary and reference (NaN without).
    speeds: [(f64, f64); 3],
    brake: f64,
    turn_in: f64,
    throttle: f64,
    /// Brake-point standard deviation over the session's fastest quarter,
    /// metres; `None` until measured or without two braking laps.
    consistency: Option<f64>,
    notes: Vec<(NoteSeverity, SharedString)>,
}

impl CornerLine {
    fn new(order: usize, row: &CornerRow) -> Self {
        let reference = row.reference_speeds;
        let pair = |primary: f64, reference: Option<f64>| (primary, reference.unwrap_or(f64::NAN));
        Self {
            order,
            id: row.zone.id.clone().into(),
            name: row.zone.name.clone().into(),
            zone: (row.zone.start, row.zone.end),
            dt: row.dt,
            entry_dt: row.entry_dt,
            exit_dt: row.exit_dt,
            speeds: [
                pair(row.speeds.entry, reference.map(|s| s.entry)),
                pair(row.speeds.apex, reference.map(|s| s.apex)),
                pair(row.speeds.exit, reference.map(|s| s.exit)),
            ],
            brake: row.brake_point_delta,
            turn_in: row.turn_in_delta,
            throttle: row.throttle_point_delta,
            consistency: None,
            notes: row
                .notes
                .iter()
                .map(|note| (note.severity, note.text.clone().into()))
                .collect(),
        }
    }

    /// The zone id (`T5`, an atlas range id, or `tN`).
    pub fn id(&self) -> &SharedString {
        &self.id
    }

    pub fn name(&self) -> &SharedString {
        &self.name
    }

    /// Time lost (+) or gained through the corner, seconds; NaN without a
    /// reference.
    pub fn dt(&self) -> f64 {
        self.dt
    }

    /// Brake-point spread (standard deviation, metres) once measured.
    pub fn consistency(&self) -> Option<f64> {
        self.consistency
    }

    fn worst_severity(&self) -> Option<NoteSeverity> {
        self.notes
            .iter()
            .map(|(severity, _)| *severity)
            .max_by_key(|severity| severity_rank(*severity))
    }
}

/// The table's columns, in display order.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Col {
    Order,
    Corner,
    Dt,
    Entry,
    Min,
    Exit,
    Brake,
    TurnIn,
    Throttle,
    Consistency,
    Notes,
}

impl Col {
    const ALL: [Self; 11] = [
        Self::Order,
        Self::Corner,
        Self::Dt,
        Self::Entry,
        Self::Min,
        Self::Exit,
        Self::Brake,
        Self::TurnIn,
        Self::Throttle,
        Self::Consistency,
        Self::Notes,
    ];

    fn key(self) -> &'static str {
        match self {
            Self::Order => "order",
            Self::Corner => "corner",
            Self::Dt => "dt",
            Self::Entry => "entry",
            Self::Min => "min",
            Self::Exit => "exit",
            Self::Brake => "brake",
            Self::TurnIn => "turn-in",
            Self::Throttle => "throttle",
            Self::Consistency => "consistency",
            Self::Notes => "notes",
        }
    }

    fn title(self) -> &'static str {
        match self {
            Self::Order => "#",
            Self::Corner => "Corner",
            Self::Dt => "Δt s",
            Self::Entry => "Entry",
            Self::Min => "Min",
            Self::Exit => "Exit",
            Self::Brake => "Brake Δ m",
            Self::TurnIn => "Turn-in Δ m",
            Self::Throttle => "Throttle Δ m",
            Self::Consistency => "Consistency",
            Self::Notes => "Notes",
        }
    }

    /// Width in rems (the table API takes pixels; see [`CornerTable::rem`]).
    fn width(self) -> f32 {
        match self {
            Self::Order => 2.25,
            Self::Corner => 5.5,
            Self::Dt => 4.75,
            Self::Entry | Self::Min | Self::Exit => 5.75,
            Self::Brake | Self::TurnIn | Self::Throttle => 5.75,
            Self::Consistency => 6.0,
            Self::Notes => 3.75,
        }
    }

    fn is_numeric(self) -> bool {
        !matches!(self, Self::Corner)
    }

    /// The sort key of a line in this column; NaN sorts last either way.
    fn sort_value(self, line: &CornerLine) -> f64 {
        match self {
            Self::Order | Self::Corner => line.order as f64,
            Self::Dt => line.dt,
            Self::Entry => line.speeds[0].0,
            Self::Min => line.speeds[1].0,
            Self::Exit => line.speeds[2].0,
            Self::Brake => line.brake,
            Self::TurnIn => line.turn_in,
            Self::Throttle => line.throttle,
            Self::Consistency => line.consistency.unwrap_or(f64::NAN),
            Self::Notes => line.notes.len() as f64,
        }
    }
}

/// The [`TableDelegate`] of the corners table: a sorted presentation
/// snapshot of the analysis' corner rows.
pub struct CornerTable {
    lines: Vec<CornerLine>,
    /// The selected corner, by id: the table's selected row index follows
    /// it through sorting and rebuilds.
    selected: Option<SharedString>,
    has_reference: bool,
    /// The sorted column and direction (`Default` is lap order).
    sort: (Col, ColumnSort),
    /// The window's rem size when the table was built: the table API sizes
    /// columns in pixels, so rem widths are resolved once here.
    rem: Pixels,
}

impl CornerTable {
    fn new(rem: Pixels) -> Self {
        Self {
            lines: Vec::new(),
            selected: None,
            has_reference: false,
            sort: (Col::Order, ColumnSort::Default),
            rem,
        }
    }

    /// The rows in display order.
    pub fn lines(&self) -> &[CornerLine] {
        &self.lines
    }

    /// The display position of a corner.
    pub fn position(&self, id: &str) -> Option<usize> {
        self.lines.iter().position(|line| line.id == id)
    }

    /// The selected corner id.
    pub fn selected(&self) -> Option<&SharedString> {
        self.selected.as_ref()
    }

    /// The selected corner.
    pub fn selected_line(&self) -> Option<&CornerLine> {
        let id = self.selected.as_ref()?;
        self.lines.iter().find(|line| &line.id == id)
    }

    /// Where the selection should be: the selected corner, else the first
    /// row (a fresh analysis opens on its worst corner).
    fn selection_target(&mut self) -> Option<usize> {
        let ix = self
            .selected
            .as_ref()
            .and_then(|id| self.position(id))
            .or_else(|| (!self.lines.is_empty()).then_some(0));
        self.selected = ix.map(|ix| self.lines[ix].id.clone());
        ix
    }

    /// Replace the rows. The first analysis with a reference sorts by Δt
    /// descending; after that the user's sort is kept.
    fn set_lines(&mut self, lines: Vec<CornerLine>, has_reference: bool) {
        if has_reference && !self.has_reference && self.sort == (Col::Order, ColumnSort::Default) {
            self.sort = (Col::Dt, ColumnSort::Descending);
        }
        if !has_reference && self.sort.0 == Col::Dt {
            self.sort = (Col::Order, ColumnSort::Default);
        }
        self.has_reference = has_reference;
        self.lines = lines;
        self.apply_sort();
    }

    fn set_consistency(&mut self, values: &HashMap<SharedString, CornerConsistency>) {
        for line in &mut self.lines {
            line.consistency = values
                .get(&line.id)
                .filter(|c| c.braking_lap_count >= 2 && c.brake_point_std_dev.is_finite())
                .map(|c| c.brake_point_std_dev);
        }
        if self.sort.0 == Col::Consistency {
            self.apply_sort();
        }
    }

    fn apply_sort(&mut self) {
        let (col, sort) = self.sort;
        let descending = sort == ColumnSort::Descending;
        let col = if sort == ColumnSort::Default {
            Col::Order
        } else {
            col
        };
        self.lines.sort_by(|a, b| {
            let (x, y) = (col.sort_value(a), col.sort_value(b));
            match (x.is_nan(), y.is_nan()) {
                (true, true) => a.order.cmp(&b.order),
                (true, false) => std::cmp::Ordering::Greater,
                (false, true) => std::cmp::Ordering::Less,
                (false, false) => {
                    let ordering = x.total_cmp(&y);
                    let ordering = if descending {
                        ordering.reverse()
                    } else {
                        ordering
                    };
                    ordering.then(a.order.cmp(&b.order))
                }
            }
        });
    }

    fn render_number(&self, text: SharedString, cx: &App) -> Div {
        h_flex()
            .w_full()
            .justify_end()
            .font_family(cx.theme().mono_font_family.clone())
            .child(text)
    }

    fn render_speed(&self, (primary, reference): (f64, f64), cx: &App) -> Div {
        h_flex()
            .w_full()
            .justify_end()
            .gap_1()
            .child(Readout::number(Some(primary), 0))
            .when(reference.is_finite(), |this| {
                this.child(
                    div().text_xs().child(
                        DeltaText::new(Some(primary - reference))
                            .decimals(1)
                            .sense(DeltaSense::HigherIsBetter),
                    ),
                )
            })
            .text_color(cx.theme().foreground)
    }

    /// A signed position delta (metres, + = the primary's event is later).
    /// Later is not better or worse by itself, so it stays uncoloured.
    fn render_metres(&self, value: f64, cx: &App) -> Div {
        let (text, _) = format_delta(Some(value), 0, DeltaSense::LowerIsBetter);
        self.render_number(text, cx)
    }
}

impl TableDelegate for CornerTable {
    fn columns_count(&self, _: &App) -> usize {
        Col::ALL.len()
    }

    fn rows_count(&self, _: &App) -> usize {
        self.lines.len()
    }

    fn column(&self, col_ix: usize, _: &App) -> Column {
        let col = Col::ALL[col_ix];
        let column = Column::new(col.key(), col.title())
            .width(self.rem * col.width())
            .movable(false);
        let column = if col.is_numeric() {
            column.text_right()
        } else {
            column
        };
        let column = match col {
            Col::Order | Col::Corner => column.fixed_left(),
            _ => column,
        };
        if col == Col::Corner {
            return column;
        }
        match self.sort {
            (sorted, ColumnSort::Ascending) if sorted == col => column.ascending(),
            (sorted, ColumnSort::Descending) if sorted == col => column.descending(),
            _ => column.sortable(),
        }
    }

    fn perform_sort(
        &mut self,
        col_ix: usize,
        sort: ColumnSort,
        window: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) {
        self.sort = (Col::ALL[col_ix], sort);
        self.apply_sort();
        // Keep the selected corner selected, by id, across the reorder. The
        // table is mid-update here, so the row index moves right after.
        if let Some(ix) = self.selection_target() {
            cx.defer_in(window, move |table, _, cx| {
                if table.selected_row() != Some(ix) {
                    table.set_selected_row(ix, cx);
                }
            });
        }
    }

    fn render_th(
        &mut self,
        col_ix: usize,
        _: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) -> impl IntoElement {
        let col = Col::ALL[col_ix];
        div()
            .size_full()
            .flex()
            .items_center()
            .when(col.is_numeric(), |this| this.justify_end())
            .text_color(cx.theme().muted_foreground)
            .child(col.title())
    }

    fn render_tr(
        &mut self,
        row_ix: usize,
        _: &mut Window,
        _: &mut Context<TableState<Self>>,
    ) -> Stateful<Div> {
        let id = self
            .lines
            .get(row_ix)
            .map(|line| line.id.clone())
            .unwrap_or_default();
        div().id(ElementId::Name(format!("corner:{id}").into()))
    }

    fn render_td(
        &mut self,
        row_ix: usize,
        col_ix: usize,
        _: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) -> impl IntoElement {
        let Some(line) = self.lines.get(row_ix) else {
            return div().into_any_element();
        };
        let theme = cx.theme();
        match Col::ALL[col_ix] {
            Col::Order => self
                .render_number((line.order + 1).to_string().into(), cx)
                .text_color(theme.muted_foreground)
                .into_any_element(),
            Col::Corner => div()
                .w_full()
                .truncate()
                .font_semibold()
                .child(line.name.clone())
                .into_any_element(),
            Col::Dt => h_flex()
                .w_full()
                .justify_end()
                .child(DeltaText::new(Some(line.dt)).decimals(3))
                .into_any_element(),
            Col::Entry => self.render_speed(line.speeds[0], cx).into_any_element(),
            Col::Min => self.render_speed(line.speeds[1], cx).into_any_element(),
            Col::Exit => self.render_speed(line.speeds[2], cx).into_any_element(),
            Col::Brake => self.render_metres(line.brake, cx).into_any_element(),
            Col::TurnIn => self.render_metres(line.turn_in, cx).into_any_element(),
            Col::Throttle => self.render_metres(line.throttle, cx).into_any_element(),
            Col::Consistency => {
                let text: SharedString = match line.consistency {
                    Some(spread) => format!("±{spread:.1}").into(),
                    None => MISSING_VALUE.into(),
                };
                self.render_number(text, cx).into_any_element()
            }
            Col::Notes => {
                let count = line.notes.len();
                h_flex()
                    .w_full()
                    .justify_end()
                    .gap_1()
                    .when_some(line.worst_severity(), |this, severity| {
                        this.child(severity_icon(severity, cx).xsmall())
                    })
                    .child(
                        div()
                            .font_family(theme.mono_font_family.clone())
                            .child(count.to_string()),
                    )
                    .into_any_element()
            }
        }
    }

    fn context_menu(
        &mut self,
        row_ix: usize,
        menu: PopupMenu,
        _: &mut Window,
        _: &mut Context<TableState<Self>>,
    ) -> PopupMenu {
        let Some(line) = self.lines.get(row_ix) else {
            return menu;
        };
        menu.menu(
            format!("Focus {}", line.name),
            Box::new(FocusCorner {
                id: line.id.clone(),
            }),
        )
    }

    fn render_empty(
        &mut self,
        _: &mut Window,
        _: &mut Context<TableState<Self>>,
    ) -> impl IntoElement {
        empty_state(
            IconName::Inbox,
            "No corners",
            "This lap has no corner zones.",
        )
    }

    fn cell_text(&self, row_ix: usize, col_ix: usize, _: &App) -> String {
        let Some(line) = self.lines.get(row_ix) else {
            return String::new();
        };
        let delta = |value: f64, decimals: usize| {
            format_delta(Some(value), decimals, DeltaSense::LowerIsBetter)
                .0
                .to_string()
        };
        let speed = |(primary, reference): (f64, f64)| {
            if reference.is_finite() {
                format!("{primary:.0} / {reference:.0}")
            } else {
                format!("{primary:.0}")
            }
        };
        match Col::ALL[col_ix] {
            Col::Order => (line.order + 1).to_string(),
            Col::Corner => line.name.to_string(),
            Col::Dt => delta(line.dt, 3),
            Col::Entry => speed(line.speeds[0]),
            Col::Min => speed(line.speeds[1]),
            Col::Exit => speed(line.speeds[2]),
            Col::Brake => delta(line.brake, 0),
            Col::TurnIn => delta(line.turn_in, 0),
            Col::Throttle => delta(line.throttle, 0),
            Col::Consistency => line
                .consistency
                .map(|spread| format!("±{spread:.1}"))
                .unwrap_or_else(|| MISSING_VALUE.to_string()),
            Col::Notes => line.notes.len().to_string(),
        }
    }
}

fn severity_icon(severity: NoteSeverity, cx: &App) -> Icon {
    let theme = cx.theme();
    match severity {
        NoteSeverity::Info => Icon::new(IconName::Info).text_color(theme.muted_foreground),
        NoteSeverity::Warning => Icon::new(IconName::TriangleAlert).text_color(theme.warning),
        NoteSeverity::Error => Icon::new(IconName::CircleX).text_color(theme.danger),
    }
}

/// Loudness order: error, warning, info.
fn severity_rank(severity: NoteSeverity) -> u8 {
    match severity {
        NoteSeverity::Info => 0,
        NoteSeverity::Warning => 1,
        NoteSeverity::Error => 2,
    }
}

fn severity_label(severity: NoteSeverity) -> &'static str {
    match severity {
        NoteSeverity::Info => "Note",
        NoteSeverity::Warning => "Warning",
        NoteSeverity::Error => "Problem",
    }
}

/// Brake-point consistency per corner id, measured for one analysis.
struct ConsistencyResult {
    analysis: Arc<Analysis>,
    values: HashMap<SharedString, CornerConsistency>,
}

pub struct CornersPanel {
    app: AppState,
    focus_handle: FocusHandle,
    /// Built on the first render (the table state needs the window).
    table: Option<Entity<TableState<CornerTable>>>,
    /// The analysis the table shows, by identity.
    shown: Option<Arc<Analysis>>,
    /// The corner focus last followed (see [`Self::follow_focus`]).
    followed_focus: Option<Selection>,
    /// The session laps of the last primary, reused across analyses of it.
    session_laps: Option<(usize, Arc<SessionLaps>)>,
    consistency: Option<ConsistencyResult>,
    consistency_task: Option<(Arc<AtomicBool>, Task<()>)>,
    renders: usize,
    _subscriptions: Vec<Subscription>,
}

impl CornersPanel {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![
            cx.observe(&app.session, |this, _, cx| this.sync_analysis(cx)),
            // Cursor motion must not re-render this panel: only a change of
            // the focused corner does anything here.
            cx.observe(&app.cursor, |this, _, cx| this.follow_focus(cx)),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle(),
            table: None,
            shown: None,
            followed_focus: None,
            session_laps: None,
            consistency: None,
            consistency_task: None,
            renders: 0,
            _subscriptions: subscriptions,
        };
        panel.sync_analysis(cx);
        panel
    }

    /// The table state, once the panel has rendered.
    pub fn table(&self) -> Option<&Entity<TableState<CornerTable>>> {
        self.table.as_ref()
    }

    /// The selected corner id.
    pub fn selected<'a>(&self, cx: &'a App) -> Option<&'a SharedString> {
        self.table.as_ref()?.read(cx).delegate().selected()
    }

    /// Times this panel rendered (a cursor move must not change it).
    pub fn render_count(&self) -> usize {
        self.renders
    }

    /// Whether brake-point consistency is still being measured.
    pub fn is_measuring(&self) -> bool {
        self.consistency_task.is_some()
    }

    /// Rebuild the table when the session's analysis changed identity.
    fn sync_analysis(&mut self, cx: &mut Context<Self>) {
        let analysis = self.app.session.read(cx).analysis().cloned();
        let same = match (&analysis, &self.shown) {
            (Some(a), Some(b)) => Arc::ptr_eq(a, b),
            (None, None) => true,
            _ => false,
        };
        if same {
            // Loading and failure states of the roles still re-render.
            cx.notify();
            return;
        }
        self.shown = analysis.clone();
        self.consistency = None;
        if let Some((cancel, _)) = self.consistency_task.take() {
            cancel.store(true, Ordering::Relaxed);
        }
        if let Some(analysis) = &analysis {
            self.measure_consistency(analysis.clone(), cx);
        }
        self.refresh_table(cx);
        cx.notify();
    }

    /// Push the shown analysis into the table, keeping the selection.
    fn refresh_table(&mut self, cx: &mut Context<Self>) {
        let Some(table) = self.table.clone() else {
            return;
        };
        let (lines, has_reference) = match &self.shown {
            Some(analysis) => (
                analysis
                    .rows()
                    .iter()
                    .enumerate()
                    .map(|(order, row)| CornerLine::new(order, row))
                    .collect::<Vec<_>>(),
                analysis.reference().is_some(),
            ),
            None => (Vec::new(), false),
        };
        let consistency = self
            .consistency
            .as_ref()
            .filter(|result| {
                self.shown
                    .as_ref()
                    .is_some_and(|shown| Arc::ptr_eq(shown, &result.analysis))
            })
            .map(|result| &result.values);
        table.update(cx, |table, cx| {
            let delegate = table.delegate_mut();
            delegate.set_lines(lines, has_reference);
            if let Some(values) = consistency {
                delegate.set_consistency(values);
            }
            table.refresh(cx);
            cx.notify();
        });
        self.reselect(cx);
    }

    /// Put the table's selected row back on the selected corner (after a
    /// rebuild), else on the first row.
    fn reselect(&mut self, cx: &mut Context<Self>) {
        let Some(table) = self.table.clone() else {
            return;
        };
        table.update(cx, |table, cx| {
            match table.delegate_mut().selection_target() {
                Some(ix) if table.selected_row() != Some(ix) => table.set_selected_row(ix, cx),
                Some(_) => {}
                None if table.selected_row().is_some() => table.clear_selection(cx),
                None => {}
            }
        });
    }

    /// Follow a corner focus from anywhere (J/H, the palette, the ruler):
    /// select its row. Only a change of focus does any work.
    fn follow_focus(&mut self, cx: &mut Context<Self>) {
        let focus = self.app.cursor.read(cx).focus();
        if focus == self.followed_focus {
            return;
        }
        self.followed_focus = focus;
        let (Some(focus), Some(table)) = (focus, self.table.clone()) else {
            return;
        };
        table.update(cx, |table, cx| {
            let delegate = table.delegate_mut();
            let Some(id) = delegate
                .lines()
                .iter()
                .find(|line| {
                    (line.zone.0 - focus.start).abs() < 1e-9
                        && (line.zone.1 - focus.end).abs() < 1e-9
                })
                .map(|line| line.id.clone())
            else {
                return;
            };
            if delegate.selected.as_ref() == Some(&id) {
                return;
            }
            delegate.selected = Some(id);
            if let Some(ix) = delegate.selection_target() {
                table.set_selected_row(ix, cx);
            }
        });
        cx.notify();
    }

    fn measure_consistency(&mut self, analysis: Arc<Analysis>, cx: &mut Context<Self>) {
        if analysis.rows().is_empty() {
            return;
        }
        let key = Arc::as_ptr(analysis.primary().unified()) as usize;
        let cached = self
            .session_laps
            .as_ref()
            .filter(|(cached, _)| *cached == key)
            .map(|(_, laps)| laps.clone());
        let cancel = Arc::new(AtomicBool::new(false));
        let background_cancel = cancel.clone();
        let measured = analysis.clone();
        let work = cx.background_spawn(async move {
            let cancel = background_cancel;
            let laps = match cached {
                Some(laps) => laps,
                None => Arc::new(SessionLaps::for_primary(measured.primary(), &cancel).ok()?),
            };
            let mut values = HashMap::new();
            for row in measured.rows() {
                if cancel.load(Ordering::Relaxed) {
                    return None;
                }
                values.insert(
                    SharedString::from(row.zone.id.clone()),
                    laps.corner_consistency(measured.primary(), row.zone.start, row.zone.end),
                );
            }
            Some((laps, values))
        });
        let task = cx.spawn(async move |this, cx| {
            let result = work.await;
            let _ = this.update(cx, |this, cx| {
                let current = this
                    .shown
                    .as_ref()
                    .is_some_and(|shown| Arc::ptr_eq(shown, &analysis));
                if !current {
                    return;
                }
                this.consistency_task = None;
                if let Some((laps, values)) = result {
                    this.session_laps = Some((key, laps));
                    if let Some(table) = this.table.clone() {
                        table.update(cx, |table, cx| {
                            table.delegate_mut().set_consistency(&values);
                            cx.notify();
                        });
                    }
                    this.consistency = Some(ConsistencyResult { analysis, values });
                    this.reselect(cx);
                }
                cx.notify();
            });
        });
        self.consistency_task = Some((cancel, task));
    }

    /// Create the table state (first render).
    /// Move keyboard focus from the panel onto its table.
    fn focus_table(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(table) = &self.table {
            let handle = gpui_kit::Focusable::focus_handle(table.read(cx), cx);
            window.focus(&handle, cx);
        }
    }

    fn ensure_table(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.table.is_some() {
            return;
        }
        let rem = window.rem_size();
        let table = cx.new(|cx| {
            TableState::new(CornerTable::new(rem), window, cx)
                .col_movable(false)
                .col_selectable(false)
                .loop_selection(false)
        });
        let subscription = cx.subscribe_in(&table, window, Self::on_table_event);
        self._subscriptions.push(subscription);
        self.table = Some(table);
        // Ctrl+N may have focused the panel before its table existed;
        // keyboard focus belongs on the rows.
        let own = self.focus_handle.clone();
        self._subscriptions
            .push(cx.on_focus(&own, window, |this, window, cx| {
                this.focus_table(window, cx)
            }));
        if own.is_focused(window) {
            cx.defer_in(window, |this, window, cx| this.focus_table(window, cx));
        }
        self.refresh_table(cx);
    }

    fn on_table_event(
        &mut self,
        table: &Entity<TableState<CornerTable>>,
        event: &TableEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match event {
            TableEvent::SelectRow(ix) => {
                let ix = *ix;
                let changed = table.update(cx, |table, _| {
                    let delegate = table.delegate_mut();
                    let id = delegate.lines().get(ix).map(|line| line.id.clone());
                    let changed = id.is_some() && id != delegate.selected;
                    if changed {
                        delegate.selected = id;
                    }
                    changed
                });
                if changed {
                    cx.notify();
                }
            }
            TableEvent::DoubleClickedRow(ix) => {
                let id = table
                    .read(cx)
                    .delegate()
                    .lines()
                    .get(*ix)
                    .map(|line| line.id.clone());
                if let Some(id) = id {
                    window.dispatch_action(Box::new(FocusCorner { id }), cx);
                }
            }
            _ => {}
        }
    }

    /// Enter: focus the corner on the table's current row.
    fn focus_selected(&mut self, _: &FocusSelectedCorner, window: &mut Window, cx: &mut App) {
        let Some(table) = &self.table else {
            return;
        };
        let table = table.read(cx);
        let id = table
            .selected_row()
            .and_then(|ix| table.delegate().lines().get(ix))
            .map(|line| line.id.clone());
        if let Some(id) = id {
            window.dispatch_action(Box::new(FocusCorner { id }), cx);
        }
    }

    fn selected_line(&self, cx: &App) -> Option<CornerLine> {
        self.table
            .as_ref()?
            .read(cx)
            .delegate()
            .selected_line()
            .cloned()
    }

    fn render_notes(&self, cx: &App) -> impl IntoElement {
        let theme = cx.theme();
        let has_reference = self
            .shown
            .as_ref()
            .is_some_and(|analysis| analysis.reference().is_some());
        let body = match self.selected_line(cx) {
            None => div()
                .text_color(theme.muted_foreground)
                .child("Select a corner to read its notes.")
                .into_any_element(),
            Some(line) => {
                let label = format!(
                    "{} notes: {}",
                    line.name,
                    if line.notes.is_empty() {
                        "none".to_string()
                    } else {
                        line.notes
                            .iter()
                            .map(|(severity, text)| {
                                format!("{}: {text}", severity_label(*severity))
                            })
                            .collect::<Vec<_>>()
                            .join("; ")
                    }
                );
                v_flex()
                    .id("corner-notes")
                    .test_support()
                    .aria_label(SharedString::from(label))
                    .gap_1()
                    .child(
                        h_flex()
                            .gap_2()
                            .child(div().font_semibold().child(line.name.clone()))
                            .when(has_reference, |this| {
                                this.child(DeltaText::new(Some(line.dt)).unit("s")).child(
                                    h_flex()
                                        .gap_1()
                                        .text_xs()
                                        .text_color(theme.muted_foreground)
                                        .child("entry")
                                        .child(DeltaText::new(Some(line.entry_dt)))
                                        .child("exit")
                                        .child(DeltaText::new(Some(line.exit_dt))),
                                )
                            }),
                    )
                    .children(line.notes.iter().map(|(severity, text)| {
                        h_flex()
                            .items_start()
                            .gap_2()
                            .child(severity_icon(*severity, cx).small().mt_0p5())
                            .child(div().flex_1().min_w_0().child(text.clone()))
                    }))
                    .when(line.notes.is_empty(), |this| {
                        this.child(
                            div()
                                .text_color(theme.muted_foreground)
                                .child("Nothing to note in this corner."),
                        )
                    })
                    .into_any_element()
            }
        };
        div()
            .id("corner-notes-region")
            .flex_shrink_0()
            .max_h(rems(12.))
            .overflow_y_scroll()
            .border_t_1()
            .border_color(theme.border)
            .p_2()
            .text_sm()
            .child(body)
    }

    fn render_source(&self, analysis: &Analysis, cx: &App) -> Option<impl IntoElement> {
        let text = match analysis.corner_source() {
            CornerSource::Atlas => return None,
            CornerSource::User => "Your corner zones for this track.",
            CornerSource::Generated => "Corners detected from braking; no Track Atlas layout.",
            CornerSource::Reference => "Track Atlas corners carried over from the reference lap.",
            CornerSource::Unmatched => return None,
        };
        Some(
            div()
                .px_2()
                .py_1()
                .text_xs()
                .text_color(cx.theme().muted_foreground)
                .child(text),
        )
    }
}

impl gpui_kit::component::dock::BasePanel for CornersPanel {
    fn panel_name(&self) -> &'static str {
        PanelKind::Corners.name()
    }

    fn closable(&self, _: &App) -> bool {
        false
    }
}

impl gpui_kit::component::dock::Panel for CornersPanel {
    fn tab_name(&self, _: &App) -> Option<SharedString> {
        Some(PanelKind::Corners.title().into())
    }

    fn title(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        PanelKind::Corners.title()
    }
}

impl gpui_kit::EventEmitter<gpui_kit::component::dock::PanelEvent> for CornersPanel {}

impl gpui_kit::Focusable for CornersPanel {
    /// The table takes keyboard focus once it exists, so Ctrl+4 lands on
    /// the rows (arrows, Enter).
    fn focus_handle(&self, cx: &App) -> FocusHandle {
        match &self.table {
            Some(table) => gpui_kit::Focusable::focus_handle(table.read(cx), cx),
            None => self.focus_handle.clone(),
        }
    }
}

impl Render for CornersPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.renders += 1;
        self.ensure_table(window, cx);
        let root = div()
            .id("corners-panel")
            .test_support()
            .key_context(CORNERS_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(
                cx.listener(|this, action, window, cx| this.focus_selected(action, window, cx)),
            )
            .size_full();
        let Some(analysis) = self.shown.clone() else {
            return root
                .child(analysis_body("corners-summary", &self.app, cx, |_| {
                    SharedString::default()
                }))
                .into_any_element();
        };
        if analysis.rows().is_empty() {
            let description = match analysis.corner_source() {
                CornerSource::Unmatched => {
                    "The lap’s GPS doesn’t match the Track Atlas layout, so corners are hidden."
                }
                _ => "No corner zones were found for this lap.",
            };
            return root
                .child(panel_body(
                    "corners-summary",
                    format!("No corners. {description}"),
                    empty_state(IconName::Map, "No corners", description),
                    cx,
                ))
                .into_any_element();
        }
        let Some(table) = self.table.clone() else {
            return root.into_any_element();
        };
        let count = analysis.rows().len();
        let label: SharedString = format!(
            "{count} corner{}{}",
            if count == 1 { "" } else { "s" },
            if analysis.reference().is_some() {
                ", sorted by time lost"
            } else {
                ""
            }
        )
        .into();
        root.child(
            v_flex()
                .id("corners-table")
                .test_support()
                .aria_label(label)
                .size_full()
                .children(self.render_source(&analysis, cx))
                .child(
                    div()
                        .flex_1()
                        .min_h_0()
                        .child(DataTable::new(&table).small().bordered(false)),
                )
                .child(self.render_notes(cx)),
        )
        .into_any_element()
    }
}

/// `14 corners · most time lost at T5 (+0.231 s)`.
pub fn corners_summary(analysis: &Analysis) -> String {
    let rows = analysis.rows();
    if rows.is_empty() {
        return "No corners for this lap".to_string();
    }
    let count = format!(
        "{} corner{}",
        rows.len(),
        if rows.len() == 1 { "" } else { "s" }
    );
    let worst = rows
        .iter()
        .filter(|row| row.dt.is_finite())
        .max_by(|a, b| a.dt.total_cmp(&b.dt));
    match (analysis.reference(), worst) {
        (Some(_), Some(row)) if row.dt > 0.0 => format!(
            "{count} · most time lost at {} ({})",
            row.zone.name,
            format_delta(Some(row.dt), 3, DeltaSense::LowerIsBetter).0
        ),
        _ => count,
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and its key binding. Called once from
/// [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::Corners, cx);
    cx.bind_keys([KeyBinding::new(
        "enter",
        FocusSelectedCorner,
        Some(CORNERS_CONTEXT),
    )]);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(order: usize, dt: f64) -> CornerLine {
        CornerLine {
            order,
            id: format!("t{}", order + 1).into(),
            name: format!("T{}", order + 1).into(),
            zone: (order as f64 * 0.1, order as f64 * 0.1 + 0.05),
            dt,
            entry_dt: f64::NAN,
            exit_dt: f64::NAN,
            speeds: [(100.0, f64::NAN); 3],
            brake: f64::NAN,
            turn_in: f64::NAN,
            throttle: f64::NAN,
            consistency: None,
            notes: Vec::new(),
        }
    }

    fn ids(table: &CornerTable) -> Vec<&str> {
        table.lines().iter().map(|line| line.id.as_ref()).collect()
    }

    #[test]
    fn a_comparison_sorts_by_time_lost_with_unknowns_last() {
        let mut table = CornerTable::new(gpui_kit::px(16.));
        table.set_lines(
            vec![line(0, 0.1), line(1, f64::NAN), line(2, 0.3), line(3, -0.2)],
            true,
        );
        assert_eq!(ids(&table), ["t3", "t1", "t4", "t2"]);
    }

    #[test]
    fn a_single_lap_keeps_lap_order_and_the_user_sort_survives_a_rebuild() {
        let mut table = CornerTable::new(gpui_kit::px(16.));
        table.set_lines(vec![line(1, f64::NAN), line(0, f64::NAN)], false);
        assert_eq!(ids(&table), ["t1", "t2"]);
        table.sort = (Col::Order, ColumnSort::Descending);
        table.apply_sort();
        table.set_lines(vec![line(0, 0.2), line(1, 0.1)], true);
        assert_eq!(ids(&table), ["t2", "t1"], "the user's sort is kept");
    }
}
