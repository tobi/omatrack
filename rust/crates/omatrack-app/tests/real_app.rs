//! The whole application on the real AiM recordings (read-only). Ignored by
//! default; run with
//! `OMATRACK_FIXTURES=~/Documents/Telemetry/26T07_PLM cargo test -p omatrack-app -- --include-ignored real_`.
//! Every configuration and cache write goes to temporary XDG roots.

mod common;

use std::time::Duration;

use gpui_kit::AppContext as _;
use gpui_kit::TestAppContext;
use gpui_kit::test::{TestAppContextExt as _, TestWindowExt as _};
use omatrack_app::actions::{Role, SelectLap};
use omatrack_app::state::{LapRef, RoleState};
use omatrack_core::format_lap_time;
use omatrack_trace::{Selection, Viewport};

fn fixtures() -> String {
    std::env::var("OMATRACK_FIXTURES")
        .expect("set OMATRACK_FIXTURES to the 26T07_PLM folder of AiM MP4 recordings")
}

#[gpui_kit::test]
#[ignore]
async fn real_run4_against_run1_through_the_workspace(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    sandbox.write_config(&format!(
        "locations:\n  - type: folder\n    name: Fixtures\n    target: {}\n",
        fixtures()
    ));
    let test = common::start(cx, sandbox.options());
    let handle = test.window.into();

    // Scan the library.
    let library = test.app.library.clone();
    cx.update(|cx| library.update(cx, |library, cx| library.rescan(cx)));
    // Background work runs when the test executor parks; `wait_for` alone
    // only advances the test clock.
    cx.run_until_parked();
    cx.wait_for(handle, Duration::from_secs(600), |_, cx| {
        let library = library.read(cx);
        library.has_scanned() && !library.is_scanning()
    })
    .await;
    let (run4, run1) = cx.update(|cx| {
        let snapshot = library.read(cx).snapshot().clone();
        assert_eq!(snapshot.recording_count(), 3);
        let find = |run: &str| {
            snapshot
                .sessions()
                .find(|node| node.file_name().contains(run))
                .cloned()
                .unwrap_or_else(|| panic!("{run} is in the library"))
        };
        (find("Run4"), find("Run1"))
    });
    let run4_best = run4.best_lap_id.expect("Run4 has a best lap");
    let run1_best = run1.best_lap_id.expect("Run1 has a best lap");
    assert_eq!(run1_best, 8);

    // Select the pair through the same action the palette and context
    // menu dispatch.
    cx.update_window(handle, |_, window, cx| {
        window.dispatch_action(
            Box::new(SelectLap {
                session: run4.id.clone().into(),
                lap: run4_best,
                role: Role::Primary,
            }),
            cx,
        );
        window.dispatch_action(
            Box::new(SelectLap {
                session: run1.id.clone().into(),
                lap: run1_best,
                role: Role::Reference,
            }),
            cx,
        );
    })
    .unwrap();
    let session = test.app.session.clone();
    cx.run_until_parked();
    cx.wait_for(handle, Duration::from_secs(600), |_, cx| {
        let session = session.read(cx);
        !session.is_loading()
            && session
                .analysis()
                .is_some_and(|analysis| analysis.reference().is_some())
    })
    .await;

    let (corners, primary_label, reference_label) = cx.update(|cx| {
        let session = session.read(cx);
        for slot in [session.primary().unwrap(), session.reference().unwrap()] {
            assert!(
                matches!(slot.state(), RoleState::Loaded(_)),
                "{:?}",
                slot.state()
            );
        }
        let analysis = session.analysis().unwrap();
        assert_eq!(analysis.primary().lap_id(), run4_best);
        assert_eq!(analysis.reference().unwrap().lap_id(), run1_best);
        (
            analysis.rows().len(),
            session.primary().unwrap().info().time.clone(),
            session.reference().unwrap().info().time.clone(),
        )
    });
    assert!(corners > 0, "the analysis has corners");
    assert_eq!(reference_label.as_ref(), "1:13.644");
    let run4_time = format_lap_time(run4.lap(run4_best).unwrap().time_ms);
    assert_eq!(primary_label.as_ref(), run4_time);

    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        let primary = window
            .find("filmstrip-primary")
            .label()
            .unwrap()
            .to_string();
        let reference = window
            .find("filmstrip-reference")
            .label()
            .unwrap()
            .to_string();
        assert!(
            primary.starts_with(&format!("Primary lap L{run4_best} {run4_time}")),
            "{primary}"
        );
        assert!(
            reference.starts_with("Reference lap L8 1:13.644"),
            "{reference}"
        );
        assert_eq!(window.find("header-track").label(), Some("Road Atlanta"));
        assert!(window.find("header-confidence").visible());
        let cursor = window.find("status-cursor").label().unwrap().to_string();
        assert!(cursor.ends_with("0:00.000"), "{cursor}");
    })
    .unwrap();

    // Put the cursor and the viewport somewhere, then swap with X.
    let (cursor, viewport) = (test.app.cursor.clone(), test.app.viewport.clone());
    cx.update(|cx| {
        cursor.update(cx, |cursor, cx| {
            cursor.set_fraction(Some(0.3), cx);
            cursor.set_selection(Some(Selection::new(0.25, 0.35)), cx);
        });
        viewport.update(cx, |viewport, cx| {
            viewport.set_viewport(Viewport::new(0.2, 0.6), cx)
        });
    });
    cx.update_window(handle, |_, window, cx| window.press("x", cx))
        .unwrap();
    cx.run_until_parked();
    cx.wait_for(handle, Duration::from_secs(600), |_, cx| {
        let session = session.read(cx);
        !session.is_loading()
            && session
                .analysis()
                .is_some_and(|analysis| analysis.primary().lap_id() == run1_best)
    })
    .await;
    cx.update(|cx| {
        let session = session.read(cx);
        assert_eq!(
            session.primary().unwrap().lap_ref(),
            &LapRef::new(run1.id.clone(), run1_best)
        );
        assert_eq!(
            session.reference().unwrap().lap_ref(),
            &LapRef::new(run4.id.clone(), run4_best)
        );
        assert_eq!(cursor.read(cx).fraction(), Some(0.3));
        assert_eq!(
            cursor.read(cx).selection(),
            Some(Selection::new(0.25, 0.35))
        );
        assert_eq!(viewport.read(cx).viewport(), Viewport::new(0.2, 0.6));
    });
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        let primary = window
            .find("filmstrip-primary")
            .label()
            .unwrap()
            .to_string();
        assert!(primary.starts_with("Primary lap L8 1:13.644"), "{primary}");
        assert!(window.find("status-selection").visible());
    })
    .unwrap();

    // = zooms in about the cursor.
    cx.update_window(handle, |_, window, cx| window.press("=", cx))
        .unwrap();
    let zoomed = cx.update(|cx| viewport.read(cx).viewport());
    assert!(zoomed.span() < 0.4, "{zoomed:?}");
    assert!(zoomed.start <= 0.3 && zoomed.end >= 0.3, "{zoomed:?}");

    // J focuses the next corner, the palette focuses one by name.
    cx.update_window(handle, |_, window, cx| window.press("j", cx))
        .unwrap();
    cx.executor().advance_clock(Duration::from_millis(300));
    cx.run_until_parked();
    let focused = cx.update(|cx| test.workspace.read(cx).focused_corner());
    assert!(focused.is_some(), "J focused a corner");
    cx.update(|cx| {
        let focus = cursor
            .read(cx)
            .focus()
            .expect("the focused zone is kept bright");
        let viewport = viewport.read(cx).viewport();
        assert!(viewport.start <= focus.start && viewport.end >= focus.end);
    });

    // The palette focuses a corner by typing its name.
    let (ix, name) = cx.update(|cx| {
        let corners = session.read(cx).analysis().unwrap().corners().to_vec();
        let ix = corners
            .iter()
            .position(|zone| zone.name == "Turn 5")
            .unwrap_or_else(|| {
                let names: Vec<_> = corners.iter().map(|zone| zone.name.clone()).collect();
                panic!("the track has a Turn 5: {names:?}")
            });
        (ix, corners[ix].name.clone())
    });
    // Typed the short way: T5 finds Turn 5.
    let query = "T5";
    cx.update_window(handle, |_, window, cx| window.press("ctrl-k", cx))
        .unwrap();
    cx.run_until_parked();
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        window.input(query, cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(handle, |_, window, cx| {
        window.render_frame(cx);
        window.press("enter", cx);
    })
    .unwrap();
    cx.executor().advance_clock(Duration::from_millis(300));
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| test.workspace.read(cx).focused_corner()),
        Some(ix),
        "the palette focused {name}"
    );

    // J and H do not wrap: from the last corner J stays, from the first
    // H stays.
    let last = cx.update(|cx| session.read(cx).analysis().unwrap().corners().len() - 1);
    for _ in 0..=last {
        cx.update_window(handle, |_, window, cx| window.press("j", cx))
            .unwrap();
    }
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| test.workspace.read(cx).focused_corner()),
        Some(last)
    );
    cx.update_window(handle, |_, window, cx| window.press("j", cx))
        .unwrap();
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| test.workspace.read(cx).focused_corner()),
        Some(last),
        "J does not wrap to the first corner"
    );
    for _ in 0..=last {
        cx.update_window(handle, |_, window, cx| window.press("h", cx))
            .unwrap();
    }
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| test.workspace.read(cx).focused_corner()),
        Some(0),
        "H does not wrap to the last corner"
    );
    cx.executor().advance_clock(Duration::from_millis(300));
    cx.run_until_parked();

    // Escape leaves the corner focus and returns to the viewport the
    // first focus started from.
    cx.update_window(handle, |_, window, cx| window.press("escape", cx))
        .unwrap();
    cx.run_until_parked();
    cx.update(|cx| {
        assert_eq!(test.workspace.read(cx).focused_corner(), None);
        assert_eq!(cursor.read(cx).focus(), None);
        assert_eq!(viewport.read(cx).viewport(), zoomed);
    });

    // With no corner focused, J from past the last corner and H from before
    // the first do nothing (no wrap).
    for (fraction, key) in [(0.999, "j"), (0.0001, "h")] {
        cx.update(|cx| cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(fraction), cx)));
        cx.update_window(handle, |_, window, cx| window.press(key, cx))
            .unwrap();
        cx.run_until_parked();
        assert_eq!(
            cx.update(|cx| test.workspace.read(cx).focused_corner()),
            None,
            "{key} at {fraction} wrapped"
        );
    }
}
