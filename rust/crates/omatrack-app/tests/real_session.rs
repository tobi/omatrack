//! Session races on the real AiM recordings (read-only): a request made
//! while an analysis is in flight must win, and replaced work must not
//! leave jobs behind. Ignored by default; run with
//! `OMATRACK_FIXTURES=~/Documents/Telemetry/26T07_PLM cargo test -p omatrack-app -- --include-ignored real_`.

mod common;

use std::time::Duration;

use gpui_kit::test::TestAppContextExt as _;
use gpui_kit::{AnyWindowHandle, Entity, TestAppContext};
use omatrack_app::state::{RoleState, Session};
use omatrack_core::alignment::Strategy;
use omatrack_core::session::StrategyRequest;
use omatrack_library::catalog::SessionNode;

fn fixtures() -> String {
    std::env::var("OMATRACK_FIXTURES")
        .expect("set OMATRACK_FIXTURES to the 26T07_PLM folder of AiM MP4 recordings")
}

/// Start on the fixtures, scan, and return Run4 and Run1.
async fn scanned(
    cx: &mut TestAppContext,
) -> (common::Sandbox, common::TestApp, SessionNode, SessionNode) {
    let sandbox = common::Sandbox::new();
    sandbox.write_config(&format!(
        "locations:\n  - type: folder\n    name: Fixtures\n    target: {}\n",
        fixtures()
    ));
    let test = common::start(cx, sandbox.options());
    let library = test.app.library.clone();
    cx.update(|cx| library.update(cx, |library, cx| library.rescan(cx)));
    cx.run_until_parked();
    cx.wait_for(test.window.into(), Duration::from_secs(600), |_, cx| {
        let library = library.read(cx);
        library.has_scanned() && !library.is_scanning()
    })
    .await;
    let (run4, run1) = cx.update(|cx| {
        let snapshot = library.read(cx).snapshot().clone();
        let find = |run: &str| {
            snapshot
                .sessions()
                .find(|node| node.file_name().contains(run))
                .cloned()
                .unwrap_or_else(|| panic!("{run} is in the library"))
        };
        (find("Run4"), find("Run1"))
    });
    (sandbox, test, run4, run1)
}

async fn settle(handle: AnyWindowHandle, session: &Entity<Session>, cx: &mut TestAppContext) {
    cx.run_until_parked();
    cx.wait_for(handle, Duration::from_secs(30), |_, cx| {
        let session = session.read(cx);
        !session.is_loading() && session.analysis().is_some()
    })
    .await;
    cx.run_until_parked();
}

fn running_jobs(test: &common::TestApp, cx: &mut TestAppContext) -> Vec<String> {
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
#[ignore]
async fn real_requests_made_during_an_analysis_win(cx: &mut TestAppContext) {
    let (_sandbox, test, run4, run1) = scanned(cx).await;
    let handle: AnyWindowHandle = test.window.into();
    let session = test.app.session.clone();
    let run4_best = run4.best_lap_id.unwrap();
    let run1_best = run1.best_lap_id.unwrap();
    session.update(cx, |session, cx| {
        session.set_primary(run4.id.clone().into(), run4_best, cx);
        session.set_reference(run1.id.clone().into(), run1_best, cx);
    });
    settle(handle, &session, cx).await;
    let (effective, available) = cx.update(|cx| {
        let analysis = session.read(cx).analysis().unwrap().clone();
        (
            analysis.strategy().unwrap(),
            analysis.available_strategies().to_vec(),
        )
    });
    let other = *available
        .iter()
        .find(|s| **s != effective && **s != Strategy::ManualDampers)
        .expect("the pair supports a second strategy");

    // A strategy change, then X before its analysis lands.
    session.update(cx, |session, cx| {
        session.set_strategy(StrategyRequest::Prefer(other), cx);
        session.swap(cx);
    });
    settle(handle, &session, cx).await;
    cx.update(|cx| {
        let session = session.read(cx);
        let analysis = session.analysis().unwrap();
        assert_eq!(session.strategy(), StrategyRequest::Prefer(other));
        assert_eq!(analysis.request(), session.strategy());
        assert_eq!(
            analysis.strategy(),
            Some(other),
            "the header tells the truth"
        );
        assert_eq!(analysis.primary().lap_id(), run1_best, "swapped");
        assert_eq!(analysis.reference().unwrap().lap_id(), run4_best);
    });

    // A lap change, then X after its load but before its analysis.
    let neighbour = cx.update(|cx| {
        let primary = session
            .read(cx)
            .primary()
            .unwrap()
            .loaded()
            .unwrap()
            .clone();
        primary.neighbour_lap(-1).map(|lap| lap.id).unwrap()
    });
    session.update(cx, |session, cx| {
        session.set_primary(run1.id.clone().into(), neighbour, cx)
    });
    cx.run_until_parked();
    cx.wait_for(handle, Duration::from_secs(600), |_, cx| {
        session
            .read(cx)
            .primary()
            .is_some_and(|slot| matches!(slot.state(), RoleState::Loaded(_)))
    })
    .await;
    session.update(cx, |session, cx| session.swap(cx));
    settle(handle, &session, cx).await;
    cx.update(|cx| {
        let analysis = session.read(cx).analysis().unwrap();
        assert_eq!(analysis.primary().lap_id(), run4_best);
        assert_eq!(analysis.reference().unwrap().lap_id(), neighbour);
    });

    // Manual dampers, then damper offsets before the analysis lands: the
    // last offset wins.
    if available.contains(&Strategy::ManualDampers) {
        session.update(cx, |session, cx| {
            session.set_strategy(StrategyRequest::Prefer(Strategy::ManualDampers), cx);
            session.set_manual_offset(0.002, cx);
            session.set_manual_offset(0.004, cx);
        });
        settle(handle, &session, cx).await;
        session.update(cx, |session, cx| {
            session.set_manual_offset(0.003, cx);
            session.set_manual_offset(0.005, cx);
        });
        settle(handle, &session, cx).await;
        cx.update(|cx| {
            let session = session.read(cx);
            let analysis = session.analysis().unwrap();
            assert_eq!(analysis.strategy(), Some(Strategy::ManualDampers));
            let offset = analysis.comparison().unwrap().manual_offset();
            assert!((offset - 0.005).abs() < 1e-12, "{offset}");
            assert!((session.manual_offset() - 0.005).abs() < 1e-12);
        });
    }

    // Rapid lap changes leave no job behind.
    session.update(cx, |session, cx| {
        session.prev_lap(cx);
        session.prev_lap(cx);
    });
    settle(handle, &session, cx).await;
    assert_eq!(running_jobs(&test, cx), Vec::<String>::new());
}
