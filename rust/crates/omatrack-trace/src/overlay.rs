//! The cursor overlay and pointer surface of a trace stack.
//!
//! Everything that moves with the pointer or the playhead lives here and
//! nowhere else: corner zones (so corner drags never touch the static
//! layer), focus dimming, the selection band, the hover line, the cursor
//! line and its crosshair dots, and the overflow scroll indicator. It is a
//! handful of quads per frame, rebuilt with the stack whenever
//! [`crate::state::CursorState`] changes.
//!
//! gpui-component's `CrossLine`/`Dot` hover markers are `div` trees laid out
//! per frame; the overlay paints the same shapes as quads to keep a cursor
//! frame to a single element with no layout pass.
//!
//! The element also owns the plot hitbox and forwards pointer input to the
//! stack's gesture machine, including moves and releases outside the plot
//! while a drag is in progress.

use std::cell::Cell;
use std::rc::Rc;
use std::sync::Arc;

use gpui_kit::{
    App, Bounds, CursorStyle, DispatchPhase, Element, ElementId, FontWeight, GlobalElementId,
    Hitbox, HitboxBehavior, InspectorElementId, IntoElement, LayoutId, MouseButton, MouseDownEvent,
    MouseMoveEvent, MouseUpEvent, Pixels, Point, Position, ScrollDelta, ScrollWheelEvent, Style,
    WeakEntity, Window, fill, point, px, relative, size,
};
use omatrack_ui::TypeStep;

use crate::interaction::{GestureCursor, KeyModifiers, PointerButton, WheelDelta};
use crate::label;
use crate::layout::LaneLayout;
use crate::palette::TracePalette;
use crate::scale::Viewport;
use crate::scene::{CornerBand, LaneStyles, TraceLayers, TraceScene};
use crate::stack::TraceStack;
use crate::state::Selection;
use crate::static_layer::event_color;

/// Width of the cursor's cap at the plot's top edge, logical pixels.
const CURSOR_CAP: f32 = 7.0;

/// Pointer distance within which an event shows its label, logical px.
pub const EVENT_HOVER_RADIUS: f64 = 6.0;
/// At most this many event labels at once (one per lane).
const EVENT_LABELS_MAX: usize = 6;

/// Everything one overlay frame paints, snapshotted by the stack's render.
pub(crate) struct TraceOverlay {
    pub stack: WeakEntity<TraceStack>,
    pub scene: Arc<TraceScene>,
    pub layout: Arc<LaneLayout>,
    pub styles: Arc<LaneStyles>,
    pub corners: Arc<[CornerBand]>,
    pub viewport: Viewport,
    pub cursor: Option<f64>,
    pub hover: Option<f64>,
    pub selection: Option<Selection>,
    pub focus: Option<Selection>,
    pub editing_corners: bool,
    pub layers: TraceLayers,
    pub palette: TracePalette,
    pub gesture: GestureCursor,
    /// Plot bounds seen by the stack's last layout; a change is reported.
    pub measured: Rc<Cell<Option<Bounds<Pixels>>>>,
}

impl IntoElement for TraceOverlay {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

fn modifiers(m: &gpui_kit::Modifiers) -> KeyModifiers {
    KeyModifiers::new(m.shift, m.control, m.alt, m.platform)
}

fn button(button: MouseButton) -> Option<PointerButton> {
    match button {
        MouseButton::Left => Some(PointerButton::Left),
        MouseButton::Middle => Some(PointerButton::Middle),
        MouseButton::Right => Some(PointerButton::Right),
        _ => None,
    }
}

impl Element for TraceOverlay {
    type RequestLayoutState = ();
    type PrepaintState = Hitbox;

    fn id(&self) -> Option<ElementId> {
        Some("trace-overlay".into())
    }

    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }

    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        let mut style = Style {
            position: Position::Absolute,
            ..Style::default()
        };
        style.inset.top = px(0.).into();
        style.inset.left = px(0.).into();
        style.size.width = relative(1.).into();
        style.size.height = relative(1.).into();
        (window.request_layout(style, [], cx), ())
    }

    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) -> Hitbox {
        if self.measured.get() != Some(bounds) {
            self.measured.set(Some(bounds));
            let stack = self.stack.clone();
            window.defer(cx, move |_, cx| {
                stack
                    .update(cx, |stack, cx| stack.set_plot_bounds(bounds, cx))
                    .ok();
            });
        }
        window.insert_hitbox(bounds, HitboxBehavior::Normal)
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        hitbox: &mut Hitbox,
        window: &mut Window,
        cx: &mut App,
    ) {
        self.paint_marks(bounds, window);
        if self.layers.events
            && let Some(hover) = self.hover
        {
            self.paint_event_labels(bounds, hover, window, cx);
        }
        let style = match self.gesture {
            GestureCursor::Default => CursorStyle::Arrow,
            GestureCursor::Crosshair => CursorStyle::Crosshair,
            GestureCursor::Grabbing => CursorStyle::ClosedHand,
            GestureCursor::ResizeColumn => CursorStyle::ResizeLeftRight,
            GestureCursor::ResizeRow => CursorStyle::ResizeUpDown,
        };
        window.set_cursor_style(style, hitbox);
        self.register_input(bounds, hitbox.clone(), window);
    }
}

impl TraceOverlay {
    fn paint_marks(&self, bounds: Bounds<Pixels>, window: &mut Window) {
        let palette = &self.palette;
        let width = bounds.size.width.as_f32();
        let height = bounds.size.height.as_f32();
        if width <= 0.0 || height <= 0.0 {
            return;
        }
        let x_for = |fraction: f64| -> f32 {
            self.viewport.x_for_fraction(fraction, 0.0, width as f64) as f32
        };
        let column = |window: &mut Window, left: f32, right: f32, color| {
            let left = left.clamp(0.0, width);
            let right = right.clamp(0.0, width);
            if right > left {
                window.paint_quad(fill(
                    Bounds::new(
                        bounds.origin + point(px(left), px(0.)),
                        size(px(right - left), px(height)),
                    ),
                    color,
                ));
            }
        };
        let vline = |window: &mut Window, x: f32, color| {
            if (0.0..=width).contains(&x) {
                window.paint_quad(fill(
                    Bounds::new(
                        bounds.origin + point(px(x.round() - 0.5), px(0.)),
                        size(px(1.), px(height)),
                    ),
                    color,
                ));
            }
        };

        // Corner zones: a quiet tint through every lane, a pixel short of
        // the zone end so neighbours read as separate; edges only as grips
        // while editing.
        for corner in self.corners.iter() {
            let (x1, x2) = (x_for(corner.start), x_for(corner.end));
            if x2 <= 0.0 || x1 >= width {
                continue;
            }
            column(window, x1, (x2 - 1.0).max(x1 + 1.0), palette.corner_band);
            if self.editing_corners {
                vline(window, x1, palette.reference);
                vline(window, x2, palette.reference);
            }
        }

        // Outside a focused corner the traces recede.
        if let Some(focus) = self.focus {
            let (x1, x2) = (x_for(focus.start), x_for(focus.end));
            column(window, 0.0, x1, palette.dim);
            column(window, x2, width, palette.dim);
        }

        if let Some(selection) = self.selection {
            column(
                window,
                x_for(selection.start),
                x_for(selection.end),
                palette.selection,
            );
        }

        // The hover line, then the cursor on top: a full-height hairline in
        // the foreground with a cap at the top edge so it reads against dense
        // traces. The crosshair dots sit where the lane readouts are taken
        // (hover first, else the cursor), so dots and values always agree.
        let hover_x = self
            .hover
            .filter(|hover| Some(*hover) != self.cursor)
            .map(x_for);
        if let Some(x) = hover_x {
            vline(window, x, palette.hover);
        }
        if let Some(cursor) = self.cursor {
            let x = x_for(cursor);
            vline(window, x, palette.cursor);
            if (0.0..=width).contains(&x) {
                window.paint_quad(fill(
                    Bounds::new(
                        bounds.origin + point(px(x.round() - CURSOR_CAP / 2.0), px(0.)),
                        size(px(CURSOR_CAP), px(2.)),
                    ),
                    palette.cursor,
                ));
            }
        }
        match (self.hover, hover_x) {
            (Some(hover), Some(x)) => self.paint_dots(bounds, x, hover, window),
            _ => {
                if let Some(cursor) = self.cursor {
                    self.paint_dots(bounds, x_for(cursor), cursor, window);
                }
            }
        }

        // Overflow indicator at the scroll region's trailing edge.
        let layout = &self.layout;
        if layout.overflows() && layout.scroll_viewport > 0.0 {
            let track = layout.scroll_viewport as f32;
            let thumb = (track * (layout.scroll_viewport / layout.scroll_content) as f32).max(16.0);
            let top = layout.pinned_height as f32
                + (track - thumb) * (layout.scroll / layout.max_scroll()) as f32;
            window.paint_quad(
                fill(
                    Bounds::new(
                        bounds.origin + point(px(width - 4.0), px(top)),
                        size(px(3.), px(thumb)),
                    ),
                    palette.label.opacity(0.45),
                )
                .corner_radii(px(1.5)),
            );
        }
    }

    /// Labels of the events under the pointer (within
    /// [`EVENT_HOVER_RADIUS`]), each on its own lane beside its flag, on
    /// the popover surface. Only a few labels exist at once; shaping reuses
    /// the label's `SharedString` (no formatting per frame).
    fn paint_event_labels(
        &self,
        bounds: Bounds<Pixels>,
        hover: f64,
        window: &mut Window,
        cx: &mut App,
    ) {
        let width = bounds.size.width.as_f32() as f64;
        if width <= 0.0 {
            return;
        }
        let events = self.scene.events();
        let radius = EVENT_HOVER_RADIUS * self.viewport.span() / width;
        let from = events.partition_point(|mark| mark.fraction < hover - radius);
        let size_text = TypeStep::Caption.size(window);
        let line_height = size_text * 1.3;
        let height = bounds.size.height.as_f32();
        let pinned = self.layout.pinned_height as f32;
        // One label per lane: the mark nearest the pointer.
        let mut drawn = 0usize;
        for slot in &self.layout.slots {
            if drawn >= EVENT_LABELS_MAX {
                break;
            }
            let (top, bottom) = if slot.pinned {
                (0.0, pinned)
            } else {
                (pinned, height)
            };
            let y = slot.y as f32;
            if y < top || y + line_height.as_f32() + 6.0 > bottom {
                continue;
            }
            let nearest = events[from..]
                .iter()
                .take_while(|mark| mark.fraction <= hover + radius)
                .filter(|mark| {
                    slot.channels().any(|index| {
                        self.scene
                            .lanes
                            .get(index)
                            .is_some_and(|lane| lane.key == mark.channel)
                    })
                })
                .min_by(|a, b| {
                    (a.fraction - hover)
                        .abs()
                        .total_cmp(&(b.fraction - hover).abs())
                });
            let Some(mark) = nearest else {
                continue;
            };
            drawn += 1;
            let color = event_color(&self.palette, mark.kind, mark.reference);
            let line = label::shape(
                mark.label.clone(),
                size_text,
                FontWeight::NORMAL,
                self.palette.foreground,
                window,
            );
            let pad = 4.0;
            let box_width = line.width.as_f32() + pad * 2.0 + 6.0;
            let box_height = line_height.as_f32() + 2.0;
            let x = self.viewport.x_for_fraction(mark.fraction, 0.0, width) as f32;
            // Right of the flag, flipped left near the plot's right edge.
            let left = if x + 6.0 + box_width > width as f32 {
                (x - 6.0 - box_width).max(0.0)
            } else {
                x + 6.0
            };
            let origin = bounds.origin + point(px(left), px(y + 3.0));
            window.paint_quad(
                fill(
                    Bounds::new(origin, size(px(box_width), px(box_height))),
                    self.palette.popover,
                )
                .corner_radii(px(3.))
                .border_widths(px(1.))
                .border_color(self.palette.grid_strong),
            );
            // The role swatch, then the text.
            window.paint_quad(fill(
                Bounds::new(
                    origin + point(px(pad), px(box_height * 0.5 - 2.0)),
                    size(px(4.), px(4.)),
                ),
                color,
            ));
            label::paint(
                &line,
                origin + point(px(pad + 6.0), px(1.)),
                line_height,
                window,
                cx,
            );
        }
    }

    fn paint_dots(&self, bounds: Bounds<Pixels>, x: f32, cursor: f64, window: &mut Window) {
        if !(0.0..=bounds.size.width.as_f32()).contains(&x) {
            return;
        }
        let map = self.scene.map.as_deref();
        let height = bounds.size.height.as_f32();
        for slot in &self.layout.slots {
            let (top, bottom) = if slot.pinned {
                (0.0, self.layout.pinned_height as f32)
            } else {
                (self.layout.pinned_height as f32, height)
            };
            for (position, index) in slot.channels().enumerate() {
                let Some(series) = self.scene.lanes.get(index) else {
                    continue;
                };
                let readout = series.readout(cursor, map);
                let range = series.range_in(self.viewport);
                let y_for = |value: f64| -> f32 {
                    let t = ((value - range.min) / range.span()).clamp(0.0, 1.0);
                    slot.y as f32 + 1.0 + (slot.height as f32 - 2.0) * (1.0 - t as f32)
                };
                let style = self.styles.get(&series.key);
                let (primary, reference) =
                    self.palette
                        .channel_colors(&series.key, position == 0, &style);
                for (value, color) in [(readout.reference, reference), (readout.primary, primary)] {
                    if !value.is_finite() {
                        continue;
                    }
                    let y = y_for(value);
                    if y < top || y > bottom {
                        continue;
                    }
                    let radius = 3.0;
                    window.paint_quad(
                        fill(
                            Bounds::new(
                                bounds.origin + point(px(x - radius), px(y - radius)),
                                size(px(radius * 2.0), px(radius * 2.0)),
                            ),
                            color,
                        )
                        .corner_radii(px(radius))
                        .border_widths(px(1.))
                        .border_color(self.palette.background),
                    );
                }
            }
        }
    }

    fn register_input(&self, bounds: Bounds<Pixels>, hitbox: Hitbox, window: &mut Window) {
        let origin = bounds.origin;
        let local = move |position: Point<Pixels>| -> (f64, f64) {
            let p = position - origin;
            (p.x.as_f32() as f64, p.y.as_f32() as f64)
        };

        let stack = self.stack.clone();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseDownEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble || !hit.is_hovered(window) {
                return;
            }
            let Some(button) = button(event.button) else {
                return;
            };
            let (x, y) = local(event.position);
            stack
                .update(cx, |stack, cx| {
                    stack.pointer_down(x, y, button, event.click_count, event.position, window, cx)
                })
                .ok();
            // The press is fully owned by the trace gesture. Letting it bubble
            // lets ancestors move focus again, and every GPUI focus change
            // refreshes the window (a static-layer repaint per press).
            cx.stop_propagation();
        });

        let stack = self.stack.clone();
        let hit = hitbox.clone();
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble {
                return;
            }
            let (x, y) = local(event.position);
            let inside = hit.is_hovered(window);
            stack
                .update(cx, |stack, cx| stack.pointer_move(x, y, inside, cx))
                .ok();
        });

        let stack = self.stack.clone();
        window.on_mouse_event(move |event: &MouseUpEvent, phase, _, cx| {
            if phase != DispatchPhase::Bubble {
                return;
            }
            let Some(button) = button(event.button) else {
                return;
            };
            let (x, _) = local(event.position);
            stack
                .update(cx, |stack, cx| stack.pointer_up(x, button, cx))
                .ok();
        });

        let stack = self.stack.clone();
        window.on_mouse_event(move |event: &ScrollWheelEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble || !hitbox.should_handle_scroll(window) {
                return;
            }
            let (x, _) = local(event.position);
            let delta = match event.delta {
                ScrollDelta::Pixels(p) => {
                    WheelDelta::pixels(p.x.as_f32() as f64, p.y.as_f32() as f64)
                }
                ScrollDelta::Lines(p) => WheelDelta::lines(p.x as f64, p.y as f64),
            };
            let modifiers = modifiers(&event.modifiers);
            stack
                .update(cx, |stack, cx| stack.wheel(x, delta, modifiers, cx))
                .ok();
            cx.stop_propagation();
        });
    }
}
