//! Key bindings and the application-level action handlers.

use gpui_kit::{App, KeyBinding};

use crate::actions::Quit;

pub(crate) fn init(cx: &mut App) {
    cx.bind_keys([KeyBinding::new("ctrl-q", Quit, None)]);
    cx.on_action(|_: &Quit, cx| cx.quit());
}
