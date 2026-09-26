//! UI integration tests for the workspace filmstrip: the real workspace in
//! a headless window over a synthetic library of MTJ recordings (so the
//! scan → load → strip pipeline runs), driven by native pointer events.

mod common;

use std::path::Path;
use std::time::Duration;

use gpui_kit::test::{TestAppContextExt as _, TestWindowExt as _};
use gpui_kit::{AnyWindowHandle, AppContext as _, ElementId, SharedString, TestAppContext};
use omatrack_app::actions::{Role, SelectLap};
use omatrack_trace::Viewport;
use omatrack_ui::LapRole;

const RATE: f64 = 50.0;
const DURATION: f64 = 90.0;
/// Lap boundaries (s): an out fragment, four laps (the second fastest), an
/// in fragment.
const BOUNDARIES: [f64; 7] = [0.0, 5.0, 25.1, 45.0, 65.0, 85.0, 90.0];

/// One MTJ recording (`*.telemetry.jsonl`): a steady lap with one braked
/// corner, `shift` km/h faster than the first driver.
fn write_recording(path: &Path, driver: &str, shift: f64) {
    let ns = |seconds: f64| (seconds * 1e9).round() as i64;
    let count = (DURATION * RATE) as usize;
    let mut speed = Vec::with_capacity(count);
    let mut brake = Vec::with_capacity(count);
    for i in 0..count {
        let t = i as f64 / RATE;
        let phase = ((t - 5.0) / 20.0).rem_euclid(1.0);
        let corner = (0.4..0.6).contains(&phase);
        speed.push(if corner { 90.0 } else { 200.0 } + shift);
        brake.push(if corner { 60.0 } else { 0.0 });
    }
    let laps: Vec<serde_json::Value> = BOUNDARIES
        .windows(2)
        .enumerate()
        .map(|(number, bounds)| {
            let complete = number > 0 && number + 2 < BOUNDARIES.len();
            serde_json::json!([number, ns(bounds[0]), ns(bounds[1]), complete as i32])
        })
        .collect();
    let lines = [
        serde_json::json!({
            "mtj": 1, "q": 20_000_000, "dur": ns(DURATION), "drv": driver,
            "ven": "Test Circuit", "utc": 1_788_000_000_000_000_000i64, "tz": "UTC",
        })
        .to_string(),
        serde_json::Value::Array(laps).to_string(),
        serde_json::json!({"n": "Speed", "hz": RATE, "u": "km/h", "v": speed}).to_string(),
        serde_json::json!({"n": "Brake Pressure F", "hz": RATE, "u": "bar", "v": brake})
            .to_string(),
    ];
    std::fs::write(path, lines.join("\n") + "\n").expect("fixture written");
}

struct Fixture {
    _sandbox: common::Sandbox,
    test: common::TestApp,
    window: AnyWindowHandle,
    run1: SharedString,
    run2: SharedString,
}

/// Scan a library of two recordings, Run1 and Run2.
async fn library(cx: &mut TestAppContext) -> Fixture {
    let sandbox = common::Sandbox::new();
    let root = sandbox.dir.path().join("library");
    let folder = root.join("2026-09-02");
    std::fs::create_dir_all(&folder).unwrap();
    write_recording(&folder.join("Run1.telemetry.jsonl"), "Ada", 0.0);
    write_recording(&folder.join("Run2.telemetry.jsonl"), "Grace", 3.0);
    sandbox.write_config(&format!(
        "locations:\n  - type: folder\n    name: Fixtures\n    target: {}\n",
        root.display()
    ));
    let test = common::start(cx, sandbox.options());
    let window: AnyWindowHandle = test.window.into();
    let library = test.app.library.clone();
    cx.update(|cx| library.update(cx, |library, cx| library.rescan(cx)));
    cx.run_until_parked();
    cx.wait_for(window, Duration::from_secs(600), |_, cx| {
        let library = library.read(cx);
        library.has_scanned() && !library.is_scanning()
    })
    .await;
    let (run1, run2) = cx.update(|cx| {
        let snapshot = library.read(cx).snapshot().clone();
        let find = |name: &str| {
            snapshot
                .sessions()
                .find(|node| node.file_name().contains(name))
                .map(|node| SharedString::from(node.id.clone()))
                .unwrap_or_else(|| panic!("{name} is in the library"))
        };
        (find("Run1"), find("Run2"))
    });
    Fixture {
        _sandbox: sandbox,
        test,
        window,
        run1,
        run2,
    }
}

impl Fixture {
    /// Select `lap` of `session` for `role`, and wait until both roles
    /// have loaded and the filmstrip shows cells on every row.
    async fn select(&self, cx: &mut TestAppContext, session: &SharedString, lap: i32, role: Role) {
        let action = SelectLap {
            session: session.clone(),
            lap,
            role,
        };
        cx.update_window(self.window, |_, window, cx| {
            window.dispatch_action(Box::new(action), cx)
        })
        .unwrap();
        self.settle(cx).await;
    }

    async fn settle(&self, cx: &mut TestAppContext) {
        cx.run_until_parked();
        let session = self.test.app.session.clone();
        let filmstrip = self.filmstrip(cx);
        cx.wait_for(self.window, Duration::from_secs(600), move |_, cx| {
            let session = session.read(cx);
            let loaded = [session.primary(), session.reference()]
                .into_iter()
                .flatten()
                .all(|slot| slot.loaded().is_some());
            loaded
                && filmstrip
                    .read(cx)
                    .rows()
                    .iter()
                    .all(|row| !row.items().is_empty())
        })
        .await;
        cx.update_window(self.window, |_, window, cx| window.render_frame(cx))
            .unwrap();
    }

    fn filmstrip(
        &self,
        cx: &mut TestAppContext,
    ) -> gpui_kit::Entity<omatrack_app::workspace::Filmstrip> {
        cx.update(|cx| self.test.workspace.read(cx).filmstrip().clone())
    }

    /// (session, roles) per row, top to bottom.
    fn rows(&self, cx: &mut TestAppContext) -> Vec<(SharedString, Vec<LapRole>)> {
        let filmstrip = self.filmstrip(cx);
        cx.update(|cx| {
            filmstrip
                .read(cx)
                .rows()
                .iter()
                .map(|row| (row.session().clone(), row.roles()))
                .collect()
        })
    }

    fn lap(&self, cx: &mut TestAppContext, role: Role) -> Option<(SharedString, i32)> {
        let session = self.test.app.session.clone();
        cx.update(|cx| {
            session
                .read(cx)
                .slot(role)
                .map(|slot| (slot.lap_ref().session().clone(), slot.lap_ref().lap()))
        })
    }

    /// Click (or right-click) the cell of `lap` on `session`'s row.
    fn click_cell(&self, cx: &mut TestAppContext, session: &SharedString, lap: i32, right: bool) {
        let strip = ElementId::Name(format!("filmstrip-{session}").into());
        cx.update_window(self.window, |_, window, cx| {
            let mut row = window.within(strip);
            if right {
                row.right_click(("lap-strip-cell", lap as u32), cx);
            } else {
                row.click(("lap-strip-cell", lap as u32), cx);
            }
        })
        .unwrap();
    }
}

#[gpui_kit::test]
async fn two_recordings_show_one_row_per_role(cx: &mut TestAppContext) {
    let f = library(cx).await;
    f.select(cx, &f.run1, 2, Role::Primary).await;
    assert_eq!(f.rows(cx), vec![(f.run1.clone(), vec![LapRole::Primary])]);

    f.select(cx, &f.run2, 3, Role::Reference).await;
    assert_eq!(
        f.rows(cx),
        vec![
            (f.run1.clone(), vec![LapRole::Primary]),
            (f.run2.clone(), vec![LapRole::Reference]),
        ],
        "the primary recording's row, then the reference's"
    );
    cx.update_window(f.window, |_, window, _| {
        let primary = window.find("filmstrip-primary");
        let reference = window.find("filmstrip-reference");
        assert!(primary.bounds().bottom() <= reference.bounds().top());
        assert!(
            primary
                .label()
                .unwrap()
                .starts_with("Primary lap L2 0:20.100 · Run1"),
            "{:?}",
            primary.label()
        );
        assert!(
            reference
                .label()
                .unwrap()
                .starts_with("Reference lap L3 0:19.900 · Run2"),
            "{:?}",
            reference.label()
        );
        // Above the docks, below the title bar.
        let strip = window.find("filmstrip").bounds();
        assert!(window.find("header-track").bounds().bottom() <= strip.top());
        assert!(strip.bottom() <= window.find("workspace-dock").bounds().top());
        // The Traces panel no longer carries its own strip.
        assert!(window.find("traces-panel").bounds().top() >= strip.bottom());
    })
    .unwrap();

    // Swap is on the filmstrip's gutter: the rows follow the roles.
    cx.update_window(f.window, |_, window, cx| window.click("filmstrip-swap", cx))
        .unwrap();
    f.settle(cx).await;
    assert_eq!(
        f.rows(cx),
        vec![
            (f.run2.clone(), vec![LapRole::Primary]),
            (f.run1.clone(), vec![LapRole::Reference]),
        ]
    );
}

#[gpui_kit::test]
async fn a_click_selects_that_rows_lap_and_keeps_the_viewport(cx: &mut TestAppContext) {
    let f = library(cx).await;
    f.select(cx, &f.run1, 2, Role::Primary).await;
    f.select(cx, &f.run2, 2, Role::Reference).await;
    let viewport = f.test.app.viewport.clone();
    let cursor = f.test.app.cursor.clone();
    cx.update(|cx| {
        viewport.update(cx, |viewport, cx| {
            viewport.set_viewport(Viewport::new(0.2, 0.6), cx)
        });
        cursor.update(cx, |cursor, cx| cursor.set_fraction(Some(0.4), cx));
    });

    // A click on the reference row selects the reference lap.
    f.click_cell(cx, &f.run2, 4, false);
    f.settle(cx).await;
    assert_eq!(f.lap(cx, Role::Reference), Some((f.run2.clone(), 4)));
    assert_eq!(f.lap(cx, Role::Primary), Some((f.run1.clone(), 2)));

    // A click on the primary row selects the primary lap.
    f.click_cell(cx, &f.run1, 3, false);
    f.settle(cx).await;
    assert_eq!(f.lap(cx, Role::Primary), Some((f.run1.clone(), 3)));
    assert_eq!(f.lap(cx, Role::Reference), Some((f.run2.clone(), 4)));
    cx.update(|cx| {
        assert_eq!(viewport.read(cx).viewport(), Viewport::new(0.2, 0.6));
        assert_eq!(cursor.read(cx).fraction(), Some(0.4));
    });

    // Clicking the lap a role already holds jumps to the lap start.
    f.click_cell(cx, &f.run1, 3, false);
    cx.run_until_parked();
    assert_eq!(f.lap(cx, Role::Primary), Some((f.run1.clone(), 3)));
    cx.update(|cx| {
        assert_eq!(cursor.read(cx).fraction(), Some(0.0));
        assert_eq!(viewport.read(cx).viewport(), Viewport::new(0.2, 0.6));
    });

    // A right click on the primary row compares against that lap.
    f.click_cell(cx, &f.run1, 4, true);
    f.settle(cx).await;
    assert_eq!(f.lap(cx, Role::Reference), Some((f.run1.clone(), 4)));
    assert_eq!(f.lap(cx, Role::Primary), Some((f.run1.clone(), 3)));
}

#[gpui_kit::test]
async fn two_laps_of_one_recording_share_a_row(cx: &mut TestAppContext) {
    let f = library(cx).await;
    f.select(cx, &f.run1, 2, Role::Primary).await;
    f.select(cx, &f.run1, 3, Role::Reference).await;
    assert_eq!(
        f.rows(cx),
        vec![(f.run1.clone(), vec![LapRole::Primary, LapRole::Reference])]
    );
    cx.update_window(f.window, |_, window, _| {
        assert!(window.try_find("filmstrip-reference").is_none());
        let row = window
            .find("filmstrip-primary")
            .label()
            .unwrap()
            .to_string();
        assert!(
            row.starts_with("Primary lap L2 0:20.100, Reference lap L3 0:19.900 · Run1"),
            "{row}"
        );
        let primary = window
            .find(("lap-strip-cell", 2u32))
            .label()
            .unwrap()
            .to_string();
        let reference = window
            .find(("lap-strip-cell", 3u32))
            .label()
            .unwrap()
            .to_string();
        assert!(primary.ends_with("primary"), "{primary}");
        assert!(reference.ends_with("reference"), "{reference}");
    })
    .unwrap();

    // On the shared row a plain click still sets the primary.
    f.click_cell(cx, &f.run1, 4, false);
    f.settle(cx).await;
    assert_eq!(f.lap(cx, Role::Primary), Some((f.run1.clone(), 4)));
    assert_eq!(f.lap(cx, Role::Reference), Some((f.run1.clone(), 3)));
}
