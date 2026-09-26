//! The primary video on a real AiM recording (read-only), with libmpv and a
//! null audio output. Ignored by default; run with
//! `OMATRACK_FIXTURES=~/Documents/Telemetry/26T07_PLM cargo test -p omatrack-app -- --include-ignored real_`.

mod common;

use gpui_kit::AppContext as _;
use gpui_kit::TestAppContext;
use gpui_kit::component::{ActiveTheme as _, Theme};
use gpui_kit::test::TestWindowExt as _;
use omatrack_app::state::VideoAvailability;

#[gpui_kit::test]
#[ignore]
fn real_primary_video_binds_and_mute_persists(cx: &mut TestAppContext) {
    // libmpv runs its own threads; the test waits on them.
    cx.executor().allow_parking();
    let root = std::env::var("OMATRACK_FIXTURES")
        .expect("set OMATRACK_FIXTURES to the 26T07_PLM folder of AiM MP4 recordings");
    let sandbox = common::Sandbox::new();
    sandbox.write_config(&format!(
        "locations:\n  - type: folder\n    target: {root}\n"
    ));
    let options = sandbox
        .options()
        .video(true)
        .audio_output(Some("null".to_string()));
    let test = common::start(cx, options);
    let library = test.app.library.clone();
    cx.update(|cx| library.update(cx, |library, cx| library.rescan(cx)));
    cx.run_until_parked();
    let run1 = cx.update(|cx| {
        library
            .read(cx)
            .snapshot()
            .sessions()
            .find(|node| node.file_name().contains("Run1"))
            .cloned()
            .expect("Run1")
    });
    let session = test.app.session.clone();
    cx.update(|cx| {
        session.update(cx, |session, cx| {
            session.set_primary(run1.id.clone().into(), 8, cx)
        })
    });
    cx.run_until_parked();
    let video = test.app.video.clone();
    cx.update(|cx| {
        let video = video.read(cx);
        assert_eq!(*video.availability(), VideoAvailability::Ready);
        assert_eq!(video.bound_path(), Some(&run1.file.path().to_path_buf()));
        assert!(video.frame_source().is_some());
    });
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(
            window.try_find("video-empty").is_none(),
            "the video view replaces the empty state"
        );
        window.press("m", cx);
    })
    .unwrap();
    let muted = cx.update(|cx| test.app.preferences.read(cx).config().video.is_muted());
    assert!(muted, "M mutes and the choice is kept in video.muted");

    // A theme change (an Omarchy hot reload) reaches the letterbox.
    let panel = cx.update(|cx| test.workspace.read(cx).panels().video.clone());
    let letterbox = |cx: &mut TestAppContext| {
        cx.update(|cx| {
            panel
                .read(cx)
                .video_view()
                .expect("a bound video has a view")
                .read(cx)
                .letterbox()
        })
    };
    assert_eq!(letterbox(cx), cx.update(|cx| cx.theme().background));
    cx.update(|cx| {
        let foreground = cx.theme().foreground;
        Theme::global_mut(cx).background = foreground;
    });
    cx.run_until_parked();
    let background = cx.update(|cx| cx.theme().background);
    assert_eq!(letterbox(cx), background);
}

/// A file libmpv cannot open is reported, not left as a black pane: `load`
/// only queues the file, so the failure arrives through the player events.
#[gpui_kit::test]
#[ignore]
fn real_an_unplayable_video_is_reported(cx: &mut TestAppContext) {
    use std::cell::RefCell;
    use std::rc::Rc;

    use omatrack_app::state::VideoEvent;

    cx.executor().allow_parking();
    let sandbox = common::Sandbox::new();
    let broken = sandbox.dir.path().join("broken.mp4");
    std::fs::write(&broken, b"not a video").unwrap();
    let options = sandbox
        .options()
        .video(true)
        .audio_output(Some("null".to_string()));
    let test = common::start(cx, options);
    let video = test.app.video.clone();
    let failures = Rc::new(RefCell::new(Vec::new()));
    let seen = failures.clone();
    cx.update(|cx| {
        cx.subscribe(&video, move |_, event, _| {
            if let VideoEvent::Failed(message) = event {
                seen.borrow_mut().push(message.to_string());
            }
        })
        .detach()
    });
    cx.update(|cx| video.update(cx, |video, cx| video.open_file(broken.clone(), 0.0, cx)));
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| video.read(cx).availability().clone()),
        VideoAvailability::Ready,
        "loadfile is asynchronous"
    );

    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
    while failures.borrow().is_empty() && std::time::Instant::now() < deadline {
        std::thread::sleep(std::time::Duration::from_millis(20));
        cx.run_until_parked();
    }
    assert_eq!(failures.borrow().len(), 1, "{:?}", failures.borrow());
    cx.update(|cx| {
        let video = video.read(cx);
        assert!(matches!(video.availability(), VideoAvailability::Failed(_)));
        assert!(video.frame_source().is_none(), "the black pane is gone");
    });
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        let empty = window.find("video-empty").label().unwrap().to_string();
        assert!(empty.starts_with("Video unavailable."), "{empty}");
    })
    .unwrap();
}
