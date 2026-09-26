//! Every default key binding, and the application-level action handlers.
//!
//! Contexts:
//! - `Workspace` is the window's root view; chords (`ctrl-*`) bind there so
//!   they work from any focused region, including text fields.
//! - `Workspace && !Input` (a predicate, not a context) holds the single keys (letters, digits, space,
//!   arrows, `[ ] = -`): they must never fire while a text field — the
//!   library search, the command palette query — has focus. The text field
//!   key context is gpui-base's `Input`; `!Input` checks the whole focus path.
//! - `Library` is the session library panel (Enter, Alt+Enter).
//! - `TraceEdit` is the trace panel while resizing lanes or editing corners
//!   (Ctrl+S saves, Escape cancels); being deeper than `Workspace` it wins
//!   over the workspace's Escape.
//! - `Preferences` is the full-window Preferences screen (Escape returns to
//!   the workspace); `PreferencesNav` is its section list (Up, Down).
//!
//! AGENTS.md shortcuts win over any other convention.

use gpui_kit::{App, KeyBinding};

use crate::actions::*;

/// A key context (one identifier). Contexts, not predicates, are what
/// `tooltip_with_action` and `Kbd::binding_for_action` take: they parse
/// the string with `KeyContext::parse`, which cannot parse an operator.
pub const WORKSPACE_CONTEXT: &str = "Workspace";
/// A binding *predicate*, only for `KeyBinding::new`. Never pass it where a
/// context is expected (a tooltip's shortcut lookup): `KeyContext::parse`
/// recurses forever on `&&` and aborts the process. Look the single keys
/// up in [`WORKSPACE_CONTEXT`]; the predicate matches it.
pub const SINGLE_KEY_PREDICATE: &str = "Workspace && !Input";
pub const LIBRARY_CONTEXT: &str = "Library";
pub const TRACE_EDIT_CONTEXT: &str = "TraceEdit";
pub const PREFERENCES_CONTEXT: &str = "Preferences";
pub const PREFERENCES_NAV_CONTEXT: &str = "PreferencesNav";

pub(crate) fn init(cx: &mut App) {
    let chord = Some(WORKSPACE_CONTEXT);
    let key = Some(SINGLE_KEY_PREDICATE);
    cx.bind_keys([
        KeyBinding::new("ctrl-q", Quit, None),
        KeyBinding::new("ctrl-k", TogglePalette, chord),
        KeyBinding::new("ctrl-,", OpenPreferences, chord),
        KeyBinding::new("ctrl-o", OpenFolder, chord),
        KeyBinding::new("ctrl-b", ToggleLibrary, chord),
        KeyBinding::new("ctrl-j", ToggleInspector, chord),
        KeyBinding::new("ctrl-1", FocusPanel1, chord),
        KeyBinding::new("ctrl-2", FocusPanel2, chord),
        KeyBinding::new("ctrl-3", FocusPanel3, chord),
        KeyBinding::new("ctrl-4", FocusPanel4, chord),
        KeyBinding::new("ctrl-5", FocusPanel5, chord),
        KeyBinding::new("ctrl-6", FocusPanel6, chord),
        KeyBinding::new("ctrl-=", ZoomIn, chord),
        KeyBinding::new("ctrl--", ZoomOut, chord),
        KeyBinding::new("ctrl-0", ZoomReset, chord),
        // Playback.
        KeyBinding::new("space", TogglePlay, key),
        KeyBinding::new("left", SeekBack, key),
        KeyBinding::new("right", SeekForward, key),
        KeyBinding::new("m", ToggleMute, key),
        KeyBinding::new("f", ToggleVideoFullscreen, key),
        KeyBinding::new("escape", ExitFullscreen, key),
        KeyBinding::new("1", ComposeLayout1, key),
        KeyBinding::new("2", ComposeLayout2, key),
        KeyBinding::new("3", ComposeLayout3, key),
        KeyBinding::new("4", ComposeLayout4, key),
        KeyBinding::new("5", ComposeLayout5, key),
        KeyBinding::new("s", ToggleSlowMotion, key),
        KeyBinding::new("p", ToggleContinuous, key),
        // Laps and traces.
        KeyBinding::new("x", SwapRoles, key),
        KeyBinding::new("a", ToggleCornerEdit, key),
        KeyBinding::new("h", PrevCorner, key),
        KeyBinding::new("j", NextCorner, key),
        KeyBinding::new("=", ZoomIn, key),
        KeyBinding::new("-", ZoomOut, key),
        KeyBinding::new("[", PrevLap, key),
        KeyBinding::new("]", NextLap, key),
        KeyBinding::new("t", ToggleXAxis, key),
        // Library rows.
        KeyBinding::new("enter", SetPrimary, Some(LIBRARY_CONTEXT)),
        KeyBinding::new("alt-enter", SetReference, Some(LIBRARY_CONTEXT)),
        // Modal trace editing.
        KeyBinding::new("ctrl-s", SaveEdit, Some(TRACE_EDIT_CONTEXT)),
        KeyBinding::new("escape", CancelEdit, Some(TRACE_EDIT_CONTEXT)),
        // Preferences.
        KeyBinding::new("escape", ClosePreferences, Some(PREFERENCES_CONTEXT)),
        KeyBinding::new("up", PrevPreferencesSection, Some(PREFERENCES_NAV_CONTEXT)),
        KeyBinding::new(
            "down",
            NextPreferencesSection,
            Some(PREFERENCES_NAV_CONTEXT),
        ),
    ]);
    cx.on_action(|_: &Quit, cx| crate::app::quit(cx));
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Every context the interface passes to a shortcut lookup must be a
    /// plain identifier (see [`SINGLE_KEY_PREDICATE`]).
    #[test]
    fn lookup_contexts_are_plain_identifiers() {
        for context in [
            WORKSPACE_CONTEXT,
            LIBRARY_CONTEXT,
            TRACE_EDIT_CONTEXT,
            PREFERENCES_CONTEXT,
            PREFERENCES_NAV_CONTEXT,
        ] {
            assert!(
                !context.is_empty()
                    && context
                        .chars()
                        .all(|c| c.is_ascii_alphanumeric() || c == '_'),
                "{context:?} must be a single context identifier"
            );
            assert!(gpui_kit::KeyContext::parse(context).is_ok(), "{context}");
        }
        assert!(
            SINGLE_KEY_PREDICATE.contains("&&"),
            "a predicate, not a context"
        );
    }
}
