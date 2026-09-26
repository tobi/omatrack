//! The status bar's job list ends every job whose work was cancelled or
//! replaced (a replaced task drops its future and the job handle with it).

#![cfg(test)]

mod common;

use gpui_kit::AppContext as _;
use gpui_kit::TestAppContext;
use omatrack_app::state::Jobs;

fn running(test: &common::TestApp, cx: &mut TestAppContext) -> Vec<String> {
    cx.update(|cx| {
        test.app
            .jobs
            .read(cx)
            .running()
            .iter()
            .map(|job| job.label().to_string())
            .collect()
    })
}

#[gpui_kit::test]
fn a_dropped_handle_ends_its_job(cx: &mut TestAppContext) {
    let jobs = cx.new(Jobs::new);
    let (kept, dropped) = jobs.update(cx, |jobs, cx| {
        (jobs.start("Kept", cx), jobs.start("Dropped", cx))
    });
    assert_eq!(cx.update(|cx| jobs.read(cx).running().len()), 2);
    drop(dropped);
    cx.run_until_parked();
    let labels: Vec<String> = cx.update(|cx| {
        jobs.read(cx)
            .running()
            .iter()
            .map(|job| job.label().to_string())
            .collect()
    });
    assert_eq!(labels, ["Kept"]);
    jobs.update(cx, |jobs, cx| jobs.finish(kept, cx));
    assert!(cx.update(|cx| !jobs.read(cx).is_busy()));
    cx.run_until_parked();
    assert!(cx.update(|cx| !jobs.read(cx).is_busy()));
}

#[gpui_kit::test]
fn rescanning_twice_leaves_no_job_running(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let library = test.app.library.clone();
    cx.update(|cx| {
        library.update(cx, |library, cx| {
            library.rescan(cx);
            library.rescan(cx);
        });
    });
    cx.run_until_parked();
    assert!(cx.update(|cx| !library.read(cx).is_scanning()));
    assert_eq!(running(&test, cx), Vec::<String>::new());
}

#[gpui_kit::test]
fn replacing_a_lap_load_leaves_no_job_running(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    // A location owning the synthetic records, so each selection really
    // starts a load job (the files do not exist: every load fails).
    sandbox.write_config(&format!(
        "locations:\n  - type: folder\n    id: synthetic\n    target: {}\n",
        sandbox.dir.path().display()
    ));
    let test = common::start(cx, sandbox.options());
    let snapshot = common::load_synthetic_library(&test, cx);
    let id = snapshot.sessions().next().unwrap().id.clone();
    let session = test.app.session.clone();
    cx.update(|cx| {
        session.update(cx, |session, cx| {
            session.set_primary(id.clone().into(), 2, cx);
            assert!(session.is_loading(), "the selection starts a load");
            session.set_primary(id.clone().into(), 3, cx);
            session.next_lap(cx);
        });
    });
    cx.run_until_parked();
    assert!(cx.update(|cx| !session.read(cx).is_loading()));
    assert_eq!(running(&test, cx), Vec::<String>::new());
}
