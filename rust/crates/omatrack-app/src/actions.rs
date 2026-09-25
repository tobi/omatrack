//! Every application command, in the `omatrack` action namespace.
//!
//! Actions are declared here and bound in `keymap.rs`, so a menu item,
//! toolbar button, command palette entry and key binding all dispatch the
//! same command.

gpui_kit::actions!(omatrack, [Quit]);
