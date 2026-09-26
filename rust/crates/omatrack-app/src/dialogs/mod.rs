//! Library dialogs: a recording's metadata overrides and a folder's
//! `TRACK.yml`.
//!
//! Both open from the library's context menu (and the recording dialog
//! from Ctrl+I on the selected row). Each dialog body is an entity created
//! when it opens and owned by the dialog builder, so it lives exactly as
//! long as the dialog. Writes go through their owners: recording
//! overrides through [`crate::state::Preferences::update`] (debounced,
//! atomic), `TRACK.yml` through `omatrack_library::track_yml::update` on
//! the background executor (atomic, unrelated keys kept).

pub mod recording_metadata;
pub mod track_yml;

use std::path::{Path, PathBuf};

use gpui_kit::{App, KeyBinding, NoAction, SharedString};
use omatrack_library::Config;
use serde_yaml::{Mapping, Value};

pub use recording_metadata::{MetadataField, RecordingMetadataForm};
pub use track_yml::{FolderField, TrackYmlForm};

/// Edit the per-recording metadata override of a library recording.
/// `session` is the catalog session id; `None` means the recording of the
/// Library's selected row.
#[derive(Debug, Clone, PartialEq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct EditRecordingMetadata {
    pub session: Option<SharedString>,
}

/// Edit the `TRACK.yml` of the folder holding a library recording.
#[derive(Debug, Clone, PartialEq, gpui_kit::Action)]
#[action(namespace = omatrack, no_json)]
pub struct EditFolderMetadata {
    pub session: SharedString,
}

/// The key contexts of the component overlays (gpui-base `Sheet`, `Dialog`)
/// and of the Preferences screen, which covers the workspace the same way.
const OVERLAY_CONTEXTS: [&str; 3] = ["Sheet", "Dialog", crate::keymap::PREFERENCES_CONTEXT];

/// The workspace's single keys (`keymap.rs`, bound in
/// `Workspace && !Input`), except Escape, which the overlays bind
/// themselves to close.
const WORKSPACE_SINGLE_KEYS: [&str; 21] = [
    "space", "left", "right", "m", "f", "1", "2", "3", "4", "5", "s", "p", "x", "a", "h", "j", "=",
    "-", "[", "]", "t",
];

/// Keep the workspace's single keys out of sheets, dialogs and Preferences.
///
/// Those overlays render inside the workspace, so without this Space on a
/// focused switch, tab or button would toggle playback instead of
/// activating the control, and `x`, `m` or `1` would act on the workspace
/// behind a modal surface. A `NoAction` binding in the overlay's context
/// out-ranks the workspace binding (a deeper context) and leaves the key to
/// the focused control; overlay bindings of their own (Escape, a Select's
/// arrows) are deeper still and unaffected.
pub(crate) fn init(cx: &mut App) {
    cx.bind_keys(OVERLAY_CONTEXTS.into_iter().flat_map(|context| {
        WORKSPACE_SINGLE_KEYS
            .into_iter()
            .map(move |key| KeyBinding::new(key, NoAction, Some(context)))
    }));
}

/// The folder of `recording` when it lies inside an enabled folder
/// location of the library: only those folders are offered for
/// `TRACK.yml` editing. A path comparison only (no file system access),
/// so it is cheap enough for building menus: catalog paths are found by
/// walking the location roots and so start with them.
pub fn user_library_folder(config: &Config, recording: &Path) -> Option<PathBuf> {
    let folder = recording.parent()?;
    config
        .folder_locations()
        .filter(|location| location.is_enabled())
        .filter_map(|location| location.target_path())
        .any(|root| folder.starts_with(&root))
        .then(|| folder.to_path_buf())
}

/// Longest text a metadata field accepts.
const MAX_TEXT_CHARS: usize = 80;

/// Why `value` is not a valid free-text metadata value, if it is not.
fn text_error(value: &str) -> Option<SharedString> {
    (value.trim().chars().count() > MAX_TEXT_CHARS)
        .then(|| format!("Use at most {MAX_TEXT_CHARS} characters.").into())
}

/// Why `value` is not a valid car number, if it is not: up to six letters
/// or digits (`52`, `7B`).
fn car_number_error(value: &str) -> Option<SharedString> {
    let value = value.trim();
    let valid = value.is_empty()
        || (value.chars().count() <= 6 && value.chars().all(|c| c.is_ascii_alphanumeric()));
    (!valid).then(|| "Use up to 6 letters or digits.".into())
}

/// The text at `path` in a metadata mapping (trimmed, non-empty).
fn nested_text(map: &Mapping, path: &[&str]) -> Option<String> {
    let (last, parents) = path.split_last()?;
    let mut current = map;
    for key in parents {
        current = current.get(*key)?.as_mapping()?;
    }
    let text = match current.get(*last)? {
        Value::String(text) => text.clone(),
        Value::Number(number) => number.to_string(),
        Value::Bool(flag) => flag.to_string(),
        _ => return None,
    };
    let text = text.trim().to_string();
    (!text.is_empty()).then_some(text)
}

/// Set (or with `None`, remove) the text at `path`, creating parent
/// mappings as needed and dropping parents a removal leaves empty. Every
/// other key, at every level, is kept.
fn set_nested(map: &mut Mapping, path: &[&str], value: Option<String>) {
    let Some((first, rest)) = path.split_first() else {
        return;
    };
    let key = Value::from(*first);
    if rest.is_empty() {
        match value {
            Some(value) => {
                map.insert(key, Value::from(value));
            }
            None => {
                map.remove(&key);
            }
        }
        return;
    }
    let mut child = match map.get(&key) {
        Some(Value::Mapping(child)) => child.clone(),
        // A scalar where a mapping belongs is replaced only by a value.
        Some(_) if value.is_none() => return,
        _ => Mapping::new(),
    };
    set_nested(&mut child, rest, value);
    if child.is_empty() {
        map.remove(&key);
    } else {
        map.insert(key, Value::Mapping(child));
    }
}

/// A trimmed input value, `None` when blank.
fn field_value(text: &str) -> Option<String> {
    let text = text.trim();
    (!text.is_empty()).then(|| text.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn yaml(text: &str) -> Mapping {
        serde_yaml::from_str(text).unwrap()
    }

    #[test]
    fn set_nested_keeps_siblings_and_prunes_empty_parents() {
        let mut map = yaml("driver: {name: Ada, mappings: {'12': Bob}}\nfiles: [a]\n");
        set_nested(&mut map, &["driver", "name"], None);
        assert_eq!(map, yaml("driver: {mappings: {'12': Bob}}\nfiles: [a]\n"));
        set_nested(&mut map, &["car", "number"], Some("52".into()));
        assert_eq!(nested_text(&map, &["car", "number"]).as_deref(), Some("52"));
        set_nested(&mut map, &["car", "number"], None);
        assert!(!map.contains_key("car"));
        set_nested(&mut map, &["event"], Some("Petit".into()));
        assert_eq!(nested_text(&map, &["event"]).as_deref(), Some("Petit"));
    }

    #[test]
    fn validation_accepts_blank_and_rejects_long_or_odd_values() {
        assert!(car_number_error("").is_none());
        assert!(car_number_error(" 52 ").is_none());
        assert!(car_number_error("7B").is_none());
        assert!(car_number_error("#52").is_some());
        assert!(car_number_error("1234567").is_some());
        assert!(text_error("Road Atlanta").is_none());
        assert!(text_error(&"x".repeat(81)).is_some());
    }

    #[test]
    fn only_folders_inside_enabled_locations_are_user_folders() {
        let root = Path::new("/data");
        let library = root.join("library");
        let mut config = Config::default();
        config.add_folder_location(&library);
        let inside = library.join("CT1").join("Run1.mp4");
        assert_eq!(
            user_library_folder(&config, &inside),
            Some(library.join("CT1"))
        );
        let outside = root.join("elsewhere").join("Run1.mp4");
        assert_eq!(user_library_folder(&config, &outside), None);
    }
}
