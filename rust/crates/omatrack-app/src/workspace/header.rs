//! The title bar: what is being compared, how it is aligned, and the two
//! window-level commands (palette, preferences).

use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, IconName, Sizable as _, StyledExt as _, TitleBar,
    button::{Button, ButtonVariants as _},
    h_flex,
    kbd::Kbd,
    searchable_list::SearchableListItem,
    select::Select,
    tag::Tag,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    Context, InteractiveElement as _, IntoElement, ParentElement as _, SharedString,
    StatefulInteractiveElement as _, Styled as _, TestSupportExt as _, Window, div,
};
use omatrack_core::alignment::Strategy;
use omatrack_core::session::StrategyRequest;
use omatrack_ui::{LapRole, RoleChip};

use crate::actions::{OpenPreferences, SwapRoles, TogglePalette};
use crate::keymap::WORKSPACE_CONTEXT;
use crate::state::{RoleSlot, RoleState};
use crate::workspace::Workspace;

/// One entry of the sync strategy select; `None` is the automatic choice.
#[derive(Debug, Clone, PartialEq)]
pub struct SyncOption {
    value: Option<Strategy>,
    title: SharedString,
}

impl SyncOption {
    pub fn automatic() -> Self {
        Self {
            value: None,
            title: "Automatic".into(),
        }
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

/// `Primary lap L8 1:13.644 · TL`, for the chip's accessible name.
fn chip_label(role: LapRole, slot: Option<&RoleSlot>) -> SharedString {
    match slot {
        Some(slot) => {
            let info = slot.info();
            let state = match slot.state() {
                RoleState::Loading => " (loading)",
                RoleState::Failed(_) => " (failed)",
                RoleState::Loaded(_) => "",
            };
            format!(
                "{} lap {} {}{}{}",
                role.label(),
                info.label,
                info.time,
                info.driver
                    .as_ref()
                    .map(|driver| format!(" · {driver}"))
                    .unwrap_or_default(),
                state
            )
            .into()
        }
        None => format!("No {} lap", role.label().to_lowercase()).into(),
    }
}

fn role_chip(role: LapRole, slot: Option<&RoleSlot>) -> impl IntoElement {
    let id = match role {
        LapRole::Primary => "header-primary",
        LapRole::Reference => "header-reference",
    };
    let chip = match slot {
        Some(slot) => {
            let info = slot.info();
            let mut chip = RoleChip::new(format!("{id}-chip"), role, info.label.clone())
                .time(info.time.clone());
            if let Some(driver) = &info.driver {
                chip = chip.driver(driver.clone());
            }
            chip
        }
        None => RoleChip::new(format!("{id}-chip"), role, "—"),
    };
    div()
        .id(id)
        .test_support()
        .aria_label(chip_label(role, slot))
        .child(chip)
}

impl Workspace {
    pub(super) fn render_header(
        &self,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let session = self.app.session.read(cx);
        let primary = session.primary();
        let reference = session.reference();
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
        let confidence = comparison.map(|comparison| comparison.confidence());
        let can_swap = primary.is_some() && reference.is_some();
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
                    h_flex()
                        .flex_1()
                        .justify_center()
                        .gap_1()
                        .child(role_chip(LapRole::Primary, primary))
                        .child(
                            Button::new("header-swap")
                                .ghost()
                                .xsmall()
                                .icon(IconName::Replace)
                                .disabled(!can_swap)
                                .accessibility_label("Swap primary and reference")
                                .tooltip_with_action(
                                    "Swap primary and reference",
                                    &SwapRoles,
                                    Some(WORKSPACE_CONTEXT),
                                )
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.app.session.update(cx, |session, cx| session.swap(cx));
                                })),
                        )
                        .child(role_chip(LapRole::Reference, reference))
                        .child(
                            Select::new(&self.sync_select)
                                .id("header-sync")
                                .xsmall()
                                .w_40()
                                .accessibility_label("Reference sync")
                                .title_prefix("Sync: ")
                                .disabled(comparison.is_none()),
                        )
                        .when_some(confidence, |this, confidence| {
                            let tag = match confidence {
                                "HIGH" => Tag::success(),
                                "LOW" => Tag::warning(),
                                _ => Tag::secondary(),
                            };
                            this.child(
                                div()
                                    .id("header-confidence")
                                    .test_support()
                                    .aria_label(SharedString::from(format!(
                                        "Sync confidence {confidence}"
                                    )))
                                    .child(tag.xsmall().child(confidence)),
                            )
                        }),
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
