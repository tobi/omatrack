//! Laps: every lap of the primary recording, for picking the next lap to
//! study or compare.
//!
//! A [`DataTable`] over the library's lap nodes of the primary's recording
//! (so it fills in as soon as a lap is selected, before the lap loads).
//! Best and representative laps read at a glance; the loaded primary and
//! reference carry their role marker.
//!
//! Commands are the library's: Enter sets the selected lap as primary,
//! Alt+Enter as reference, a double-click sets the primary, and the context
//! menu offers both. Every path dispatches [`SelectLap`].

use gpui_kit::component::{
    ActiveTheme as _, IconName, Sizable as _, h_flex,
    menu::PopupMenu,
    table::{Column, ColumnSort, DataTable, TableDelegate, TableEvent, TableState},
    tag::Tag,
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, AppContext as _, Context, Div, ElementId, Entity, FocusHandle, InteractiveElement as _,
    IntoElement, KeyBinding, ParentElement as _, Pixels, Render, SharedString, Stateful,
    StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _, Window, div,
};
use omatrack_core::format_lap_time;
use omatrack_core::session::LapStripKind;
use omatrack_library::catalog::LapNode;
use omatrack_ui::TypeScale as _;
use omatrack_ui::{DeltaSense, LapRole, MISSING_VALUE, Swatch, format_delta};

use crate::actions::{Role, SelectLap, SetPrimary, SetReference};
use crate::panels::{PanelKind, SELECT_A_LAP, empty_state, panel_body};
use crate::state::AppState;

/// The panel's key context: Enter / Alt+Enter set the selected lap.
pub const LAPS_CONTEXT: &str = "Laps";

/// One lap as the table shows it (plain data).
#[derive(Debug, Clone, PartialEq)]
pub struct LapLine {
    /// Position in recording order.
    order: usize,
    lap: i32,
    label: SharedString,
    time_ms: f64,
    delta_to_best_ms: Option<f64>,
    best: bool,
    representative: bool,
    complete: bool,
    kind: LapStripKind,
    role: Option<LapRole>,
}

impl LapLine {
    fn new(order: usize, node: &LapNode, roles: &[(LapRole, i32)]) -> Self {
        Self {
            order,
            lap: node.lap_id,
            label: node.label.clone().into(),
            time_ms: node.time_ms,
            delta_to_best_ms: node.delta_to_best_ms,
            best: node.best,
            representative: node.representative,
            complete: node.complete,
            kind: node.kind,
            role: roles
                .iter()
                .find(|(_, lap)| *lap == node.lap_id)
                .map(|(role, _)| *role),
        }
    }

    pub fn lap(&self) -> i32 {
        self.lap
    }

    pub fn label(&self) -> &SharedString {
        &self.label
    }
}

fn kind_label(kind: LapStripKind) -> &'static str {
    match kind {
        LapStripKind::Flying => "Flying",
        LapStripKind::Out => "Out",
        LapStripKind::In => "In",
        LapStripKind::Fragment => "Fragment",
        LapStripKind::PitStop => "Pit stop",
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Col {
    Lap,
    Time,
    Delta,
    Kind,
    Complete,
}

impl Col {
    const ALL: [Self; 5] = [
        Self::Lap,
        Self::Time,
        Self::Delta,
        Self::Kind,
        Self::Complete,
    ];

    fn key(self) -> &'static str {
        match self {
            Self::Lap => "lap",
            Self::Time => "time",
            Self::Delta => "delta",
            Self::Kind => "kind",
            Self::Complete => "complete",
        }
    }

    fn title(self) -> &'static str {
        match self {
            Self::Lap => "Lap",
            Self::Time => "Time",
            Self::Delta => "Δ best",
            Self::Kind => "Kind",
            Self::Complete => "Complete",
        }
    }

    /// Width in rems.
    fn width(self) -> f32 {
        match self {
            Self::Lap => 4.5,
            Self::Time => 5.5,
            Self::Delta => 5.0,
            Self::Kind => 5.5,
            Self::Complete => 5.0,
        }
    }

    fn is_numeric(self) -> bool {
        matches!(self, Self::Time | Self::Delta)
    }
}

/// The [`TableDelegate`] of the laps table.
pub struct LapTable {
    lines: Vec<LapLine>,
    /// The selected lap, by id.
    selected: Option<i32>,
    sort: (Col, ColumnSort),
    /// The window's rem size when built (column widths are pixels).
    rem: Pixels,
}

impl LapTable {
    fn new(rem: Pixels) -> Self {
        Self {
            lines: Vec::new(),
            selected: None,
            sort: (Col::Lap, ColumnSort::Default),
            rem,
        }
    }

    pub fn lines(&self) -> &[LapLine] {
        &self.lines
    }

    /// The selected lap id.
    pub fn selected(&self) -> Option<i32> {
        self.selected
    }

    fn position(&self, lap: i32) -> Option<usize> {
        self.lines.iter().position(|line| line.lap == lap)
    }

    fn set_lines(&mut self, lines: Vec<LapLine>) {
        self.lines = lines;
        self.apply_sort();
    }

    fn apply_sort(&mut self) {
        let (col, sort) = self.sort;
        let descending = sort == ColumnSort::Descending;
        let key = |line: &LapLine| -> f64 {
            match (sort, col) {
                (ColumnSort::Default, _) | (_, Col::Lap) => line.order as f64,
                (_, Col::Time) if line.time_ms > 0.0 => line.time_ms,
                (_, Col::Delta) => line.delta_to_best_ms.unwrap_or(f64::NAN),
                _ => f64::NAN,
            }
        };
        self.lines.sort_by(|a, b| {
            let (x, y) = (key(a), key(b));
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

    /// The selected lap's row, else the first primary-role row, else none.
    fn selection_target(&mut self) -> Option<usize> {
        let ix = self
            .selected
            .and_then(|lap| self.position(lap))
            .or_else(|| {
                self.lines
                    .iter()
                    .position(|line| line.role == Some(LapRole::Primary))
            });
        self.selected = ix.map(|ix| self.lines[ix].lap);
        ix
    }
}

impl TableDelegate for LapTable {
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
            .movable(false)
            .when(col.is_numeric(), Column::text_right);
        match col {
            Col::Lap | Col::Time | Col::Delta => match self.sort {
                (sorted, ColumnSort::Ascending) if sorted == col => column.ascending(),
                (sorted, ColumnSort::Descending) if sorted == col => column.descending(),
                _ => column.sortable(),
            },
            Col::Kind | Col::Complete => column,
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
        let lap = self.lines.get(row_ix).map_or(-1, |line| line.lap);
        div().id(ElementId::Name(format!("lap:{lap}").into()))
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
        // Out, in, pit and partial laps are context, not candidates.
        let text = if line.representative {
            theme.foreground
        } else {
            theme.muted_foreground
        };
        match Col::ALL[col_ix] {
            Col::Lap => h_flex()
                .w_full()
                .gap_1()
                .child(h_flex().flex_shrink_0().w_7().gap_0p5().when_some(
                    line.role,
                    |this, role| {
                        this.child(Swatch::new(role.color(theme)).xsmall()).child(
                            div()
                                .text_label()
                                .numeric()
                                .text_color(theme.muted_foreground)
                                .child(role.marker()),
                        )
                    },
                ))
                .child(div().text_color(text).child(line.label.clone()))
                .into_any_element(),
            Col::Time => h_flex()
                .w_full()
                .justify_end()
                .numeric()
                .text_color(text)
                .child(if line.time_ms > 0.0 {
                    SharedString::from(format_lap_time(line.time_ms))
                } else {
                    MISSING_VALUE.into()
                })
                .into_any_element(),
            Col::Delta => h_flex()
                .w_full()
                .justify_end()
                .map(|this| {
                    if line.best {
                        this.child(Tag::secondary().xsmall().child("Best"))
                    } else {
                        // Every other lap is slower by definition: the gap is
                        // metadata, not a gain or loss, so it stays neutral.
                        let delta = line.delta_to_best_ms.map(|ms| ms / 1000.0);
                        this.numeric()
                            .text_color(theme.muted_foreground)
                            .child(format_delta(delta, 3, DeltaSense::LowerIsBetter).0)
                    }
                })
                .into_any_element(),
            Col::Kind => div()
                .text_color(text)
                .child(kind_label(line.kind))
                .into_any_element(),
            Col::Complete => div()
                .text_color(theme.muted_foreground)
                .child(if line.complete { "Yes" } else { "No" })
                .into_any_element(),
        }
    }

    fn context_menu(
        &mut self,
        row_ix: usize,
        menu: PopupMenu,
        _: &mut Window,
        cx: &mut Context<TableState<Self>>,
    ) -> PopupMenu {
        let Some(line) = self.lines.get(row_ix) else {
            return menu;
        };
        let Some(session) = AppState::try_global(cx)
            .and_then(|app| app.session.read(cx).primary())
            .map(|slot| slot.lap_ref().session().clone())
        else {
            return menu;
        };
        menu.menu(
            format!("Set {} as primary", line.label),
            Box::new(SelectLap {
                session: session.clone(),
                lap: line.lap,
                role: Role::Primary,
            }),
        )
        .menu(
            format!("Set {} as reference", line.label),
            Box::new(SelectLap {
                session,
                lap: line.lap,
                role: Role::Reference,
            }),
        )
    }

    fn render_empty(
        &mut self,
        _: &mut Window,
        _: &mut Context<TableState<Self>>,
    ) -> impl IntoElement {
        empty_state(IconName::Inbox, "No laps", "This recording has no laps.")
    }

    fn cell_text(&self, row_ix: usize, col_ix: usize, _: &App) -> String {
        let Some(line) = self.lines.get(row_ix) else {
            return String::new();
        };
        match Col::ALL[col_ix] {
            Col::Lap => line.label.to_string(),
            Col::Time => format_lap_time(line.time_ms),
            Col::Delta if line.best => "Best".to_string(),
            Col::Delta => format_delta(
                line.delta_to_best_ms.map(|ms| ms / 1000.0),
                3,
                DeltaSense::LowerIsBetter,
            )
            .0
            .to_string(),
            Col::Kind => kind_label(line.kind).to_string(),
            Col::Complete => if line.complete { "Yes" } else { "No" }.to_string(),
        }
    }
}

pub struct LapsPanel {
    app: AppState,
    focus_handle: FocusHandle,
    /// Built on the first render (the table state needs the window).
    table: Option<Entity<TableState<LapTable>>>,
    /// The catalog session the table lists.
    session: Option<SharedString>,
    _subscriptions: Vec<Subscription>,
}

impl LapsPanel {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let subscriptions = vec![
            cx.observe(&app.session, |this, _, cx| this.sync(cx)),
            cx.observe(&app.library, |this, _, cx| this.sync(cx)),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle(),
            table: None,
            session: None,
            _subscriptions: subscriptions,
        };
        panel.sync(cx);
        panel
    }

    /// The table state, once the panel has rendered.
    pub fn table(&self) -> Option<&Entity<TableState<LapTable>>> {
        self.table.as_ref()
    }

    /// The lap on the table's selected row.
    pub fn selected(&self, cx: &App) -> Option<i32> {
        let table = self.table.as_ref()?.read(cx);
        let ix = table.selected_row()?;
        table.delegate().lines().get(ix).map(LapLine::lap)
    }

    /// Rebuild the rows from the library and the session's roles.
    fn sync(&mut self, cx: &mut Context<Self>) {
        let session = self.app.session.read(cx);
        let primary = session.primary().map(|slot| slot.lap_ref().clone());
        let reference = session.reference().map(|slot| slot.lap_ref().clone());
        let session_id = primary.as_ref().map(|lap| lap.session().clone());
        let previous = std::mem::replace(&mut self.session, session_id.clone());
        let Some(table) = self.table.clone() else {
            cx.notify();
            return;
        };
        let lines = session_id
            .as_ref()
            .and_then(|id| {
                let library = self.app.library.read(cx);
                let node = library.snapshot().session(id)?;
                let mut roles = Vec::new();
                if let Some(primary) = &primary {
                    roles.push((LapRole::Primary, primary.lap()));
                }
                if let Some(reference) = reference.as_ref().filter(|r| r.session() == id) {
                    roles.push((LapRole::Reference, reference.lap()));
                }
                Some(
                    node.laps
                        .iter()
                        .enumerate()
                        .map(|(order, lap)| LapLine::new(order, lap, &roles))
                        .collect::<Vec<_>>(),
                )
            })
            .unwrap_or_default();
        let new_recording = previous != session_id;
        let primary_lap = primary.as_ref().map(|lap| lap.lap());
        table.update(cx, |table, cx| {
            let delegate = table.delegate_mut();
            if new_recording {
                delegate.selected = primary_lap;
            }
            delegate.set_lines(lines);
            let target = delegate.selection_target();
            table.refresh(cx);
            match target {
                Some(ix) if table.selected_row() != Some(ix) => table.set_selected_row(ix, cx),
                Some(_) => {}
                None if table.selected_row().is_some() => table.clear_selection(cx),
                None => {}
            }
            cx.notify();
        });
        cx.notify();
    }

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
            TableState::new(LapTable::new(rem), window, cx)
                .col_movable(false)
                .col_selectable(false)
                .loop_selection(false)
        });
        self._subscriptions
            .push(cx.subscribe_in(&table, window, Self::on_table_event));
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
        self.session = None;
        self.sync(cx);
    }

    fn on_table_event(
        &mut self,
        table: &Entity<TableState<LapTable>>,
        event: &TableEvent,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        match event {
            TableEvent::SelectRow(ix) => {
                let ix = *ix;
                table.update(cx, |table, _| {
                    let delegate = table.delegate_mut();
                    if let Some(line) = delegate.lines.get(ix) {
                        delegate.selected = Some(line.lap);
                    }
                });
            }
            TableEvent::DoubleClickedRow(_) => self.select_lap(Role::Primary, window, cx),
            _ => {}
        }
    }

    /// Load the selected lap into `role` (the same action as the library
    /// and the palette).
    fn select_lap(&mut self, role: Role, window: &mut Window, cx: &mut App) {
        let (Some(session), Some(lap)) = (self.session.clone(), self.selected(cx)) else {
            return;
        };
        window.dispatch_action(Box::new(SelectLap { session, lap, role }), cx);
    }
}

impl gpui_kit::component::dock::BasePanel for LapsPanel {
    fn panel_name(&self) -> &'static str {
        PanelKind::Laps.name()
    }

    fn closable(&self, _: &App) -> bool {
        false
    }
}

impl gpui_kit::component::dock::Panel for LapsPanel {
    fn tab_name(&self, _: &App) -> Option<SharedString> {
        Some(PanelKind::Laps.title().into())
    }

    fn title(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        PanelKind::Laps.title()
    }
}

impl gpui_kit::EventEmitter<gpui_kit::component::dock::PanelEvent> for LapsPanel {}

impl gpui_kit::Focusable for LapsPanel {
    /// The table takes keyboard focus once it exists (Ctrl+5 lands on the
    /// rows).
    fn focus_handle(&self, cx: &App) -> FocusHandle {
        match &self.table {
            Some(table) => gpui_kit::Focusable::focus_handle(table.read(cx), cx),
            None => self.focus_handle.clone(),
        }
    }
}

impl Render for LapsPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.ensure_table(window, cx);
        let root = div()
            .id("laps-panel")
            .test_support()
            .key_context(LAPS_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(|this, _: &SetPrimary, window, cx| {
                this.select_lap(Role::Primary, window, cx)
            }))
            .on_action(cx.listener(|this, _: &SetReference, window, cx| {
                this.select_lap(Role::Reference, window, cx)
            }))
            .size_full();
        let (Some(table), Some(session)) = (self.table.clone(), self.session.clone()) else {
            return root.child(panel_body(
                "laps-summary",
                format!("No lap selected. {SELECT_A_LAP}"),
                empty_state(IconName::Inbox, "No lap selected", SELECT_A_LAP),
                cx,
            ));
        };
        let library = self.app.library.read(cx);
        let title: SharedString = library
            .snapshot()
            .session(&session)
            .map(|node| node.title.clone().into())
            .unwrap_or_default();
        let count = table.read(cx).delegate().lines().len();
        root.child(
            v_flex()
                .id("laps-table")
                .test_support()
                .aria_label(SharedString::from(format!("{count} laps of {title}")))
                .size_full()
                .child(
                    div()
                        .px_2()
                        .py_1()
                        .text_label()
                        .truncate()
                        .text_color(cx.theme().muted_foreground)
                        .child(title),
                )
                .child(
                    div()
                        .flex_1()
                        .min_h_0()
                        .child(DataTable::new(&table).small().bordered(false)),
                ),
        )
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and its key bindings (the library's Set primary /
/// Set reference commands, on the selected lap). Called once from
/// [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::Laps, cx);
    cx.bind_keys([
        KeyBinding::new("enter", SetPrimary, Some(LAPS_CONTEXT)),
        KeyBinding::new("alt-enter", SetReference, Some(LAPS_CONTEXT)),
    ]);
}
