//! The six pages of the Preferences screen.

use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, IconName, Sizable as _, WindowExt as _,
    button::{Button, ButtonVariant, ButtonVariants as _},
    dialog::DialogButtonProps,
    h_flex,
    select::Select,
    switch::Switch,
    text::TextView,
    v_flex,
};
use gpui_kit::{
    AnyElement, App, Context, ElementId, InteractiveElement as _, IntoElement, ParentElement as _,
    SharedString, StatefulInteractiveElement as _, Styled as _, TestSupportExt as _, Window, div,
};
use omatrack_library::config::track_key;
use omatrack_ui::theme::{FontOrigin, ThemeFonts, ThemeOrigin, ThemeStatus};

use super::layout::{card, control, note, page, row, row_with, value};
use super::{PreferencesSection, PreferencesView};
use crate::state::AppState;

impl PreferencesView {
    pub(super) fn render_library(&self, cx: &mut Context<Self>) -> AnyElement {
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
                    SharedString::from(folder.target.clone().unwrap_or_default()),
                    folder.is_enabled(),
                )
            })
            .collect::<Vec<_>>();
        let mut rows = folders
            .into_iter()
            .map(|(id, name, target, enabled)| {
                let remove_name = name.clone();
                let remove_id = id.clone();
                let title = if enabled {
                    name.clone()
                } else {
                    format!("{name} (disabled)").into()
                };
                row_with(
                    title,
                    Some(target),
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
                    cx,
                )
                .id(ElementId::Name(format!("prefs-folder-{id}").into()))
                .test_support()
                .aria_label(name)
                .into_any_element()
            })
            .collect::<Vec<_>>();
        if rows.is_empty() {
            rows.push(note(
                "prefs-no-folders",
                "No library folders yet. Add the folder your loggers' files are copied to.",
                cx,
            ));
        }
        let actions = h_flex()
            .px_4()
            .py_3()
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
            )
            .into_any_element();
        rows.push(actions);
        page(
            PreferencesSection::Library,
            [
                card("prefs-folders", "Folders", rows, cx),
                v_flex()
                    .text_sm()
                    .text_color(cx.theme().muted_foreground)
                    .child(
                        "Recordings are never modified. The only files Omatrack writes into a \
                         library folder are the TRACK.yml files you edit.",
                    )
                    .into_any_element(),
            ],
            cx,
        )
    }

    pub(super) fn render_traces(&self, cx: &mut Context<Self>) -> AnyElement {
        let fit = self
            .app
            .preferences
            .read(cx)
            .config()
            .trace
            .is_fitting_channels();
        let fit_row = row(
            "Fit lanes to the panel",
            Some(
                "Every visible lane shares the panel height. Off, lanes keep their own height and scroll.",
            ),
            Switch::new("prefs-fit-lanes")
                .checked(fit)
                .accessibility_label("Fit lanes to the panel")
                .on_change(cx.listener(|this, fit: &bool, _, cx| {
                    let fit = *fit;
                    this.app.preferences.update(cx, |preferences, cx| {
                        preferences.update(cx, |config| config.trace.fit_channels = Some(fit));
                    });
                })),
            cx,
        );
        let axis_row = row(
            "X axis",
            Some("Distance aligns laps by track position; time shows each lap as driven."),
            control(
                Select::new(&self.x_axis)
                    .id("prefs-x-axis")
                    .small()
                    .accessibility_label("X axis"),
            ),
            cx,
        );
        page(
            PreferencesSection::Traces,
            [card(
                "prefs-trace-display",
                "Display",
                [fit_row, axis_row],
                cx,
            )],
            cx,
        )
    }

    pub(super) fn render_video(&self, cx: &mut Context<Self>) -> AnyElement {
        let video = self.app.preferences.read(cx).config().video.clone();
        let muted = video.is_muted();
        let continuous = video.is_continuous_playback();
        let has_hud_position = video.hud_position().is_some();
        let mute_row = row(
            "Mute video",
            Some("Onboard audio stays off until you unmute (M)."),
            Switch::new("prefs-video-muted")
                .checked(muted)
                .accessibility_label("Mute video")
                .on_change(cx.listener(|this, muted: &bool, _, cx| {
                    let wanted = *muted;
                    let video = this.app.video.clone();
                    if video.read(cx).is_muted(cx) != wanted {
                        video.update(cx, |video, cx| video.toggle_mute(cx));
                    }
                })),
            cx,
        );
        let continuous_row = row(
            "Continuous playback",
            Some("Play on into the next lap without the 3-2-1 countdown (P)."),
            Switch::new("prefs-continuous")
                .checked(continuous)
                .accessibility_label("Continuous playback")
                .on_change(cx.listener(|this, continuous: &bool, _, cx| {
                    let wanted = *continuous;
                    let video = this.app.video.clone();
                    if video.read(cx).is_continuous(cx) != wanted {
                        video.update(cx, |video, cx| video.toggle_continuous(cx));
                    }
                })),
            cx,
        );
        let sync_row = row(
            "Reference sync",
            Some(
                "How the reference lap is aligned to the primary. Automatic uses GPS, then \
                 dampers, then lap %.",
            ),
            control(
                Select::new(&self.reference_sync)
                    .id("prefs-reference-sync")
                    .small()
                    .accessibility_label("Reference sync"),
            ),
            cx,
        );
        let pacing_row = row(
            "Reference playback",
            Some(
                "Corners holds 1x through each corner; GPS follows the map continuously; \
                 Recording plays both at 1x.",
            ),
            control(
                Select::new(&self.reference_playback)
                    .id("prefs-reference-playback")
                    .small()
                    .accessibility_label("Reference playback"),
            ),
            cx,
        );
        let hud_row = row(
            "HUD position",
            Some("Drag the telemetry HUD over the video to move it."),
            Button::new("prefs-reset-hud")
                .small()
                .outline()
                .icon(IconName::Undo2)
                .label("Reset")
                .accessibility_label("Reset HUD position")
                .disabled(!has_hud_position)
                .on_click(cx.listener(|this, _, _, cx| {
                    this.app.preferences.update(cx, |preferences, cx| {
                        preferences.update(cx, |config| config.video.set_hud_position(None));
                    });
                })),
            cx,
        );
        page(
            PreferencesSection::Video,
            [
                card(
                    "prefs-video-playback",
                    "Playback",
                    [mute_row, continuous_row],
                    cx,
                ),
                card(
                    "prefs-video-reference",
                    "Reference lap",
                    [sync_row, pacing_row],
                    cx,
                ),
                card("prefs-video-hud", "Overlay", [hud_row], cx),
            ],
            cx,
        )
    }

    pub(super) fn render_drivers(&self, cx: &mut Context<Self>) -> AnyElement {
        let editor = div()
            .px_4()
            .py_3()
            .child(self.drivers.clone())
            .into_any_element();
        page(
            PreferencesSection::Drivers,
            [
                card("prefs-driver-names", "Driver names", [editor], cx),
                div()
                    .text_sm()
                    .text_color(cx.theme().muted_foreground)
                    .child(
                        "* names every id without its own entry. A TRACK.yml or a \
                         recording's own metadata takes precedence.",
                    )
                    .into_any_element(),
            ],
            cx,
        )
    }

    pub(super) fn render_tracks(&self, cx: &mut Context<Self>) -> AnyElement {
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
        let mut rows = overrides
            .into_iter()
            .map(|(key, name, count)| {
                let reset_key = key.clone();
                let reset_name = name.clone();
                let noun = if count == 1 { "corner" } else { "corners" };
                row_with(
                    name.clone(),
                    Some(format!("{count} edited {noun}").into()),
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
                    cx,
                )
                .id(ElementId::Name(format!("prefs-track-{key}").into()))
                .test_support()
                .aria_label(name)
                .into_any_element()
            })
            .collect::<Vec<_>>();
        if rows.is_empty() {
            rows.push(note(
                "prefs-no-corner-edits",
                "No edited corners. Every track uses its Track Atlas corners.",
                cx,
            ));
        }
        let revision = row(
            "Revision",
            None,
            value(omatrack_core::track::atlas_revision(), true, cx),
            cx,
        );
        let attribution = omatrack_core::track::ATTRIBUTION.replace('\n', "\n\n");
        let attribution = div()
            .id("atlas-attribution")
            .test_support()
            .aria_label("Track Atlas attribution")
            .px_4()
            .py_3()
            .text_sm()
            .text_color(cx.theme().muted_foreground)
            .child(TextView::markdown("atlas-attribution-text", attribution))
            .into_any_element();
        page(
            PreferencesSection::Tracks,
            [
                card("prefs-corner-edits", "Corner edits", rows, cx),
                card(
                    "prefs-track-atlas",
                    "Track Atlas",
                    [revision, attribution],
                    cx,
                ),
            ],
            cx,
        )
    }

    pub(super) fn render_appearance(&self, cx: &mut Context<Self>) -> AnyElement {
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
        let mut theme_rows = vec![
            row(
                "Theme",
                Some(
                    "Switching the Omarchy theme restyles Omatrack at once. Without one, the \
                     built-in dark theme applies.",
                ),
                value(theme_name, false, cx),
                cx,
            ),
            row("Source", None, value(source, false, cx), cx),
        ];
        if let Some(location) = location {
            theme_rows.push(row("Folder", None, value(location, true, cx), cx));
        }
        let fonts = ThemeFonts::global(cx).cloned();
        let font_rows = match fonts {
            Some(fonts) => {
                let origin = |origin: FontOrigin| match origin {
                    FontOrigin::Desktop => "From the desktop font settings.",
                    FontOrigin::Bundled => "Bundled with Omatrack.",
                };
                vec![
                    row(
                        "Interface",
                        Some(origin(fonts.ui().origin())),
                        value(fonts.ui().name().clone(), false, cx),
                        cx,
                    ),
                    row(
                        "Numbers",
                        Some(origin(fonts.mono().origin())),
                        value(fonts.mono().name().clone(), true, cx),
                        cx,
                    ),
                ]
            }
            None => vec![note("prefs-no-fonts", "The platform default fonts.", cx)],
        };
        page(
            PreferencesSection::Appearance,
            [
                card("prefs-theme", "Theme", theme_rows, cx),
                card("prefs-fonts", "Fonts", font_rows, cx),
            ],
            cx,
        )
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
