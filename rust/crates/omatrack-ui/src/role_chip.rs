//! The chip that names one side of a comparison: which lap is the primary and
//! which the reference.

use std::rc::Rc;

use gpui_kit::component::{
    ActiveTheme as _, Selectable, Sizable as _, StyledExt as _, Theme,
    button::{Button, ButtonVariants as _},
    h_flex,
};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, ClickEvent, ElementId, Hsla, InteractiveElement as _, IntoElement, ParentElement as _,
    RenderOnce, SharedString, Styled as _, Window, div,
};

use crate::{Readout, Swatch};

/// The two sides of a lap comparison.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum LapRole {
    /// The lap being studied; drawn in the theme's `primary` (the Omarchy
    /// accent).
    Primary,
    /// The lap it is compared against; drawn in the theme's `warning`.
    Reference,
}

impl LapRole {
    /// The series color for this role.
    pub fn color(self, theme: &Theme) -> Hsla {
        match self {
            Self::Primary => theme.primary,
            Self::Reference => theme.warning,
        }
    }

    /// The single-letter marker used beside the color (`P` / `R`).
    pub fn marker(self) -> &'static str {
        match self {
            Self::Primary => "P",
            Self::Reference => "R",
        }
    }

    /// The role's name in interface copy.
    pub fn label(self) -> &'static str {
        match self {
            Self::Primary => "Primary",
            Self::Reference => "Reference",
        }
    }
}

type ClickHandler = Rc<dyn Fn(&ClickEvent, &mut Window, &mut App)>;

/// A compact chip naming a lap in a comparison role:
/// swatch, role marker, driver, lap label and lap time.
///
/// With [`RoleChip::on_click`] it is a ghost [`Button`], so hover, pressed,
/// keyboard focus and the selected state come from the component system;
/// without a handler it is plain, non-interactive content.
#[derive(IntoElement)]
pub struct RoleChip {
    id: ElementId,
    role: LapRole,
    driver: Option<SharedString>,
    lap: SharedString,
    time: Option<SharedString>,
    selected: bool,
    on_click: Option<ClickHandler>,
}

impl RoleChip {
    /// `id` should be domain-derived (for example `"role-chip-primary"`).
    pub fn new(id: impl Into<ElementId>, role: LapRole, lap: impl Into<SharedString>) -> Self {
        Self {
            id: id.into(),
            role,
            driver: None,
            lap: lap.into(),
            time: None,
            selected: false,
            on_click: None,
        }
    }

    pub fn driver(mut self, driver: impl Into<SharedString>) -> Self {
        self.driver = Some(driver.into());
        self
    }

    /// The lap time, preformatted (`1:13.644`).
    pub fn time(mut self, time: impl Into<SharedString>) -> Self {
        self.time = Some(time.into());
        self
    }

    pub fn on_click(
        mut self,
        handler: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_click = Some(Rc::new(handler));
        self
    }

    pub fn role(&self) -> LapRole {
        self.role
    }
}

impl Selectable for RoleChip {
    fn selected(mut self, selected: bool) -> Self {
        self.selected = selected;
        self
    }

    fn is_selected(&self) -> bool {
        self.selected
    }
}

impl RenderOnce for RoleChip {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let theme = cx.theme();
        let content = h_flex()
            .gap_1p5()
            .text_sm()
            .whitespace_nowrap()
            .child(Swatch::new(self.role.color(theme)))
            .child(
                div()
                    .text_color(theme.muted_foreground)
                    .font_family(theme.mono_font_family.clone())
                    .child(self.role.marker()),
            )
            .when_some(self.driver, |this, driver| {
                this.child(div().font_medium().child(driver))
            })
            .child(
                div()
                    .text_color(theme.muted_foreground)
                    .child(self.lap.clone()),
            )
            .when_some(self.time, |this, time| this.child(Readout::new(time)));

        let accessible = SharedString::from(format!("{} lap {}", self.role.label(), self.lap));
        match self.on_click {
            Some(handler) => Button::new(self.id)
                .ghost()
                .small()
                .selected(self.selected)
                .tooltip(self.role.label())
                .accessibility_label(accessible)
                .child(content)
                .on_click(move |event, window, cx| handler(event, window, cx))
                .into_any_element(),
            None => div()
                .id(self.id)
                .px_2()
                .py_0p5()
                .child(content)
                .into_any_element(),
        }
    }
}
