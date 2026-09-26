//! The title bar: where (track, event), how the pair is aligned (sync and
//! its confidence), and the two window-level commands (palette,
//! preferences). Driver and lap details live in the filmstrip below it
//! ([`super::filmstrip`]), never duplicated here.

use gpui_kit::component::{
    ActiveTheme as _, IconName, Sizable as _, StyledExt as _, TitleBar,
    button::{Button, ButtonVariants as _},
    h_flex,
    kbd::Kbd,
    searchable_list::SearchableListItem,
    select::Select,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    Context, InteractiveElement as _, IntoElement, ParentElement as _, SharedString,
    StatefulInteractiveElement as _, Styled as _, TestSupportExt as _, Window, div,
};
use omatrack_core::alignment::Strategy;
use omatrack_core::session::StrategyRequest;

use crate::actions::{OpenPreferences, TogglePalette};
use crate::keymap::WORKSPACE_CONTEXT;
use crate::workspace::Workspace;

/// One entry of the sync strategy select; `None` is the automatic choice.
#[derive(Debug, Clone, PartialEq)]
pub struct SyncOption {
    value: Option<Strategy>,
    title: SharedString,
}

impl SyncOption {
    pub fn automatic() -> Self {
        Self::automatic_resolved(None)
    }

    /// The automatic choice, naming the basis it resolved to
    /// (`Auto · Lap time %`) when one is known.
    pub fn automatic_resolved(basis: Option<SharedString>) -> Self {
        let title = match basis {
            Some(basis) => format!("Auto · {basis}").into(),
            None => "Automatic".into(),
        };
        Self { value: None, title }
    }

    pub fn strategy(strategy: Strategy) -> Self {
        Self {
            value: Some(strategy),
            title: strategy.label().into(),
        }
    }

    /// The request this option stands for.
    pub fn request(value: Option<Strategy>) -> StrategyRequest {
        value.map_or(StrategyRequest::Auto, StrategyRequest::Prefer)
    }
}

impl SearchableListItem for SyncOption {
    type Value = Option<Strategy>;

    fn title(&self) -> SharedString {
        self.title.clone()
    }

    fn value(&self) -> &Self::Value {
        &self.value
    }
}

/// `Lap time % · 0 anchors · LOW confidence.`, plus a caution under LOW
/// and why GPS was rejected, when it was: the sync badge's tooltip and
/// accessible name.
pub fn sync_summary(
    basis: &str,
    anchors: i32,
    confidence: &str,
    gps_rejection: Option<&str>,
) -> SharedString {
    let basis = if basis.is_empty() {
        "No alignment"
    } else {
        basis
    };
    let anchors = match anchors {
        1 => "1 GPS anchor".to_string(),
        n => format!("{n} GPS anchors"),
    };
    let caution = match confidence {
        "LOW" => " Deltas are approximate.",
        "NONE" => " The reference cannot be aligned.",
        _ => "",
    };
    let rejection = gps_rejection
        .map(|text| format!(" {text}"))
        .unwrap_or_default();
    format!("{basis} · {anchors} · {confidence} confidence.{caution}{rejection}").into()
}

impl Workspace {
    pub(super) fn render_header(
        &self,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let session = self.app.session.read(cx);
        let primary = session.primary();
        let track = primary
            .map(|slot| slot.info().track.clone())
            .unwrap_or_else(|| SharedString::from("Omatrack"));
        let event = primary.map(|slot| {
            let info = slot.info();
            match &info.session_name {
                Some(name) => format!("{} · {name}", info.day),
                None => info.day.to_string(),
            }
        });
        let analysis = session.analysis();
        let comparison = analysis.and_then(|analysis| analysis.comparison());
        let rejection = analysis.and_then(|analysis| super::status::gps_rejection(analysis));
        let sync = comparison.map(|comparison| {
            let confidence = comparison.confidence();
            let summary = sync_summary(
                comparison.basis(),
                comparison.alignment().gps_anchors,
                confidence,
                rejection.as_deref(),
            );
            (confidence, summary)
        });
        let theme = cx.theme();

        TitleBar::new().child(
            h_flex()
                .w_full()
                .gap_3()
                .pr_2()
                .child(
                    h_flex()
                        .id("header-track")
                        .test_support()
                        .aria_label(track.clone())
                        .gap_2()
                        .min_w_0()
                        .child(div().text_sm().font_semibold().truncate().child(track))
                        .when_some(event, |this, event| {
                            this.child(
                                div()
                                    .text_xs()
                                    .text_color(theme.muted_foreground)
                                    .truncate()
                                    .child(event),
                            )
                        }),
                )
                .child(
                    h_flex().flex_1().min_w_0().justify_center().gap_3().child(
                        h_flex()
                            .id("header-sync-group")
                            .gap_1()
                            .child(
                                // The select fills its parent; the box
                                // fixes its width so the badge sits
                                // right beside it.
                                div().w_56().child(
                                    Select::new(&self.sync_select)
                                        .id("header-sync")
                                        .xsmall()
                                        .accessibility_label("Reference sync")
                                        .title_prefix("Sync: ")
                                        .disabled(comparison.is_none()),
                                ),
                            )
                            .when_some(sync, |this, (confidence, summary)| {
                                this.child(super::status::confidence_badge(
                                    "header-confidence",
                                    confidence,
                                    summary,
                                    cx,
                                ))
                            }),
                    ),
                )
                .child(
                    h_flex()
                        .gap_1()
                        .child(
                            Button::new("header-palette")
                                .ghost()
                                .xsmall()
                                .icon(IconName::Search)
                                .label("Commands")
                                .children(Kbd::binding_for_action(
                                    &TogglePalette,
                                    Some(WORKSPACE_CONTEXT),
                                    window,
                                ))
                                .tooltip_with_action(
                                    "Command palette",
                                    &TogglePalette,
                                    Some(WORKSPACE_CONTEXT),
                                )
                                .on_click(cx.listener(|this, _, window, cx| {
                                    this.toggle_palette(window, cx);
                                })),
                        )
                        .child(
                            Button::new("header-preferences")
                                .ghost()
                                .xsmall()
                                .icon(IconName::Settings)
                                .accessibility_label("Preferences…")
                                .tooltip_with_action(
                                    "Preferences…",
                                    &OpenPreferences,
                                    Some(WORKSPACE_CONTEXT),
                                )
                                .on_click(cx.listener(|this, _, window, cx| {
                                    this.open_preferences(window, cx);
                                })),
                        ),
                ),
        )
    }
}
