//! Recording metadata: the per-recording override (layer 1 of the
//! metadata precedence) of driver, event, session and car, stored in
//! `recording_metadata[<path>]` of `omatrack.yml`.
//!
//! Each field shows the value that would apply with the current draft and
//! the layer it comes from ([`omatrack_library::metadata`]), so clearing a
//! field shows what the recording falls back to.

use std::collections::HashMap;
use std::path::PathBuf;

use gpui_kit::component::{
    ActiveTheme as _, Sizable as _, WindowExt as _,
    button::{Button, ButtonVariants as _},
    dialog::{DialogAction, DialogClose, DialogFooter},
    form::{Field, Form},
    input::{Input, InputEvent, InputState},
    v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, AppContext as _, Context, Entity, InteractiveElement as _, IntoElement,
    ParentElement as _, Render, SharedString, StatefulInteractiveElement as _, Styled as _,
    Subscription, Task, TestSupportExt as _, Window, div,
};
use omatrack_library::metadata::{MetadataSources, Sourced};
use omatrack_library::{
    Config, EffectiveMetadata, MetadataLayer, RecordingSummary, SessionNode, effective_metadata,
};
use serde_yaml::Mapping;

use super::{car_number_error, field_value, nested_text, set_nested, text_error};
use crate::state::AppState;

/// One editable field of a recording's metadata.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum MetadataField {
    Driver,
    Event,
    Session,
    CarNumber,
    CarClass,
}

impl MetadataField {
    pub const ALL: [Self; 5] = [
        Self::Driver,
        Self::Event,
        Self::Session,
        Self::CarNumber,
        Self::CarClass,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Self::Driver => "Driver",
            Self::Event => "Event",
            Self::Session => "Session",
            Self::CarNumber => "Car number",
            Self::CarClass => "Car class",
        }
    }

    /// The element id of the field's text input.
    pub fn input_id(self) -> &'static str {
        match self {
            Self::Driver => "metadata-driver",
            Self::Event => "metadata-event",
            Self::Session => "metadata-session",
            Self::CarNumber => "metadata-car-number",
            Self::CarClass => "metadata-car-class",
        }
    }

    /// The element id of the line under the input: the effective value
    /// and its source, or the validation error.
    pub fn status_id(self) -> &'static str {
        match self {
            Self::Driver => "metadata-driver-status",
            Self::Event => "metadata-event-status",
            Self::Session => "metadata-session-status",
            Self::CarNumber => "metadata-car-number-status",
            Self::CarClass => "metadata-car-class-status",
        }
    }

    /// Where the field lives in a metadata mapping (`TRACK.yml` shape).
    fn path(self) -> &'static [&'static str] {
        match self {
            Self::Driver => &["driver", "name"],
            Self::Event => &["event"],
            Self::Session => &["session"],
            Self::CarNumber => &["car", "number"],
            Self::CarClass => &["car", "class"],
        }
    }

    fn error(self, value: &str) -> Option<SharedString> {
        match self {
            Self::CarNumber => car_number_error(value),
            _ => text_error(value),
        }
    }

    fn effective(self, metadata: &EffectiveMetadata) -> Option<&Sourced<String>> {
        match self {
            Self::Driver => metadata.driver.as_ref(),
            Self::Event => metadata.event.as_ref(),
            Self::Session => metadata.session.as_ref(),
            Self::CarNumber => metadata.car_number.as_ref(),
            Self::CarClass => metadata.car_class.as_ref(),
        }
    }
}

/// How the interface names a precedence layer.
pub fn layer_name(layer: MetadataLayer) -> &'static str {
    match layer {
        MetadataLayer::RecordingOverride => "This recording",
        MetadataLayer::FolderMetadata => "TRACK.yml",
        MetadataLayer::Preferences => "Preferences",
        MetadataLayer::Recording => "Recording",
        MetadataLayer::Inferred => "File name",
    }
}

/// The body of the recording metadata dialog. Owns the draft (one input
/// per field) and the preview of the effective metadata; the saved
/// override belongs to [`crate::state::Preferences`].
pub struct RecordingMetadataForm {
    app: AppState,
    path: PathBuf,
    summary: RecordingSummary,
    /// The recording's merged `TRACK.yml` chain; `None` while it is read.
    folder: Option<Mapping>,
    inputs: Vec<(MetadataField, Entity<InputState>)>,
    errors: HashMap<MetadataField, SharedString>,
    preview: EffectiveMetadata,
    _load: Task<()>,
    _subscriptions: Vec<Subscription>,
}

impl RecordingMetadataForm {
    pub fn new(
        app: AppState,
        node: &SessionNode,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) -> Self {
        let path = node.file.path().to_path_buf();
        let current = app
            .preferences
            .read(cx)
            .config()
            .recording_override(&path)
            .cloned()
            .unwrap_or_default();
        let inputs = MetadataField::ALL
            .into_iter()
            .map(|field| {
                let value = nested_text(&current, field.path()).unwrap_or_default();
                let input = cx.new(|cx| {
                    InputState::new(window, cx)
                        .placeholder("Not set")
                        .default_value(value)
                });
                (field, input)
            })
            .collect::<Vec<_>>();
        let mut subscriptions = inputs
            .iter()
            .map(|(_, input)| {
                cx.subscribe(input, |this, _, event: &InputEvent, cx| {
                    if matches!(event, InputEvent::Change) {
                        this.refresh(cx);
                    }
                })
            })
            .collect::<Vec<_>>();
        subscriptions.push(cx.observe(&app.preferences, |this, _, cx| this.refresh(cx)));

        // The TRACK.yml chain is file I/O: read it off the UI thread.
        let directory = path.parent().map(PathBuf::from);
        let load = cx.spawn(async move |this, cx| {
            let folder = cx
                .background_spawn(async move {
                    directory
                        .map(|directory| {
                            omatrack_library::track_yml::read_hierarchy(&directory, true).0
                        })
                        .unwrap_or_default()
                })
                .await;
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no deferred result; this weak entity/window handle may already be gone.")]
            let _ = this.update(cx, |this, cx| {
                this.folder = Some(folder);
                this.refresh(cx);
            });
        });

        let mut form = Self {
            app,
            path,
            summary: node.summary.clone(),
            folder: None,
            inputs,
            errors: HashMap::new(),
            preview: EffectiveMetadata::default(),
            _load: load,
            _subscriptions: subscriptions,
        };
        form.refresh(cx);
        form
    }

    /// The input of `field`.
    ///
    /// # Panics
    /// Panics if construction failed to create an input for every field; callers can
    /// only pass the fixed field enum.
    #[expect(
        clippy::expect_used,
        reason = "Construction creates one input for every variant of this fixed field enum."
    )]
    pub fn input(&self, field: MetadataField) -> &Entity<InputState> {
        &self
            .inputs
            .iter()
            .find(|(candidate, _)| *candidate == field)
            .expect("every field has an input")
            .1
    }

    /// The value `field` resolves to with the current draft, and its layer.
    pub fn effective(&self, field: MetadataField) -> Option<(&str, MetadataLayer)> {
        field
            .effective(&self.preview)
            .map(|sourced| (sourced.value.as_str(), sourced.layer))
    }

    /// The override the draft describes: the saved one (unknown keys
    /// kept) with every field replaced by its input.
    fn draft(&self, cx: &App) -> Mapping {
        let mut draft = self
            .app
            .preferences
            .read(cx)
            .config()
            .recording_override(&self.path)
            .cloned()
            .unwrap_or_default();
        for (field, input) in &self.inputs {
            set_nested(
                &mut draft,
                field.path(),
                field_value(&input.read(cx).value()),
            );
        }
        draft
    }

    /// Revalidate the draft and recompute the preview (on input, on a
    /// preferences change, when the folder chain arrives).
    fn refresh(&mut self, cx: &mut Context<'_, Self>) {
        self.errors = self
            .inputs
            .iter()
            .filter_map(|(field, input)| Some((*field, field.error(&input.read(cx).value())?)))
            .collect();
        let draft = self.draft(cx);
        let config = self.app.preferences.read(cx).config();
        // Only the layers effective_metadata reads, with the draft as the
        // recording's override.
        let mut sources = Config::default();
        sources.driver_mappings = config.driver_mappings.clone();
        sources.track_assignments = config.track_assignments.clone();
        sources
            .recording_metadata
            .insert(self.path.to_string_lossy().into_owned(), draft);
        let empty = Mapping::new();
        let folder = self.folder.as_ref().unwrap_or(&empty);
        self.preview = effective_metadata(
            &self.path,
            MetadataSources::new(folder, &sources).with_summary(Some(&self.summary)),
        );
        cx.notify();
    }

    /// Save the draft as the recording's override (an empty draft removes
    /// it) and rescan so the library shows it. False, with the errors
    /// shown, when a field is invalid; the dialog then stays open.
    pub fn save(&mut self, cx: &mut Context<'_, Self>) -> bool {
        self.refresh(cx);
        if !self.errors.is_empty() {
            return false;
        }
        let key = self.path.to_string_lossy().into_owned();
        let draft = self.draft(cx);
        let after = (!draft.is_empty()).then_some(draft);
        let before = self
            .app
            .preferences
            .read(cx)
            .config()
            .recording_metadata
            .get(&key)
            .cloned();
        if before == after {
            return true;
        }
        self.app.preferences.update(cx, |preferences, cx| {
            preferences.update(cx, |config| match after {
                Some(draft) => {
                    config.recording_metadata.insert(key, draft);
                }
                None => {
                    config.recording_metadata.remove(&key);
                }
            });
        });
        self.app
            .library
            .update(cx, super::super::state::library::Library::rescan);
        true
    }

    fn status(&self, field: MetadataField) -> (SharedString, bool) {
        if let Some(error) = self.errors.get(&field) {
            return (error.clone(), true);
        }
        if self.folder.is_none() {
            return ("Reading folder metadata…".into(), false);
        }
        match self.effective(field) {
            Some((value, layer)) => (format!("{value} · {}", layer_name(layer)).into(), false),
            None => ("Not set anywhere".into(), false),
        }
    }
}

impl Render for RecordingMetadataForm {
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let theme = cx.theme();
        let fields = self.inputs.iter().map(|(field, input)| {
            let (status, invalid) = self.status(*field);
            let label = if invalid {
                format!("{}: {status}", field.label())
            } else {
                format!("{} in effect: {status}", field.label())
            };
            Field::new().label(field.label()).child(
                v_flex()
                    .w_full()
                    .gap_1()
                    .child(
                        Input::new(input)
                            .id(field.input_id())
                            .aria_label(field.label())
                            .small()
                            .when(invalid, |input| input.border_color(theme.danger)),
                    )
                    .child(
                        div()
                            .id(field.status_id())
                            .test_support()
                            .aria_label(label)
                            .text_xs()
                            .text_color(if invalid {
                                theme.danger
                            } else {
                                theme.muted_foreground
                            })
                            .child(status),
                    ),
            )
        });
        v_flex()
            .id("recording-metadata")
            .test_support()
            .gap_3()
            .child(div().text_sm().text_color(theme.muted_foreground).child(
                "Overrides apply to this recording only. An empty field falls back to \
                         TRACK.yml, preferences and the recording itself.",
            ))
            .child(
                div()
                    .text_xs()
                    .font_family(theme.mono_font_family.clone())
                    .text_color(theme.muted_foreground)
                    .truncate()
                    .child(self.path.display().to_string()),
            )
            .child(Form::new().children(fields))
    }
}

/// Open the metadata dialog of the library recording `session`. Returns
/// the dialog body, or `None` when the recording is not in the library.
pub fn open(
    app: &AppState,
    session: &str,
    window: &mut Window,
    cx: &mut App,
) -> Option<Entity<RecordingMetadataForm>> {
    let node = app.library.read(cx).snapshot().session(session)?.clone();
    let form = cx.new(|cx| RecordingMetadataForm::new(app.clone(), &node, window, cx));
    let title = SharedString::from(node.file_name());
    let body = form.clone();
    window.open_dialog(cx, move |dialog, window, _| {
        let saving = body.clone();
        dialog
            .title(title.clone())
            .w(window.rem_size() * 34.)
            .child(body.clone())
            .footer(
                DialogFooter::new()
                    .child(DialogClose::new().child(Button::new("metadata-cancel").label("Cancel")))
                    .child(
                        DialogAction::new()
                            .child(Button::new("metadata-save").primary().label("Save")),
                    ),
            )
            .on_ok(move |_, _, cx| saving.update(cx, RecordingMetadataForm::save))
    });
    let first = form.read(cx).input(MetadataField::Driver).clone();
    first.update(cx, |input, cx| input.focus(window, cx));
    Some(form)
}
