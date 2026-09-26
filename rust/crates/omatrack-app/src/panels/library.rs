//! Library: the session tree, Track > Date > Session > Laps.
//!
//! A sidebar-styled panel: search, track/year/driver facets and Rescan above
//! a keyboard-navigable tree. Enter or a double-click loads a lap as the
//! primary; Alt+Enter or the context menu loads it as the reference (the
//! footer shows both commands with their keys). The context menu also opens
//! a recording's metadata (Ctrl+I on the selected row) and, inside a library
//! folder, the folder's `TRACK.yml`.
//!
//! Rows share right-aligned columns: a lap's time and a recording's best sit
//! in one tabular-figure column, the gap to the best (or a count) in the one after it.
//! Cells never wrap: the tree is a uniform list, so every row keeps one
//! fixed height.

use omatrack_ui::TypeScale as _;
use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Icon, IconName, IndexPath, Sizable as _, StyledExt as _,
    button::{Button, ButtonVariants as _},
    h_flex,
    input::{Input, InputEvent, InputState},
    kbd::Kbd,
    list::ListItem,
    searchable_list::SearchableListItem,
    select::{Select, SelectEvent, SelectState},
    spinner::Spinner,
    tag::Tag,
    tree::{TreeEntry, TreeEvent, TreeItem, TreeState, tree},
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, AppContext as _, Context, Entity, EventEmitter, FocusHandle, Focusable,
    InteractiveElement as _, IntoElement, KeyBinding, ParentElement as _, Rems, Render, RenderOnce,
    SharedString, StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _,
    Window, div, rems,
};
use omatrack_core::format_lap_time;
use omatrack_library::track_yml::driver_id_key;
use omatrack_library::{Config, LibrarySnapshot, MetadataLayer, SessionNode};
use omatrack_ui::{DeltaSense, LapRole, Swatch, format_delta};

use crate::actions::{
    OpenFolder, Rescan, RevealRecording, Role, SelectLap, SetPrimary, SetReference,
};
use crate::dialogs::{self, EditFolderMetadata, EditRecordingMetadata};
use crate::keymap::{LIBRARY_CONTEXT, WORKSPACE_CONTEXT};
use crate::panels::{PanelKind, empty_state};
use crate::state::{AppState, LibraryEvent, ScanStatus, SessionEvent};

/// The time column: fits `11:49.212` in tabular figures at `text_sm`.
const TIME_COLUMN: Rems = Rems(4.75);
/// The trailing column: the gap to the best (`+12.345`), the Best tag, a
/// recording's lap count or a folder's recording count.
const TRAIL_COLUMN: Rems = Rems(3.5);
/// The lane before a row's label: a folder's chevron, a lap's role marker.
/// One width, so a lap's label lines up under its recording's title.
const LEAD_LANE: Rems = Rems(1.25);
/// Indent per tree level, and the inset of the top level.
const INDENT_STEP: f32 = 0.625;
const INDENT_BASE: f32 = 0.25;

/// One facet choice; `None` is "all".
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FacetOption {
    value: Option<String>,
    /// The menu row and the accessible value: `All tracks`, `Road Atlanta (3)`.
    title: SharedString,
    /// The trigger: the facet's name while unfiltered, else the value.
    short: SharedString,
    all: bool,
}

impl SearchableListItem for FacetOption {
    type Value = Option<String>;

    fn title(&self) -> SharedString {
        self.title.clone()
    }

    fn display_title(&self) -> Option<AnyElement> {
        Some(
            FacetTrigger {
                text: self.short.clone(),
                muted: self.all,
            }
            .into_any_element(),
        )
    }

    fn value(&self) -> &Self::Value {
        &self.value
    }
}

/// A facet's trigger text: muted like a placeholder while it filters
/// nothing, so an active filter reads at a glance.
#[derive(IntoElement)]
struct FacetTrigger {
    text: SharedString,
    muted: bool,
}

impl RenderOnce for FacetTrigger {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        div()
            .truncate()
            .when(self.muted, |this| {
                this.text_color(cx.theme().muted_foreground)
            })
            .child(self.text)
    }
}

type FacetSelect = Entity<SelectState<Vec<FacetOption>>>;

/// The three library facets.
#[derive(Debug, Clone, Copy)]
enum Facet {
    Track,
    Year,
    Driver,
}

impl Facet {
    fn name(self) -> &'static str {
        match self {
            Self::Track => "Track",
            Self::Year => "Year",
            Self::Driver => "Driver",
        }
    }

    fn all(self) -> &'static str {
        match self {
            Self::Track => "All tracks",
            Self::Year => "All years",
            Self::Driver => "All drivers",
        }
    }

    /// The unfiltered choice: `All tracks` in the menu, `Track` (muted) on
    /// the trigger, so three facets fit a narrow sidebar.
    fn all_option(self) -> FacetOption {
        FacetOption {
            value: None,
            title: self.all().into(),
            short: self.name().into(),
            all: true,
        }
    }
}

/// What a tree row shows (presentation snapshot, rebuilt with the tree;
/// every string is formatted here, never per frame).
#[derive(Debug, Clone)]
enum Row {
    Track {
        name: SharedString,
        recordings: usize,
    },
    Day {
        heading: SharedString,
        recordings: usize,
    },
    Session {
        /// The session name, else the driver, else the file name.
        title: SharedString,
        /// The driver beside a session name.
        driver: Option<Driver>,
        best: Option<SharedString>,
        laps: usize,
        /// Roles held by this recording's laps (marked while collapsed).
        roles: Vec<LapRole>,
    },
    Lap {
        session: SharedString,
        lap: i32,
        /// `L8`; `None` for an incomplete lap when the complete laps are
        /// numbered in sequence rather than by the recording (its number
        /// would then read as another lap's).
        number: Option<SharedString>,
        /// `Out`, `In`, `Pit`, `Frag` for an interval that is not a counted
        /// flying lap.
        kind: Option<SharedString>,
        time: SharedString,
        /// Counts for best: its time is a comparable lap time.
        counts: bool,
        /// The gap to the recording's best (`+0.911`).
        delta: Option<SharedString>,
        best: bool,
        role: Option<LapRole>,
    },
}

/// A driver as the library shows them.
#[derive(Debug, Clone, PartialEq)]
pub(crate) struct Driver {
    pub(crate) name: SharedString,
    /// Only the logger's driver id is known (no metadata layer names it):
    /// shown as `Driver 1` and set apart from real names.
    unnamed: bool,
}

impl Row {
    fn lap_target(&self) -> Option<(SharedString, i32)> {
        match self {
            Self::Lap { session, lap, .. } => Some((session.clone(), *lap)),
            _ => None,
        }
    }
}

/// A recording's driver: the resolved name, or for a bare logger id
/// `Driver 1` flagged unnamed (never a made-up name).
pub(crate) fn driver_of(node: &SessionNode) -> Option<Driver> {
    let sourced = node.metadata.driver.as_ref()?;
    match node.summary.driver_id() {
        Some(id) if sourced.layer == MetadataLayer::Recording => Some(Driver {
            name: format!("Driver {}", driver_id_key(id)).into(),
            unnamed: true,
        }),
        _ => Some(Driver {
            name: sourced.value.clone().into(),
            unnamed: false,
        }),
    }
}

/// The title and driver a recording row shows.
fn session_heading(node: &SessionNode) -> (SharedString, Option<Driver>) {
    let driver = driver_of(node);
    match (&node.session_name, driver) {
        (Some(session), driver) => (session.clone().into(), driver),
        (None, Some(driver)) if !driver.unnamed => (driver.name, None),
        // Only a driver id: the file names the recording.
        (None, Some(driver)) => (node.file_name().into(), Some(driver)),
        (None, None) => (node.title.clone().into(), None),
    }
}

fn plural(count: usize, one: &str, many: &str) -> String {
    format!("{count} {}", if count == 1 { one } else { many })
}

/// A lap row's accessible name: `L8, 1:13.644, best lap, reference`.
fn lap_spoken(
    number: Option<&str>,
    kind: Option<&str>,
    time: &str,
    delta: Option<&str>,
    best: bool,
    role: Option<LapRole>,
) -> String {
    let name = match (number, kind) {
        (Some(number), Some(kind)) => format!("{number} {kind}"),
        (Some(one), None) | (None, Some(one)) => one.to_string(),
        (None, None) => String::new(),
    };
    [
        Some(name),
        Some(time.to_string()),
        delta.map(|delta| format!("{delta} to best")),
        best.then(|| "best lap".to_string()),
        role.map(|role| role.label().to_lowercase()),
    ]
    .into_iter()
    .flatten()
    .collect::<Vec<_>>()
    .join(", ")
}

/// Rows by tree item id, plus the session each id belongs to.
#[derive(Default)]
struct RowIndex {
    rows: HashMap<SharedString, Row>,
    session_of: HashMap<SharedString, SharedString>,
    best_lap: HashMap<SharedString, i32>,
    /// Sessions whose folder lies in a library folder (`TRACK.yml` can be
    /// edited there).
    user_folders: HashSet<SharedString>,
}

pub struct LibraryPanel {
    app: AppState,
    focus_handle: FocusHandle,
    search: Entity<InputState>,
    track_facet: FacetSelect,
    year_facet: FacetSelect,
    driver_facet: FacetSelect,
    tree: Entity<TreeState>,
    index: Rc<RowIndex>,
    /// Folders the user opened or closed, against their default.
    opened: HashSet<SharedString>,
    closed: HashSet<SharedString>,
    /// Recordings the last scan could not read (the notification autohides;
    /// the panel keeps saying so until the next scan).
    unreadable: usize,
    _subscriptions: Vec<Subscription>,
}

impl LibraryPanel {
    pub fn new(app: AppState, window: &mut Window, cx: &mut Context<'_, Self>) -> Self {
        let search = cx.new(|cx| InputState::new(window, cx).placeholder("Search library"));
        let facet = |facet: Facet, window: &mut Window, cx: &mut Context<'_, Self>| {
            let options = vec![facet.all_option()];
            cx.new(|cx| SelectState::new(options, Some(IndexPath::default()), window, cx))
        };
        let track_facet = facet(Facet::Track, window, cx);
        let year_facet = facet(Facet::Year, window, cx);
        let driver_facet = facet(Facet::Driver, window, cx);
        let tree = cx.new(|cx| TreeState::new(cx));

        let subscriptions = vec![
            cx.subscribe_in(&search, window, |this, search, event, _, cx| {
                if matches!(event, InputEvent::Change) {
                    let query = search.read(cx).value().to_string();
                    this.app
                        .library
                        .update(cx, |library, cx| library.set_query(query, cx));
                }
            }),
            cx.subscribe(&track_facet, |this, _, event, cx| {
                let SelectEvent::Confirm(value) = event;
                let slug = value.clone().flatten();
                this.app
                    .library
                    .update(cx, |library, cx| library.set_track_facet(slug, cx));
            }),
            cx.subscribe(&year_facet, |this, _, event, cx| {
                let SelectEvent::Confirm(value) = event;
                let year = value.clone().flatten().and_then(|year| year.parse().ok());
                this.app
                    .library
                    .update(cx, |library, cx| library.set_year_facet(year, cx));
            }),
            cx.subscribe(&driver_facet, |this, _, event, cx| {
                let SelectEvent::Confirm(value) = event;
                let driver = value.clone().flatten();
                this.app
                    .library
                    .update(cx, |library, cx| library.set_driver_facet(driver, cx));
            }),
            cx.subscribe_in(
                &app.library,
                window,
                |this, _, event, window, cx| match event {
                    LibraryEvent::SnapshotChanged => {
                        this.sync_facets(window, cx);
                        this.rebuild(cx);
                    }
                    LibraryEvent::ScanFinished { unreadable, .. } => {
                        this.unreadable = *unreadable;
                        cx.notify();
                    }
                },
            ),
            cx.observe(&app.library, |_, _, cx| cx.notify()),
            cx.subscribe(&app.session, |this, _, event, cx| {
                if matches!(
                    event,
                    SessionEvent::PrimaryChanged
                        | SessionEvent::ReferenceChanged
                        | SessionEvent::Swapped
                ) {
                    this.rebuild(cx);
                }
            }),
            // The footer's commands follow the selected row.
            cx.observe(&tree, |_, _, cx| cx.notify()),
            cx.subscribe(&tree, |this, _, event, _| match event {
                TreeEvent::Expanded(id) => {
                    this.closed.remove(id);
                    this.opened.insert(id.clone());
                }
                TreeEvent::Collapsed(id) => {
                    this.opened.remove(id);
                    this.closed.insert(id.clone());
                }
            }),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle(),
            search,
            track_facet,
            year_facet,
            driver_facet,
            tree,
            index: Rc::default(),
            opened: HashSet::new(),
            closed: HashSet::new(),
            unreadable: 0,
            _subscriptions: subscriptions,
        };
        panel.sync_facets(window, cx);
        panel.rebuild(cx);
        panel
    }

    /// The search field's state.
    pub fn search(&self) -> &Entity<InputState> {
        &self.search
    }

    /// The tree's state.
    pub fn tree(&self) -> &Entity<TreeState> {
        &self.tree
    }

    /// Move keyboard focus to the tree.
    pub fn focus_tree(&self, window: &mut Window, cx: &mut App) {
        self.tree.update(cx, |tree, cx| tree.focus(window, cx));
    }

    /// Select and reveal a row by its catalog id (expanding its parents).
    pub fn reveal(&mut self, id: &SharedString, cx: &mut Context<'_, Self>) {
        self.tree.update(cx, |tree, cx| {
            tree.reveal_item(id, gpui_kit::ScrollStrategy::Center, cx);
            let ix = tree.index_of(id);
            tree.set_selected_index(ix, cx);
        });
    }

    fn is_open(&self, id: &SharedString, default: bool) -> bool {
        if self.closed.contains(id) {
            false
        } else if self.opened.contains(id) {
            true
        } else {
            default
        }
    }

    fn sync_facets(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        let library = self.app.library.read(cx);
        let facets = library.facets().clone();
        let current = (
            library.track_facet().map(str::to_string),
            library.year_facet().map(|year| year.to_string()),
            library.driver_facet().map(str::to_string),
        );
        // Drivers known only by their logger id read as the tree shows them.
        let driver_names: HashMap<String, SharedString> = library
            .snapshot()
            .sessions()
            .filter_map(|node| {
                let driver = driver_of(node)?;
                Some((node.driver.clone()?, driver.name))
            })
            .collect();
        let options = |facet: Facet, values: &[omatrack_library::Facet]| {
            std::iter::once(facet.all_option())
                .chain(values.iter().map(|value| {
                    let label = driver_names
                        .get(&value.value)
                        .filter(|_| matches!(facet, Facet::Driver))
                        .cloned()
                        .unwrap_or_else(|| value.label.clone().into());
                    FacetOption {
                        value: Some(value.value.clone()),
                        title: format!("{label} ({})", value.count).into(),
                        short: label,
                        all: false,
                    }
                }))
                .collect::<Vec<_>>()
        };
        for (select, items, selected) in [
            (
                self.track_facet.clone(),
                options(Facet::Track, &facets.tracks),
                current.0,
            ),
            (
                self.year_facet.clone(),
                options(Facet::Year, &facets.years),
                current.1,
            ),
            (
                self.driver_facet.clone(),
                options(Facet::Driver, &facets.drivers),
                current.2,
            ),
        ] {
            select.update(cx, |select, cx| {
                select.set_items(items, window, cx);
                select.set_selected_value(&selected, window, cx);
            });
        }
    }

    /// Rebuild the tree from the filtered snapshot, keeping expansion and
    /// the selected row by id.
    fn rebuild(&mut self, cx: &mut Context<'_, Self>) {
        let snapshot = self.app.library.read(cx).filtered().clone();
        let session = self.app.session.read(cx);
        let primary = session.primary().map(|slot| slot.lap_ref().row_id());
        let reference = session.reference().map(|slot| slot.lap_ref().row_id());
        let config = self.app.preferences.read(cx).config();
        let (items, index) =
            self.build_items(&snapshot, config, primary.as_ref(), reference.as_ref());
        self.index = Rc::new(index);
        self.tree.update(cx, |tree, cx| {
            let selected = tree.selected_item().map(|item| item.id.clone());
            tree.set_items(items, cx);
            if let Some(id) = selected {
                let ix = tree.index_of(&id);
                tree.set_selected_index(ix, cx);
            }
        });
        cx.notify();
    }

    #[expect(
        clippy::too_many_lines,
        reason = "Build the track/date/session tree and its row index in one traversal of the same snapshot."
    )]
    fn build_items(
        &self,
        snapshot: &LibrarySnapshot,
        config: &Config,
        primary: Option<&SharedString>,
        reference: Option<&SharedString>,
    ) -> (Vec<TreeItem>, RowIndex) {
        let mut index = RowIndex::default();
        let role_of = |id: &SharedString| {
            if Some(id) == primary {
                Some(LapRole::Primary)
            } else if Some(id) == reference {
                Some(LapRole::Reference)
            } else {
                None
            }
        };
        let mut items = Vec::new();
        for track in snapshot.tracks() {
            let track_id = SharedString::from(track.id.clone());
            let mut days = Vec::new();
            let mut track_count = 0;
            for date in &track.dates {
                let day_id = SharedString::from(date.id.clone());
                let mut sessions = Vec::new();
                for node in &date.sessions {
                    let session_id = SharedString::from(node.id.clone());
                    if dialogs::user_library_folder(config, node.file.path()).is_some() {
                        index.user_folders.insert(session_id.clone());
                    }
                    // Complete laps carry the recording's numbers when every
                    // label is `L{lap id}`; only then can an out or in lap
                    // share the scheme without reading as another lap.
                    let numbered = node
                        .laps
                        .iter()
                        .filter(|lap| lap.complete)
                        .all(|lap| lap.label == format!("L{}", lap.lap_id));
                    let mut roles = Vec::new();
                    let laps = node
                        .laps
                        .iter()
                        .map(|lap| {
                            let id = SharedString::from(lap.id.clone());
                            let role = role_of(&id);
                            roles.extend(role);
                            let (number, kind) = if lap.complete {
                                let kind = (!lap.representative).then_some("Pit");
                                (Some(lap.label.clone()), kind.map(str::to_string))
                            } else {
                                let number = numbered.then(|| format!("L{}", lap.lap_id));
                                (number, Some(lap.label.clone()))
                            };
                            let time = format_lap_time(lap.time_ms);
                            let delta = lap.delta_to_best_ms.filter(|_| !lap.best).map(|ms| {
                                format_delta(Some(ms / 1000.0), 3, DeltaSense::LowerIsBetter).0
                            });
                            let spoken = lap_spoken(
                                number.as_deref(),
                                kind.as_deref(),
                                &time,
                                delta.as_deref(),
                                lap.best,
                                role,
                            );
                            index.rows.insert(
                                id.clone(),
                                Row::Lap {
                                    session: session_id.clone(),
                                    lap: lap.lap_id,
                                    number: number.map(SharedString::from),
                                    kind: kind.map(SharedString::from),
                                    time: time.into(),
                                    counts: lap.representative,
                                    delta,
                                    best: lap.best,
                                    role,
                                },
                            );
                            index.session_of.insert(id.clone(), session_id.clone());
                            TreeItem::new(id, spoken)
                        })
                        .collect::<Vec<_>>();
                    if let Some(best) = node.best_lap_id {
                        index.best_lap.insert(session_id.clone(), best);
                    }
                    let (title, driver) = session_heading(node);
                    let best = node
                        .best_time_ms
                        .map(|ms| SharedString::from(format_lap_time(ms)));
                    let spoken = [
                        Some(title.to_string()),
                        driver.as_ref().map(|driver| driver.name.to_string()),
                        Some(plural(node.lap_count, "lap", "laps")),
                        best.as_ref().map(|best| format!("best {best}")),
                    ]
                    .into_iter()
                    .flatten()
                    .collect::<Vec<_>>()
                    .join(", ");
                    let open = self.is_open(&session_id, !roles.is_empty());
                    index.rows.insert(
                        session_id.clone(),
                        Row::Session {
                            title,
                            driver,
                            best,
                            laps: node.lap_count,
                            roles,
                        },
                    );
                    index
                        .session_of
                        .insert(session_id.clone(), session_id.clone());
                    sessions.push(
                        TreeItem::new(session_id, spoken)
                            .expanded(open)
                            .children(laps),
                    );
                }
                track_count += sessions.len();
                let spoken = format!(
                    "{}, {}",
                    date.heading,
                    plural(sessions.len(), "recording", "recordings")
                );
                index.rows.insert(
                    day_id.clone(),
                    Row::Day {
                        heading: date.heading.clone().into(),
                        recordings: sessions.len(),
                    },
                );
                let open = self.is_open(&day_id, true);
                days.push(
                    TreeItem::new(day_id, spoken)
                        .expanded(open)
                        .children(sessions),
                );
            }
            let spoken = format!(
                "{}, {}",
                track.name,
                plural(track_count, "recording", "recordings")
            );
            index.rows.insert(
                track_id.clone(),
                Row::Track {
                    name: track.name.clone().into(),
                    recordings: track_count,
                },
            );
            let open = self.is_open(&track_id, true);
            items.push(
                TreeItem::new(track_id, spoken)
                    .expanded(open)
                    .children(days),
            );
        }
        (items, index)
    }

    fn selected_row(&self, cx: &App) -> Option<(SharedString, Row)> {
        let tree = self.tree.read(cx);
        let id = tree.selected_item()?.id.clone();
        let row = self.index.rows.get(&id)?.clone();
        Some((id, row))
    }

    fn load_selected(&mut self, role: Role, window: &mut Window, cx: &mut Context<'_, Self>) {
        if self.selected_row(cx).is_none() {
            return;
        }
        // Enter on a recording loads its fastest lap.
        match self.selected_target(cx) {
            Some((session, lap)) => self.app.session.update(cx, |state, cx| match role {
                Role::Primary => state.set_primary(session, lap, cx),
                Role::Reference => state.set_reference(session, lap, cx),
            }),
            // A track or day: Enter opens or closes it.
            None if role == Role::Primary => window.dispatch_action(
                Box::new(gpui_kit::base::actions::Confirm { secondary: false }),
                cx,
            ),
            None => {}
        }
    }

    fn on_set_primary(&mut self, _: &SetPrimary, window: &mut Window, cx: &mut Context<'_, Self>) {
        self.load_selected(Role::Primary, window, cx);
    }

    fn on_set_reference(
        &mut self,
        _: &SetReference,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) {
        self.load_selected(Role::Reference, window, cx);
    }

    /// The recording a dialog should edit: the action's, else the one the
    /// selected row belongs to.
    fn target_session(&self, session: Option<&SharedString>, cx: &App) -> Option<SharedString> {
        session.cloned().or_else(|| {
            let (id, _) = self.selected_row(cx)?;
            self.index.session_of.get(&id).cloned()
        })
    }

    fn on_edit_recording_metadata(
        &mut self,
        action: &EditRecordingMetadata,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) {
        let Some(session) = self.target_session(action.session.as_ref(), cx) else {
            return;
        };
        // The dialog returns focus to the row it was opened for (the
        // context menu that dispatched this is already gone).
        self.focus_tree(window, cx);
        dialogs::recording_metadata::open(&self.app, &session, window, cx);
    }

    fn on_edit_folder_metadata(
        &mut self,
        action: &EditFolderMetadata,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) {
        let folder = {
            let library = self.app.library.read(cx);
            let Some(node) = library.snapshot().session(&action.session) else {
                return;
            };
            dialogs::user_library_folder(self.app.preferences.read(cx).config(), node.file.path())
        };
        let Some(folder) = folder else {
            return;
        };
        self.focus_tree(window, cx);
        dialogs::track_yml::open(&self.app, folder, window, cx);
    }

    fn render_toolbar(&self, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let scanning = self.app.library.read(cx).is_scanning();
        v_flex()
            .gap_2()
            .p_2()
            .border_b_1()
            .border_color(cx.theme().sidebar_border)
            .child(
                h_flex()
                    .gap_1()
                    .child(
                        Input::new(&self.search)
                            .id("library-search")
                            .small()
                            .cleanable(true)
                            .prefix(Icon::new(IconName::Search).small())
                            .flex_1(),
                    )
                    .child(
                        Button::new("library-rescan")
                            .ghost()
                            .small()
                            .icon(IconName::RotateCw)
                            .loading(scanning)
                            .accessibility_label("Rescan library")
                            .tooltip_with_action("Rescan library", &Rescan, Some(WORKSPACE_CONTEXT))
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.app
                                    .library
                                    .update(cx, super::super::state::library::Library::rescan);
                            })),
                    )
                    .child(
                        Button::new("library-add-folder")
                            .ghost()
                            .small()
                            .icon(IconName::Plus)
                            .accessibility_label("Add folder…")
                            .tooltip_with_action(
                                "Add folder…",
                                &OpenFolder,
                                Some(WORKSPACE_CONTEXT),
                            )
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.app.library.update(
                                    cx,
                                    super::super::state::library::Library::prompt_add_folder,
                                );
                            })),
                    ),
            )
            .child(
                // The Select's own root is `size_full`; each sits in a
                // shrinkable third so the row never overflows the sidebar.
                h_flex().gap_1().children(
                    [
                        (&self.track_facet, "facet-track", Facet::Track),
                        (&self.year_facet, "facet-year", Facet::Year),
                        (&self.driver_facet, "facet-driver", Facet::Driver),
                    ]
                    .map(|(state, id, facet)| {
                        div().flex_1().min_w_0().child(
                            Select::new(state)
                                .id(id)
                                .small()
                                .accessibility_label(facet.name())
                                .menu_width(rems(16.)),
                        )
                    }),
                ),
            )
    }

    /// One muted status line under the toolbar: what the filter hides, what
    /// the last scan could not read.
    fn render_status(&self, cx: &mut Context<'_, Self>) -> Option<impl IntoElement + use<>> {
        let library = self.app.library.read(cx);
        let filtered = library.has_filter() && !library.filtered().is_empty();
        if !filtered && self.unreadable == 0 {
            return None;
        }
        let shown = library.filtered().recording_count();
        let total = library.snapshot().recording_count();
        let theme = cx.theme();
        let filter_text = SharedString::from(format!(
            "{shown} of {}",
            plural(total, "recording", "recordings")
        ));
        let unreadable = SharedString::from(format!(
            "{} couldn’t be read",
            plural(self.unreadable, "recording", "recordings")
        ));
        Some(
            v_flex()
                .px_3()
                .py_1()
                .gap_1()
                .text_xs()
                .text_color(theme.muted_foreground)
                .border_b_1()
                .border_color(theme.sidebar_border)
                .when(filtered, |this| {
                    this.child(
                        h_flex()
                            .id("library-filter-status")
                            .test_support()
                            .aria_label(filter_text.clone())
                            .gap_2()
                            .justify_between()
                            .child(filter_text)
                            .child(
                                Button::new("library-status-clear")
                                    .ghost()
                                    .xsmall()
                                    .label("Clear filters")
                                    .on_click(cx.listener(|this, _, window, cx| {
                                        this.clear_filters(window, cx);
                                    })),
                            ),
                    )
                })
                .when(self.unreadable > 0, |this| {
                    this.child(
                        h_flex()
                            .id("library-unreadable")
                            .test_support()
                            .aria_label(unreadable.clone())
                            .gap_1p5()
                            .child(
                                Icon::new(IconName::TriangleAlert)
                                    .xsmall()
                                    .text_color(theme.warning),
                            )
                            .child(unreadable),
                    )
                }),
        )
    }

    fn clear_filters(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) {
        self.search
            .update(cx, |search, cx| search.set_value("", window, cx));
        self.app
            .library
            .update(cx, super::super::state::library::Library::clear_filter);
        self.sync_facets(window, cx);
    }

    /// The lap (or a recording's best lap) Enter would load.
    fn selected_target(&self, cx: &App) -> Option<(SharedString, i32)> {
        let (id, row) = self.selected_row(cx)?;
        row.lap_target().or_else(|| match row {
            Row::Session { .. } => self.index.best_lap.get(&id).map(|lap| (id.clone(), *lap)),
            _ => None,
        })
    }

    /// The two load commands with their keys, acting on the selected row:
    /// the panel's main task stays visible and its keyboard path learnable.
    fn render_footer(
        &self,
        window: &Window,
        cx: &mut Context<'_, Self>,
    ) -> impl IntoElement + use<> {
        let enabled = self.selected_target(cx).is_some();
        let primary = LapRole::Primary.color(cx.theme());
        let reference = LapRole::Reference.color(cx.theme());
        let button = |id: &'static str, label: &'static str, role: Role| {
            let (action, color): (&dyn gpui_kit::Action, _) = match role {
                Role::Primary => (&SetPrimary, primary),
                Role::Reference => (&SetReference, reference),
            };
            Button::new(id)
                .ghost()
                .xsmall()
                .accessibility_label(label)
                .child(Swatch::new(color).xsmall())
                .child(label)
                .children(Kbd::binding_for_action(
                    action,
                    Some(LIBRARY_CONTEXT),
                    window,
                ))
                .disabled(!enabled)
                .on_click(cx.listener(move |this, _, window, cx| {
                    this.load_selected(role, window, cx);
                }))
        };
        h_flex()
            .id("library-footer")
            .test_support()
            .flex_shrink_0()
            .gap_1()
            .px_2()
            .py_1()
            .border_t_1()
            .border_color(cx.theme().sidebar_border)
            .child(button("library-set-primary", "Set primary", Role::Primary))
            .child(button(
                "library-set-reference",
                "Set reference",
                Role::Reference,
            ))
    }

    fn render_scan_progress(&self, cx: &App) -> Option<impl IntoElement + use<>> {
        let ScanStatus::Scanning {
            discovered,
            summarized,
        } = self.app.library.read(cx).status().clone()
        else {
            return None;
        };
        let text = if discovered == 0 {
            SharedString::from("Looking for recordings…")
        } else {
            format!("Reading {summarized} of {discovered} recordings…").into()
        };
        Some(
            h_flex()
                .id("library-progress")
                .test_support()
                .aria_label(text.clone())
                .gap_2()
                .px_3()
                .py_1()
                .text_xs()
                .text_color(cx.theme().muted_foreground)
                .child(Spinner::new().xsmall())
                .child(text),
        )
    }

    fn render_empty(&self, cx: &mut Context<'_, Self>) -> Option<AnyElement> {
        let library = self.app.library.read(cx);
        let (icon, title, description, action): (IconName, &str, SharedString, Option<&str>) =
            if !library.snapshot().is_empty() {
                if library.filtered().is_empty() {
                    (
                        IconName::Search,
                        "No recordings match",
                        "Change the search text or the filters.".into(),
                        Some("clear"),
                    )
                } else {
                    return None;
                }
            } else if library.is_scanning() {
                (
                    IconName::LoaderCircle,
                    "Scanning the library",
                    "Recordings appear here as soon as the scan finishes.".into(),
                    None,
                )
            } else if !library.has_locations() {
                (
                    IconName::FolderOpen,
                    "No library folders",
                    "Add a folder of AiM, MoTeC, Cosworth or RaceLogic recordings.".into(),
                    Some("add"),
                )
            } else if library.has_scanned() {
                (
                    IconName::Inbox,
                    "No recordings found",
                    "The library folders contain no supported recordings.".into(),
                    Some("add"),
                )
            } else {
                (
                    IconName::FolderOpen,
                    "Library not scanned yet",
                    "Rescan to read the library folders.".into(),
                    Some("rescan"),
                )
            };
        let button = action.map(|action| match action {
            "clear" => Button::new("library-clear-filters")
                .outline()
                .small()
                .label("Clear filters")
                .on_click(cx.listener(|this, _, window, cx| {
                    this.clear_filters(window, cx);
                })),
            "rescan" => Button::new("library-empty-rescan")
                .outline()
                .small()
                .label("Rescan")
                .on_click(cx.listener(|this, _, _, cx| {
                    this.app
                        .library
                        .update(cx, super::super::state::library::Library::rescan);
                })),
            _ => Button::new("library-empty-add-folder")
                .outline()
                .small()
                .label("Add folder…")
                .on_click(cx.listener(|this, _, _, cx| {
                    this.app
                        .library
                        .update(cx, super::super::state::library::Library::prompt_add_folder);
                })),
        });
        let label = SharedString::from(format!("{title}. {description}"));
        Some(
            v_flex()
                .id("library-empty")
                .test_support()
                .aria_label(label)
                .size_full()
                .items_center()
                .justify_center()
                .gap_3()
                .p_4()
                .child(empty_state(icon, title, description))
                .children(button)
                .into_any_element(),
        )
    }

    fn render_tree(&self) -> impl IntoElement {
        let index = self.index.clone();
        let menu_index = self.index.clone();
        tree(&self.tree, move |_, entry, _selected, _, cx| {
            render_row(entry, &index, cx)
        })
        .context_menu(move |_, entry, menu, _, _| {
            context_menu_entries(&menu_index, &entry.item().id)
                .into_iter()
                .fold(menu, |menu, entry| match entry {
                    MenuEntry::Item(label, action) => menu.menu(label, action),
                    MenuEntry::Separator => menu.separator(),
                })
        })
        .size_full()
        .text_sm()
    }
}

/// One entry of a row's context menu.
enum MenuEntry {
    Item(SharedString, Box<dyn gpui_kit::Action>),
    Separator,
}

/// The context menu of the row `id`: load the lap (a recording's best
/// lap) into either role, the recording's metadata, its folder's
/// `TRACK.yml` when the folder is a library folder, and Reveal.
fn context_menu_entries(index: &RowIndex, id: &SharedString) -> Vec<MenuEntry> {
    let Some(row) = index.rows.get(id) else {
        return Vec::new();
    };
    let session = index.session_of.get(id).cloned();
    let target = row.lap_target().or_else(|| {
        let session = session.clone()?;
        let lap = *index.best_lap.get(&session)?;
        Some((session, lap))
    });
    let prefix = if row.lap_target().is_some() {
        ""
    } else {
        "Best lap "
    };
    let mut entries = Vec::new();
    if let Some((session, lap)) = target {
        entries.push(MenuEntry::Item(
            format!("Set {prefix}as primary").trim().to_string().into(),
            Box::new(SelectLap {
                session: session.clone(),
                lap,
                role: Role::Primary,
            }),
        ));
        entries.push(MenuEntry::Item(
            format!("Set {prefix}as reference")
                .trim()
                .to_string()
                .into(),
            Box::new(SelectLap {
                session,
                lap,
                role: Role::Reference,
            }),
        ));
    }
    if let Some(session) = session {
        entries.push(MenuEntry::Separator);
        entries.push(MenuEntry::Item(
            "Recording metadata…".into(),
            Box::new(EditRecordingMetadata {
                session: Some(session.clone()),
            }),
        ));
        if index.user_folders.contains(&session) {
            entries.push(MenuEntry::Item(
                "Edit TRACK.yml…".into(),
                Box::new(EditFolderMetadata {
                    session: session.clone(),
                }),
            ));
        }
        entries.push(MenuEntry::Separator);
        entries.push(MenuEntry::Item(
            "Reveal in file manager".into(),
            Box::new(RevealRecording { session }),
        ));
    }
    entries
}

#[expect(
    clippy::cast_precision_loss,
    reason = "UI geometry deliberately projects bounded counts and f64 telemetry coordinates into f32 pixels."
)]
#[expect(
    clippy::too_many_lines,
    reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
)]
fn render_row(entry: &TreeEntry, index: &RowIndex, cx: &App) -> ListItem {
    let id = entry.item().id.clone();
    let theme = cx.theme();
    // Laps sit one level in from their recording's chevron, so their role
    // lane lines up under it and their label under the recording's title.
    let depth = match index.rows.get(&id) {
        Some(Row::Lap { .. }) => entry.depth().saturating_sub(1),
        _ => entry.depth(),
    };
    let indent = rems(INDENT_BASE + INDENT_STEP * depth as f32);
    // One fixed height: the tree is a uniform list.
    let item = ListItem::new(id.clone())
        .h_7()
        .py_0()
        .pl(indent)
        .pr_2()
        .text_sm();
    let Some(row) = index.rows.get(&id) else {
        return item.child(entry.item().label.clone());
    };
    let lead = || {
        h_flex()
            .flex_shrink_0()
            .w(LEAD_LANE)
            .h_full()
            .items_center()
    };
    let chevron = |entry: &TreeEntry| {
        lead().when(entry.is_folder(), |this| {
            this.child(
                Icon::new(if entry.is_expanded() {
                    IconName::ChevronDown
                } else {
                    IconName::ChevronRight
                })
                .xsmall()
                .text_color(theme.muted_foreground),
            )
        })
    };
    let label = || h_flex().flex_1().min_w_0().gap_1p5().overflow_hidden();
    // The time column: tabular figures, right-aligned, never wrapping or shrinking.
    let time_cell = |time: Option<SharedString>, muted: bool| {
        div()
            .flex_shrink_0()
            .min_w(TIME_COLUMN)
            .whitespace_nowrap()
            .text_right()
            .numeric()
            .when(muted, |this| this.text_color(theme.muted_foreground))
            .children(time)
    };
    let trail_cell = || {
        h_flex()
            .flex_shrink_0()
            .w(TRAIL_COLUMN)
            .justify_end()
            .whitespace_nowrap()
            .text_xs()
            .text_color(theme.muted_foreground)
    };
    let row = row.clone();
    match row {
        Row::Track { name, recordings } => item.child(
            h_flex()
                .w_full()
                .child(chevron(entry))
                .child(label().child(div().truncate().font_semibold().child(name)))
                .child(trail_cell().child(recordings.to_string())),
        ),
        Row::Day {
            heading,
            recordings,
        } => item.child(
            h_flex()
                .w_full()
                .child(chevron(entry))
                .child(
                    label().child(
                        div()
                            .truncate()
                            .text_color(theme.muted_foreground)
                            .child(heading),
                    ),
                )
                .child(trail_cell().child(recordings.to_string())),
        ),
        Row::Session {
            title,
            driver,
            best,
            laps,
            roles,
        } => {
            // A collapsed recording still says which roles it holds.
            let roles = if entry.is_expanded() {
                Vec::new()
            } else {
                roles
            };
            item.child(
                h_flex()
                    .w_full()
                    .gap_1()
                    .child(
                        h_flex().flex_1().min_w_0().child(chevron(entry)).child(
                            label()
                                .child(div().flex_shrink_0().child(title))
                                .when_some(driver, |this, driver| {
                                    this.child(
                                        div()
                                            .min_w_0()
                                            .truncate()
                                            .text_color(theme.muted_foreground)
                                            .when(driver.unnamed, gpui_kit::Styled::italic)
                                            .child(driver.name),
                                    )
                                })
                                .children(
                                    roles
                                        .into_iter()
                                        .map(|role| Swatch::new(role.color(theme)).xsmall()),
                                ),
                        ),
                    )
                    .child(match best {
                        Some(best) => time_cell(Some(best), false),
                        // Nothing counted for best: say so, not a blank.
                        None => time_cell(Some(omatrack_ui::MISSING_VALUE.into()), true),
                    })
                    .child(trail_cell().child(plural(laps, "lap", "laps"))),
            )
        }
        Row::Lap {
            session: session_id,
            lap,
            number,
            kind,
            time,
            counts,
            delta,
            best,
            role,
        } => item
            .on_click(move |event, window, cx| {
                if event.click_count() == 2 {
                    window.dispatch_action(
                        Box::new(SelectLap {
                            session: session_id.clone(),
                            lap,
                            role: Role::Primary,
                        }),
                        cx,
                    );
                }
            })
            .child(
                h_flex()
                    .w_full()
                    .gap_1()
                    .child(
                        h_flex()
                            .flex_1()
                            .min_w_0()
                            // Role lane: the swatch and P/R letter of a
                            // loaded role, under the recording's chevron.
                            .child(lead().gap_0p5().when_some(role, |this, role| {
                                this.child(Swatch::new(role.color(theme)).xsmall()).child(
                                    div()
                                        .text_xs()
                                        .numeric()
                                        .text_color(role.color(theme))
                                        .child(role.marker()),
                                )
                            }))
                            .child(
                                label()
                                    .when_some(number, |this, number| {
                                        this.child(
                                            div()
                                                .flex_shrink_0()
                                                .when(
                                                    role.is_some(),
                                                    gpui_kit::base::StyledExt::font_semibold,
                                                )
                                                .child(number),
                                        )
                                    })
                                    .when_some(kind, |this, kind| {
                                        this.child(
                                            div()
                                                .flex_shrink_0()
                                                .text_xs()
                                                .text_color(theme.muted_foreground)
                                                .child(kind),
                                        )
                                    }),
                            ),
                    )
                    // A fragment's or pit lap's duration is not a lap time.
                    .child(time_cell(Some(time), !counts))
                    .child(
                        trail_cell()
                            .when(best, |this| {
                                this.child(Tag::secondary().xsmall().child("Best"))
                            })
                            // Every other lap is slower than the best by
                            // definition: the gap is metadata, not a
                            // gain/loss signal, so it stays muted.
                            .when_some(delta, |this, delta| {
                                this.child(div().numeric().child(delta))
                            }),
                    ),
            ),
    }
}

impl gpui_kit::component::dock::BasePanel for LibraryPanel {
    fn panel_name(&self) -> &'static str {
        PanelKind::Library.name()
    }

    fn closable(&self, _: &App) -> bool {
        false
    }

    fn zoomable(&self, _: &App) -> bool {
        false
    }
}

impl gpui_kit::component::dock::Panel for LibraryPanel {
    fn tab_name(&self, _: &App) -> Option<SharedString> {
        Some(PanelKind::Library.title().into())
    }

    fn title(&mut self, _: &mut Window, _: &mut Context<'_, Self>) -> impl IntoElement {
        PanelKind::Library.title()
    }
}

impl EventEmitter<gpui_kit::component::dock::PanelEvent> for LibraryPanel {}

impl Focusable for LibraryPanel {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for LibraryPanel {
    fn render(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let (body, footer) = match self.render_empty(cx) {
            Some(empty) => (empty, None),
            None => (
                div()
                    .flex_1()
                    .min_h_0()
                    .size_full()
                    .child(self.render_tree())
                    .into_any_element(),
                Some(self.render_footer(window, cx)),
            ),
        };
        v_flex()
            .id("library-panel")
            .test_support()
            .key_context(LIBRARY_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(Self::on_set_primary))
            .on_action(cx.listener(Self::on_set_reference))
            .on_action(cx.listener(Self::on_edit_recording_metadata))
            .on_action(cx.listener(Self::on_edit_folder_metadata))
            .size_full()
            .bg(cx.theme().sidebar)
            .text_color(cx.theme().sidebar_foreground)
            .child(self.render_toolbar(cx))
            .children(self.render_scan_progress(cx))
            .children(self.render_status(cx))
            .child(div().flex_1().min_h_0().child(body))
            .children(footer)
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and, when it has them, its own actions and key
/// bindings. Called once from [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::Library, cx);
    dialogs::init(cx);
    // Get Info on the selected recording.
    cx.bind_keys([KeyBinding::new(
        "ctrl-i",
        EditRecordingMetadata { session: None },
        Some(LIBRARY_CONTEXT),
    )]);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn labels(index: &RowIndex, id: &str) -> Vec<String> {
        context_menu_entries(index, &SharedString::from(id.to_string()))
            .into_iter()
            .map(|entry| match entry {
                MenuEntry::Item(label, _) => label.to_string(),
                MenuEntry::Separator => "---".to_string(),
            })
            .collect()
    }

    fn index(user_folder: bool) -> RowIndex {
        let session = SharedString::from("s1");
        let lap = SharedString::from("s1/l2");
        let mut index = RowIndex::default();
        index.rows.insert(
            session.clone(),
            Row::Session {
                title: "Q1".into(),
                driver: None,
                best: None,
                laps: 1,
                roles: Vec::new(),
            },
        );
        index.rows.insert(
            lap.clone(),
            Row::Lap {
                session: session.clone(),
                lap: 2,
                number: Some("L2".into()),
                kind: None,
                time: "1:16.500".into(),
                counts: true,
                delta: None,
                best: true,
                role: None,
            },
        );
        index.rows.insert(
            "track".into(),
            Row::Track {
                name: "Test".into(),
                recordings: 1,
            },
        );
        index.session_of.insert(session.clone(), session.clone());
        index.session_of.insert(lap, session.clone());
        index.best_lap.insert(session.clone(), 2);
        if user_folder {
            index.user_folders.insert(session);
        }
        index
    }

    #[test]
    fn recordings_offer_metadata_and_library_folders_offer_track_yml() {
        let expected = [
            "Set Best lap as primary",
            "Set Best lap as reference",
            "---",
            "Recording metadata…",
            "Edit TRACK.yml…",
            "---",
            "Reveal in file manager",
        ];
        assert_eq!(labels(&index(true), "s1"), expected);
        assert_eq!(
            labels(&index(true), "s1/l2")[..2],
            ["Set as primary", "Set as reference"]
        );
        // Outside the library folders there is no TRACK.yml entry.
        let outside = labels(&index(false), "s1");
        assert!(!outside.iter().any(|label| label.contains("TRACK.yml")));
        assert!(outside.iter().any(|label| label == "Recording metadata…"));
        // A track row has no recording to act on.
        assert!(labels(&index(true), "track").is_empty());
    }
}
