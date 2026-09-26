//! Text labels painted by the custom trace elements (corner ruler, track
//! map, damper strip).
//!
//! Follows gpui-component's `plot::label` idiom: shape one line with the
//! window's text style and paint it at a baseline-free origin. Sizes are
//! steps of the one type scale ([`omatrack_ui::TypeStep`], rem-based, so labels follow
//! the application zoom) and figures are tabular, matching the div-based
//! chrome.

use gpui_kit::{
    App, FontWeight, Hsla, Pixels, Point, ShapedLine, SharedString, TextAlign, TextRun, Window,
};
use omatrack_ui::tabular_figures;

/// Shape one line of `text` with the window's font at `size` and `weight`.
pub(crate) fn shape(
    text: SharedString,
    size: Pixels,
    weight: FontWeight,
    color: Hsla,
    window: &mut Window,
) -> ShapedLine {
    let mut font = window.text_style().font();
    font.weight = weight;
    font.features = tabular_figures();
    let run = TextRun {
        len: text.len(),
        font,
        color,
        background_color: None,
        underline: None,
        strikethrough: None,
    };
    window.text_system().shape_line(text, size, &[run], None)
}

/// Paint a shaped line with its top-left at `origin`, `height` tall.
pub(crate) fn paint(
    line: &ShapedLine,
    origin: Point<Pixels>,
    height: Pixels,
    window: &mut Window,
    cx: &mut App,
) {
    // Painting only fails when the glyph atlas cannot grow; a missing label
    // is the right degradation for a chart annotation.
    if let Err(error) = line.paint(origin, height, TextAlign::Left, None, window, cx) {
        log::warn!("could not paint trace label: {error}");
    }
}
