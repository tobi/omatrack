//! The type system: one interface family, a six-step size scale and
//! tabular figures for every number.
//!
//! **Family.** Interface text and numbers share the theme's interface family
//! (bundled Inter unless the desktop chose another). Numbers turn on the
//! OpenType `tnum` feature ([`TypeScale::numeric`]), so digits keep one
//! advance and columns of values align without the width and texture of a
//! monospace face next to proportional labels. The monospace family is kept
//! for what is genuinely code: paths, file contents, identifiers.
//!
//! **Scale.** Seven steps in rems, so they follow the application zoom (at
//! the default 16 px rem: 11 / 12 / 13 / 14 / 16 / 20 / 40 px):
//!
//! | Step | rem | Use |
//! |---|---|---|
//! | [`Caption`](TypeStep::Caption) | 0.6875 | axis ticks, corner ruler, map labels, secondary cell text |
//! | [`Label`](TypeStep::Label) | 0.75 | lane legends and readouts, status bar, table metadata |
//! | [`Body`](TypeStep::Body) | 0.8125 | tables, inspector, notes |
//! | [`Title`](TypeStep::Title) | 0.875 | panel and section titles |
//! | [`Heading`](TypeStep::Heading) | 1.0 | the one heading of a surface |
//! | [`Display`](TypeStep::Display) | 1.25 | the video HUD's primary figures |
//! | [`Stage`](TypeStep::Stage) | 2.5 | the one live number of the fullscreen stage (Δt) |
//!
//! **Weights.** Regular for values and prose, medium for names that
//! identify a row or lane, semibold only for a surface's heading. Never
//! bold a number to make it louder: colour carries role, size carries rank.
//!
//! Custom elements that shape their own text use [`TypeStep::size`] and
//! [`tabular_figures`] so painted labels match the div-based ones.

use std::sync::{Arc, LazyLock};

use gpui_kit::{FontFeatures, Pixels, Styled, Window, rems};

/// One step of the type scale; see the module docs.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum TypeStep {
    Caption,
    Label,
    Body,
    Title,
    Heading,
    Display,
    Stage,
}

impl TypeStep {
    /// The step as a share of the rem.
    pub const fn rems(self) -> f32 {
        match self {
            TypeStep::Caption => 0.6875,
            TypeStep::Label => 0.75,
            TypeStep::Body => 0.8125,
            TypeStep::Title => 0.875,
            TypeStep::Heading => 1.0,
            TypeStep::Display => 1.25,
            TypeStep::Stage => 2.5,
        }
    }

    /// The step in logical pixels at the window's current rem (for
    /// elements that shape text themselves).
    pub fn size(self, window: &Window) -> Pixels {
        window.rem_size() * self.rems()
    }
}

static TABULAR: LazyLock<FontFeatures> =
    LazyLock::new(|| FontFeatures(Arc::new(vec![("tnum".into(), 1)])));

/// OpenType features for numbers: tabular (fixed-advance) figures. Cheap to
/// clone (one shared allocation).
pub fn tabular_figures() -> FontFeatures {
    TABULAR.clone()
}

/// The type scale on any styled element.
pub trait TypeScale: Styled + Sized {
    /// Text size of one [`TypeStep`].
    fn text_step(self, step: TypeStep) -> Self {
        self.text_size(rems(step.rems()))
    }

    /// 11 px: axis ticks, ruler and map labels, secondary cell text.
    fn text_caption(self) -> Self {
        self.text_step(TypeStep::Caption)
    }

    /// 12 px: lane legends and readouts, status bar, table metadata.
    fn text_label(self) -> Self {
        self.text_step(TypeStep::Label)
    }

    /// 13 px: tables, inspector, notes.
    fn text_body(self) -> Self {
        self.text_step(TypeStep::Body)
    }

    /// 14 px: panel and section titles.
    fn text_title(self) -> Self {
        self.text_step(TypeStep::Title)
    }

    /// 16 px: the heading of a surface.
    fn text_heading(self) -> Self {
        self.text_step(TypeStep::Heading)
    }

    /// 20 px: the video HUD's primary figures.
    fn text_display(self) -> Self {
        self.text_step(TypeStep::Display)
    }

    /// 40 px: the fullscreen stage's live delta, the one number read from
    /// across the room.
    fn text_stage(self) -> Self {
        self.text_step(TypeStep::Stage)
    }

    /// Tabular figures in the interface family: every number, so values
    /// align in columns and do not jitter as they change.
    fn numeric(self) -> Self {
        self.font_features(tabular_figures())
    }
}

impl<T: Styled> TypeScale for T {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_scale_ascends_through_the_design_sizes() {
        let steps = [
            TypeStep::Caption,
            TypeStep::Label,
            TypeStep::Body,
            TypeStep::Title,
            TypeStep::Heading,
            TypeStep::Display,
            TypeStep::Stage,
        ];
        let px: Vec<f32> = steps.iter().map(|s| s.rems() * 16.0).collect();
        assert_eq!(px, [11.0, 12.0, 13.0, 14.0, 16.0, 20.0, 40.0]);
    }

    #[test]
    fn numbers_use_tabular_figures() {
        let features = tabular_figures();
        assert_eq!(features.tag_value_list(), &[("tnum".to_string(), 1)]);
        // One shared allocation, not one per call.
        assert!(Arc::ptr_eq(&features.0, &tabular_figures().0));
    }
}
