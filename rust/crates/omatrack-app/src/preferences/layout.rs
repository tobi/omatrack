//! The building blocks of a Preferences page: a titled page, grouped cards
//! (the kit's [`GroupBox`]) and labelled setting rows, title and
//! description on the left, the control on the right.

use gpui_kit::component::{
    ActiveTheme as _, StyledExt as _,
    group_box::{GroupBox, GroupBoxVariants as _},
    h_flex, v_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    AnyElement, App, Div, ElementId, InteractiveElement as _, IntoElement, ParentElement as _,
    SharedString, StyleRefinement, Styled as _, TestSupportExt as _, div, rems,
};

use super::PreferencesSection;

/// The width of a row's select or text control, so the controls of a card
/// line up on one right edge.
pub(super) const CONTROL_WIDTH: f32 = 14.;

/// A section's page: its title and description, then its cards.
pub(super) fn page(
    section: PreferencesSection,
    cards: impl IntoIterator<Item = AnyElement>,
    cx: &App,
) -> AnyElement {
    let theme = cx.theme();
    v_flex()
        .id(section.page_id())
        .test_support()
        .w_full()
        .gap_6()
        .child(
            v_flex()
                .gap_1()
                .child(
                    div()
                        .text_xl()
                        .font_semibold()
                        .text_color(theme.foreground)
                        .child(section.title()),
                )
                .child(
                    div()
                        .text_sm()
                        .text_color(theme.muted_foreground)
                        .child(section.description()),
                ),
        )
        .children(cards)
        .into_any_element()
}

/// A titled group of rows, divided by hairlines.
pub(super) fn card(
    id: &'static str,
    title: &'static str,
    rows: impl IntoIterator<Item = AnyElement>,
    cx: &App,
) -> AnyElement {
    let border = cx.theme().border;
    GroupBox::new()
        .id(id)
        .fill()
        .gap_2()
        .title(title)
        .title_style(StyleRefinement::default().text_sm().font_medium())
        .content_style(
            StyleRefinement::default()
                .p_0()
                .gap_0()
                .border_1()
                .border_color(border),
        )
        .children(rows.into_iter().enumerate().map(|(ix, row)| {
            div()
                .when(ix > 0, |this| this.border_t_1().border_color(border))
                .child(row)
        }))
        .into_any_element()
}

/// One setting: `title` and an optional `description` on the left, the
/// control on the right.
pub(super) fn row(
    title: impl Into<SharedString>,
    description: Option<&'static str>,
    control: impl IntoElement,
    cx: &App,
) -> AnyElement {
    row_with(
        title.into(),
        description.map(SharedString::from),
        control,
        cx,
    )
    .into_any_element()
}

/// [`row`] with an owned description and an element id.
pub(super) fn row_with(
    title: SharedString,
    description: Option<SharedString>,
    control: impl IntoElement,
    cx: &App,
) -> Div {
    let theme = cx.theme();
    h_flex()
        .w_full()
        .px_4()
        .py_3()
        .gap_6()
        .child(
            v_flex()
                .flex_1()
                .min_w_0()
                .gap_0p5()
                .child(
                    div()
                        .text_sm()
                        .font_medium()
                        .text_color(theme.foreground)
                        .child(title),
                )
                .when_some(description, |this, description| {
                    this.child(
                        div()
                            .text_sm()
                            .text_color(theme.muted_foreground)
                            .child(description),
                    )
                }),
        )
        .child(h_flex().flex_none().gap_2().child(control))
}

/// A select or other sized control, at the width all rows share.
pub(super) fn control(child: impl IntoElement) -> Div {
    div().w(rems(CONTROL_WIDTH)).child(child)
}

/// A read-only value in a row, right-aligned where a control would be.
pub(super) fn value(text: impl Into<SharedString>, mono: bool, cx: &App) -> Div {
    let theme = cx.theme();
    div()
        .max_w(rems(24.))
        .text_sm()
        .text_color(theme.muted_foreground)
        .truncate()
        .when(mono, |this| {
            this.font_family(theme.mono_font_family.clone())
        })
        .child(text.into())
}

/// A muted line inside a card, for an empty list.
pub(super) fn note(id: impl Into<ElementId>, text: &'static str, cx: &App) -> AnyElement {
    div()
        .id(id.into())
        .px_4()
        .py_3()
        .text_sm()
        .text_color(cx.theme().muted_foreground)
        .child(text)
        .into_any_element()
}
