//! Text labels painted by the custom trace elements (corner ruler, track
//! map, damper strip, and the static layer's value axes and callouts).
//!
//! Follows gpui-component's `plot::label` idiom: shape one line with the
//! window's text style and paint it at a baseline-free origin. Sizes are
//! steps of the one type scale ([`omatrack_ui::TypeStep`], rem-based, so labels follow
//! the application zoom) and figures are tabular, matching the div-based
//! chrome.

use gpui_kit::{
    App, FontWeight, Hsla, Pixels, Point, ShapedLine, SharedString, TextAlign, TextRun, Window,
};
use omatrack_ui::{tabular_figures, trace_figures};

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

/// Shape one line of trace-area numerals (value ticks, apex callouts): the
/// theme's monospace `family` with [`omatrack_ui::trace_figures`]. `runs`
/// colours consecutive byte lengths of `text` (the last run takes the rest).
pub(crate) fn shape_numerals(
    text: SharedString,
    family: &SharedString,
    size: Pixels,
    runs: &[(usize, Hsla)],
    window: &mut Window,
) -> ShapedLine {
    let mut font = window.text_style().font();
    font.family = family.clone();
    font.weight = FontWeight::NORMAL;
    font.features = trace_figures();
    let mut rest = text.len();
    let mut shaped: Vec<TextRun> = Vec::with_capacity(runs.len());
    for (ix, (len, color)) in runs.iter().enumerate() {
        let len = if ix + 1 == runs.len() {
            rest
        } else {
            (*len).min(rest)
        };
        rest -= len;
        shaped.push(TextRun {
            len,
            font: font.clone(),
            color: *color,
            background_color: None,
            underline: None,
            strikethrough: None,
        });
    }
    window.text_system().shape_line(text, size, &shaped, None)
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
    let _ = line.paint(origin, height, TextAlign::Left, None, window, cx);
}
