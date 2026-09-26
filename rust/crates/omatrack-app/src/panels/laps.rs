//! Laps: the loaded event's laps, one group per recording, for picking
//! the next lap to study or compare.
//!
//! The default left surface (the Library tree is the tab beside it, for
//! other events). Scope is the primary's event, its track and day: every
//! recording of that day becomes a group (plus the reference's recording
//! when it comes from another day). A group reads as its driver, `9 timed
//! laps, best 1:16.091` and a lap-time trend line; its timed laps
//! (`counts_for_best`) follow in recording order with a bar that grows
//! with the gap to that driver's best (scaled per group) and the gap
//! itself; out, in, pit and partial laps wait behind a `Show out and in
//! laps (N)` disclosure. The laps holding a role are filled and carry the
//! role badge; groups without a role start collapsed.
//!
//! Commands are the library's and the filmstrip's: Enter sets the lap
//! under the keyboard cursor as primary, Alt+Enter as reference (Enter on
//! a group or disclosure row opens or closes it); a click selects the lap
//! for the group's role (the reference when the group holds only the
//! reference, else the primary), a right click or Alt+click sets the
//! reference. Every path dispatches [`SelectLap`].

use std::cell::RefCell;
use std::collections::HashSet;
use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::base::actions::{SelectDown, SelectUp};
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Icon, IconName, Sizable as _, StyledExt as _,
    button::{Button, ButtonVariants as _},
    h_flex,
    kbd::Kbd,
    list::ListItem,
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, Bounds, Context, FocusHandle, Hsla, InteractiveElement as _, IntoElement, KeyBinding,
    MouseButton, ParentElement as _, Pixels, Rems, Render, RenderOnce, Role as AccessRole,
    ScrollHandle, SharedString, StatefulInteractiveElement as _, Styled as _, Subscription,
    TestSupportExt as _, Window, canvas, div, fill, point, px, relative, rems, size,
};
use omatrack_core::format_lap_time;
use omatrack_library::{LibrarySnapshot, SessionNode};
use omatrack_trace::decimate::PathPoint;
use omatrack_trace::lanes::PathBuffer;
use omatrack_ui::TypeScale as _;
use omatrack_ui::{DeltaSense, LapRole, MISSING_VALUE, format_delta};

use crate::actions::{FocusPanel6, Role, SelectLap, SetPrimary, SetReference};
use crate::panels::{PanelKind, SELECT_A_LAP, empty_state};
use crate::state::{AppState, LapRef};

/// The panel's key context: Up / Down move the cursor, Enter / Alt+Enter
/// set the lap under it.
pub const LAPS_CONTEXT: &str = "Laps";

/// The lane before a row's label: a group's chevron, a lap's role badge.
const LEAD_LANE: Rems = Rems(1.5);
/// The lap label column (`L10`, `Out`).
const LABEL_COLUMN: Rems = Rems(2.25);
/// The lap time column: fits `11:49.212` in tabular figures.
const TIME_COLUMN: Rems = Rems(4.25);
/// The gap column: `+11.440` or `Best`.
const DELTA_COLUMN: Rems = Rems(3.25);
/// The gap bar's thickness, and its shortest fill (the best lap's tick).
const BAR_HEIGHT: Rems = Rems(0.25);
const BAR_MIN: Rems = Rems(0.1875);
/// The lap-time trend line beside a group's heading.
const TREND_WIDTH: Rems = Rems(4.5);
const TREND_HEIGHT: Rems = Rems(1.25);
/// Space above every group after the first.
const GROUP_GAP: Rems = Rems(0.75);

/// One lap as the sidebar shows it (plain data, formatted once per sync).
#[derive(Debug, Clone, PartialEq)]
pub struct LapLine {
    /// The catalog lap id (`<session>/l:<lap>`): the row's identity.
    id: SharedString,
    lap: i32,
    /// `L8`; `Out`, `In`, `Pit`, `Frag` for a lap that is not timed.
    label: SharedString,
    time: SharedString,
    /// The gap to the group's best (`+0.911`), for a timed lap.
    delta: Option<SharedString>,
    best: bool,
    /// Counts for best: a comparable lap time, listed without disclosure.
    timed: bool,
    /// The gap as a share of the group's largest gap, `0..=1`.
    bar: Option<f32>,
    role: Option<LapRole>,
}

impl LapLine {
    pub fn lap(&self) -> i32 {
        self.lap
    }

    pub fn label(&self) -> &SharedString {
        &self.label
    }

    pub fn is_timed(&self) -> bool {
        self.timed
    }

    pub fn is_best(&self) -> bool {
        self.best
    }

    /// The bar's share of the row's bar lane (`None` for untimed laps).
    pub fn bar(&self) -> Option<f32> {
        self.bar
    }

    pub fn role(&self) -> Option<LapRole> {
        self.role
    }

    /// The row's accessible name: `L8, 1:13.644, best lap, reference`.
    fn spoken(&self) -> SharedString {
        [
            Some(self.label.to_string()),
            Some(self.time.to_string()),
            self.delta.as_ref().map(|delta| format!("{delta} to best")),
            self.best.then(|| "best lap".to_string()),
            self.role.map(|role| role.label().to_lowercase()),
        ]
        .into_iter()
        .flatten()
        .collect::<Vec<_>>()
        .join(", ")
        .into()
    }
}

/// One recording of the event (plain data).
#[derive(Debug, Clone, PartialEq)]
pub struct LapGroup {
    session: SharedString,
    /// The driver, else the recording's session name or file.
    title: SharedString,
    /// `9 timed laps, best 1:16.091` (prefixed by the session name when
    /// another group has the same title).
    summary: SharedString,
    laps: Vec<LapLine>,
    /// Timed lap times in recording order, as `0..=1` where 1 is the best
    /// (the trend line), and the best's position among them.
    trend: Arc<[f32]>,
    trend_best: Option<usize>,
    roles: Vec<LapRole>,
}

impl LapGroup {
    pub fn session(&self) -> &SharedString {
        &self.session
    }

    pub fn title(&self) -> &SharedString {
        &self.title
    }

    pub fn summary(&self) -> &SharedString {
        &self.summary
    }

    /// Every lap, in recording order.
    pub fn laps(&self) -> &[LapLine] {
        &self.laps
    }

    pub fn lap(&self, lap: i32) -> Option<&LapLine> {
        self.laps.iter().find(|line| line.lap == lap)
    }

    /// Laps behind the disclosure: not timed.
    pub fn untimed(&self) -> usize {
        self.laps.iter().filter(|line| !line.timed).count()
    }

    pub fn timed(&self) -> usize {
        self.laps.len() - self.untimed()
    }

    /// The roles this recording's laps hold.
    pub fn roles(&self) -> &[LapRole] {
        &self.roles
    }

    /// The role a plain click on one of its laps asks for (the filmstrip's
    /// rule): the reference when the group holds only the reference.
    pub fn click_role(&self) -> Role {
        if self.roles == [LapRole::Reference] {
            Role::Reference
        } else {
            Role::Primary
        }
    }
}

/// The groups of the event holding `primary`: every recording of its track
/// and day in catalog order, then the reference's recording when it comes
/// from elsewhere. Empty without a primary in the snapshot.
pub fn event_groups(
    snapshot: &LibrarySnapshot,
    primary: Option<&LapRef>,
    reference: Option<&LapRef>,
) -> Vec<LapGroup> {
    let Some(primary) = primary else {
        return Vec::new();
    };
    let Some(day) = snapshot
        .tracks()
        .iter()
        .flat_map(|track| track.dates.iter())
        .find(|day| {
            day.sessions
                .iter()
                .any(|node| node.id == primary.session().as_ref())
        })
    else {
        return Vec::new();
    };
    let mut nodes: Vec<&SessionNode> = day.sessions.iter().collect();
    if let Some(node) = reference
        .filter(|reference| {
            !nodes
                .iter()
                .any(|node| node.id == reference.session().as_ref())
        })
        .and_then(|reference| snapshot.session(reference.session()))
    {
        nodes.push(node);
    }
    let titles: Vec<SharedString> = nodes.iter().map(|node| group_title(node)).collect();
    nodes
        .iter()
        .zip(&titles)
        .map(|(node, title)| {
            let shared = titles.iter().filter(|other| *other == title).count() > 1;
            build_group(node, title.clone(), shared, primary, reference)
        })
        .collect()
}

fn group_title(node: &SessionNode) -> SharedString {
    match crate::panels::library::driver_of(node) {
        Some(driver) => driver.name,
        None => node
            .session_name
            .clone()
            .map(SharedString::from)
            .unwrap_or_else(|| node.title.clone().into()),
    }
}

fn build_group(
    node: &SessionNode,
    title: SharedString,
    shared_title: bool,
    primary: &LapRef,
    reference: Option<&LapRef>,
) -> LapGroup {
    let role_of = |lap: i32| {
        let holds = |slot: Option<&LapRef>| {
            slot.is_some_and(|slot| slot.session().as_ref() == node.id && slot.lap() == lap)
        };
        if holds(Some(primary)) {
            Some(LapRole::Primary)
        } else if holds(reference) {
            Some(LapRole::Reference)
        } else {
            None
        }
    };
    let timed: Vec<f64> = node
        .laps
        .iter()
        .filter(|lap| lap.representative && lap.time_ms > 0.0)
        .map(|lap| lap.time_ms)
        .collect();
    let best = timed.iter().copied().fold(f64::INFINITY, f64::min);
    let worst = timed.iter().copied().fold(f64::NEG_INFINITY, f64::max);
    let spread = worst - best;
    let laps: Vec<LapLine> = node
        .laps
        .iter()
        .map(|lap| {
            let is_timed = lap.representative && lap.time_ms > 0.0;
            let gap = lap.delta_to_best_ms.filter(|_| is_timed);
            LapLine {
                id: lap.id.clone().into(),
                lap: lap.lap_id,
                label: lap.label.clone().into(),
                time: if lap.time_ms > 0.0 {
                    format_lap_time(lap.time_ms).into()
                } else {
                    MISSING_VALUE.into()
                },
                delta: gap
                    .filter(|_| !lap.best)
                    .map(|ms| format_delta(Some(ms / 1000.0), 3, DeltaSense::LowerIsBetter).0),
                best: lap.best,
                timed: is_timed,
                bar: is_timed.then(|| {
                    if spread > 0.0 {
                        ((lap.time_ms - best) / spread).clamp(0.0, 1.0) as f32
                    } else {
                        0.0
                    }
                }),
                role: role_of(lap.lap_id),
            }
        })
        .collect();
    let trend: Arc<[f32]> = timed
        .iter()
        .map(|time| {
            if spread > 0.0 {
                (1.0 - (time - best) / spread) as f32
            } else {
                0.5
            }
        })
        .collect();
    let trend_best = timed.iter().position(|time| *time == best);
    let mut roles: Vec<LapRole> = laps.iter().filter_map(|line| line.role).collect();
    roles.sort_by_key(|role| *role == LapRole::Reference);
    let count = timed.len();
    let mut summary = match node.best_time_ms.filter(|_| count > 0) {
        Some(best) => format!(
            "{count} timed {}, best {}",
            if count == 1 { "lap" } else { "laps" },
            format_lap_time(best)
        ),
        None => "No timed laps".to_string(),
    };
    if shared_title && let Some(session) = &node.session_name {
        summary = format!("{session} \u{b7} {summary}");
    }
    LapGroup {
        session: node.id.clone().into(),
        title,
        summary: summary.into(),
        laps,
        trend,
        trend_best,
        roles,
    }
}

/// A keyboard-cursor position, by domain id (never an index).
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum LapsCursor {
    /// A group's heading row.
    Group(SharedString),
    /// A lap row: the session and lap id.
    Lap(SharedString, i32),
    /// A group's `Show out and in laps` row.
    Disclosure(SharedString),
}

/// Reused geometry of one trend line (no allocation once warm).
#[derive(Default)]
struct TrendBuffers {
    points: Vec<PathPoint>,
    path: PathBuffer,
}

pub struct LapsPanel {
    app: AppState,
    focus_handle: FocusHandle,
    groups: Rc<[LapGroup]>,
    /// One trend buffer per group, in group order.
    trends: Vec<Rc<RefCell<TrendBuffers>>>,
    /// Groups the user opened or closed, against their default (open when
    /// it holds a role).
    opened: HashSet<SharedString>,
    closed: HashSet<SharedString>,
    /// Groups whose untimed laps are shown.
    disclosed: HashSet<SharedString>,
    cursor: Option<LapsCursor>,
    /// The primary last synced (the cursor follows a new primary).
    primary: Option<LapRef>,
    scroll: ScrollHandle,
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
            groups: Rc::from(Vec::new()),
            trends: Vec::new(),
            opened: HashSet::new(),
            closed: HashSet::new(),
            disclosed: HashSet::new(),
            cursor: None,
            primary: None,
            scroll: ScrollHandle::new(),
            _subscriptions: subscriptions,
        };
        panel.sync(cx);
        panel
    }

    /// The event's groups, in display order.
    pub fn groups(&self) -> &[LapGroup] {
        &self.groups
    }

    pub fn group(&self, session: &str) -> Option<&LapGroup> {
        self.groups
            .iter()
            .find(|group| group.session.as_ref() == session)
    }

    pub fn cursor(&self) -> Option<&LapsCursor> {
        self.cursor.as_ref()
    }

    /// The lap under the keyboard cursor.
    pub fn cursor_lap(&self) -> Option<(SharedString, i32)> {
        match &self.cursor {
            Some(LapsCursor::Lap(session, lap)) => Some((session.clone(), *lap)),
            _ => None,
        }
    }

    pub fn is_expanded(&self, session: &str) -> bool {
        let Some(group) = self.group(session) else {
            return false;
        };
        if group.roles.is_empty() {
            self.opened.contains(session)
        } else {
            !self.closed.contains(session)
        }
    }

    pub fn is_disclosed(&self, session: &str) -> bool {
        self.disclosed.contains(session)
    }

    /// The laps a group lists now: timed laps, laps holding a role, and
    /// the rest once disclosed (none while collapsed).
    pub fn visible_laps<'a>(&'a self, group: &'a LapGroup) -> impl Iterator<Item = &'a LapLine> {
        let expanded = self.is_expanded(&group.session);
        let disclosed = self.is_disclosed(&group.session);
        group
            .laps
            .iter()
            .filter(move |line| expanded && (line.timed || disclosed || line.role.is_some()))
    }

    /// Every row in display order (the cursor's path).
    pub fn rows(&self) -> Vec<LapsCursor> {
        let mut rows = Vec::new();
        for group in self.groups.iter() {
            rows.push(LapsCursor::Group(group.session.clone()));
            rows.extend(
                self.visible_laps(group)
                    .map(|line| LapsCursor::Lap(group.session.clone(), line.lap)),
            );
            if self.is_expanded(&group.session) && group.untimed() > 0 {
                rows.push(LapsCursor::Disclosure(group.session.clone()));
            }
        }
        rows
    }

    /// Rebuild the groups from the library and the session's roles.
    fn sync(&mut self, cx: &mut Context<Self>) {
        let session = self.app.session.read(cx);
        let primary = session.primary().map(|slot| slot.lap_ref().clone());
        let reference = session.reference().map(|slot| slot.lap_ref().clone());
        let groups = event_groups(
            self.app.library.read(cx).snapshot(),
            primary.as_ref(),
            reference.as_ref(),
        );
        if *groups != *self.groups {
            self.trends.resize_with(groups.len(), || {
                Rc::new(RefCell::new(TrendBuffers::default()))
            });
            self.groups = groups.into();
        }
        if primary != self.primary {
            self.primary = primary.clone();
            if let Some(primary) = &primary {
                self.cursor = Some(LapsCursor::Lap(primary.session().clone(), primary.lap()));
                self.scroll_to_cursor();
            }
        }
        let rows = self.rows();
        if self
            .cursor
            .as_ref()
            .is_some_and(|cursor| !rows.contains(cursor))
        {
            self.cursor = rows.first().cloned();
        }
        cx.notify();
    }

    fn scroll_to_cursor(&self) {
        let Some(cursor) = &self.cursor else {
            return;
        };
        if let Some(ix) = self.rows().iter().position(|row| row == cursor) {
            self.scroll.scroll_to_item(ix);
        }
    }

    fn move_cursor(&mut self, step: isize, cx: &mut Context<Self>) {
        let rows = self.rows();
        if rows.is_empty() {
            return;
        }
        let ix = match self
            .cursor
            .as_ref()
            .and_then(|cursor| rows.iter().position(|row| row == cursor))
        {
            Some(ix) => ix.saturating_add_signed(step).min(rows.len() - 1),
            None => 0,
        };
        self.cursor = Some(rows[ix].clone());
        self.scroll.scroll_to_item(ix);
        cx.notify();
    }

    /// Open or close a group.
    pub fn toggle_group(&mut self, session: &SharedString, cx: &mut Context<Self>) {
        let Some(group) = self.group(session) else {
            return;
        };
        // Record the choice against the group's default (open with a role).
        let toggled = if group.roles.is_empty() {
            &mut self.opened
        } else {
            &mut self.closed
        };
        if !toggled.remove(session) {
            toggled.insert(session.clone());
        }
        cx.notify();
    }

    /// Show or hide a group's untimed laps.
    pub fn toggle_disclosure(&mut self, session: &SharedString, cx: &mut Context<Self>) {
        if !self.disclosed.remove(session) {
            self.disclosed.insert(session.clone());
        }
        cx.notify();
    }

    /// Enter: set the cursor's lap as primary, or open/close its row.
    fn on_set_primary(&mut self, _: &SetPrimary, window: &mut Window, cx: &mut Context<Self>) {
        match self.cursor.clone() {
            Some(LapsCursor::Lap(session, lap)) => select(session, lap, Role::Primary, window, cx),
            Some(LapsCursor::Group(session)) => self.toggle_group(&session, cx),
            Some(LapsCursor::Disclosure(session)) => self.toggle_disclosure(&session, cx),
            None => {}
        }
    }

    /// Alt+Enter: set the cursor's lap as reference.
    fn on_set_reference(&mut self, _: &SetReference, window: &mut Window, cx: &mut Context<Self>) {
        if let Some((session, lap)) = self.cursor_lap() {
            select(session, lap, Role::Reference, window, cx);
        }
    }

    /// A pointer press on a row: it takes the cursor and the focus.
    fn point_at(&mut self, cursor: LapsCursor, window: &mut Window, cx: &mut Context<Self>) {
        self.cursor = Some(cursor);
        window.focus(&self.focus_handle, cx);
        cx.notify();
    }

    fn render_group(
        &self,
        ix: usize,
        group: &LapGroup,
        focused: bool,
        cx: &mut Context<Self>,
    ) -> ListItem {
        let theme = cx.theme();
        let session = group.session.clone();
        let expanded = self.is_expanded(&session);
        let cursor = LapsCursor::Group(session.clone());
        let at_cursor = focused && self.cursor.as_ref() == Some(&cursor);
        row(
            SharedString::from(format!("laps-group:{session}")),
            at_cursor,
            theme.ring,
        )
        .py_1()
        .when(ix > 0, |this| this.mt(GROUP_GAP))
        .role(AccessRole::Button)
        .aria_expanded(expanded)
        .aria_label(SharedString::from(format!(
            "{}, {}",
            group.title, group.summary
        )))
        .on_click(cx.listener(move |this, _, window, cx| {
            this.point_at(cursor.clone(), window, cx);
            this.toggle_group(&session, cx);
        }))
        .child(
            h_flex()
                .w_full()
                .gap_2()
                .child(
                    h_flex().flex_shrink_0().w(LEAD_LANE).child(
                        Icon::new(if expanded {
                            IconName::ChevronDown
                        } else {
                            IconName::ChevronRight
                        })
                        .xsmall()
                        .text_color(theme.muted_foreground),
                    ),
                )
                .child(
                    v_flex()
                        .flex_1()
                        .min_w_0()
                        // The trend rides beside the title so the summary
                        // keeps the full width (it never truncates at the
                        // default dock width).
                        .child(
                            h_flex()
                                .w_full()
                                .gap_2()
                                .child(
                                    div()
                                        .flex_1()
                                        .min_w_0()
                                        .truncate()
                                        .text_title()
                                        .font_medium()
                                        .text_color(theme.sidebar_foreground)
                                        .child(group.title.clone()),
                                )
                                .when(group.trend.len() > 1, |this| {
                                    this.child(Trend {
                                        values: group.trend.clone(),
                                        best: group.trend_best,
                                        buffers: self.trends[ix].clone(),
                                        color: theme.muted_foreground,
                                        mark: theme.sidebar_foreground,
                                    })
                                }),
                        )
                        .child(
                            div()
                                .truncate()
                                .text_label()
                                .numeric()
                                .text_color(theme.muted_foreground)
                                .child(group.summary.clone()),
                        ),
                ),
        )
    }

    fn render_lap(
        &self,
        group: &LapGroup,
        line: &LapLine,
        focused: bool,
        cx: &mut Context<Self>,
    ) -> ListItem {
        let theme = cx.theme();
        let session = group.session.clone();
        let lap = line.lap;
        let cursor = LapsCursor::Lap(session.clone(), lap);
        let at_cursor = focused && self.cursor.as_ref() == Some(&cursor);
        let click_role = group.click_role();
        let muted = !line.timed;
        let bar_color = match line.role {
            Some(role) => role.color(theme),
            None => theme.muted_foreground.opacity(0.55),
        };
        let right_cursor = cursor.clone();
        let right_session = session.clone();
        row(line.id.clone(), at_cursor, theme.ring)
            .selected(line.role.is_some())
            .role(AccessRole::ListItem)
            .aria_selected(line.role.is_some())
            .aria_label(line.spoken())
            .on_click(
                cx.listener(move |this, event: &gpui_kit::ClickEvent, window, cx| {
                    this.point_at(cursor.clone(), window, cx);
                    let role = if event.modifiers().alt {
                        Role::Reference
                    } else {
                        click_role
                    };
                    select(session.clone(), lap, role, window, cx);
                }),
            )
            .on_mouse_down(
                MouseButton::Right,
                cx.listener(move |this, _, window, cx| {
                    this.point_at(right_cursor.clone(), window, cx);
                    select(right_session.clone(), lap, Role::Reference, window, cx);
                }),
            )
            .child(
                h_flex()
                    .w_full()
                    .gap_2()
                    .text_body()
                    .child(
                        h_flex()
                            .flex_shrink_0()
                            .w(LEAD_LANE)
                            .when_some(line.role, |this, role| this.child(RoleBadge { role })),
                    )
                    .child(
                        div()
                            .flex_shrink_0()
                            .w(LABEL_COLUMN)
                            .numeric()
                            .whitespace_nowrap()
                            .text_color(if line.role.is_some() {
                                theme.sidebar_foreground
                            } else {
                                theme.muted_foreground
                            })
                            .child(line.label.clone()),
                    )
                    .child(
                        div()
                            .flex_shrink_0()
                            .w(TIME_COLUMN)
                            .numeric()
                            .whitespace_nowrap()
                            .text_color(if muted {
                                theme.muted_foreground
                            } else {
                                theme.sidebar_foreground
                            })
                            .child(line.time.clone()),
                    )
                    .child(
                        div()
                            .flex_1()
                            .min_w_0()
                            .h(BAR_HEIGHT)
                            .rounded_full()
                            .when(line.bar.is_some(), |this| this.bg(theme.muted))
                            .when_some(line.bar, |this, share| {
                                this.child(
                                    div()
                                        .h_full()
                                        .rounded_full()
                                        .min_w(BAR_MIN)
                                        .w(relative(share))
                                        .bg(bar_color),
                                )
                            }),
                    )
                    .child(
                        div()
                            .flex_shrink_0()
                            .w(DELTA_COLUMN)
                            .text_right()
                            .numeric()
                            .whitespace_nowrap()
                            .map(|this| {
                                if line.best {
                                    this.text_color(theme.sidebar_foreground).child("Best")
                                } else {
                                    // Every other lap is slower by definition:
                                    // the gap is metadata, not a gain or loss.
                                    this.text_color(theme.muted_foreground)
                                        .children(line.delta.clone())
                                }
                            }),
                    ),
            )
    }

    fn render_disclosure(
        &self,
        group: &LapGroup,
        focused: bool,
        cx: &mut Context<Self>,
    ) -> ListItem {
        let theme = cx.theme();
        let session = group.session.clone();
        let cursor = LapsCursor::Disclosure(session.clone());
        let at_cursor = focused && self.cursor.as_ref() == Some(&cursor);
        let disclosed = self.is_disclosed(&session);
        let label: SharedString = if disclosed {
            "Hide out and in laps".into()
        } else {
            format!("Show out and in laps ({})", group.untimed()).into()
        };
        row(
            SharedString::from(format!("laps-disclose:{session}")),
            at_cursor,
            theme.ring,
        )
        .role(AccessRole::Button)
        .aria_expanded(disclosed)
        .aria_label(label.clone())
        .on_click(cx.listener(move |this, _, window, cx| {
            this.point_at(cursor.clone(), window, cx);
            this.toggle_disclosure(&session, cx);
        }))
        .child(
            h_flex()
                .w_full()
                .gap_2()
                .child(div().flex_shrink_0().w(LEAD_LANE))
                .child(
                    div()
                        .truncate()
                        .text_label()
                        .text_color(theme.muted_foreground)
                        .child(label),
                ),
        )
    }

    /// The two load commands with their keys, acting on the cursor's lap.
    fn render_footer(&self, window: &Window, cx: &mut Context<Self>) -> impl IntoElement + use<> {
        let enabled = self.cursor_lap().is_some();
        let button = |id: &'static str, label: &'static str, role: Role| {
            let action: &dyn gpui_kit::Action = match role {
                Role::Primary => &SetPrimary,
                Role::Reference => &SetReference,
            };
            Button::new(id)
                .ghost()
                .xsmall()
                .accessibility_label(label)
                .children(Kbd::binding_for_action(action, Some(LAPS_CONTEXT), window))
                .child(label)
                .disabled(!enabled)
                .on_click(cx.listener(move |this, _, window, cx| {
                    if let Some((session, lap)) = this.cursor_lap() {
                        select(session, lap, role, window, cx);
                    }
                }))
        };
        h_flex()
            .id("laps-footer")
            .test_support()
            .flex_shrink_0()
            .gap_1()
            .px_2()
            .py_1()
            .border_t_1()
            .border_color(cx.theme().sidebar_border)
            .child(button("laps-set-primary", "Set primary", Role::Primary))
            .child(button(
                "laps-set-reference",
                "Set reference",
                Role::Reference,
            ))
    }

    fn render_empty(&self, cx: &mut Context<Self>) -> impl IntoElement + use<> {
        v_flex()
            .id("laps-empty")
            .test_support()
            .aria_label(SharedString::from(format!(
                "No lap selected. {SELECT_A_LAP}"
            )))
            .size_full()
            .items_center()
            .justify_center()
            .gap_3()
            .p_4()
            .child(empty_state(
                IconName::Inbox,
                "No lap selected",
                SELECT_A_LAP,
            ))
            .child(
                Button::new("laps-browse-library")
                    .outline()
                    .small()
                    .label("Browse library")
                    .on_click(cx.listener(|_, _, window, cx| {
                        window.dispatch_action(Box::new(FocusPanel6), cx)
                    })),
            )
    }
}

/// Dispatch the one lap-selection action.
fn select(session: SharedString, lap: i32, role: Role, window: &mut Window, cx: &mut App) {
    window.dispatch_action(Box::new(SelectLap { session, lap, role }), cx);
}

/// A sidebar row: one fixed geometry for groups, laps and disclosures, with
/// the keyboard cursor drawn as a ring (the role fill is the selection).
fn row(id: SharedString, at_cursor: bool, ring: Hsla) -> ListItem {
    ListItem::new(id)
        .min_h_7()
        .py_0()
        .px_2()
        .rounded_md()
        .border_1()
        .border_color(if at_cursor { ring } else { ring.opacity(0.) })
}

/// The circled role letter of a lap holding a role.
#[derive(IntoElement)]
struct RoleBadge {
    role: LapRole,
}

impl RenderOnce for RoleBadge {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        div()
            .flex()
            .flex_shrink_0()
            .size(rems(1.125))
            .rounded_full()
            .items_center()
            .justify_center()
            .bg(self.role.color(theme))
            .text_color(theme.background)
            .text_caption()
            .font_medium()
            .child(self.role.marker())
    }
}

/// A group's lap-time trend: timed laps left to right, faster higher, the
/// best marked with a dot. Painted with the trace mesh into reused buffers.
#[derive(IntoElement)]
struct Trend {
    values: Arc<[f32]>,
    best: Option<usize>,
    buffers: Rc<RefCell<TrendBuffers>>,
    color: Hsla,
    mark: Hsla,
}

impl RenderOnce for Trend {
    fn render(self, _: &mut Window, _: &mut App) -> impl IntoElement {
        let Self {
            values,
            best,
            buffers,
            color,
            mark,
        } = self;
        div().flex_shrink_0().w(TREND_WIDTH).h(TREND_HEIGHT).child(
            canvas(
                |_, _, _| {},
                move |bounds: Bounds<Pixels>, _, window, _| {
                    let dot = px(4.);
                    let inset = f32::from(dot) / 2.0 + 0.5;
                    let width = f32::from(bounds.size.width) - 2.0 * inset;
                    let height = f32::from(bounds.size.height) - 2.0 * inset;
                    let last = (values.len().max(2) - 1) as f32;
                    let at = |ix: usize| {
                        (
                            inset + width * ix as f32 / last,
                            inset + height * (1.0 - values[ix].clamp(0.0, 1.0)),
                        )
                    };
                    let mut buffers = buffers.borrow_mut();
                    let TrendBuffers { points, path } = &mut *buffers;
                    points.clear();
                    points.extend((0..values.len()).map(|ix| {
                        let (x, y) = at(ix);
                        PathPoint::new(f64::from(x), f64::from(y))
                    }));
                    path.clear();
                    omatrack_trace::mesh::stroke(points, 1.0, path);
                    path.finish();
                    for chunk in path.translated(bounds.origin) {
                        window.paint_path(chunk, color);
                    }
                    if let Some(ix) = best {
                        let (x, y) = at(ix);
                        let center = bounds.origin + point(px(x), px(y));
                        window.paint_quad(
                            fill(Bounds::centered_at(center, size(dot, dot)), mark)
                                .corner_radii(dot / 2.0),
                        );
                    }
                },
            )
            .size_full(),
        )
    }
}

impl gpui_kit::component::dock::BasePanel for LapsPanel {
    fn panel_name(&self) -> &'static str {
        PanelKind::Laps.name()
    }

    fn closable(&self, _: &App) -> bool {
        false
    }

    fn zoomable(&self, _: &App) -> bool {
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
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for LapsPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let root = v_flex()
            .id("laps-panel")
            .test_support()
            .key_context(LAPS_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(Self::on_set_primary))
            .on_action(cx.listener(Self::on_set_reference))
            .on_action(cx.listener(|this, _: &SelectUp, _, cx| this.move_cursor(-1, cx)))
            .on_action(cx.listener(|this, _: &SelectDown, _, cx| this.move_cursor(1, cx)))
            .size_full()
            .bg(theme.sidebar)
            .text_color(theme.sidebar_foreground);
        if self.groups.is_empty() {
            return root.child(self.render_empty(cx));
        }
        let focused = self.focus_handle.is_focused(window);
        let groups = self.groups.clone();
        let mut rows: Vec<gpui_kit::AnyElement> = Vec::new();
        for (ix, group) in groups.iter().enumerate() {
            rows.push(self.render_group(ix, group, focused, cx).into_any_element());
            for line in self.visible_laps(group) {
                rows.push(self.render_lap(group, line, focused, cx).into_any_element());
            }
            if self.is_expanded(&group.session) && group.untimed() > 0 {
                rows.push(
                    self.render_disclosure(group, focused, cx)
                        .into_any_element(),
                );
            }
        }
        let laps: usize = groups.iter().map(LapGroup::timed).sum();
        root.child(
            div()
                .id("laps-list")
                .test_support()
                .role(AccessRole::List)
                .aria_label(SharedString::from(format!(
                    "{} recordings, {laps} timed laps",
                    groups.len()
                )))
                .flex_1()
                .min_h_0()
                .px_1()
                .py_2()
                .overflow_y_scroll()
                .track_scroll(&self.scroll)
                .children(rows),
        )
        .child(self.render_footer(window, cx))
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and its key bindings. Called once from
/// [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::Laps, cx);
    cx.bind_keys([
        KeyBinding::new("up", SelectUp, Some(LAPS_CONTEXT)),
        KeyBinding::new("down", SelectDown, Some(LAPS_CONTEXT)),
        KeyBinding::new("enter", SetPrimary, Some(LAPS_CONTEXT)),
        KeyBinding::new("alt-enter", SetReference, Some(LAPS_CONTEXT)),
    ]);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rows_announce_lap_time_gap_and_role() {
        let line = LapLine {
            id: "s/l:8".into(),
            lap: 8,
            label: "L8".into(),
            time: "1:13.644".into(),
            delta: None,
            best: true,
            timed: true,
            bar: Some(0.0),
            role: Some(LapRole::Reference),
        };
        assert_eq!(line.spoken(), "L8, 1:13.644, best lap, reference");
    }
}
