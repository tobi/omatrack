//! A small color sample that identifies a series (a lap role, a channel).

use gpui_kit::component::{ActiveTheme as _, Sizable, Size};
use gpui_kit::{App, Hsla, IntoElement, RenderOnce, Styled as _, Window, div};

/// A small square of one color, sized with the component size tiers.
///
/// Decorative: the caller pairs it with a text label, because a color alone
/// must not carry meaning. The color comes from the caller and should be a
/// theme token (`cx.theme().primary`, `cx.theme().chart_2`, ...).
#[derive(IntoElement)]
pub struct Swatch {
    color: Hsla,
    size: Size,
}

impl Swatch {
    pub fn new(color: Hsla) -> Self {
        Self {
            color,
            size: Size::Small,
        }
    }
}

impl Sizable for Swatch {
    fn with_size(mut self, size: impl Into<Size>) -> Self {
        self.size = size.into();
        self
    }
}

impl RenderOnce for Swatch {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let swatch = div()
            .flex_shrink_0()
            .bg(self.color)
            .rounded(cx.theme().radius_tokens().sm);
        match self.size {
            Size::XSmall => swatch.size_1p5(),
            Size::Small => swatch.size_2(),
            Size::Large => swatch.size_3(),
            _ => swatch.size_2p5(),
        }
    }
}
