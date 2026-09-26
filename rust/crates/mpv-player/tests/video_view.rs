//! Headless UI integration tests for `VideoView` with a test-double
//! `FrameSource`: painting, atlas hygiene, loading/error states, size
//! reporting and animation-frame demand.

#![cfg(test)]

use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use gpui_kit::component::{ActiveTheme as _, Root};
use gpui_kit::test::TestWindowExt as _;
use gpui_kit::{
    AppContext as _, DevicePixels, Entity, Size, TestAppContext, WindowHandle, px, size,
};
use mpv_player::{FrameSlot, FrameSource, MediaStatus, VideoFrame, VideoView, VideoViewEvent};

struct MockSource {
    slot: FrameSlot,
    status: Mutex<MediaStatus>,
    playing: AtomicBool,
    target: Mutex<Option<Size<DevicePixels>>>,
    target_reports: AtomicUsize,
}

impl MockSource {
    fn new(status: MediaStatus) -> Arc<Self> {
        Arc::new(Self {
            slot: FrameSlot::new(),
            status: Mutex::new(status),
            playing: AtomicBool::new(false),
            target: Mutex::new(None),
            target_reports: AtomicUsize::new(0),
        })
    }

    fn publish(&self, width: u32, height: u32, media_time: f64) -> VideoFrame {
        let frame = VideoFrame::from_bgra(
            width,
            height,
            vec![0x80; (width * height * 4) as usize],
            media_time,
        )
        .expect("valid frame");
        self.slot.publish(Some(frame.clone()));
        frame
    }

    fn set_status(&self, status: MediaStatus) {
        *self.status.lock().unwrap() = status;
        self.slot.signal();
    }

    fn set_playing(&self, playing: bool) {
        self.playing.store(playing, Ordering::SeqCst);
        self.slot.signal();
    }
}

impl FrameSource for MockSource {
    fn latest_frame(&self) -> Option<VideoFrame> {
        self.slot.latest()
    }

    fn frame_generation(&self) -> u64 {
        self.slot.generation()
    }

    fn frame_signal(&self) -> async_channel::Receiver<()> {
        self.slot.receiver()
    }

    #[expect(
        clippy::significant_drop_tightening,
        reason = "Publish the value and its generation/count in the same critical section."
    )]
    fn set_target_size(&self, size: Option<Size<DevicePixels>>) {
        let mut target = self.target.lock().unwrap();
        if *target != size {
            *target = size;
            self.target_reports.fetch_add(1, Ordering::SeqCst);
        }
    }

    fn status(&self) -> MediaStatus {
        self.status.lock().unwrap().clone()
    }

    fn is_playing(&self) -> bool {
        self.playing.load(Ordering::SeqCst)
    }
}

fn open(
    cx: &mut TestAppContext,
    source: Arc<MockSource>,
) -> (
    WindowHandle<Root>,
    Entity<VideoView>,
    Arc<Mutex<Vec<VideoViewEvent>>>,
) {
    cx.update(gpui_kit::init);
    let events = Arc::new(Mutex::new(Vec::new()));
    let mut view = None;
    let recorded = events.clone();
    let handle = cx.open_window(size(px(640.), px(360.)), |window, cx| {
        let letterbox = cx.theme().background;
        let video = cx.new(|cx| VideoView::new("primary-video", source, letterbox, window, cx));
        cx.subscribe(&video, move |_, _, event: &VideoViewEvent, _| {
            recorded.lock().unwrap().push(event.clone());
        })
        .detach();
        view = Some(video.clone());
        Root::new(video, window, cx)
    });
    (handle, view.expect("view created"), events)
}

#[gpui_kit::test]
#[expect(
    clippy::cast_sign_loss,
    reason = "Test fixtures use bounded sample counts, indices and pixel coordinates; rounding is intentional."
)]
fn paints_the_latest_frame_and_drops_replaced_images(cx: &mut TestAppContext) {
    let source = MockSource::new(MediaStatus::Ready);
    let first = source.publish(160, 90, 1.0);
    let (handle, view, events) = open(cx, source.clone());

    cx.update_window(handle.into(), |_, window, cx| {
        window.render_frame(cx);
        let painted = view.read(cx).image().cloned().expect("first frame adopted");
        assert_eq!(painted.id, first.image().id);
        assert!(
            window.has_image_atlas_entry(first.image()),
            "first frame painted into the atlas"
        );
        assert!(window.try_find("video-loading").is_none());
        assert!(window.find("primary-video").visible());
        assert_eq!(view.read(cx).dropped_image_count(), 0);
    })
    .unwrap();
    assert_eq!(
        events.lock().unwrap().as_slice(),
        &[VideoViewEvent::FirstFrame]
    );

    // The view reports its device size so the backend renders at display size.
    let reported = source.target.lock().unwrap().expect("target size reported");
    assert!(reported.width.0 > 0 && reported.height.0 > 0);

    for step in 0..3 {
        let previous = view.read_with(cx, |view, _| view.image().cloned().unwrap());
        let next = source.publish(160, 90, 2.0 + f64::from(step));
        cx.run_until_parked();
        cx.update_window(handle.into(), |_, window, cx| {
            window.render_frame(cx);
            let view = view.read(cx);
            assert_eq!(view.image().unwrap().id, next.image().id);
            assert_eq!(view.dropped_image_count(), step as usize + 1);
            assert!(
                !window.has_image_atlas_entry(&previous),
                "replaced frame left the atlas"
            );
            assert!(window.has_image_atlas_entry(next.image()));
        })
        .unwrap();
    }
    // FirstFrame is announced once, not per frame.
    assert_eq!(events.lock().unwrap().len(), 1);
}

#[gpui_kit::test]
#[expect(
    clippy::significant_drop_tightening,
    reason = "Read assertions from one consistent event snapshot; the guard is released at the end of the test."
)]
fn shows_loading_until_a_frame_arrives_then_errors(cx: &mut TestAppContext) {
    let source = MockSource::new(MediaStatus::Loading);
    let (handle, view, events) = open(cx, source.clone());

    cx.update_window(handle.into(), |_, window, cx| {
        window.render_frame(cx);
        let loading = window.find("video-loading");
        assert!(loading.visible());
        assert_eq!(loading.label(), Some("Loading video"));
        assert!(window.try_find("video-error").is_none());
    })
    .unwrap();

    source.publish(64, 36, 0.5);
    cx.run_until_parked();
    cx.update_window(handle.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(
            window.try_find("video-loading").is_none(),
            "loading state leaves with the first frame"
        );
        assert!(view.read(cx).image().is_some());
    })
    .unwrap();

    // A new load clears the frame: loading shows again, and FirstFrame will
    // be announced anew.
    source.slot.publish(None);
    cx.run_until_parked();
    cx.update_window(handle.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("video-loading").visible());
        assert_eq!(view.read(cx).dropped_image_count(), 1);
    })
    .unwrap();

    source.set_status(MediaStatus::Failed(
        "Video playback failed: unrecognized file format".into(),
    ));
    cx.run_until_parked();
    cx.update_window(handle.into(), |_, window, cx| {
        window.render_frame(cx);
        let error = window.find("video-error");
        assert!(error.visible());
        assert_eq!(
            error.label(),
            Some("Video playback failed: unrecognized file format")
        );
        assert!(window.try_find("video-loading").is_none());
    })
    .unwrap();
    let events = events.lock().unwrap();
    assert_eq!(events.first(), Some(&VideoViewEvent::FirstFrame));
    assert_eq!(
        events.last(),
        Some(&VideoViewEvent::Error(
            "Video playback failed: unrecognized file format".into()
        ))
    );
}

#[gpui_kit::test]
fn requests_animation_frames_only_while_playing(cx: &mut TestAppContext) {
    let source = MockSource::new(MediaStatus::Ready);
    source.publish(64, 36, 0.0);
    let (handle, view, _) = open(cx, source.clone());

    let frames_after_render = |cx: &mut TestAppContext| {
        cx.update_window(handle.into(), |_, window, cx| {
            window.render_frame(cx);
            window.simulate_next_frame(cx)
        })
        .unwrap()
    };

    assert_eq!(
        frames_after_render(cx),
        0,
        "paused: no animation-frame demand"
    );

    source.set_playing(true);
    cx.run_until_parked();
    assert!(
        frames_after_render(cx) >= 1,
        "playing: one request per rendered frame"
    );
    // The request re-notifies the view, so demand continues frame after frame.
    assert!(frames_after_render(cx) >= 1);

    source.set_playing(false);
    cx.run_until_parked();
    // The frame rendered for the pause may still carry the last request.
    frames_after_render(cx);
    assert_eq!(frames_after_render(cx), 0, "paused again: demand stops");
    assert!(view.read_with(cx, |view, _| view.image().is_some()));
}

#[gpui_kit::test]
fn replacing_the_source_drops_the_old_image(cx: &mut TestAppContext) {
    let first_source = MockSource::new(MediaStatus::Ready);
    let first = first_source.publish(64, 36, 0.0);
    let (handle, view, events) = open(cx, first_source);
    cx.update_window(handle.into(), |_, window, cx| window.render_frame(cx))
        .unwrap();

    let second_source = MockSource::new(MediaStatus::Ready);
    let second = second_source.publish(32, 18, 0.0);
    cx.update_window(handle.into(), |_, window, cx| {
        view.update(cx, |view, cx| {
            view.set_source(second_source.clone(), window, cx);
        });
        window.render_frame(cx);
        assert!(!window.has_image_atlas_entry(first.image()));
        assert!(window.has_image_atlas_entry(second.image()));
    })
    .unwrap();
    assert_eq!(
        events.lock().unwrap().as_slice(),
        &[VideoViewEvent::FirstFrame, VideoViewEvent::FirstFrame]
    );
}

#[gpui_kit::test]
fn hiding_the_view_withdraws_its_render_size(cx: &mut TestAppContext) {
    let source = MockSource::new(MediaStatus::Ready);
    source.publish(64, 36, 0.0);
    source.set_playing(true);
    let (handle, view, _) = open(cx, source.clone());
    cx.update_window(handle.into(), |_, window, cx| window.render_frame(cx))
        .unwrap();
    let shown = source
        .target
        .lock()
        .unwrap()
        .expect("visible view reports a size");

    cx.update_window(handle.into(), |_, window, cx| {
        view.update(cx, |view, cx| view.set_visible(false, cx));
        assert!(!view.read(cx).is_visible());
        window.render_frame(cx);
    })
    .unwrap();
    assert_eq!(
        *source.target.lock().unwrap(),
        None,
        "hidden: the backend stops drawing"
    );
    // Still painted while hidden (the owner kept it in the tree): prepaint
    // must not report the size again, and no animation frames are demanded.
    // Requests made by the frames rendered while visible are flushed first.
    cx.update_window(handle.into(), |_, window, cx| {
        window.simulate_next_frame(cx)
    })
    .unwrap();
    let demand = cx
        .update_window(handle.into(), |_, window, cx| {
            window.render_frame(cx);
            window.simulate_next_frame(cx)
        })
        .unwrap();
    assert_eq!(*source.target.lock().unwrap(), None);
    assert_eq!(demand, 0, "hidden: no animation-frame demand while playing");

    cx.update_window(handle.into(), |_, window, cx| {
        view.update(cx, |view, cx| view.set_visible(true, cx));
        window.render_frame(cx);
    })
    .unwrap();
    assert_eq!(
        *source.target.lock().unwrap(),
        Some(shown),
        "shown again: the size is reported on the next paint"
    );
}

#[gpui_kit::test]
fn releasing_or_replacing_the_view_withdraws_its_render_size(cx: &mut TestAppContext) {
    let first_source = MockSource::new(MediaStatus::Ready);
    first_source.publish(64, 36, 0.0);
    let (handle, view, _) = open(cx, first_source.clone());
    cx.update_window(handle.into(), |_, window, cx| window.render_frame(cx))
        .unwrap();
    assert!(first_source.target.lock().unwrap().is_some());

    // Replacing the source: the old one has no view any more.
    let second_source = MockSource::new(MediaStatus::Ready);
    second_source.publish(64, 36, 0.0);
    cx.update_window(handle.into(), |_, window, cx| {
        view.update(cx, |view, cx| {
            view.set_source(second_source.clone(), window, cx);
        });
        window.render_frame(cx);
    })
    .unwrap();
    assert_eq!(*first_source.target.lock().unwrap(), None);
    assert!(second_source.target.lock().unwrap().is_some());

    // Releasing the view: its window drops the root, the root the view.
    drop(view);
    cx.update_window(handle.into(), |_, window, _| window.remove_window())
        .unwrap();
    cx.run_until_parked();
    assert_eq!(
        *second_source.target.lock().unwrap(),
        None,
        "a released view stops the backend drawing"
    );
}
