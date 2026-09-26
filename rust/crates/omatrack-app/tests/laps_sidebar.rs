//! UI tests for the Laps sidebar: the loaded event's recordings as groups,
//! timed laps with per-group gap bars, the out/in disclosure, and the
//! Enter / Alt+Enter / click role commands.
//!
//! The synthetic library has one day at Test Circuit with two recordings
//! (Ada, Grace), five laps each: out, three flying, in. The files do not
//! exist, so a load fails, but the session keeps the lap each role asked
//! for, which is what these tests observe.

#![cfg(test)]

mod common;

use gpui_kit::test::TestWindowExt as _;
use gpui_kit::{AnyWindowHandle, AppContext as _, Entity, SharedString, TestAppContext};
use omatrack_app::actions::{Role, SelectLap};
use omatrack_app::panels::laps::{LapsCursor, LapsPanel};
use omatrack_library::SessionNode;
use omatrack_ui::LapRole;

/// An element id built like `format!`.
macro_rules! id {
    ($($arg:tt)*) => {
        SharedString::from(format!($($arg)*))
    };
}

struct Event {
    _sandbox: common::Sandbox,
    test: common::TestApp,
    handle: AnyWindowHandle,
    laps: Entity<LapsPanel>,
    ada: SessionNode,
    grace: SessionNode,
}

/// The synthetic library installed and Ada's lap 3 (her best) primary.
fn event(cx: &mut TestAppContext) -> Event {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    let snapshot = common::load_synthetic_library(&test, cx);
    let find = |name: &str| {
        snapshot
            .sessions()
            .find(|node| node.file_name().contains(name))
            .unwrap()
            .clone()
    };
    let (ada, grace) = (find("Run1"), find("Run2"));
    let handle: AnyWindowHandle = test.window.into();
    select(handle, &ada, 3, Role::Primary, cx);
    let laps = test.workspace.read_with(cx, |w, _| w.panels().laps.clone());
    Event {
        _sandbox: sandbox,
        test,
        handle,
        laps,
        ada,
        grace,
    }
}

fn select(
    handle: AnyWindowHandle,
    node: &SessionNode,
    lap: i32,
    role: Role,
    cx: &mut TestAppContext,
) {
    cx.update_window(handle, |_, window, cx| {
        window.dispatch_action(
            Box::new(SelectLap {
                session: node.id.clone().into(),
                lap,
                role,
            }),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
}

type SelectedLap = Option<(String, i32)>;

fn roles(event: &Event, cx: &mut TestAppContext) -> (SelectedLap, SelectedLap) {
    cx.update(|cx| {
        let session = event.test.app.session.read(cx);
        let of = |slot: Option<&omatrack_app::state::RoleSlot>| {
            slot.map(|slot| (slot.lap_ref().session().to_string(), slot.lap_ref().lap()))
        };
        (of(session.primary()), of(session.reference()))
    })
}

#[gpui_kit::test]
fn the_event_groups_every_recording_with_its_timed_laps(cx: &mut TestAppContext) {
    let event = event(cx);
    cx.update(|cx| {
        let laps = event.laps.read(cx);
        let groups = laps.groups();
        assert_eq!(groups.len(), 2, "both recordings of the day");
        let (ada, grace) = (&groups[0], &groups[1]);
        assert_eq!(ada.session().as_ref(), event.ada.id);
        assert_eq!(ada.title().as_ref(), "Ada");
        assert_eq!(grace.title().as_ref(), "Grace");
        assert_eq!(ada.summary().as_ref(), "3 timed laps, best 1:15.250");
        assert_eq!(grace.summary().as_ref(), "3 timed laps, best 1:16.000");
        assert_eq!((ada.timed(), ada.untimed()), (3, 2));
        assert!(ada.lap(3).unwrap().is_best());
        assert_eq!(ada.lap(3).unwrap().role(), Some(LapRole::Primary));
        assert!(!ada.lap(1).unwrap().is_timed(), "the out lap is not timed");
        assert_eq!(ada.roles(), [LapRole::Primary]);
        assert!(grace.roles().is_empty());
    });
    cx.update_window(event.handle, |_, window, cx| {
        window.render_frame(cx);
        let row = window.find(id!("{}/l:3", event.ada.id));
        assert_eq!(
            row.selected(),
            Some(true),
            "the primary's row is highlighted"
        );
        assert_eq!(
            row.label(),
            Some("L2, 1:15.250, best lap, primary"),
            "the best lap reads Best, not a gap"
        );
        let slower = window.find(id!("{}/l:2", event.ada.id));
        assert_eq!(slower.label(), Some("L1, 1:16.500, +1.250 to best"));
        assert_ne!(slower.selected(), Some(true));
    })
    .unwrap();
}

#[gpui_kit::test]
fn gaps_and_bars_measure_from_the_events_best_lap(cx: &mut TestAppContext) {
    let event = event(cx);
    cx.update(|cx| {
        let laps = event.laps.read(cx);
        let bar = |group: usize, lap: i32| laps.groups()[group].lap(lap).unwrap().bar();
        // Ada 76.5 / 75.25 / 75.75, Grace 77 / 76 / 76.5: one 1.75 s scale
        // from the event's best (Ada's 75.25).
        assert_eq!(bar(0, 3), Some(0.0), "the event's best has no gap");
        assert_eq!(bar(1, 2), Some(1.0), "the event's slowest fills the lane");
        assert!((bar(0, 2).unwrap() - 1.25 / 1.75).abs() < 1e-6);
        assert!((bar(0, 4).unwrap() - 0.5 / 1.75).abs() < 1e-6);
        assert!((bar(1, 4).unwrap() - 1.25 / 1.75).abs() < 1e-6);
        assert_eq!(bar(0, 1), None, "untimed laps carry no bar");
        // Only the event's best reads Best; Grace's own best is a gap.
        let grace = &laps.groups()[1];
        assert!(grace.laps().iter().all(|line| !line.is_best()));
        assert_eq!(
            grace.lap(3).unwrap().spoken().as_ref(),
            "L2, 1:16.000, +0.750 to best",
            "gaps are to the event's best"
        );
    });
}

#[gpui_kit::test]
fn the_primarys_group_leads_then_the_references(cx: &mut TestAppContext) {
    let event = event(cx);
    select(event.handle, &event.grace, 3, Role::Primary, cx);
    select(event.handle, &event.ada, 3, Role::Reference, cx);
    cx.update(|cx| {
        let laps = event.laps.read(cx);
        let titles: Vec<_> = laps
            .groups()
            .iter()
            .map(|g| g.title().to_string())
            .collect();
        assert_eq!(titles, ["Grace", "Ada"]);
    });
}

#[gpui_kit::test]
fn groups_without_a_role_start_collapsed_and_open_on_click(cx: &mut TestAppContext) {
    let event = event(cx);
    let grace = event.grace.id.clone();
    cx.update(|cx| {
        let laps = event.laps.read(cx);
        assert!(laps.is_expanded(&event.ada.id));
        assert!(!laps.is_expanded(&grace), "no role: collapsed");
    });
    cx.update_window(event.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.try_find(id!("{grace}/l:2")).is_none());
        let header = window.find(id!("laps-group:{grace}"));
        assert_eq!(header.expanded(), Some(false));
        window.click(id!("laps-group:{grace}"), cx);
        window.render_frame(cx);
        assert!(window.find(id!("{grace}/l:2")).visible());
        assert_eq!(
            window.find(id!("laps-group:{grace}")).expanded(),
            Some(true)
        );
    })
    .unwrap();
    // Taking a role in another group keeps the user's choice.
    select(event.handle, &event.grace, 4, Role::Reference, cx);
    cx.update(|cx| {
        let laps = event.laps.read(cx);
        assert!(laps.is_expanded(&grace));
        assert_eq!(laps.group(&grace).unwrap().roles(), [LapRole::Reference]);
    });
}

#[gpui_kit::test]
fn out_and_in_laps_wait_behind_a_counted_disclosure(cx: &mut TestAppContext) {
    let event = event(cx);
    let ada = event.ada.id.clone();
    cx.update_window(event.handle, |_, window, cx| {
        window.render_frame(cx);
        let disclosure = window.find(id!("laps-disclose:{ada}"));
        assert_eq!(disclosure.label(), Some("Show out and in laps (2)"));
        assert_eq!(disclosure.expanded(), Some(false));
        assert!(window.try_find(id!("{ada}/l:1")).is_none());
        window.click(id!("laps-disclose:{ada}"), cx);
        window.render_frame(cx);
        assert!(window.find(id!("{ada}/l:1")).visible());
        assert!(window.find(id!("{ada}/l:5")).visible());
        assert_eq!(
            window.find(id!("laps-disclose:{ada}")).label(),
            Some("Hide out and in laps")
        );
    })
    .unwrap();
    // An untimed lap holding a role shows without the disclosure.
    select(event.handle, &event.ada, 5, Role::Reference, cx);
    cx.update(|cx| {
        event.laps.update(cx, |laps, cx| {
            laps.toggle_disclosure(&SharedString::from(ada.clone()), cx);
        });
    });
    cx.update(|cx| {
        let laps = event.laps.read(cx);
        let group = laps.group(&ada).unwrap();
        let shown: Vec<i32> = laps
            .visible_laps(group)
            .map(omatrack_app::panels::laps::LapLine::lap)
            .collect();
        assert_eq!(shown, [2, 3, 4, 5]);
    });
}

#[gpui_kit::test]
fn enter_sets_the_primary_and_alt_enter_the_reference(cx: &mut TestAppContext) {
    let event = event(cx);
    let ada = event.ada.id.clone();
    // Ctrl+1 lands on the sidebar with the cursor on the primary's lap.
    cx.update_window(event.handle, |_, window, cx| {
        window.press("ctrl-1", cx);
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(
        cx.update(|cx| event.laps.read(cx).cursor_lap()),
        Some((SharedString::from(ada.clone()), 3))
    );
    cx.update_window(event.handle, |_, window, cx| {
        window.press("down", cx);
        window.press("alt-enter", cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(
        roles(&event, cx),
        (Some((ada.clone(), 3)), Some((ada.clone(), 4))),
        "Alt+Enter set the reference; the primary stays"
    );
    cx.update_window(event.handle, |_, window, cx| {
        window.press("up", cx);
        window.press("up", cx);
        window.press("enter", cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(roles(&event, cx).0, Some((ada, 2)), "Enter set L2");

    // Down walks past the disclosure onto the next group; Enter opens it.
    let grace = SharedString::from(event.grace.id.clone());
    cx.update_window(event.handle, |_, window, cx| {
        for _ in 0..4 {
            window.press("down", cx);
        }
    })
    .unwrap();
    assert_eq!(
        cx.update(|cx| event.laps.read(cx).cursor().cloned()),
        Some(LapsCursor::Group(grace.clone()))
    );
    cx.update_window(event.handle, |_, window, cx| window.press("enter", cx))
        .unwrap();
    assert!(cx.update(|cx| event.laps.read(cx).is_expanded(&grace)));
    // The footer names both commands with their keys.
    cx.update_window(event.handle, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("laps-set-primary").visible());
        assert!(window.find("laps-set-reference").visible());
    })
    .unwrap();
}

#[gpui_kit::test]
fn clicks_follow_the_filmstrip_rules(cx: &mut TestAppContext) {
    let event = event(cx);
    let ada = event.ada.id.clone();
    let grace = event.grace.id.clone();
    cx.update_window(event.handle, |_, window, cx| {
        window.render_frame(cx);
        // A right click sets the reference.
        window.right_click(id!("{ada}/l:4"), cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(roles(&event, cx).1, Some((ada.clone(), 4)));
    cx.update_window(event.handle, |_, window, cx| {
        window.render_frame(cx);
        window.click(id!("{ada}/l:2"), cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(
        roles(&event, cx),
        (Some((ada.clone(), 2)), Some((ada.clone(), 4))),
        "a plain click in the primary's group sets the primary"
    );
    // In a group holding only the reference, a click moves the reference.
    select(event.handle, &event.grace, 3, Role::Reference, cx);
    cx.update_window(event.handle, |_, window, cx| {
        window.render_frame(cx);
        window.click(id!("{grace}/l:4"), cx);
    })
    .unwrap();
    cx.run_until_parked();
    assert_eq!(
        roles(&event, cx),
        (Some((ada.clone(), 2)), Some((grace.clone(), 4)))
    );
}

#[gpui_kit::test]
fn without_a_lap_the_sidebar_points_at_the_library(cx: &mut TestAppContext) {
    let sandbox = common::Sandbox::new();
    let test = common::start(cx, sandbox.options());
    common::load_synthetic_library(&test, cx);
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("laps-empty").visible());
        assert!(window.try_find("library-panel").is_none(), "behind the tab");
        window.click("laps-browse-library", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(test.window.into(), |_, window, cx| {
        window.render_frame(cx);
        assert!(
            window.find("library-panel").visible(),
            "Browse library shows the tree"
        );
    })
    .unwrap();
}
