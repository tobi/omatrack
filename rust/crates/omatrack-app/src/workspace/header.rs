//! The title bar: the comparison in one line.
//!
//! Where (track, event), who against whom (the primary and reference pills: driver,
//! lap, time), the headline lap-time difference, how the pair is aligned (the sync
//! button: basis and confidence, opening the strategy menu), and the window-level
//! commands (palette, preferences).
//!
//! The pills summarise the comparison; the filmstrip below the title bar
//! ([`super::filmstrip`]) stays the place to browse and pick laps. A pill
//! click takes focus to the left surface, where laps are chosen by role.
//!
//! Width: the bar reads everything at 1440 px and wider. Narrower, it
//! drops the event line, the words around the Δ and the sync verb, and the
//! Commands label, in that order of importance; driver names truncate
//! last. Nothing wraps or clips.

use gpui_kit::AssetSource as _;
use gpui_kit::assets::IconName as AssetIcon;
use gpui_kit::component::{
    ActiveTheme as _, Disableable as _, Icon, IconName, Sizable as _, StyledExt as _, Theme,
    TitleBar,
    button::{Button, ButtonVariants as _},
    h_flex,
    kbd::Kbd,
    menu::{DropdownMenu as _, PopupMenuItem},
    searchable_list::SearchableListItem,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, Context, InteractiveElement as _, IntoElement, ParentElement as _, Role,
    SharedString, StatefulInteractiveElement as _, Styled as _, TestSupportExt as _, Window, div,
};
use omatrack_core::alignment::Strategy;
use omatrack_core::session::{CornerSource, StrategyRequest};
use omatrack_ui::{DeltaSense, DeltaText, LapRole, TypeScale as _};

use crate::actions::{FocusPanel1, OpenPreferences, SwapRoles, TogglePalette};
use crate::keymap::WORKSPACE_CONTEXT;
use crate::panels::PanelKind;
use crate::state::{RoleSlot, RoleState};
use crate::workspace::Workspace;

gpui_kit::assets::icon_assets!(HeaderIconAssets, [ArrowLeftRight]);

/// Below this window width (in rem) the title bar sheds its secondary
/// words (see the module docs): 1440 px at the default 16 px rem.
const COMPACT_BELOW_REMS: f32 = 90.;

/// The swap icon between the pills (not in the default icon bundle).
pub(super) fn swap_icon() -> Icon {
    match HeaderIconAssets.load(&AssetIcon::ArrowLeftRight.path()) {
        Ok(Some(bytes)) => Icon::default().data(&bytes),
        _ => Icon::empty(),
    }
}

/// One entry of the sync strategy menu; `None` is the automatic choice.
#[derive(Debug, Clone, PartialEq, Eq)]
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
/// and why GPS was rejected, when it was: the sync button's accessible
/// name and the footnote of its menu.
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

/// `Synced by lap time %` (the basis in running text: a leading capital
/// is lowered unless the word is an acronym, `GPS · continuous`).
pub fn sync_phrase(basis: &str) -> SharedString {
    if basis.is_empty() {
        return "Not aligned".into();
    }
    let mut chars = basis.chars();
    let first = chars.next().unwrap_or_default();
    let second = chars.next();
    let acronym = second.is_some_and(char::is_uppercase);
    let basis = if acronym {
        basis.to_string()
    } else {
        first.to_lowercase().chain(basis.chars().skip(1)).collect()
    };
    format!("Synced by {basis}").into()
}

/// `High confidence`, `Low confidence`, for the sync button.
pub fn confidence_words(confidence: &str) -> &'static str {
    match confidence {
        "HIGH" => "High confidence",
        "MED" => "Medium confidence",
        "LOW" => "Low confidence",
        _ => "Not aligned",
    }
}

/// The words after the headline lap-time Δ: `s off reference` when the
/// primary is slower, `s up on reference` when faster, `s, level` when
/// the two round to the same thousandth.
pub fn lap_time_phrase(delta: f64) -> &'static str {
    let (_, trend) = omatrack_ui::format_delta(Some(delta), 3, DeltaSense::LowerIsBetter);
    match trend {
        omatrack_ui::DeltaTrend::Loss => "s off reference",
        omatrack_ui::DeltaTrend::Gain => "s up on reference",
        omatrack_ui::DeltaTrend::Even => "s, level with reference",
    }
}

/// What a role pill shows: `Driver 1`, `L10`, `1:16.091`.
struct PillContent {
    driver: SharedString,
    lap: SharedString,
    time: SharedString,
    state: PillState,
}

#[derive(Clone, Copy, PartialEq)]
enum PillState {
    Empty,
    Loading,
    Loaded,
    Failed,
}

impl PillContent {
    fn of(slot: Option<&RoleSlot>) -> Self {
        let Some(slot) = slot else {
            return Self {
                driver: SharedString::default(),
                lap: SharedString::default(),
                time: SharedString::default(),
                state: PillState::Empty,
            };
        };
        let info = slot.info();
        Self {
            driver: info.driver.clone().unwrap_or_else(|| info.title.clone()),
            lap: info.label.clone(),
            time: info.time.clone(),
            state: match slot.state() {
                RoleState::Loading => PillState::Loading,
                RoleState::Loaded(_) => PillState::Loaded,
                RoleState::Failed(_) => PillState::Failed,
            },
        }
    }

    /// `Primary lap L10 1:16.091, Driver 1`, or `No reference lap`.
    fn spoken(&self, role: LapRole) -> SharedString {
        let state = match self.state {
            PillState::Empty => return format!("No {} lap", role.label().to_lowercase()).into(),
            PillState::Loading => " (loading)",
            PillState::Failed => " (failed)",
            PillState::Loaded => "",
        };
        format!(
            "{} lap {} {}{state}, {}",
            role.label(),
            self.lap,
            self.time,
            self.driver
        )
        .into()
    }
}

/// The role disc: the marker letter on the role colour.
fn role_disc(role: LapRole, theme: &Theme) -> impl IntoElement {
    div()
        .flex_shrink_0()
        .size_4()
        .flex()
        .items_center()
        .justify_center()
        .rounded_full()
        .bg(role.color(theme))
        .text_color(theme.background)
        .text_caption()
        .font_semibold()
        .child(role.marker())
}

impl Workspace {
    /// One role pill: disc, driver, lap and time; a click goes to the left
    /// surface to choose that role's lap.
    fn role_pill(role: LapRole, slot: Option<&RoleSlot>, cx: &mut Context<'_, Self>) -> AnyElement {
        let theme = cx.theme();
        let content = PillContent::of(slot);
        let (id, tooltip) = match role {
            LapRole::Primary => ("header-primary", "Choose the primary lap"),
            LapRole::Reference => ("header-reference", "Choose the reference lap"),
        };
        let time_color = match content.state {
            PillState::Loaded => theme.foreground,
            PillState::Failed => theme.danger,
            PillState::Loading | PillState::Empty => theme.muted_foreground,
        };
        let empty = content.state == PillState::Empty;
        Button::new(id)
            .outline()
            .small()
            .rounded_full()
            .h_7()
            .pl_1()
            .pr_3()
            .min_w_0()
            .flex_shrink(1.)
            .accessibility_label(content.spoken(role))
            .tooltip_with_action(tooltip, &FocusPanel1, Some(WORKSPACE_CONTEXT))
            .child(
                h_flex()
                    .min_w_0()
                    .gap_2()
                    .text_body()
                    .whitespace_nowrap()
                    .child(role_disc(role, theme))
                    .when(empty, |this| {
                        this.child(
                            div()
                                .text_color(theme.muted_foreground)
                                .child(format!("No {}", role.label().to_lowercase())),
                        )
                    })
                    .when(!empty, |this| {
                        this.child(
                            div()
                                .min_w_0()
                                .truncate()
                                .font_medium()
                                .text_color(theme.foreground)
                                .child(content.driver.clone()),
                        )
                        .child(
                            div()
                                .flex_shrink_0()
                                .numeric()
                                .text_color(theme.muted_foreground)
                                .child(content.lap.clone()),
                        )
                        .child(
                            div()
                                .flex_shrink_0()
                                .numeric()
                                .font_semibold()
                                .text_color(time_color)
                                .child(match content.state {
                                    PillState::Loading if content.time.is_empty() => {
                                        SharedString::from("Loading…")
                                    }
                                    _ => content.time.clone(),
                                }),
                        )
                    }),
            )
            .on_click(cx.listener(|this, _, window, cx| {
                this.focus_panel(PanelKind::Laps, window, cx);
            }))
            .into_any_element()
    }

    /// The sync button: basis and confidence, warning-tinted when deltas
    /// are approximate; it opens the strategy menu.
    fn sync_button(&self, compact: bool, cx: &mut Context<'_, Self>) -> Option<AnyElement> {
        let session = self.app.session.read(cx);
        let analysis = session.analysis()?;
        let comparison = analysis.comparison()?;
        let confidence = comparison.confidence().to_owned();
        let basis = comparison.basis().to_owned();
        let rejection = super::status::gps_rejection(analysis);
        let summary = sync_summary(
            &basis,
            comparison.alignment().gps_anchors,
            &confidence,
            rejection.as_deref(),
        );
        let low = super::status::approximate(&confidence);
        let theme = cx.theme();
        let phrase: SharedString = if compact {
            SharedString::from(basis)
        } else {
            sync_phrase(&basis)
        };
        let options = self.sync_options.clone();
        let current = self.sync_current;
        let session = self.app.session.clone();
        // Corners carried over from the reference are a sync matter: the
        // primary's GPS misses the circuit, so its zones come through the map.
        let footnote: SharedString = if analysis.corner_source() == CornerSource::Reference {
            format!(
                "{summary} Corners are placed through the reference lap: the primary’s GPS misses the circuit."
            )
            .into()
        } else {
            summary.clone()
        };
        Some(
            Button::new("header-sync")
                .small()
                .h_7()
                .outline()
                .when(low, gpui_kit::component::button::ButtonVariants::warning)
                .dropdown_caret(true)
                .accessibility_label(SharedString::from(format!("Reference sync: {summary}")))
                .when(low, |this| this.icon(IconName::TriangleAlert))
                .child(
                    h_flex()
                        .gap_2()
                        .text_body()
                        .whitespace_nowrap()
                        .child(div().text_color(theme.foreground).child(phrase))
                        .child(
                            div()
                                .when(!low, |this| this.text_color(theme.muted_foreground))
                                .child(confidence_words(&confidence)),
                        ),
                )
                .dropdown_menu(move |menu, window, _| {
                    let mut menu = menu.label("Reference sync");
                    for option in &options {
                        let value = *option.value();
                        let session = session.clone();
                        menu = menu.item(
                            PopupMenuItem::new(option.title())
                                .checked(value == current)
                                .on_click(move |_, _, cx| {
                                    session.update(cx, |session, cx| {
                                        session.set_strategy(SyncOption::request(value), cx);
                                    });
                                }),
                        );
                    }
                    menu.separator()
                        .label(footnote.clone())
                        .max_w(window.rem_size() * 24.)
                })
                .into_any_element(),
        )
    }

    #[expect(
        clippy::too_many_lines,
        reason = "Keep this declarative layout or paint pass together so element order and geometry remain reviewable."
    )]
    pub(super) fn render_header(
        &self,
        window: &mut Window,
        cx: &mut Context<'_, Self>,
    ) -> impl IntoElement {
        let compact = f32::from(window.viewport_size().width) / f32::from(window.rem_size())
            < COMPACT_BELOW_REMS;
        let session = self.app.session.read(cx);
        let primary = session.primary().cloned();
        let reference = session.reference().cloned();
        let track = primary.as_ref().map_or_else(
            || SharedString::from("Omatrack"),
            |slot| slot.info().track.clone(),
        );
        let event = primary.as_ref().map(|slot| {
            let info = slot.info();
            match &info.session_name {
                Some(name) => format!("{}, {name}", info.day),
                None => info.day.to_string(),
            }
        });
        let lap_delta = session
            .analysis()
            .and_then(|analysis| analysis.lap_time_delta());
        let can_swap = reference.is_some();

        let muted = cx.theme().muted_foreground;
        let comparison = primary.as_ref().map(|primary| {
            h_flex()
                .id("header-comparison")
                .min_w_0()
                .flex_shrink(1.)
                .gap_2()
                .child(Self::role_pill(LapRole::Primary, Some(primary), cx))
                .child(
                    h_flex()
                        .flex_shrink_0()
                        .gap_0p5()
                        .child(div().text_body().text_color(muted).child("against"))
                        .child(
                            Button::new("header-swap")
                                .ghost()
                                .xsmall()
                                .icon(self.swap_icon.clone())
                                .disabled(!can_swap)
                                .accessibility_label("Swap primary and reference")
                                .tooltip_with_action(
                                    "Swap primary and reference",
                                    &SwapRoles,
                                    Some(WORKSPACE_CONTEXT),
                                )
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.app
                                        .session
                                        .update(cx, super::super::state::session::Session::swap);
                                })),
                        ),
                )
                .child(Self::role_pill(LapRole::Reference, reference.as_ref(), cx))
        });

        let headline = lap_delta.map(|delta| {
            let theme = cx.theme();
            let (text, _) = omatrack_ui::format_delta(Some(delta), 3, DeltaSense::LowerIsBetter);
            let phrase = lap_time_phrase(delta);
            h_flex()
                .id("header-delta")
                .role(Role::Status)
                .test_support()
                .aria_label(SharedString::from(format!("Lap time {text} {phrase}")))
                .flex_shrink_0()
                .items_baseline()
                .gap_1()
                // The lap-time difference is the laps' own times: exact at
                // any alignment confidence, so it is never muted or `≈`.
                .child(
                    div()
                        .text_display()
                        .font_semibold()
                        .child(DeltaText::new(Some(delta)).decimals(3)),
                )
                .child(
                    div()
                        .text_label()
                        .text_color(theme.muted_foreground)
                        .whitespace_nowrap()
                        .child(if compact { "s" } else { phrase }),
                )
        });

        let sync = self.sync_button(compact, cx);
        let theme = cx.theme();

        TitleBar::new().h_11().child(
            h_flex()
                .w_full()
                .h_full()
                .min_w_0()
                .gap_5()
                .pr_2()
                .child(
                    h_flex()
                        .id("header-track")
                        .test_support()
                        .aria_label(track.clone())
                        .flex_shrink(1.)
                        .min_w_16()
                        .items_baseline()
                        .gap_2()
                        .child(div().text_heading().font_semibold().truncate().child(track))
                        .when_some(event.filter(|_| !compact), |this, event| {
                            this.child(
                                div()
                                    .text_label()
                                    .text_color(theme.muted_foreground)
                                    .whitespace_nowrap()
                                    .child(event),
                            )
                        }),
                )
                .children(comparison)
                .children(headline)
                .children(sync.map(|sync| div().flex_shrink_0().child(sync)))
                .child(div().flex_1())
                .child(
                    h_flex()
                        .flex_shrink_0()
                        .gap_1()
                        .child(
                            Button::new("header-palette")
                                .outline()
                                .small()
                                .h_7()
                                .icon(IconName::Search)
                                .when(!compact, |this| this.label("Commands"))
                                .accessibility_label("Commands")
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
                                .small()
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_swap_icon_is_embedded() {
        let bytes = HeaderIconAssets
            .load(&AssetIcon::ArrowLeftRight.path())
            .unwrap()
            .unwrap();
        assert!(bytes.starts_with(b"<svg"));
    }

    #[test]
    fn the_basis_reads_as_running_text() {
        assert_eq!(sync_phrase("Lap time %").as_ref(), "Synced by lap time %");
        assert_eq!(
            sync_phrase("GPS · continuous").as_ref(),
            "Synced by GPS · continuous"
        );
        assert_eq!(sync_phrase("").as_ref(), "Not aligned");
    }

    #[test]
    fn the_headline_names_its_direction() {
        assert_eq!(lap_time_phrase(2.447), "s off reference");
        assert_eq!(lap_time_phrase(-0.5), "s up on reference");
        assert_eq!(lap_time_phrase(0.0002), "s, level with reference");
    }
}
