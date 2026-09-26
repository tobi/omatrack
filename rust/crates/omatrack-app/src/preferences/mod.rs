//! Preferences: a sheet of tabs over `omatrack.yml`.
//!
//! | Tab | Edits |
//! | --- | --- |
//! | Library | folder locations (add, remove, rescan) |
//! | Traces | fit lanes, x axis |
//! | Video | mute, reference sync and playback, continuous playback, HUD position |
//! | Drivers | `driver_mappings` |
//! | Tracks | per-track corner overrides; Track Atlas revision and attribution |
//! | Appearance | the theme in use (readonly: Omarchy themes are followed) |
//!
//! Every edit goes through the entity that owns it, which writes
//! `omatrack.yml` through [`crate::state::Preferences::update`] (debounced,
//! atomic, off the UI thread): mute and continuous playback through the
//! video controller, the sync strategy through the session, folders
//! through the library. The sheet is opened by [`open`]; its body is a
//! [`PreferencesView`] entity owned by the sheet builder, so it lives
//! exactly as long as the sheet. Escape closes the sheet (the component's
//! `Sheet` context) and `Root` returns focus to the trigger.

mod drivers;

use gpui_kit::component::{
    ActiveTheme as _, IconName, IndexPath, Sizable as _, StyledExt as _, ThemeStyled as _,
    WindowExt as _,
    button::{Button, ButtonVariant, ButtonVariants as _},
    description_list::DescriptionList,
    dialog::DialogButtonProps,
    form::{Field, Form},
    h_flex,
    searchable_list::SearchableListItem,
    select::{Select, SelectEvent, SelectState},
    switch::Switch,
    tab::{Tab, TabBar},
    text::TextView,
    v_flex,
};
use gpui_kit::component::{Disableable as _, Theme};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, AppContext as _, Context, ElementId, Entity, FocusHandle,
    InteractiveElement as _, IntoElement, ParentElement as _, Render, SharedString,
    StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _, Window, div,
};
use omatrack_core::alignment::Strategy;
use omatrack_core::playback::ReferencePlayback;
use omatrack_library::config::{XAxis, track_key};
use omatrack_ui::theme::{ThemeOrigin, ThemeStatus};

pub use drivers::DriverMappingsEditor;

use crate::state::{AppState, PreferencesEvent};
use crate::workspace::SyncOption;

/// One page of the preferences sheet.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum PreferencesTab {
    #[default]
    Library,
    Traces,
    Video,
    Drivers,
    Tracks,
    Appearance,
}

impl PreferencesTab {
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

    /// The element id of the tab's page.
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

    fn ix(self) -> usize {
        Self::ALL
            .iter()
            .position(|tab| *tab == self)
            .unwrap_or_default()
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

/// The preferences sheet's body.
pub struct PreferencesView {
    app: AppState,
    tab: PreferencesTab,
    /// One Tab stop per tab, in tab order (Enter or Space selects it).
    tab_focus: Vec<FocusHandle>,
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
            // Keys and other surfaces edit the same settings while the
            // sheet is open: keep the selects in step with the document.
            cx.subscribe_in(&app.preferences, window, |this, _, event, window, cx| {
                if let PreferencesEvent::Changed = event {
                    this.sync_selects(window, cx);
                }
            }),
            cx.observe(&app.preferences, |_, _, cx| cx.notify()),
            cx.observe(&app.library, |_, _, cx| cx.notify()),
            cx.observe_global::<ThemeStatus>(|_, cx| cx.notify()),
            cx.observe_global::<Theme>(|_, cx| cx.notify()),
        ];

        let mut view = Self {
            app,
            tab: PreferencesTab::default(),
            tab_focus: PreferencesTab::ALL
                .iter()
                .map(|_| cx.focus_handle().tab_stop(true))
                .collect(),
            x_axis,
            reference_sync,
            reference_playback,
            drivers,
            _subscriptions: subscriptions,
        };
        view.sync_selects(window, cx);
        view
    }

    /// The page on show.
    pub fn tab(&self) -> PreferencesTab {
        self.tab
    }

    /// Show `tab` and move keyboard focus to its tab.
    pub fn select_tab(&mut self, tab: PreferencesTab, window: &mut Window, cx: &mut Context<Self>) {
        self.tab = tab;
        if let Some(handle) = self.tab_focus.get(tab.ix()) {
            window.focus(handle, cx);
        }
        cx.notify();
    }

    /// The tab that has keyboard focus, if one has.
    pub fn focused_tab(&self, window: &Window) -> Option<PreferencesTab> {
        PreferencesTab::ALL
            .into_iter()
            .zip(&self.tab_focus)
            .find(|(_, handle)| handle.is_focused(window))
            .map(|(tab, _)| tab)
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

    fn confirm_remove_folder(
        &mut self,
        id: String,
        name: SharedString,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let app = self.app.clone();
        window.open_alert_dialog(cx, move |alert, _, _| {
            let app = app.clone();
            let id = id.clone();
            alert
                .title(SharedString::from(format!(
                    "Remove “{name}” from the library?"
                )))
                .description("Its recordings stay on disk.")
                .button_props(
                    DialogButtonProps::default()
                        .ok_text("Remove")
                        .ok_variant(ButtonVariant::Danger)
                        .show_cancel(true),
                )
                .on_ok(move |_, _, cx| {
                    app.preferences.update(cx, |preferences, cx| {
                        preferences.update(cx, |config| {
                            config.remove_location(&id);
                        });
                    });
                    app.library.update(cx, |library, cx| library.rescan(cx));
                    true
                })
        });
    }

    fn confirm_reset_corners(
        &mut self,
        key: String,
        name: SharedString,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let app = self.app.clone();
        window.open_alert_dialog(cx, move |alert, _, _| {
            let app = app.clone();
            let key = key.clone();
            alert
                .title(SharedString::from(format!("Reset the corners of {name}?")))
                .description("Your edited corners are removed and the Track Atlas corners apply.")
                .button_props(
                    DialogButtonProps::default()
                        .ok_text("Reset")
                        .ok_variant(ButtonVariant::Danger)
                        .show_cancel(true),
                )
                .on_ok(move |_, _, cx| {
                    reset_corners(&app, &key, cx);
                    true
                })
        });
    }

    fn render_tabs(&self, window: &Window, cx: &mut Context<Self>) -> impl IntoElement {
        let tabs = PreferencesTab::ALL
            .iter()
            .zip(&self.tab_focus)
            .map(|(tab, focus)| {
                let focused = focus.is_focused(window);
                Tab::new()
                    .label(tab.title())
                    .track_focus(focus)
                    .when(focused, |tab| tab.focus_ring_style(window, cx))
            });
        TabBar::new("preferences-tabs")
            .underline()
            .small()
            .selected_index(self.tab.ix())
            .children(tabs)
            .on_click(cx.listener(|this, ix: &usize, window, cx| {
                if let Some(tab) = PreferencesTab::ALL.get(*ix) {
                    this.select_tab(*tab, window, cx);
                }
            }))
    }

    fn render_page(&mut self, cx: &mut Context<Self>) -> AnyElement {
        match self.tab {
            PreferencesTab::Library => self.render_library(cx).into_any_element(),
            PreferencesTab::Traces => self.render_traces(cx).into_any_element(),
            PreferencesTab::Video => self.render_video(cx).into_any_element(),
            PreferencesTab::Drivers => self.render_drivers(cx).into_any_element(),
            PreferencesTab::Tracks => self.render_tracks(cx).into_any_element(),
            PreferencesTab::Appearance => self.render_appearance(cx).into_any_element(),
        }
    }

    fn render_library(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let scanning = self.app.library.read(cx).is_scanning();
        let folders = self
            .app
            .preferences
            .read(cx)
            .config()
            .folder_locations()
            .map(|folder| {
                (
                    folder.resolved_id(),
                    SharedString::from(folder.display_name()),
                    folder.target.clone().unwrap_or_default(),
                    folder.is_enabled(),
                )
            })
            .collect::<Vec<_>>();
        let theme = cx.theme();
        let rows = folders.into_iter().map(|(id, name, target, enabled)| {
            let remove_name = name.clone();
            let remove_id = id.clone();
            h_flex()
                .id(ElementId::Name(format!("prefs-folder-{id}").into()))
                .test_support()
                .aria_label(name.clone())
                .gap_2()
                .py_1()
                .child(
                    v_flex()
                        .flex_1()
                        .min_w_0()
                        .child(
                            h_flex()
                                .gap_2()
                                .child(div().text_sm().truncate().child(name.clone()))
                                .when(!enabled, |this| {
                                    this.child(
                                        div()
                                            .text_xs()
                                            .text_color(theme.muted_foreground)
                                            .child("Disabled"),
                                    )
                                }),
                        )
                        .child(
                            div()
                                .text_xs()
                                .font_family(theme.mono_font_family.clone())
                                .text_color(theme.muted_foreground)
                                .truncate()
                                .child(target),
                        ),
                )
                .child(
                    Button::new(ElementId::Name(format!("prefs-remove-folder-{id}").into()))
                        .small()
                        .outline()
                        .label("Remove…")
                        .accessibility_label(format!("Remove {name}…"))
                        .on_click(cx.listener(move |this, _, window, cx| {
                            this.confirm_remove_folder(
                                remove_id.clone(),
                                remove_name.clone(),
                                window,
                                cx,
                            )
                        })),
                )
        });
        let rows = rows.collect::<Vec<_>>();
        let empty = rows.is_empty();
        v_flex()
            .id(PreferencesTab::Library.page_id())
            .test_support()
            .gap_3()
            .child(section_title("Folders"))
            .child(muted_text(
                "Omatrack reads recordings from these folders and their subfolders. Nothing \
                 is written into them except TRACK.yml files you edit.",
                cx,
            ))
            .children(rows)
            .when(empty, |this| {
                this.child(muted_text("No library folders.", cx))
            })
            .child(
                h_flex()
                    .gap_2()
                    .child(
                        Button::new("prefs-add-folder")
                            .small()
                            .icon(IconName::Plus)
                            .label("Add folder…")
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.app
                                    .library
                                    .update(cx, |library, cx| library.prompt_add_folder(cx));
                            })),
                    )
                    .child(
                        Button::new("prefs-rescan")
                            .small()
                            .ghost()
                            .icon(IconName::RotateCw)
                            .label("Rescan")
                            .loading(scanning)
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.app
                                    .library
                                    .update(cx, |library, cx| library.rescan(cx));
                            })),
                    ),
            )
    }

    fn render_traces(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let fit = self
            .app
            .preferences
            .read(cx)
            .config()
            .trace
            .is_fitting_channels();
        v_flex()
            .id(PreferencesTab::Traces.page_id())
            .test_support()
            .gap_3()
            .child(
                Form::new()
                    .child(
                        Field::new()
                            .label_indent(false)
                            .description("Every visible lane shares the panel height; off, lanes keep their own height and scroll.")
                            .child(
                                Switch::new("prefs-fit-lanes")
                                    .label("Fit lanes to the panel")
                                    .checked(fit)
                                    .on_change(cx.listener(|this, fit: &bool, _, cx| {
                                        let fit = *fit;
                                        this.app.preferences.update(cx, |preferences, cx| {
                                            preferences.update(cx, |config| {
                                                config.trace.fit_channels = Some(fit)
                                            });
                                        });
                                    })),
                            ),
                    )
                    .child(
                        Field::new()
                            .label("X axis")
                            .description("Distance aligns laps by track position; time shows each lap as driven.")
                            .child(
                                Select::new(&self.x_axis)
                                    .id("prefs-x-axis")
                                    .small()
                                    .accessibility_label("X axis"),
                            ),
                    ),
            )
    }

    fn render_video(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let video = self.app.preferences.read(cx).config().video.clone();
        let muted = video.is_muted();
        let continuous = video.is_continuous_playback();
        let has_hud_position = video.hud_position().is_some();
        v_flex()
            .id(PreferencesTab::Video.page_id())
            .test_support()
            .gap_3()
            .child(
                Form::new()
                    .child(
                        Field::new().label_indent(false).child(
                            Switch::new("prefs-video-muted")
                                .label("Mute video")
                                .checked(muted)
                                .on_change(cx.listener(|this, muted: &bool, _, cx| {
                                    let wanted = *muted;
                                    let video = this.app.video.clone();
                                    if video.read(cx).is_muted(cx) != wanted {
                                        video.update(cx, |video, cx| video.toggle_mute(cx));
                                    }
                                })),
                        ),
                    )
                    .child(
                        Field::new()
                            .label("Reference sync")
                            .description("How the reference lap is aligned to the primary. Automatic uses GPS, then dampers, then lap %.")
                            .child(
                                Select::new(&self.reference_sync)
                                    .id("prefs-reference-sync")
                                    .small()
                                    .accessibility_label("Reference sync"),
                            ),
                    )
                    .child(
                        Field::new()
                            .label("Reference playback")
                            .description("Corners holds 1x through each corner; GPS follows the map continuously; Recording plays both at 1x.")
                            .child(
                                Select::new(&self.reference_playback)
                                    .id("prefs-reference-playback")
                                    .small()
                                    .accessibility_label("Reference playback"),
                            ),
                    )
                    .child(
                        Field::new().label_indent(false).child(
                            Switch::new("prefs-continuous")
                                .label("Continuous playback")
                                .checked(continuous)
                                .on_change(cx.listener(|this, continuous: &bool, _, cx| {
                                    let wanted = *continuous;
                                    let video = this.app.video.clone();
                                    if video.read(cx).is_continuous(cx) != wanted {
                                        video.update(cx, |video, cx| video.toggle_continuous(cx));
                                    }
                                })),
                        ),
                    )
                    .child(
                        Field::new()
                            .label_indent(false)
                            .description("Drag the HUD over the video to move it.")
                            .child(
                                h_flex().child(
                                    Button::new("prefs-reset-hud")
                                        .small()
                                        .outline()
                                        .icon(IconName::Undo2)
                                        .label("Reset HUD position")
                                        .disabled(!has_hud_position)
                                        .on_click(cx.listener(|this, _, _, cx| {
                                            this.app.preferences.update(cx, |preferences, cx| {
                                                preferences.update(cx, |config| {
                                                    config.video.set_hud_position(None)
                                                });
                                            });
                                        })),
                                ),
                            ),
                    ),
            )
    }

    fn render_drivers(&self, cx: &mut Context<Self>) -> impl IntoElement {
        v_flex()
            .id(PreferencesTab::Drivers.page_id())
            .test_support()
            .gap_3()
            .child(muted_text(
                "Names for the driver ids a logger records. * names every id without its own \
                 entry. A TRACK.yml or a recording's own metadata takes precedence.",
                cx,
            ))
            .child(self.drivers.clone())
    }

    fn render_tracks(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let overrides = self
            .app
            .preferences
            .read(cx)
            .config()
            .tracks
            .iter()
            .filter(|(_, track)| !track.corner_zones().is_empty())
            .map(|(key, track)| (key.clone(), track_title(key), track.corner_zones().len()))
            .collect::<Vec<_>>();
        let theme = cx.theme();
        let rows = overrides.into_iter().map(|(key, name, count)| {
            let reset_key = key.clone();
            let reset_name = name.clone();
            let noun = if count == 1 { "corner" } else { "corners" };
            h_flex()
                .id(ElementId::Name(format!("prefs-track-{key}").into()))
                .test_support()
                .aria_label(name.clone())
                .gap_2()
                .py_1()
                .child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .text_sm()
                        .truncate()
                        .child(name.clone()),
                )
                .child(
                    div()
                        .text_xs()
                        .text_color(theme.muted_foreground)
                        .child(format!("{count} edited {noun}")),
                )
                .child(
                    Button::new(ElementId::Name(format!("prefs-reset-corners-{key}").into()))
                        .small()
                        .outline()
                        .label("Reset…")
                        .accessibility_label(format!("Reset the corners of {name}…"))
                        .on_click(cx.listener(move |this, _, window, cx| {
                            this.confirm_reset_corners(
                                reset_key.clone(),
                                reset_name.clone(),
                                window,
                                cx,
                            )
                        })),
                )
        });
        let rows = rows.collect::<Vec<_>>();
        let empty = rows.is_empty();
        let attribution = omatrack_core::track::ATTRIBUTION.replace('\n', "\n\n");
        v_flex()
            .id(PreferencesTab::Tracks.page_id())
            .test_support()
            .gap_3()
            .child(section_title("Corner edits"))
            .children(rows)
            .when(empty, |this| {
                this.child(muted_text(
                    "No edited corners. Every track uses its Track Atlas corners.",
                    cx,
                ))
            })
            .child(section_title("Track Atlas"))
            .child(DescriptionList::new().columns(1).item(
                "Revision",
                SharedString::from(omatrack_core::track::atlas_revision()),
                1,
            ))
            .child(
                div()
                    .id("atlas-attribution")
                    .test_support()
                    .aria_label("Track Atlas attribution")
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(TextView::markdown("atlas-attribution-text", attribution)),
            )
    }

    fn render_appearance(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let status = ThemeStatus::global(cx).cloned();
        let (theme_name, source, location) = match &status {
            Some(status) => match status.origin() {
                ThemeOrigin::Omarchy(path) => (
                    status.name().clone(),
                    SharedString::from("Omarchy"),
                    Some(SharedString::from(path.display().to_string())),
                ),
                ThemeOrigin::BuiltIn => (status.label(), SharedString::from("Built in"), None),
            },
            None => (
                SharedString::from("Default"),
                SharedString::from("Built in"),
                None,
            ),
        };
        let mut list = DescriptionList::new()
            .columns(1)
            .item("Theme", theme_name, 1)
            .item("Source", source, 1);
        if let Some(location) = location {
            list = list.item("Folder", location, 1);
        }
        v_flex()
            .id(PreferencesTab::Appearance.page_id())
            .test_support()
            .gap_3()
            .child(list)
            .child(muted_text(
                "Omarchy themes are followed automatically: switching the Omarchy theme \
                 restyles Omatrack at once. Without an Omarchy theme the built-in dark theme \
                 applies.",
                cx,
            ))
    }

    fn render_footer(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let preferences = self.app.preferences.read(cx);
        let path = preferences.paths().config_file().display().to_string();
        let error = preferences.last_error().map(SharedString::from);
        let theme = cx.theme();
        v_flex()
            .gap_1()
            .pt_3()
            .border_t_1()
            .border_color(theme.border)
            .text_xs()
            .text_color(theme.muted_foreground)
            .child("Changes are saved automatically to")
            .child(
                div()
                    .font_family(theme.mono_font_family.clone())
                    .truncate()
                    .child(path),
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
            })
    }
}

impl Render for PreferencesView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let page = self.render_page(cx);
        let tabs = self.render_tabs(window, cx).into_any_element();
        v_flex()
            .id("preferences")
            .test_support()
            .key_context("Preferences")
            .gap_4()
            .pt_1()
            .pb_4()
            .child(tabs)
            .child(page)
            .child(self.render_footer(cx))
    }
}

fn section_title(title: &'static str) -> impl IntoElement {
    div().text_sm().font_semibold().child(title)
}

fn muted_text(text: &'static str, cx: &App) -> impl IntoElement {
    div()
        .text_sm()
        .text_color(cx.theme().muted_foreground)
        .child(text)
}

/// A `tracks.<key>` key as a name: the Track Atlas name when the key
/// resolves, else the key with spaces.
fn track_title(key: &str) -> SharedString {
    let spaced = key.replace('_', " ");
    omatrack_core::track::find_track(&spaced)
        .or_else(|| omatrack_core::track::find_track(key))
        .map(|track| SharedString::from(track.name.to_string()))
        .unwrap_or_else(|| spaced.into())
}

/// Drop the corner edits of `tracks.<key>`. When the primary lap is on
/// that track the session drops its override too (and re-analyses).
fn reset_corners(app: &AppState, key: &str, cx: &mut App) {
    let primary_track = app
        .session
        .read(cx)
        .primary()
        .and_then(|slot| slot.track_key())
        .map(|track| track_key(track));
    if primary_track.as_deref() == Some(key) {
        app.session
            .update(cx, |session, cx| session.set_corner_override(None, cx));
    } else {
        let key = key.to_string();
        app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| config.set_track_corners(&key, None));
        });
    }
}

/// Open the preferences sheet on the right of `window`.
pub fn open(window: &mut Window, cx: &mut App) -> Option<Entity<PreferencesView>> {
    let app = AppState::try_global(cx)?.clone();
    let view = cx.new(|cx| PreferencesView::new(app, window, cx));
    let body = view.clone();
    window.open_sheet(cx, move |sheet, window, _| {
        sheet
            .title("Preferences")
            .size(window.rem_size() * 36.)
            .child(body.clone())
    });
    Some(view)
}
