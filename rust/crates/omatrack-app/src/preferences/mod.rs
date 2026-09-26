//! Preferences: a full-window screen over `omatrack.yml`.
//!
//! | Section | Edits |
//! | --- | --- |
//! | Library | folder locations (add, remove, rescan) |
//! | Traces | fit lanes, x axis |
//! | Video | mute, continuous playback, reference sync and playback, HUD position |
//! | Drivers | `driver_mappings` |
//! | Tracks | per-track corner overrides; Track Atlas revision and attribution |
//! | Appearance | the theme and fonts in use (read-only: Omarchy and the desktop lead) |
//!
//! The workspace shows the screen in place of its dock area and status bar
//! ([`crate::Workspace::open_preferences`]); the dock area stays alive
//! behind it, so the layout, loaded laps and cursor are untouched. The
//! screen is a section list (the kit's `Sidebar`, one Tab stop, Up and Down
//! move between sections) and a centred column of grouped settings.
//! Escape or Done returns to the workspace and restores focus.
//!
//! Every edit goes through the entity that owns it, which writes
//! `omatrack.yml` through [`crate::state::Preferences::update`] (debounced,
//! atomic, off the UI thread): mute and continuous playback through the
//! video controller, the sync strategy through the session, folders
//! through the library.
//!
//! The kit's `setting::Settings` was evaluated and not used: its page
//! selection is private state that cannot be driven by an action or read
//! back, its section items are not keyboard focusable, its field ids are
//! fixed, and it has no readable-width column.

mod drivers;
mod icons;
mod layout;
mod pages;

use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName, IndexPath, Sizable as _, StyledExt as _, Theme, TitleBar,
    button::{Button, ButtonVariants as _},
    h_flex,
    kbd::Kbd,
    scroll::ScrollableElement as _,
    searchable_list::SearchableListItem,
    select::{SelectEvent, SelectState},
    sidebar::{Sidebar, SidebarMenu, SidebarMenuItem},
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, AppContext as _, ClickEvent, Context, Entity, FocusHandle,
    InteractiveElement as _, IntoElement, ParentElement as _, Render, SharedString,
    StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _, Window, div,
    rems,
};
use omatrack_core::alignment::Strategy;
use omatrack_core::playback::ReferencePlayback;
use omatrack_library::config::XAxis;
use omatrack_ui::theme::{ThemeFonts, ThemeStatus};

pub use drivers::DriverMappingsEditor;

use crate::actions::{ClosePreferences, NextPreferencesSection, PrevPreferencesSection};
use crate::keymap::{PREFERENCES_CONTEXT, PREFERENCES_NAV_CONTEXT};
use crate::state::{AppState, PreferencesEvent};
use crate::workspace::SyncOption;
use omatrack_ui::TypeScale as _;

/// The readable width of the settings column.
const CONTENT_MAX_WIDTH: f32 = 46.;
/// The width of the section list.
const NAV_WIDTH: f32 = 15.;

/// One section of the Preferences screen.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum PreferencesSection {
    #[default]
    Library,
    Traces,
    Video,
    Drivers,
    Tracks,
    Appearance,
}

impl PreferencesSection {
    pub const ALL: [Self; 6] = [
        Self::Library,
        Self::Traces,
        Self::Video,
        Self::Drivers,
        Self::Tracks,
        Self::Appearance,
    ];

    pub fn title(self) -> &'static str {
        match self {
            Self::Library => "Library",
            Self::Traces => "Traces",
            Self::Video => "Video",
            Self::Drivers => "Drivers",
            Self::Tracks => "Tracks",
            Self::Appearance => "Appearance",
        }
    }

    /// The line under the page title.
    pub fn description(self) -> &'static str {
        match self {
            Self::Library => {
                "Folders Omatrack reads recordings and onboard video from, with their subfolders."
            }
            Self::Traces => "How the trace lanes are laid out and what the x axis measures.",
            Self::Video => "Onboard playback, and how the reference video follows the primary.",
            Self::Drivers => "Names for the driver ids that loggers record.",
            Self::Tracks => "Your corner edits and the Track Atlas data beneath them.",
            Self::Appearance => "Omatrack follows the Omarchy theme and the desktop fonts.",
        }
    }

    /// The element id of the section's page.
    pub fn page_id(self) -> &'static str {
        match self {
            Self::Library => "preferences-library",
            Self::Traces => "preferences-traces",
            Self::Video => "preferences-video",
            Self::Drivers => "preferences-drivers",
            Self::Tracks => "preferences-tracks",
            Self::Appearance => "preferences-appearance",
        }
    }

    /// The element id of the section's entry in the section list.
    pub fn nav_id(self) -> SharedString {
        // The kit's sidebar names items `{menu}-{item}`; one menu.
        format!("0-{}", self.ix()).into()
    }

    fn icon(self) -> Icon {
        match self {
            Self::Library => IconName::FolderOpen.into(),
            Self::Traces => icons::icon(gpui_kit::assets::IconName::ChartSpline),
            Self::Video => icons::icon(gpui_kit::assets::IconName::Video),
            Self::Drivers => icons::icon(gpui_kit::assets::IconName::Users),
            Self::Tracks => icons::icon(gpui_kit::assets::IconName::Route),
            Self::Appearance => IconName::Palette.into(),
        }
    }

    fn ix(self) -> usize {
        Self::ALL
            .iter()
            .position(|section| *section == self)
            .unwrap_or_default()
    }

    /// The section `step` places away, clamped to the list (no wrap).
    fn step(self, step: isize) -> Self {
        let ix = self
            .ix()
            .saturating_add_signed(step)
            .min(Self::ALL.len() - 1);
        Self::ALL[ix]
    }
}

/// A labelled option of a preference `Select`.
#[derive(Debug, Clone, PartialEq)]
pub struct Choice<T> {
    value: T,
    title: SharedString,
}

impl<T> Choice<T> {
    fn new(value: T, title: impl Into<SharedString>) -> Self {
        Self {
            value,
            title: title.into(),
        }
    }
}

impl<T: Clone + PartialEq> SearchableListItem for Choice<T> {
    type Value = T;

    fn title(&self) -> SharedString {
        self.title.clone()
    }

    fn value(&self) -> &T {
        &self.value
    }
}

type ChoiceSelect<T> = Entity<SelectState<Vec<Choice<T>>>>;

/// The Preferences screen below the title bar: section list and page.
pub struct PreferencesView {
    app: AppState,
    section: PreferencesSection,
    /// The section list's one Tab stop.
    nav_focus: FocusHandle,
    x_axis: ChoiceSelect<XAxis>,
    reference_sync: Entity<SelectState<Vec<SyncOption>>>,
    reference_playback: ChoiceSelect<ReferencePlayback>,
    drivers: Entity<DriverMappingsEditor>,
    _subscriptions: Vec<Subscription>,
}

impl PreferencesView {
    pub fn new(app: AppState, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let x_axis = cx.new(|cx| {
            SelectState::new(
                vec![
                    Choice::new(XAxis::Distance, "Distance"),
                    Choice::new(XAxis::Time, "Time"),
                ],
                Some(IndexPath::default()),
                window,
                cx,
            )
        });
        let reference_sync = cx.new(|cx| {
            let options = std::iter::once(SyncOption::automatic())
                .chain(
                    [
                        Strategy::Gps,
                        Strategy::PreCornerDampers,
                        Strategy::ManualDampers,
                        Strategy::LapPercentage,
                    ]
                    .map(SyncOption::strategy),
                )
                .collect::<Vec<_>>();
            SelectState::new(options, Some(IndexPath::default()), window, cx)
        });
        let reference_playback = cx.new(|cx| {
            SelectState::new(
                vec![
                    Choice::new(ReferencePlayback::Corners, "Corners"),
                    Choice::new(ReferencePlayback::Gps, "GPS"),
                    Choice::new(ReferencePlayback::Recording, "Recording"),
                ],
                Some(IndexPath::default()),
                window,
                cx,
            )
        });
        let drivers = cx.new(|cx| DriverMappingsEditor::new(app.clone(), window, cx));

        let subscriptions = vec![
            cx.subscribe(&x_axis, |this, _, event, cx| {
                let SelectEvent::Confirm(Some(axis)) = event else {
                    return;
                };
                this.set_x_axis(*axis, cx);
            }),
            cx.subscribe(&reference_sync, |this, _, event, cx| {
                let SelectEvent::Confirm(value) = event;
                let request = SyncOption::request((*value).flatten());
                this.app
                    .session
                    .update(cx, |session, cx| session.set_strategy(request, cx));
            }),
            cx.subscribe(&reference_playback, |this, _, event, cx| {
                let SelectEvent::Confirm(Some(playback)) = event else {
                    return;
                };
                let key = playback.key().to_string();
                this.app.preferences.update(cx, |preferences, cx| {
                    preferences.update(cx, |config| config.video.reference_playback = Some(key));
                });
            }),
            // The palette and other surfaces edit the same settings while
            // the screen is open: keep the selects in step with the document.
            cx.subscribe_in(&app.preferences, window, |this, _, event, window, cx| {
                if let PreferencesEvent::Changed = event {
                    this.sync_selects(window, cx);
                }
            }),
            cx.observe(&app.preferences, |_, _, cx| cx.notify()),
            cx.observe(&app.library, |_, _, cx| cx.notify()),
            cx.observe_global::<ThemeStatus>(|_, cx| cx.notify()),
            cx.observe_global::<ThemeFonts>(|_, cx| cx.notify()),
            cx.observe_global::<Theme>(|_, cx| cx.notify()),
        ];

        let mut view = Self {
            app,
            section: PreferencesSection::default(),
            nav_focus: cx.focus_handle().tab_stop(true),
            x_axis,
            reference_sync,
            reference_playback,
            drivers,
            _subscriptions: subscriptions,
        };
        view.sync_selects(window, cx);
        view
    }

    /// The section on show.
    pub fn section(&self) -> PreferencesSection {
        self.section
    }

    /// Show `section` and move keyboard focus to the section list.
    pub fn select_section(
        &mut self,
        section: PreferencesSection,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.section = section;
        self.focus_nav(window, cx);
        cx.notify();
    }

    /// Give the section list keyboard focus.
    pub fn focus_nav(&self, window: &mut Window, cx: &mut App) {
        window.focus(&self.nav_focus, cx);
    }

    /// Whether the section list has keyboard focus.
    pub fn is_nav_focused(&self, window: &Window) -> bool {
        self.nav_focus.is_focused(window)
    }

    fn step_section(&mut self, step: isize, window: &mut Window, cx: &mut Context<Self>) {
        let next = self.section.step(step);
        if next != self.section {
            self.select_section(next, window, cx);
        }
    }

    fn sync_selects(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let video = self.app.preferences.read(cx).config().video.clone();
        let axis = self.app.preferences.read(cx).config().trace.x_axis();
        let strategy = video.reference_sync();
        let playback = video.reference_playback();
        self.x_axis.update(cx, |select, cx| {
            if select.selected_value() != Some(&axis) {
                select.set_selected_value(&axis, window, cx);
            }
        });
        self.reference_sync.update(cx, |select, cx| {
            if select.selected_value() != Some(&strategy) {
                select.set_selected_value(&strategy, window, cx);
            }
        });
        self.reference_playback.update(cx, |select, cx| {
            if select.selected_value() != Some(&playback) {
                select.set_selected_value(&playback, window, cx);
            }
        });
    }

    /// The default x axis is the one the traces show: change both.
    fn set_x_axis(&mut self, axis: XAxis, cx: &mut Context<Self>) {
        let trace_axis = match axis {
            XAxis::Distance => omatrack_trace::XAxis::Distance,
            XAxis::Time => omatrack_trace::XAxis::Time,
        };
        if self.app.viewport.read(cx).axis() != trace_axis {
            self.app
                .viewport
                .update(cx, |viewport, cx| viewport.set_axis(trace_axis, cx));
        }
        self.app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.trace.x_axis = Some(axis));
        });
    }

    fn render_nav(&self, window: &Window, cx: &mut Context<Self>) -> AnyElement {
        let focused = self.is_nav_focused(window);
        let theme = cx.theme();
        let ring = theme.ring;
        let items = PreferencesSection::ALL.map(|section| {
            let active = section == self.section;
            SidebarMenuItem::new(section.title())
                .icon(section.icon())
                .active(active)
                .h_8()
                .when(active, |item| item.bg(theme.list_active))
                .border_1()
                .border_color(gpui_kit::transparent_black())
                .when(active && focused, |item| item.border_color(ring))
                .on_click(cx.listener(move |this, _: &ClickEvent, window, cx| {
                    this.select_section(section, window, cx);
                }))
        });
        let preferences = self.app.preferences.read(cx);
        let file = preferences.paths().config_file();
        let full = SharedString::from(file.display().to_string());
        // The directory, home as `~`, truncated; the file name in full.
        let home = std::env::var_os("HOME").map(std::path::PathBuf::from);
        let directory = file.parent().map(|dir| match &home {
            Some(home) if dir.starts_with(home) => {
                format!("~/{}", dir.strip_prefix(home).unwrap_or(dir).display())
            }
            _ => dir.display().to_string(),
        });
        let name = file
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_default();
        let error = preferences.last_error().map(SharedString::from);
        let footer = v_flex()
            .id("preferences-saved-to")
            .test_support()
            .w_full()
            .gap_1()
            .text_label()
            .text_color(theme.muted_foreground)
            .child("Changes save automatically to")
            .child(
                v_flex()
                    .id("preferences-config-path")
                    .aria_label(full.clone())
                    .tooltip(move |window, cx| {
                        gpui_kit::component::tooltip::Tooltip::new(full.clone()).build(window, cx)
                    })
                    .w_full()
                    .min_w_0()
                    .text_caption()
                    .font_family(theme.mono_font_family.clone())
                    .children(directory.map(|dir| div().w_full().truncate().child(dir)))
                    .child(div().text_color(theme.foreground).truncate().child(name)),
            )
            .when_some(error, |this, error| {
                this.child(
                    div()
                        .id("preferences-save-error")
                        .test_support()
                        .aria_label(error.clone())
                        .text_color(theme.danger)
                        .child(error),
                )
            });
        div()
            .id("preferences-nav")
            .test_support()
            .aria_label("Preferences sections")
            .key_context(PREFERENCES_NAV_CONTEXT)
            .track_focus(&self.nav_focus)
            .on_action(cx.listener(|this, _: &PrevPreferencesSection, window, cx| {
                this.step_section(-1, window, cx)
            }))
            .on_action(cx.listener(|this, _: &NextPreferencesSection, window, cx| {
                this.step_section(1, window, cx)
            }))
            .h_full()
            .flex_none()
            .child(
                Sidebar::new("preferences-sections")
                    .w(rems(NAV_WIDTH))
                    .collapsible(false)
                    .child(SidebarMenu::new().children(items))
                    .footer(footer),
            )
            .into_any_element()
    }

    fn render_page(&mut self, cx: &mut Context<Self>) -> AnyElement {
        match self.section {
            PreferencesSection::Library => self.render_library(cx),
            PreferencesSection::Traces => self.render_traces(cx),
            PreferencesSection::Video => self.render_video(cx),
            PreferencesSection::Drivers => self.render_drivers(cx),
            PreferencesSection::Tracks => self.render_tracks(cx),
            PreferencesSection::Appearance => self.render_appearance(cx),
        }
    }
}

impl Render for PreferencesView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let page = self.render_page(cx);
        let nav = self.render_nav(window, cx);
        h_flex()
            .id("preferences")
            .test_support()
            .key_context(PREFERENCES_CONTEXT)
            .size_full()
            .items_start()
            .bg(cx.theme().background)
            .child(nav)
            .child(
                div()
                    .id("preferences-content")
                    .flex_1()
                    .min_w_0()
                    .h_full()
                    .overflow_y_scrollbar()
                    .child(
                        v_flex()
                            .w_full()
                            .max_w(rems(CONTENT_MAX_WIDTH))
                            .px_10()
                            .pt_8()
                            .pb_12()
                            .child(page),
                    ),
            )
    }
}

/// The title bar while Preferences are open: the screen's name, and Done
/// (Escape) back to the workspace.
pub(crate) fn title_bar(
    on_done: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static,
    window: &Window,
    cx: &App,
) -> impl IntoElement {
    let theme = cx.theme();
    TitleBar::new().child(
        h_flex()
            .w_full()
            .gap_3()
            .pr_2()
            .child(
                div()
                    .id("preferences-title")
                    .test_support()
                    .aria_label("Preferences")
                    .text_body()
                    .font_semibold()
                    .text_color(theme.foreground)
                    .child("Preferences"),
            )
            .child(div().flex_1())
            .child(
                Button::new("preferences-done")
                    .small()
                    .primary()
                    .label("Done")
                    .children(Kbd::binding_for_action(
                        &ClosePreferences,
                        Some(PREFERENCES_CONTEXT),
                        window,
                    ))
                    .accessibility_label("Done: back to the workspace")
                    .on_click(on_done),
            ),
    )
}
