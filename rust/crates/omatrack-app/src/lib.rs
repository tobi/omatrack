//! Omatrack 2.0: the application shell.
//!
//! The shell composes feature crates into one window: a client-drawn title
//! bar, a dock workspace and a status bar, all from `gpui_kit::component`.
//! Analysis lives in `omatrack-core`, the library model in
//! `omatrack-library`, trace drawing in `omatrack-trace`, video in
//! `mpv-player`, and the theme bridge plus domain components in
//! `omatrack-ui`.

pub mod actions;
mod keymap;
mod welcome;
mod workspace;

use gpui_kit::component::{Root, TitleBar};
use gpui_kit::{App, AppContext as _, Bounds, WindowBounds, WindowHandle, WindowOptions, px, size};
use omatrack_ui::theme::{self, ThemeSource};

pub use workspace::{DOCK_AREA_ID, LAYOUT_VERSION, Workspace};

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
                    cx.quit();
                }
            })
            .detach();
            cx.activate(true);
        });
}

/// Initialize the component library, the desktop theme and the keymap.
pub fn init(cx: &mut App) {
    init_with_theme(ThemeSource::system(), cx);
}

/// [`init`] with an explicit theme source; tests pass `ThemeSource::none()`
/// or a temporary home.
pub fn init_with_theme(source: ThemeSource, cx: &mut App) {
    gpui_kit::init(cx);
    theme::install(source, cx);
    keymap::init(cx);
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
pub fn open_main_window(cx: &mut App) -> anyhow::Result<WindowHandle<Root>> {
    let options = main_window_options(cx);
    cx.open_window(options, |window, cx| {
        let workspace = cx.new(|cx| Workspace::new(window, cx));
        cx.new(|cx| Root::new(workspace, window, cx))
    })
}
