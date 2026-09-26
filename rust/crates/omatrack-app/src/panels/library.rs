//! Library: the session tree, Track > Date > Session > Laps.
//!
//! A sidebar-styled panel: search, track/year/driver facets and Rescan above
//! a keyboard-navigable tree. Enter or a double-click loads a lap as the
//! primary; Alt+Enter or the context menu loads it as the reference.

use std::collections::{HashMap, HashSet};
use std::rc::Rc;

use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName, IndexPath, Sizable as _, StyledExt as _,
    button::{Button, ButtonVariants as _},
    h_flex,
    input::{Input, InputEvent, InputState},
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
    App, AppContext as _, Context, Entity, EventEmitter, FocusHandle, Focusable,
    InteractiveElement as _, IntoElement, ParentElement as _, Render, SharedString,
    StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _, Window, div,
    rems,
};
use omatrack_core::format_lap_time;
use omatrack_library::{LibrarySnapshot, SessionNode};
use omatrack_ui::{DeltaSense, DeltaText, LapRole, Swatch};

use crate::actions::{
    OpenFolder, Rescan, RevealRecording, Role, SelectLap, SetPrimary, SetReference,
};
use crate::keymap::{LIBRARY_CONTEXT, WORKSPACE_CONTEXT};
use crate::panels::{PanelKind, empty_state};
use crate::state::{AppState, LibraryEvent, ScanStatus, SessionEvent};

/// One facet choice; `None` is "all".
#[derive(Debug, Clone, PartialEq)]
pub struct FacetOption {
    value: Option<String>,
    title: SharedString,
}

impl SearchableListItem for FacetOption {
    type Value = Option<String>;

    fn title(&self) -> SharedString {
        self.title.clone()
    }

    fn value(&self) -> &Self::Value {
        &self.value
    }
}

type FacetSelect = Entity<SelectState<Vec<FacetOption>>>;

/// What a tree row shows (presentation snapshot, rebuilt with the tree).
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
        title: SharedString,
        best: Option<SharedString>,
        laps: usize,
    },
    Lap {
        session: SharedString,
        lap: i32,
        label: SharedString,
        time: SharedString,
        delta_to_best: Option<f64>,
        best: bool,
        role: Option<LapRole>,
    },
}

impl Row {
    fn lap_target(&self) -> Option<(SharedString, i32)> {
        match self {
            Row::Lap { session, lap, .. } => Some((session.clone(), *lap)),
            _ => None,
        }
    }
}

/// Rows by tree item id, plus the session each id belongs to.
#[derive(Default)]
struct RowIndex {
    rows: HashMap<SharedString, Row>,
    session_of: HashMap<SharedString, SharedString>,
    best_lap: HashMap<SharedString, i32>,
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
    _subscriptions: Vec<Subscription>,
}

impl LibraryPanel {
    pub fn new(app: AppState, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let search = cx
            .new(|cx| InputState::new(window, cx).placeholder("Search tracks, drivers, sessions"));
        let facet = |label: &str, window: &mut Window, cx: &mut Context<Self>| {
            let options = vec![FacetOption {
                value: None,
                title: label.to_string().into(),
            }];
            cx.new(|cx| SelectState::new(options, Some(IndexPath::default()), window, cx))
        };
        let track_facet = facet("All tracks", window, cx);
        let year_facet = facet("All years", window, cx);
        let driver_facet = facet("All drivers", window, cx);
        let tree = cx.new(|cx| TreeState::new(cx));

        let subscriptions = vec![
            cx.subscribe_in(&search, window, |this, search, event, _, cx| {
                if let InputEvent::Change = event {
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
            cx.subscribe_in(&app.library, window, |this, _, event, window, cx| {
                if let LibraryEvent::SnapshotChanged = event {
                    this.sync_facets(window, cx);
                    this.rebuild(cx);
                }
            }),
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
    pub fn reveal(&mut self, id: &SharedString, cx: &mut Context<Self>) {
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

    fn sync_facets(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let library = self.app.library.read(cx);
        let facets = library.facets().clone();
        let current = (
            library.track_facet().map(str::to_string),
            library.year_facet().map(|year| year.to_string()),
            library.driver_facet().map(str::to_string),
        );
        let options = |all: &str, facets: &[omatrack_library::Facet]| {
            std::iter::once(FacetOption {
                value: None,
                title: all.to_string().into(),
            })
            .chain(facets.iter().map(|facet| FacetOption {
                value: Some(facet.value.clone()),
                title: format!("{} ({})", facet.label, facet.count).into(),
            }))
            .collect::<Vec<_>>()
        };
        for (select, items, selected) in [
            (
                self.track_facet.clone(),
                options("All tracks", &facets.tracks),
                current.0,
            ),
            (
                self.year_facet.clone(),
                options("All years", &facets.years),
                current.1,
            ),
            (
                self.driver_facet.clone(),
                options("All drivers", &facets.drivers),
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
    fn rebuild(&mut self, cx: &mut Context<Self>) {
        let snapshot = self.app.library.read(cx).filtered().clone();
        let session = self.app.session.read(cx);
        let primary = session.primary().map(|slot| slot.lap_ref().row_id());
        let reference = session.reference().map(|slot| slot.lap_ref().row_id());
        let (items, index) = self.build_items(&snapshot, primary.as_ref(), reference.as_ref());
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

    fn build_items(
        &self,
        snapshot: &LibrarySnapshot,
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
        let holds_role = |node: &SessionNode| {
            node.laps.iter().any(|lap| {
                let id = SharedString::from(lap.id.clone());
                Some(&id) == primary || Some(&id) == reference
            })
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
                    let laps = node
                        .laps
                        .iter()
                        .map(|lap| {
                            let id = SharedString::from(lap.id.clone());
                            index.rows.insert(
                                id.clone(),
                                Row::Lap {
                                    session: session_id.clone(),
                                    lap: lap.lap_id,
                                    label: lap.label.clone().into(),
                                    time: format_lap_time(lap.time_ms).into(),
                                    delta_to_best: lap
                                        .delta_to_best_ms
                                        .filter(|_| !lap.best)
                                        .map(|ms| ms / 1000.0),
                                    best: lap.best,
                                    role: role_of(&id),
                                },
                            );
                            index.session_of.insert(id.clone(), session_id.clone());
                            TreeItem::new(id, lap.label.clone())
                        })
                        .collect::<Vec<_>>();
                    if let Some(best) = node.best_lap_id {
                        index.best_lap.insert(session_id.clone(), best);
                    }
                    index.rows.insert(
                        session_id.clone(),
                        Row::Session {
                            title: node.title.clone().into(),
                            best: node
                                .best_time_ms
                                .map(|ms| SharedString::from(format_lap_time(ms))),
                            laps: node.lap_count,
                        },
                    );
                    index
                        .session_of
                        .insert(session_id.clone(), session_id.clone());
                    let open = self.is_open(&session_id, holds_role(node));
                    sessions.push(
                        TreeItem::new(session_id, node.title.clone())
                            .expanded(open)
                            .children(laps),
                    );
                }
                track_count += sessions.len();
                index.rows.insert(
                    day_id.clone(),
                    Row::Day {
                        heading: date.heading.clone().into(),
                        recordings: sessions.len(),
                    },
                );
                let open = self.is_open(&day_id, true);
                days.push(
                    TreeItem::new(day_id, date.heading.clone())
                        .expanded(open)
                        .children(sessions),
                );
            }
            index.rows.insert(
                track_id.clone(),
                Row::Track {
                    name: track.name.clone().into(),
                    recordings: track_count,
                },
            );
            let open = self.is_open(&track_id, true);
            items.push(
                TreeItem::new(track_id, track.name.clone())
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

    fn load_selected(&mut self, role: Role, window: &mut Window, cx: &mut Context<Self>) {
        let Some((id, row)) = self.selected_row(cx) else {
            return;
        };
        let target = row.lap_target().or_else(|| match row {
            // Enter on a recording loads its fastest lap.
            Row::Session { .. } => self.index.best_lap.get(&id).map(|lap| (id.clone(), *lap)),
            _ => None,
        });
        match target {
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

    fn on_set_primary(&mut self, _: &SetPrimary, window: &mut Window, cx: &mut Context<Self>) {
        self.load_selected(Role::Primary, window, cx);
    }

    fn on_set_reference(&mut self, _: &SetReference, window: &mut Window, cx: &mut Context<Self>) {
        self.load_selected(Role::Reference, window, cx);
    }

    fn render_toolbar(&self, cx: &mut Context<Self>) -> impl IntoElement {
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
                                    .update(cx, |library, cx| library.rescan(cx));
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
                                this.app
                                    .library
                                    .update(cx, |library, cx| library.prompt_add_folder(cx));
                            })),
                    ),
            )
            .child(
                h_flex()
                    .gap_1()
                    .child(
                        Select::new(&self.track_facet)
                            .id("facet-track")
                            .small()
                            .accessibility_label("Track")
                            .flex_1()
                            .min_w_0(),
                    )
                    .child(
                        Select::new(&self.year_facet)
                            .id("facet-year")
                            .small()
                            .accessibility_label("Year")
                            .flex_1()
                            .min_w_0(),
                    )
                    .child(
                        Select::new(&self.driver_facet)
                            .id("facet-driver")
                            .small()
                            .accessibility_label("Driver")
                            .flex_1()
                            .min_w_0(),
                    ),
            )
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

    fn render_empty(&self, cx: &mut Context<Self>) -> Option<gpui_kit::AnyElement> {
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
                    this.search
                        .update(cx, |search, cx| search.set_value("", window, cx));
                    this.app
                        .library
                        .update(cx, |library, cx| library.clear_filter(cx));
                    this.sync_facets(window, cx);
                })),
            "rescan" => Button::new("library-empty-rescan")
                .outline()
                .small()
                .label("Rescan")
                .on_click(cx.listener(|this, _, _, cx| {
                    this.app
                        .library
                        .update(cx, |library, cx| library.rescan(cx));
                })),
            _ => Button::new("library-empty-add-folder")
                .outline()
                .small()
                .label("Add folder…")
                .on_click(cx.listener(|this, _, _, cx| {
                    this.app
                        .library
                        .update(cx, |library, cx| library.prompt_add_folder(cx));
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
            let id = entry.item().id.clone();
            let Some(row) = menu_index.rows.get(&id) else {
                return menu;
            };
            let session = menu_index.session_of.get(&id).cloned();
            let target = row.lap_target().or_else(|| {
                let session = session.clone()?;
                let lap = *menu_index.best_lap.get(&session)?;
                Some((session, lap))
            });
            let prefix = if row.lap_target().is_some() {
                ""
            } else {
                "Best lap "
            };
            let mut menu = menu;
            if let Some((session, lap)) = target {
                menu = menu
                    .menu(
                        format!("Set {prefix}as primary").trim().to_string(),
                        Box::new(SelectLap {
                            session: session.clone(),
                            lap,
                            role: Role::Primary,
                        }),
                    )
                    .menu(
                        format!("Set {prefix}as reference").trim().to_string(),
                        Box::new(SelectLap {
                            session,
                            lap,
                            role: Role::Reference,
                        }),
                    );
            }
            if let Some(session) = session {
                menu = menu.separator().menu(
                    "Reveal in file manager",
                    Box::new(RevealRecording { session }),
                );
            }
            menu
        })
        .size_full()
        .text_sm()
    }
}

fn render_row(entry: &TreeEntry, index: &RowIndex, cx: &App) -> ListItem {
    let id = entry.item().id.clone();
    let theme = cx.theme();
    let indent = rems(0.25 + 0.75 * entry.depth() as f32);
    let chevron = |entry: &TreeEntry| {
        div()
            .flex_shrink_0()
            .size_4()
            .when(entry.is_folder(), |this| {
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
    let count = |count: usize| {
        div()
            .text_xs()
            .text_color(theme.muted_foreground)
            .child(count.to_string())
    };
    let item = ListItem::new(id.clone()).pl(indent).pr_2().py_0p5();
    let Some(row) = index.rows.get(&id) else {
        return item.child(entry.item().label.clone());
    };
    match row.clone() {
        Row::Track { name, recordings } => item.child(
            h_flex()
                .w_full()
                .gap_1()
                .child(chevron(entry))
                .child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .truncate()
                        .font_semibold()
                        .child(name),
                )
                .child(count(recordings)),
        ),
        Row::Day {
            heading,
            recordings,
        } => item.child(
            h_flex()
                .w_full()
                .gap_1()
                .child(chevron(entry))
                .child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .truncate()
                        .text_color(theme.muted_foreground)
                        .child(heading),
                )
                .child(count(recordings)),
        ),
        Row::Session { title, best, laps } => item.child(
            h_flex()
                .w_full()
                .gap_1()
                .child(chevron(entry))
                .child(div().flex_1().min_w_0().truncate().child(title))
                .child(
                    div()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child(format!("{laps} laps")),
                )
                .when_some(best, |this, best| {
                    this.child(
                        div()
                            .w(rems(4.5))
                            .text_right()
                            .font_family(theme.mono_font_family.clone())
                            .text_color(theme.muted_foreground)
                            .child(best),
                    )
                }),
        ),
        Row::Lap {
            session: session_id,
            lap,
            label,
            time,
            delta_to_best,
            best,
            role,
        } => {
            item.on_click(move |event, window, cx| {
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
                    // Role lane: the swatch and P/R letter of a loaded role.
                    .child(h_flex().flex_shrink_0().w(rems(1.75)).gap_0p5().when_some(
                        role,
                        |this, role| {
                            this.child(Swatch::new(role.color(theme)).xsmall()).child(
                                div()
                                    .text_xs()
                                    .font_family(theme.mono_font_family.clone())
                                    .text_color(theme.muted_foreground)
                                    .child(role.marker()),
                            )
                        },
                    ))
                    .child(div().w(rems(2.5)).child(label))
                    .child(
                        div()
                            .w(rems(4.5))
                            .text_right()
                            .font_family(theme.mono_font_family.clone())
                            .child(time),
                    )
                    .child(
                        h_flex()
                            .flex_1()
                            .justify_end()
                            .when(best, |this| {
                                this.child(Tag::secondary().xsmall().child("Best"))
                            })
                            .when_some(delta_to_best, |this, delta| {
                                this.child(
                                    DeltaText::new(Some(delta))
                                        .decimals(3)
                                        .sense(DeltaSense::LowerIsBetter),
                                )
                            }),
                    ),
            )
        }
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

    fn title(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
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
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let body = match self.render_empty(cx) {
            Some(empty) => empty,
            None => div()
                .flex_1()
                .min_h_0()
                .size_full()
                .child(self.render_tree())
                .into_any_element(),
        };
        v_flex()
            .id("library-panel")
            .test_support()
            .key_context(LIBRARY_CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(Self::on_set_primary))
            .on_action(cx.listener(Self::on_set_reference))
            .size_full()
            .bg(cx.theme().sidebar)
            .text_color(cx.theme().sidebar_foreground)
            .child(self.render_toolbar(cx))
            .children(self.render_scan_progress(cx))
            .child(div().flex_1().min_h_0().child(body))
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name) and, when it has them, its own actions and key
/// bindings. Called once from [`crate::panels::init`].
pub fn init(cx: &mut gpui_kit::App) {
    crate::panels::register(PanelKind::Library, cx);
}
