//! Folder metadata: edit the `TRACK.yml` of a library folder.
//!
//! Recordings inherit every `TRACK.yml` above them, closer folders winning
//! key by key. This dialog edits one folder's file: each field shows what
//! it would inherit from the parent folders when left empty. Only the keys
//! Omatrack owns are rewritten ([`track_yml::OWNED_KEYS`]); unknown keys,
//! and unknown entries inside owned mappings (`driver.mappings`), are kept.
//! Reading and the atomic write run on the background executor.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Sizable as _, WindowExt as _,
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
use omatrack_library::track_yml;
use serde_yaml::Mapping;

use super::{car_number_error, field_value, nested_text, set_nested, text_error};
use crate::state::AppState;

/// One editable field of a folder's `TRACK.yml`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum FolderField {
    FolderName,
    Track,
    Event,
    Series,
    Driver,
    CarNumber,
    CarClass,
}

impl FolderField {
    pub const ALL: [Self; 7] = [
        Self::FolderName,
        Self::Track,
        Self::Event,
        Self::Series,
        Self::Driver,
        Self::CarNumber,
        Self::CarClass,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Self::FolderName => "Folder name",
            Self::Track => "Track",
            Self::Event => "Event",
            Self::Series => "Series",
            Self::Driver => "Driver",
            Self::CarNumber => "Car number",
            Self::CarClass => "Car class",
        }
    }

    /// The element id of the field's text input.
    pub fn input_id(self) -> &'static str {
        match self {
            Self::FolderName => "track-yml-folder-name",
            Self::Track => "track-yml-track",
            Self::Event => "track-yml-event",
            Self::Series => "track-yml-series",
            Self::Driver => "track-yml-driver",
            Self::CarNumber => "track-yml-car-number",
            Self::CarClass => "track-yml-car-class",
        }
    }

    fn status_id(self) -> &'static str {
        match self {
            Self::FolderName => "track-yml-folder-name-status",
            Self::Track => "track-yml-track-status",
            Self::Event => "track-yml-event-status",
            Self::Series => "track-yml-series-status",
            Self::Driver => "track-yml-driver-status",
            Self::CarNumber => "track-yml-car-number-status",
            Self::CarClass => "track-yml-car-class-status",
        }
    }

    /// Where the field lives in `TRACK.yml`.
    fn path(self) -> &'static [&'static str] {
        match self {
            Self::FolderName => &["folder", "name"],
            Self::Track => &["track", "name"],
            Self::Event => &["event"],
            Self::Series => &["series"],
            Self::Driver => &["driver", "name"],
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
}

/// Where the folder's document is in its read/write cycle.
#[derive(Debug, Clone, PartialEq)]
enum Document {
    Loading,
    /// The folder's own document (empty when the file does not exist).
    Ready {
        document: Mapping,
        exists: bool,
    },
    /// The file exists but could not be read: it is never overwritten.
    Unreadable(SharedString),
}

/// The body of the `TRACK.yml` dialog.
pub struct TrackYmlForm {
    app: AppState,
    directory: PathBuf,
    document: Document,
    /// The parents' merged metadata (what an empty field inherits).
    inherited: Mapping,
    inputs: Vec<(FolderField, Entity<InputState>)>,
    errors: HashMap<FolderField, SharedString>,
    saving: bool,
    save_error: Option<SharedString>,
    _load: Task<()>,
    save_task: Option<Task<()>>,
    _subscriptions: Vec<Subscription>,
}

impl TrackYmlForm {
    pub fn new(
        app: AppState,
        directory: PathBuf,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) -> Self {
        let inputs = FolderField::ALL
            .into_iter()
            .map(|field| {
                let input = cx.new(|cx| {
                    let mut input = InputState::new(window, cx).placeholder("Not set");
                    input.set_disabled(true, cx);
                    input
                });
                (field, input)
            })
            .collect::<Vec<_>>();
        let subscriptions = inputs
            .iter()
            .map(|(_, input)| {
                cx.subscribe(input, |this, _, event: &InputEvent, cx| {
                    if matches!(event, InputEvent::Change) {
                        this.validate(cx);
                    }
                })
            })
            .collect::<Vec<_>>();

        let path = directory.clone();
        let load = cx.spawn_in(window, async move |this, cx| {
            let (document, inherited) =
                cx.background_spawn(async move { read_folder(&path) }).await;
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no deferred result; this weak entity/window handle may already be gone.")]
            let _ = this.update_in(cx, |this, window, cx| {
                this.apply_loaded(document, inherited, window, cx);
            });
        });

        Self {
            app,
            directory,
            document: Document::Loading,
            inherited: Mapping::new(),
            inputs,
            errors: HashMap::new(),
            saving: false,
            save_error: None,
            _load: load,
            save_task: None,
            _subscriptions: subscriptions,
        }
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
    pub fn input(&self, field: FolderField) -> &Entity<InputState> {
        &self
            .inputs
            .iter()
            .find(|(candidate, _)| *candidate == field)
            .expect("every field has an input")
            .1
    }

    /// True once the document is read and editable.
    pub fn is_ready(&self) -> bool {
        matches!(self.document, Document::Ready { .. })
    }

    pub fn is_saving(&self) -> bool {
        self.saving
    }

    /// True when a field is invalid, the document cannot be edited, or a
    /// write is in flight.
    pub fn is_blocked(&self) -> bool {
        !self.errors.is_empty() || !self.is_ready() || self.saving
    }

    fn apply_loaded(
        &mut self,
        document: Document,
        inherited: Mapping,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) {
        if let Document::Ready { document, .. } = &document {
            for (field, input) in &self.inputs {
                let value = nested_text(document, field.path()).unwrap_or_default();
                let placeholder = match nested_text(&inherited, field.path()) {
                    Some(parent) => format!("Inherited: {parent}"),
                    None => "Not set".to_string(),
                };
                input.update(cx, |input, cx| {
                    input.set_value(value, window, cx);
                    input.set_placeholder(placeholder, window, cx);
                    input.set_disabled(false, cx);
                });
            }
            if let Some((_, first)) = self.inputs.first() {
                first.update(cx, |input, cx| input.focus(window, cx));
            }
        }
        self.document = document;
        self.inherited = inherited;
        self.validate(cx);
    }

    fn validate(&mut self, cx: &mut Context<'_, Self>) {
        self.errors = self
            .inputs
            .iter()
            .filter_map(|(field, input)| Some((*field, field.error(&input.read(cx).value())?)))
            .collect();
        cx.notify();
    }

    /// The Omatrack-owned keys the draft describes: the file's own owned
    /// keys with every field replaced by its input.
    fn owned_draft(&self, document: &Mapping, cx: &App) -> Mapping {
        let mut owned = Mapping::new();
        for key in track_yml::OWNED_KEYS {
            if let Some(value) = document.get(*key) {
                owned.insert((*key).into(), value.clone());
            }
        }
        for (field, input) in &self.inputs {
            set_nested(
                &mut owned,
                field.path(),
                field_value(&input.read(cx).value()),
            );
        }
        owned
    }

    /// Write the draft atomically on the background executor, then close
    /// the dialog and rescan. Returns true only when there is nothing to
    /// write (the dialog closes at once); a write closes it when it
    /// succeeds and shows the error when it fails.
    pub fn save(&mut self, window: &mut Window, cx: &mut Context<'_, Self>) -> bool {
        self.validate(cx);
        let Document::Ready { document, exists } = &self.document else {
            return false;
        };
        if !self.errors.is_empty() || self.saving {
            return false;
        }
        let owned = self.owned_draft(document, cx);
        let unchanged = if *exists {
            let mut current = Mapping::new();
            for key in track_yml::OWNED_KEYS {
                if let Some(value) = document.get(*key) {
                    current.insert((*key).into(), value.clone());
                }
            }
            current == owned
        } else {
            owned.is_empty()
        };
        if unchanged {
            return true;
        }
        self.saving = true;
        self.save_error = None;
        cx.notify();
        let directory = self.directory.clone();
        self.save_task = Some(cx.spawn_in(window, async move |this, cx| {
            let result = cx
                .background_spawn(async move { track_yml::update(&directory, &owned) })
                .await;
            #[expect(clippy::let_underscore_must_use, reason = "A dropped view needs no deferred result; this weak entity/window handle may already be gone.")]
            let _ = this.update_in(cx, |this, window, cx| {
                this.saving = false;
                match result {
                    Ok(_) => {
                        this.app
                            .library
                            .update(cx, super::super::state::library::Library::rescan);
                        window.close_dialog(cx);
                    }
                    Err(error) => {
                        this.save_error = Some(format!("Couldn’t save TRACK.yml. {error}").into());
                    }
                }
                cx.notify();
            });
        }));
        false
    }

    fn status(&self, field: FolderField) -> Option<(SharedString, bool)> {
        if let Some(error) = self.errors.get(&field) {
            return Some((error.clone(), true));
        }
        None
    }
}

/// The folder's own document and its parents' merged metadata.
fn read_folder(directory: &Path) -> (Document, Mapping) {
    let (inherited, _) = track_yml::read_hierarchy(directory, false);
    let Some(path) = track_yml::file_path(directory) else {
        return (
            Document::Unreadable("The folder no longer exists.".into()),
            inherited,
        );
    };
    if !path.is_file() {
        return (
            Document::Ready {
                document: Mapping::new(),
                exists: false,
            },
            inherited,
        );
    }
    let document = match track_yml::read_document(&path) {
        Ok(document) => Document::Ready {
            document,
            exists: true,
        },
        Err(error) => Document::Unreadable(format!("Couldn’t read TRACK.yml. {error}").into()),
    };
    (document, inherited)
}

impl Render for TrackYmlForm {
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let theme = cx.theme();
        let notice: Option<(SharedString, bool)> = match &self.document {
            Document::Loading => Some(("Reading TRACK.yml…".into(), false)),
            Document::Unreadable(message) => Some((message.clone(), true)),
            Document::Ready { .. } => self.save_error.clone().map(|error| (error, true)),
        };
        let fields = self.inputs.iter().map(|(field, input)| {
            let status = self.status(*field);
            let invalid = status.is_some();
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
                    .when_some(status, |this, (message, _)| {
                        this.child(
                            div()
                                .id(field.status_id())
                                .test_support()
                                .aria_label(format!("{}: {message}", field.label()))
                                .text_xs()
                                .text_color(theme.danger)
                                .child(message),
                        )
                    }),
            )
        });
        let file = self.directory.join(track_yml::FILE_NAME);
        v_flex()
            .id("track-yml")
            .test_support()
            .gap_3()
            .child(div().text_sm().text_color(theme.muted_foreground).child(
                "Recordings in this folder and its subfolders use these values. An \
                         empty field inherits from the parent folders.",
            ))
            .child(
                div()
                    .text_xs()
                    .font_family(theme.mono_font_family.clone())
                    .text_color(theme.muted_foreground)
                    .truncate()
                    .child(file.display().to_string()),
            )
            .when_some(notice, |this, (message, is_error)| {
                this.child(
                    div()
                        .id("track-yml-notice")
                        .test_support()
                        .aria_label(message.clone())
                        .text_sm()
                        .text_color(if is_error {
                            theme.danger
                        } else {
                            theme.muted_foreground
                        })
                        .child(message),
                )
            })
            .child(Form::new().children(fields))
    }
}

/// Open the `TRACK.yml` dialog for `directory` (a user library folder).
pub fn open(
    app: &AppState,
    directory: PathBuf,
    window: &mut Window,
    cx: &mut App,
) -> Entity<TrackYmlForm> {
    let title = SharedString::from(format!(
        "TRACK.yml · {}",
        directory.file_name().map_or_else(
            || directory.display().to_string(),
            |name| name.to_string_lossy().into_owned()
        )
    ));
    let form = cx.new(|cx| TrackYmlForm::new(app.clone(), directory, window, cx));
    let body = form.clone();
    window.open_dialog(cx, move |dialog, window, cx| {
        let saving = body.clone();
        let (blocked, busy) = {
            let form = body.read(cx);
            (form.is_blocked(), form.is_saving())
        };
        dialog
            .title(title.clone())
            .w(window.rem_size() * 34.)
            .child(body.clone())
            .footer(
                DialogFooter::new()
                    .child(
                        DialogClose::new().child(Button::new("track-yml-cancel").label("Cancel")),
                    )
                    .child(
                        DialogAction::new().child(
                            Button::new("track-yml-save")
                                .primary()
                                .label("Save")
                                .loading(busy)
                                .disabled(blocked),
                        ),
                    ),
            )
            .on_ok(move |_, window, cx| saving.update(cx, |form, cx| form.save(window, cx)))
    });
    form
}
