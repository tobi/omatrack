//! Application bootstrap: initialization, the main window, quitting.

use gpui_kit::component::{Root, TitleBar};
use gpui_kit::{App, AppContext as _, Bounds, WindowBounds, WindowHandle, WindowOptions, px, size};
use omatrack_ui::theme::{self, ThemeSource};

use crate::state::{AppState, StateOptions};
use crate::workspace::Workspace;

/// How the application starts: the desktop theme source and the state
/// options (paths, default library, video, startup scan).
#[derive(Debug, Clone)]
pub struct AppOptions {
    theme: ThemeSource,
    state: StateOptions,
}

impl AppOptions {
    /// The user's Omarchy theme, XDG paths, video and a startup scan.
    pub fn system() -> Self {
        Self {
            theme: ThemeSource::system(),
            state: StateOptions::system(),
        }
    }

    /// Built-in theme and explicit state options (tests, tools).
    pub fn isolated(state: StateOptions) -> Self {
        Self {
            theme: ThemeSource::none(),
            state,
        }
    }

    #[must_use]
    pub fn theme(mut self, theme: ThemeSource) -> Self {
        self.theme = theme;
        self
    }

    #[must_use]
    pub fn state(mut self, state: StateOptions) -> Self {
        self.state = state;
        self
    }
}

/// Start the application: open the main window and run until quit.
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("warn")).init();
    gpui_kit::application()
        .with_assets(gpui_kit::assets::Assets)
        .run(|cx| {
            init(cx);
            if let Err(error) = open_main_window(cx) {
                log::error!("Cannot open the main window: {error:#}");
                cx.quit();
                return;
            }
            cx.on_window_closed(|cx, _| {
                if cx.windows().is_empty() {
                    quit(cx);
                }
            })
            .detach();
            cx.activate(true);
        });
}

/// Initialize with the user's desktop theme and files.
pub fn init(cx: &mut App) {
    init_with(AppOptions::system(), cx);
}

/// Initialize the component library, the theme, the keymap, the command
/// registry, the dock panels and the application state.
pub fn init_with(options: AppOptions, cx: &mut App) {
    gpui_kit::init(cx);
    theme::install(options.theme, cx);
    crate::keymap::init(cx);
    crate::commands::init(cx);
    crate::panels::init(cx);
    AppState::install(options.state, cx);
}

/// The main window's options: client-drawn title bar, 1600x1000 centered,
/// never smaller than 1024x640.
///
/// Window geometry is a platform boundary, so these are pixels, not rems.
pub fn main_window_options(cx: &App) -> WindowOptions {
    WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
            None,
            size(px(1600.), px(1000.)),
            cx,
        ))),
        window_min_size: Some(size(px(1024.), px(640.))),
        app_id: Some("omatrack".into()),
        ..TitleBar::window_options()
    }
}

/// Open the main window with [`Workspace`] under the component `Root`.
/// Requires [`init`] (or [`init_with`]) first.
///
/// # Errors
/// Returns the platform error if GPUI cannot create the main window.
pub fn open_main_window(cx: &mut App) -> anyhow::Result<WindowHandle<Root>> {
    let options = main_window_options(cx);
    cx.open_window(options, |window, cx| {
        let workspace = cx.new(|cx| Workspace::new(window, cx));
        cx.new(|cx| Root::new(workspace, window, cx))
    })
}

/// Save pending preferences and quit.
pub(crate) fn quit(cx: &mut App) {
    if let Some(state) = AppState::try_global(cx) {
        let preferences = state.preferences.clone();
        preferences.update(cx, super::state::preferences::Preferences::flush);
    }
    cx.quit();
}
