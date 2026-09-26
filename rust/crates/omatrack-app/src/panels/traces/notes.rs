//! What the analysis says about the focused corner, beside the lanes.
//!
//! The card sits in the right half of the lanes: a focused corner is placed
//! in the left half, so the card never covers it. The notes come from the
//! corner checks registry (`omatrack_core::corners::checks`), never from a
//! string built here; severity reads from an icon as well as its colour.

use gpui_kit::component::{
    ActiveTheme as _, Icon, IconName, Sizable as _, StyledExt as _, h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, Context, Div, InteractiveElement as _, IntoElement, ParentElement as _, Role,
    SharedString, StatefulInteractiveElement as _, Styled as _, TestSupportExt as _, div,
};
use omatrack_core::corners::NoteSeverity;
use omatrack_core::session::CornerSpeeds;
use omatrack_ui::TypeScale as _;
use omatrack_ui::{DeltaSense, DeltaText};

use super::TracesPanel;

fn speed(v: f64) -> SharedString {
    if v.is_finite() {
        format!("{v:.0}").into()
    } else {
        omatrack_ui::MISSING_VALUE.into()
    }
}

/// One row of the entry / min / exit speed grid: a role label and three
/// right-aligned tabular cells.
fn speed_row(label: SharedString, cells: [SharedString; 3], muted: bool, cx: &App) -> Div {
    let theme = cx.theme();
    h_flex()
        .gap_2()
        .child(
            div()
                .w_16()
                .flex_shrink_0()
                .text_color(theme.muted_foreground)
                .child(label),
        )
        .children(cells.into_iter().map(|cell| {
            div()
                .flex_1()
                .text_right()
                .numeric()
                .when(muted, |d| d.text_color(theme.muted_foreground))
                .child(cell)
        }))
}

fn speed_cells(speeds: &CornerSpeeds) -> [SharedString; 3] {
    [speed(speeds.entry), speed(speeds.apex), speed(speeds.exit)]
}

impl TracesPanel {
    pub(super) fn render_notes(&self, cx: &mut Context<Self>) -> Option<AnyElement> {
        let zone = self.focused_corner()?;
        let analysis = self.analysis()?;
        let row = analysis.row(zone)?;
        let theme = cx.theme();
        let name = SharedString::from(row.zone.name.clone());
        let comparing = row.reference.is_some();
        let approximate = crate::workspace::status::analysis_approximate(analysis);
        let facts = v_flex()
            .id("trace-corner-speeds")
            .test_support()
            .gap_0p5()
            .child(speed_row(
                "km/h".into(),
                ["Entry".into(), "Min".into(), "Exit".into()],
                true,
                cx,
            ))
            .child(speed_row(
                "Primary".into(),
                speed_cells(&row.speeds),
                false,
                cx,
            ))
            .when_some(row.reference_speeds.as_ref(), |el, reference| {
                el.child(speed_row(
                    "Reference".into(),
                    speed_cells(reference),
                    false,
                    cx,
                ))
            });
        let spoken = format!(
            "{name}: {} notes",
            if row.notes.is_empty() {
                "no".to_string()
            } else {
                row.notes.len().to_string()
            }
        );
        let notes = row.notes.iter().map(|note| {
            let (icon, color) = match note.severity {
                NoteSeverity::Info => (IconName::Info, theme.muted_foreground),
                NoteSeverity::Warning => (IconName::TriangleAlert, theme.warning),
                NoteSeverity::Error => (IconName::CircleX, theme.danger),
            };
            h_flex()
                .items_start()
                .gap_2()
                .child(
                    Icon::new(icon)
                        .xsmall()
                        .text_color(color)
                        .flex_shrink_0()
                        .mt_0p5(),
                )
                .child(div().flex_1().min_w_0().child(note.text.clone()))
        });
        Some(
            v_flex()
                .id("trace-corner-notes")
                .role(Role::Note)
                .aria_label(SharedString::from(spoken))
                .test_support()
                .absolute()
                .top_2()
                .right_4()
                .w_72()
                .gap_2()
                .p_3()
                .rounded(theme.radius_lg)
                .border_1()
                .border_color(theme.border)
                .bg(theme.popover)
                .text_color(theme.popover_foreground)
                .shadow_md()
                .text_xs()
                .child(
                    h_flex()
                        .gap_2()
                        .child(div().flex_1().text_sm().font_medium().child(name))
                        .when(comparing, |el| {
                            el.child(
                                DeltaText::new(Some(row.dt))
                                    .decimals(if approximate { 2 } else { 3 })
                                    .approximate(approximate)
                                    .sense(DeltaSense::LowerIsBetter)
                                    .unit("s"),
                            )
                        }),
                )
                .child(facts)
                .when(row.notes.is_empty(), |el| {
                    el.child(
                        div()
                            .text_color(theme.muted_foreground)
                            .child("Nothing to note in this corner."),
                    )
                })
                .children(notes)
                .into_any_element(),
        )
    }
}
