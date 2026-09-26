//! Text labels painted by the custom trace elements (corner ruler, track
//! map, damper strip).
//!
//! Follows gpui-component's `plot::label` idiom: shape one line with the
//! window's text style and paint it at a baseline-free origin. Sizes come
//! from the window's rem, so labels follow the application zoom.

use gpui_kit::component::plot::label::truncate_text_to_width;
use gpui_kit::{
    App, FontWeight, Hsla, Pixels, Point, ShapedLine, SharedString, TextAlign, TextRun, Window,
};

/// `text_xs` of the current rem (0.75 rem).
pub(crate) fn xs(window: &Window) -> Pixels {
    window.rem_size() * 0.75
}

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

/// Shape `text` truncated with an ellipsis to `max_width` logical pixels;
/// `None` when not even the ellipsis fits.
pub(crate) fn shape_fitted(
    text: &SharedString,
    size: Pixels,
    weight: FontWeight,
    color: Hsla,
    max_width: f32,
    window: &mut Window,
) -> Option<ShapedLine> {
    if max_width < size.as_f32() * 0.6 {
        return None;
    }
    let fitted = truncate_text_to_width(text, size, max_width, window);
    let line = shape(fitted, size, weight, color, window);
    (line.width().as_f32() <= max_width + 0.5).then_some(line)
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
