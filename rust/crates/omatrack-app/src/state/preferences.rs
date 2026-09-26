//! `omatrack.yml` as an entity: the one configuration document, mutated in
//! memory and written back atomically after a short debounce.

use std::time::Duration;

use gpui_kit::{AppContext as _, Context, EventEmitter, Task};
use omatrack_library::{Config, ConfigFile, Paths};

/// How long edits coalesce before the document is written.
pub const SAVE_DEBOUNCE: Duration = Duration::from_millis(500);

/// What a [`Preferences`] change means to its observers.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PreferencesEvent {
    /// The in-memory configuration changed.
    Changed,
    /// Writing `omatrack.yml` failed; the message is user-facing.
    SaveFailed(String),
}

/// Owns [`Config`] and the file it came from.
///
/// Every edit goes through [`Preferences::update`]: the document changes in
/// memory at once and is serialized on the UI thread, then written by a
/// background task [`SAVE_DEBOUNCE`] after the last edit. [`Preferences::flush`]
/// writes synchronously (quit). A document that exists but could not be read
/// is never overwritten (see [`ConfigFile`]).
pub struct Preferences {
    paths: Paths,
    file: ConfigFile,
    dirty: bool,
    revision: u64,
    save_task: Option<Task<()>>,
    last_error: Option<String>,
}

impl EventEmitter<PreferencesEvent> for Preferences {}

impl Preferences {
    /// Read `omatrack.yml` below `paths`. A missing file is the default
    /// configuration. Reads the (small) document synchronously: call it at
    /// startup, before the first window.
    pub fn open(paths: Paths, cx: &mut Context<Self>) -> Self {
        let file = ConfigFile::open(paths.config_file());
        let last_error = file.load_error().map(|error| error.to_string());
        cx.on_app_quit(|this: &mut Self, _| {
            this.flush_now();
            async {}
        })
        .detach();
        Self {
            paths,
            file,
            dirty: false,
            revision: 0,
            save_task: None,
            last_error,
        }
    }

    pub fn paths(&self) -> &Paths {
        &self.paths
    }

    pub fn config(&self) -> &Config {
        self.file.config()
    }

    /// Why the document on disk could not be read or written, if it could not.
    pub fn last_error(&self) -> Option<&str> {
        self.last_error.as_deref()
    }

    /// True while an edit has not been written yet.
    pub fn is_dirty(&self) -> bool {
        self.dirty
    }

    /// Apply `edit` to the configuration and schedule a save. Observers are
    /// notified only when the document actually changed.
    pub fn update(&mut self, cx: &mut Context<Self>, edit: impl FnOnce(&mut Config)) {
        let before = self.file.config().clone();
        edit(self.file.config_mut());
        if *self.file.config() == before {
            return;
        }
        self.dirty = true;
        self.revision += 1;
        self.schedule_save(cx);
        cx.emit(PreferencesEvent::Changed);
        cx.notify();
    }

    fn schedule_save(&mut self, cx: &mut Context<Self>) {
        if !self.file.is_writable() {
            return;
        }
        // Replacing the task cancels the previous debounce.
        self.save_task = Some(cx.spawn(async move |this, cx| {
            cx.background_executor().timer(SAVE_DEBOUNCE).await;
            let Ok(Some((config, path, revision))) = this.update(cx, |this, _| {
                this.dirty.then(|| {
                    (
                        this.file.config().clone(),
                        this.file.path().to_path_buf(),
                        this.revision,
                    )
                })
            }) else {
                return;
            };
            let result = cx.background_spawn(async move { config.save(&path) }).await;
            let _ = this.update(cx, |this, cx| match result {
                Ok(()) => {
                    if this.revision == revision {
                        this.dirty = false;
                    }
                    this.last_error = None;
                }
                Err(error) => {
                    let message = format!("Couldn’t save preferences. {error}");
                    this.last_error = Some(message.clone());
                    cx.emit(PreferencesEvent::SaveFailed(message));
                }
            });
        }));
    }

    /// Write a pending edit now, on the calling thread (quit, tests).
    pub fn flush(&mut self, cx: &mut Context<Self>) {
        if let Some(message) = self.flush_now() {
            cx.emit(PreferencesEvent::SaveFailed(message));
        }
    }

    fn flush_now(&mut self) -> Option<String> {
        self.save_task = None;
        if !self.dirty || !self.file.is_writable() {
            return None;
        }
        match self.file.save() {
            Ok(()) => {
                self.dirty = false;
                self.last_error = None;
                None
            }
            Err(error) => {
                let message = format!("Couldn’t save preferences. {error}");
                self.last_error = Some(message.clone());
                Some(message)
            }
        }
    }
}
