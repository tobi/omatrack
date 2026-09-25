//! [`VideoView`]: the GPUI surface that shows a [`FrameSource`].
//!
//! Ownership: the view owns only presentation state (the image it currently
//! paints and whether it has announced the first frame). Playback state lives
//! in the source. Frames reach the view through the source's latest-wins
//! wake-up channel; the view swaps its image there, drops the replaced image
//! from the GPU sprite atlas at once (a video would otherwise leak one
//! texture per frame), and re-renders.
//!
//! Render demand: the backend renders only while some view reports a target
//! size. The surface reports it during prepaint, which runs only while the
//! view is painted, so a view that stops being painted must withdraw it
//! explicitly: [`VideoView::set_visible`]`(false)` (an inactive dock tab, a
//! layout that hides one player), replacing the source, and releasing the
//! view all report "no target", after which the backend consumes frames
//! without drawing them.

use std::sync::Arc;

use gpui_kit::component::spinner::Spinner;
use gpui_kit::component::{ActiveTheme as _, Icon, IconName, Sizable as _};
use gpui_kit::{
    App, Bounds, Context, Corners, DevicePixels, Element, ElementId, EventEmitter, GlobalElementId,
    Hsla, InspectorElementId, InteractiveElement as _, IntoElement, LayoutId, ParentElement as _,
    Pixels, Refineable as _, Render, RenderImage, Role, SharedString, Size,
    StatefulInteractiveElement as _, Style, StyleRefinement, Styled, Subscription, Task,
    TestSupportExt as _, Window, div, fill, point, prelude::FluentBuilder as _, size,
};

use smallvec::SmallVec;

use crate::source::{FrameSource, MediaStatus};

/// Events a [`VideoView`] emits.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum VideoViewEvent {
    /// The first frame of the current source was painted (emitted again
    /// after the source is replaced or its frame is cleared by a new load).
    FirstFrame,
    /// The source reported a failure.
    Error(SharedString),
}

/// Displays the latest frame of a [`FrameSource`], aspect-fit on a letterbox
/// color supplied by the owner (use a theme token, for example the theme's
/// background or a dedicated video surface role).
///
/// While the source is playing and the view is visible, it requests an
/// animation frame on every render, so owners that observe it
/// (`cx.observe(&video_view, ..)`) can pull a
/// [`PlaybackClock`](crate::PlaybackClock) once per display frame.
///
/// GPUI does not tell an element that it stopped being painted. An owner
/// that keeps the view alive while not showing it (a background dock tab, a
/// collapsed pane) must call [`VideoView::set_visible`]`(false)`, or the
/// backend keeps rendering frames nobody sees.
pub struct VideoView {
    id: ElementId,
    source: Arc<dyn FrameSource>,
    letterbox: Hsla,
    visible: bool,
    image: Option<Arc<RenderImage>>,
    generation: u64,
    first_frame_emitted: bool,
    reported_error: Option<String>,
    dropped_images: usize,
    _signal: Task<()>,
    _release: Subscription,
}

impl EventEmitter<VideoViewEvent> for VideoView {}

impl VideoView {
    /// Creates a view of `source`. `id` must be a stable, domain-derived
    /// identity (for example `"primary-video"`), unique within its parent.
    pub fn new(
        id: impl Into<ElementId>,
        source: Arc<dyn FrameSource>,
        letterbox: Hsla,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let signal = Self::listen(&source, window, cx);
        let release = cx.on_release(|view: &mut Self, cx: &mut App| {
            // Nobody paints this source any more: stop the backend drawing.
            view.source.set_target_size(None);
            if let Some(image) = view.image.take() {
                cx.drop_image(image, None);
            }
        });
        let mut view = Self {
            id: id.into(),
            source,
            letterbox,
            visible: true,
            image: None,
            generation: u64::MAX,
            first_frame_emitted: false,
            reported_error: None,
            dropped_images: 0,
            _signal: signal,
            _release: release,
        };
        // Adopt the current frame now (no loading flash on the first draw),
        // but announce it after construction so an owner subscribing right
        // after `cx.new` still receives `FirstFrame`.
        let announcements = view.pull(window);
        if !announcements.is_empty() {
            cx.defer_in(window, move |_, _, cx| {
                for event in announcements {
                    cx.emit(event);
                }
            });
        }
        view
    }

    fn listen(
        source: &Arc<dyn FrameSource>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Task<()> {
        let signal = source.frame_signal();
        cx.spawn_in(window, async move |view, cx| {
            while signal.recv().await.is_ok() {
                if view
                    .update_in(cx, |view, window, cx| view.sync_frame(window, cx))
                    .is_err()
                {
                    break;
                }
            }
        })
    }

    /// Replaces the source; the current image is dropped from the atlas.
    pub fn set_source(
        &mut self,
        source: Arc<dyn FrameSource>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        // The old source loses its only view.
        self.source.set_target_size(None);
        self._signal = Self::listen(&source, window, cx);
        self.source = source;
        self.generation = u64::MAX;
        self.first_frame_emitted = false;
        self.reported_error = None;
        self.sync_frame(window, cx);
    }

    /// Shows or hides the view. A hidden view withdraws its target size, so
    /// the backend consumes frames without drawing them, and stops requesting
    /// animation frames; showing it again reports its size on the next paint
    /// and the backend redraws the current frame at that size.
    ///
    /// Call it when the view stays alive but leaves the screen (an inactive
    /// dock tab, a layout that hides this player). Views start visible.
    pub fn set_visible(&mut self, visible: bool, cx: &mut Context<Self>) {
        if self.visible == visible {
            return;
        }
        self.visible = visible;
        if !visible {
            self.source.set_target_size(None);
        }
        cx.notify();
    }

    /// Whether the view reports its size to the source (see
    /// [`VideoView::set_visible`]).
    pub fn is_visible(&self) -> bool {
        self.visible
    }

    /// Changes the letterbox color.
    pub fn set_letterbox(&mut self, letterbox: Hsla, cx: &mut Context<Self>) {
        if self.letterbox != letterbox {
            self.letterbox = letterbox;
            cx.notify();
        }
    }

    /// The letterbox color.
    pub fn letterbox(&self) -> Hsla {
        self.letterbox
    }

    /// The source shown by this view.
    pub fn source(&self) -> &Arc<dyn FrameSource> {
        &self.source
    }

    /// The image currently painted.
    pub fn image(&self) -> Option<&Arc<RenderImage>> {
        self.image.as_ref()
    }

    /// How many replaced images have been dropped from the sprite atlas.
    pub fn dropped_image_count(&self) -> usize {
        self.dropped_images
    }

    /// Pulls the source's latest frame and status. Called when the source
    /// signals; replaced images are dropped from the atlas immediately.
    fn sync_frame(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        for event in self.pull(window) {
            cx.emit(event);
        }
        cx.notify();
    }

    /// Adopts the source's latest frame and status, dropping a replaced
    /// image from the atlas at once, and returns the events to announce.
    fn pull(&mut self, window: &mut Window) -> SmallVec<[VideoViewEvent; 2]> {
        let mut events = SmallVec::new();
        let generation = self.source.frame_generation();
        if generation != self.generation {
            self.generation = generation;
            let next = self
                .source
                .latest_frame()
                .map(|frame| frame.image().clone());
            let changed = match (&self.image, &next) {
                (Some(current), Some(next)) => current.id != next.id,
                (None, None) => false,
                _ => true,
            };
            if changed {
                if let Some(previous) = std::mem::replace(&mut self.image, next) {
                    // Remove the replaced texture from this window's atlas.
                    // A no-op if it was never painted.
                    let _ = window.drop_image(previous);
                    self.dropped_images += 1;
                }
                match self.image {
                    Some(_) if !self.first_frame_emitted => {
                        self.first_frame_emitted = true;
                        events.push(VideoViewEvent::FirstFrame);
                    }
                    None => self.first_frame_emitted = false,
                    Some(_) => {}
                }
            }
        }

        match self.source.status() {
            MediaStatus::Failed(message) => {
                if self.reported_error.as_deref() != Some(message.as_str()) {
                    self.reported_error = Some(message.clone());
                    events.push(VideoViewEvent::Error(message.into()));
                }
            }
            _ => self.reported_error = None,
        }
        events
    }
}

impl Render for VideoView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if self.visible && self.source.is_playing() {
            // One notify per display frame while playing: the next frame is
            // pulled even if the wake-up raced, and observers can sample the
            // clock in step with the display.
            window.request_animation_frame();
        }
        let status = self.source.status();
        let loading =
            self.image.is_none() && matches!(status, MediaStatus::Loading | MediaStatus::Ready);
        let error = match status {
            MediaStatus::Failed(message) => Some(message),
            _ => None,
        };
        let theme = cx.theme();
        let muted = theme.muted_foreground;
        let danger = theme.danger;

        div()
            .id(self.id.clone())
            .role(Role::Video)
            .aria_label("Video")
            .test_support()
            .size_full()
            .relative()
            .overflow_hidden()
            .child(
                VideoSurface::new(self.image.clone(), self.letterbox)
                    .when(self.visible, |surface| {
                        surface.report_size_to(self.source.clone())
                    })
                    .size_full(),
            )
            .when(loading && error.is_none(), |this| {
                this.child(
                    div()
                        .id("video-loading")
                        .role(Role::Status)
                        .aria_label("Loading video")
                        .test_support()
                        .absolute()
                        .inset_0()
                        .flex()
                        .items_center()
                        .justify_center()
                        .gap_2()
                        .text_sm()
                        .text_color(muted)
                        .child(Spinner::new().small().color(muted))
                        .child("Loading video…"),
                )
            })
            .when_some(error, |this, message| {
                this.child(
                    div()
                        .id("video-error")
                        .role(Role::Alert)
                        .aria_label(SharedString::from(message.clone()))
                        .test_support()
                        .absolute()
                        .inset_0()
                        .flex()
                        .items_center()
                        .justify_center()
                        .gap_2()
                        .px_4()
                        .text_sm()
                        .text_color(muted)
                        .child(
                            Icon::new(IconName::TriangleAlert)
                                .small()
                                .text_color(danger),
                        )
                        .child(message),
                )
            })
    }
}

/// Fits `content` (device pixels) inside `container`, centered, preserving
/// its aspect ratio. An empty content size fills nothing.
pub fn aspect_fit(container: Bounds<Pixels>, content: Size<DevicePixels>) -> Bounds<Pixels> {
    let container_width = f32::from(container.size.width);
    let container_height = f32::from(container.size.height);
    if content.width.0 <= 0
        || content.height.0 <= 0
        || container_width <= 0.0
        || container_height <= 0.0
    {
        return Bounds::new(container.center(), size(Pixels::ZERO, Pixels::ZERO));
    }
    let scale =
        (container_width / content.width.0 as f32).min(container_height / content.height.0 as f32);
    let width = content.width.0 as f32 * scale;
    let height = content.height.0 as f32 * scale;
    let origin = point(
        container.origin.x + gpui_kit::px((container_width - width) / 2.0),
        container.origin.y + gpui_kit::px((container_height - height) / 2.0),
    );
    Bounds::new(origin, size(gpui_kit::px(width), gpui_kit::px(height)))
}

/// A low-level element painting one video image aspect-fit on a letterbox
/// fill. [`VideoView`] uses it; it can also be embedded directly.
pub struct VideoSurface {
    image: Option<Arc<RenderImage>>,
    letterbox: Hsla,
    size_sink: Option<Arc<dyn FrameSource>>,
    style: StyleRefinement,
}

impl VideoSurface {
    /// A surface showing `image` (or only the letterbox when `None`).
    pub fn new(image: Option<Arc<RenderImage>>, letterbox: Hsla) -> Self {
        Self {
            image,
            letterbox,
            size_sink: None,
            style: StyleRefinement::default(),
        }
    }

    /// Reports the surface's device-pixel size to `source` during prepaint
    /// (only changes reach the backend), so it renders at display size.
    pub fn report_size_to(mut self, source: Arc<dyn FrameSource>) -> Self {
        self.size_sink = Some(source);
        self
    }
}

impl Styled for VideoSurface {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

impl IntoElement for VideoSurface {
    type Element = Self;

    fn into_element(self) -> Self::Element {
        self
    }
}

impl Element for VideoSurface {
    type RequestLayoutState = ();
    type PrepaintState = ();

    fn id(&self) -> Option<ElementId> {
        None
    }

    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }

    fn request_layout(
        &mut self,
        _id: Option<&GlobalElementId>,
        _inspector_id: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, Self::RequestLayoutState) {
        let mut style = Style::default();
        style.refine(&self.style);
        (window.request_layout(style, [], cx), ())
    }

    fn prepaint(
        &mut self,
        _id: Option<&GlobalElementId>,
        _inspector_id: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _request_layout: &mut Self::RequestLayoutState,
        window: &mut Window,
        _cx: &mut App,
    ) -> Self::PrepaintState {
        if let Some(sink) = &self.size_sink {
            let device = bounds.size.to_device_pixels(window.scale_factor());
            let visible = device.width.0 > 0 && device.height.0 > 0;
            sink.set_target_size(visible.then_some(device));
        }
    }

    fn paint(
        &mut self,
        _id: Option<&GlobalElementId>,
        _inspector_id: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _request_layout: &mut Self::RequestLayoutState,
        _prepaint: &mut Self::PrepaintState,
        window: &mut Window,
        _cx: &mut App,
    ) {
        window.paint_quad(fill(bounds, self.letterbox));
        if let Some(image) = &self.image {
            let fitted = aspect_fit(bounds, image.size(0));
            if let Err(error) =
                window.paint_image(fitted, fitted, Corners::default(), image.clone(), 0, false)
            {
                log::warn!("video frame paint failed: {error}");
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui_kit::px;

    #[test]
    fn aspect_fit_letterboxes_and_pillarboxes() {
        let container = Bounds::new(point(px(10.), px(20.)), size(px(400.), px(400.)));
        let wide = aspect_fit(container, Size::new(DevicePixels(1920), DevicePixels(1080)));
        assert_eq!(wide.size, size(px(400.), px(225.)));
        assert_eq!(wide.origin, point(px(10.), px(20.) + px(87.5)));

        let container = Bounds::new(point(px(0.), px(0.)), size(px(800.), px(225.)));
        let tall = aspect_fit(container, Size::new(DevicePixels(1920), DevicePixels(1080)));
        assert_eq!(tall.size, size(px(400.), px(225.)));
        assert_eq!(tall.origin, point(px(200.), px(0.)));
    }

    #[test]
    fn aspect_fit_of_nothing_is_empty() {
        let container = Bounds::new(point(px(0.), px(0.)), size(px(100.), px(100.)));
        let fitted = aspect_fit(container, Size::new(DevicePixels(0), DevicePixels(10)));
        assert_eq!(fitted.size, size(px(0.), px(0.)));
    }
}
