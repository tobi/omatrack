//! Manual check of the player: `cargo run -p mpv-player --example play -- FILE`.
//!
//! Space plays/pauses, Left/Right seek 2 s, M mutes, Ctrl+Q quits. The
//! status line shows the interpolated clock and the software render cost.

use std::cell::Cell;
use std::path::PathBuf;
use std::process::ExitCode;
use std::rc::Rc;

use gpui_kit::component::{ActiveTheme as _, Root};
use gpui_kit::{
    App, AppContext as _, Context, Entity, FocusHandle, InteractiveElement as _, IntoElement,
    KeyBinding, ParentElement as _, Render, SharedString, Styled as _, Subscription, Window,
    WindowOptions, actions, div,
};
use mpv_player::{Player, PlayerOptions, VideoView};

actions!(
    play_example,
    [TogglePause, SeekForward, SeekBackward, ToggleMute, Quit]
);

impl Eq for TogglePause {}
impl Eq for SeekForward {}
impl Eq for SeekBackward {}
impl Eq for ToggleMute {}
impl Eq for Quit {}

const CONTEXT: &str = "PlayExample";
const SKIP_SECONDS: f64 = 2.0;

struct PlayWindow {
    player: Player,
    video: Entity<VideoView>,
    focus_handle: FocusHandle,
    _video_frames: Subscription,
}

impl PlayWindow {
    fn new(player: Player, window: &mut Window, cx: &mut Context<'_, Self>) -> Self {
        let letterbox = cx.theme().background;
        let source = player.frame_source();
        let video = cx.new(|cx| VideoView::new("example-video", source, letterbox, window, cx));
        // The view is notified once per display frame while playing; follow
        // it so the status line samples the clock at the same cadence.
        let video_frames = cx.observe(&video, |_, _, cx| cx.notify());
        let focus_handle = cx.focus_handle();
        focus_handle.focus(window, cx);
        Self {
            player,
            video,
            focus_handle,
            _video_frames: video_frames,
        }
    }

    fn toggle_pause(&mut self, _: &TogglePause, _: &mut Window, cx: &mut Context<'_, Self>) {
        self.player.toggle();
        cx.notify();
    }

    fn seek_forward(&mut self, _: &SeekForward, _: &mut Window, cx: &mut Context<'_, Self>) {
        self.player.seek_relative(SKIP_SECONDS);
        cx.notify();
    }

    fn seek_backward(&mut self, _: &SeekBackward, _: &mut Window, cx: &mut Context<'_, Self>) {
        self.player.seek_relative(-SKIP_SECONDS);
        cx.notify();
    }

    fn toggle_mute(&mut self, _: &ToggleMute, _: &mut Window, cx: &mut Context<'_, Self>) {
        self.player.set_mute(!self.player.state().muted);
        cx.notify();
    }

    fn status_line(&self) -> SharedString {
        let state = self.player.state();
        let position = self.player.clock().estimate(std::time::Instant::now());
        let render_stats = self.player.render_stats();
        format!(
            "{} {:.3} / {:.3} s · {}x{} · render {:.2} ms, convert {:.2} ms{}",
            if state.paused { "Paused" } else { "Playing" },
            position,
            state.duration,
            render_stats.last_size.0,
            render_stats.last_size.1,
            render_stats.average_render_ms(),
            render_stats.average_convert_ms(),
            if state.muted { " · muted" } else { "" },
        )
        .into()
    }
}

impl Render for PlayWindow {
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let theme = cx.theme();
        div()
            .id("play-window")
            .key_context(CONTEXT)
            .track_focus(&self.focus_handle)
            .on_action(cx.listener(Self::toggle_pause))
            .on_action(cx.listener(Self::seek_forward))
            .on_action(cx.listener(Self::seek_backward))
            .on_action(cx.listener(Self::toggle_mute))
            .size_full()
            .flex()
            .flex_col()
            .bg(theme.background)
            .text_color(theme.foreground)
            .child(div().flex_1().min_h_0().child(self.video.clone()))
            .child(
                div()
                    .px_3()
                    .py_1()
                    .text_xs()
                    .font_family(theme.mono_font_family.clone())
                    .text_color(theme.muted_foreground)
                    .border_t_1()
                    .border_color(theme.border)
                    .child(self.status_line()),
            )
    }
}

fn main() -> ExitCode {
    env_logger_init();
    let Some(path) = std::env::args_os().nth(1).map(PathBuf::from) else {
        eprintln!("usage: play <video file>");
        return ExitCode::from(2);
    };
    let player = match Player::new(PlayerOptions::default().client_name("mpv-player example")) {
        Ok(player) => player,
        Err(error) => {
            log::error!("cannot initialize libmpv: {error}");
            return ExitCode::FAILURE;
        }
    };
    if let Err(error) = player.load(&path) {
        log::error!("cannot open {}: {error}", path.display());
        return ExitCode::FAILURE;
    }
    let failed = Rc::new(Cell::new(false));
    let startup_failed = failed.clone();
    gpui_kit::application()
        .with_assets(gpui_kit::assets::Assets)
        .run(move |cx: &mut App| {
            gpui_kit::init(cx);
            cx.bind_keys([
                KeyBinding::new("space", TogglePause, Some(CONTEXT)),
                KeyBinding::new("right", SeekForward, Some(CONTEXT)),
                KeyBinding::new("left", SeekBackward, Some(CONTEXT)),
                KeyBinding::new("m", ToggleMute, Some(CONTEXT)),
                KeyBinding::new("ctrl-q", Quit, None),
            ]);
            cx.on_action(|_: &Quit, cx: &mut App| cx.quit());
            if let Err(error) = cx.open_window(WindowOptions::default(), |window, cx| {
                let view = cx.new(|cx| PlayWindow::new(player, window, cx));
                cx.new(|cx| Root::new(view, window, cx))
            }) {
                log::error!("cannot open window: {error}");
                startup_failed.set(true);
                cx.quit();
                return;
            }
            cx.activate(true);
        });
    if failed.get() {
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    }
}

/// Routes `log` output to stderr at warn level without an extra dependency.
fn env_logger_init() {
    struct Stderr;
    impl log::Log for Stderr {
        fn enabled(&self, metadata: &log::Metadata<'_>) -> bool {
            metadata.level() <= log::Level::Warn
        }
        fn log(&self, record: &log::Record<'_>) {
            if self.enabled(record.metadata()) {
                eprintln!("{} {}: {}", record.level(), record.target(), record.args());
            }
        }
        fn flush(&self) {}
    }
    static LOGGER: Stderr = Stderr;
    if log::set_logger(&LOGGER).is_ok() {
        log::set_max_level(log::LevelFilter::Warn);
    }
}
