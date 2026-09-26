//! The lap clock of the fullscreen controls: `0:45.320 / 1:32.100`, the
//! primary lap's time at the cursor and its length.
//!
//! Its own small entity so the cursor re-renders only this text while the
//! video plays, never the panel. The label is formatted only when the
//! displayed millisecond changes.

use gpui_kit::component::ActiveTheme as _;
use gpui_kit::{
    Context, InteractiveElement as _, IntoElement, ParentElement as _, Render, Role, SharedString,
    StatefulInteractiveElement as _, Styled as _, Subscription, TestSupportExt as _, Window, div,
};
use omatrack_core::laps::format_lap_time;
use omatrack_core::monotonic::interpolate_fraction;
use omatrack_ui::{MISSING_VALUE, TypeScale as _};

use crate::state::AppState;

pub(super) struct LapClock {
    app: AppState,
    shown: Option<(i64, i64)>,
    text: SharedString,
    _subscriptions: Vec<Subscription>,
}

impl LapClock {
    pub(super) fn new(app: AppState, cx: &mut Context<'_, Self>) -> Self {
        let subscriptions = vec![
            cx.observe(&app.cursor, |_, _, cx| cx.notify()),
            cx.observe(&app.video, |_, _, cx| cx.notify()),
        ];
        Self {
            app,
            shown: None,
            text: MISSING_VALUE.into(),
            _subscriptions: subscriptions,
        }
    }

    /// The lap time at the cursor and the lap length, ms.
    #[expect(
        clippy::cast_possible_truncation,
        reason = "The displayed clock intentionally rounds seconds to integer milliseconds and formats them as f64."
    )]
    fn times(&self, cx: &gpui_kit::App) -> Option<(i64, i64)> {
        let video = self.app.video.read(cx);
        let timeline = video.timeline(crate::actions::Role::Primary)?;
        let time = &timeline.unified().time;
        let (first, last) = (*time.first()?, *time.last()?);
        let fraction = self.app.cursor.read(cx).fraction().unwrap_or(0.0);
        let at = interpolate_fraction(time, fraction.clamp(0.0, 1.0)) - first;
        Some((
            (at * 1000.0).round() as i64,
            ((last - first) * 1000.0).round() as i64,
        ))
    }
}

impl Render for LapClock {
    #[expect(
        clippy::cast_precision_loss,
        reason = "The displayed clock intentionally rounds seconds to integer milliseconds and formats them as f64."
    )]
    fn render(&mut self, _: &mut Window, cx: &mut Context<'_, Self>) -> impl IntoElement {
        let times = self.times(cx);
        if times != self.shown {
            self.shown = times;
            self.text = match times {
                Some((at, length)) => format!(
                    "{} / {}",
                    format_lap_time(at as f64),
                    format_lap_time(length as f64)
                )
                .into(),
                None => MISSING_VALUE.into(),
            };
        }
        div()
            .id("video-lap-clock")
            .role(Role::Timer)
            .aria_label(self.text.clone())
            .test_support()
            .flex_shrink_0()
            .text_label()
            .numeric()
            .text_color(cx.theme().foreground)
            .child(self.text.clone())
    }
}
