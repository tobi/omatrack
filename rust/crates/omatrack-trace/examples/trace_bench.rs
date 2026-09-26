//! Trace geometry benchmark, mirroring the Qt `TraceView::benchmarkGeometry`.
//!
//! Workload: 8 lanes × 2 laps (primary + reference through a nonlinear map,
//! Δ, filled pedals, stepped gear) of ~90 s at 50 Hz, a zoom sweep from 1× to
//! 10,000× about a wandering anchor, device pixel ratio 2. The Qt precedent
//! ran a 1280×800 logical window at DPR 2 (2560 physical columns); the
//! stress row doubles the width to 2560 logical pixels.
//!
//! Reported per frame:
//! - geometry: decimation + meshing into `Path` vertices for every lane
//!   (what this crate controls; the Qt "benchmarkGeometry" number);
//! - submit: the CPU copies GPUI's `paint_path` makes of those paths
//!   (translate-clone, scale to device pixels, scene insert clone);
//! - hover: the per-cursor overlay work (readouts and crosshair dots for
//!   every lane), the Qt "TraceCursorOverlay::benchmarkGeometry" number.
//!
//! With `OMATRACK_FIXTURES` set, the fastest laps of the Run4/Run1 AiM
//! recordings replace the synthetic laps (read-only).
//!
//! Run: `cargo run --release --locked -p omatrack-trace --example trace_bench`

use std::sync::Arc;
use std::time::Instant;

use gpui_kit::{point, px};
use omatrack_trace::lanes::{BuildInput, ChannelGeometry, Scratch, SpreadGeometry};
use omatrack_trace::layout::{LayoutMode, layout_lanes};
use omatrack_trace::scene::{LaneKind, LaneSeries, LaneStyles, TraceScene, YRange};
use omatrack_trace::{Viewport, synthetic};

const FRAMES: usize = 310;
const DPR: f32 = 2.0;
/// Other session laps drawn in the Consistency row.
const CONSISTENCY_LAPS: usize = 8;

struct Stats {
    avg: f64,
    worst: f64,
    cold: f64,
    p95: f64,
}

fn stats(samples: &mut [f64]) -> Stats {
    let cold = samples[0];
    let avg = samples.iter().sum::<f64>() / samples.len() as f64;
    let worst = samples.iter().cloned().fold(0.0, f64::max);
    samples.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let p95 = samples[(samples.len() as f64 * 0.95) as usize - 1];
    Stats {
        avg,
        worst,
        cold,
        p95,
    }
}

/// The zoom sweep: log-spaced 1× → 10,000× and back, about a drifting anchor.
fn sweep() -> Vec<Viewport> {
    let half = FRAMES / 2;
    (0..FRAMES)
        .map(|i| {
            let t = if i < half {
                i as f64 / (half - 1) as f64
            } else {
                (FRAMES - 1 - i) as f64 / (half - 1) as f64
            };
            let zoom = 10f64.powf(4.0 * t);
            let anchor = 0.37 + 0.2 * (i as f64 * 0.05).sin();
            let span = 1.0 / zoom;
            let start = (anchor - 0.5 * span).clamp(0.0, 1.0 - span);
            Viewport::new(start, start + span)
        })
        .collect()
}

fn real_scene() -> Option<TraceScene> {
    use omatrack_core::alignment::Strategy;
    use omatrack_core::{ChannelOverrides, Comparison, Recording, fastest_lap_index};
    let root = std::env::var("OMATRACK_FIXTURES").ok()?;
    let dir = std::path::PathBuf::from(root).join("CT1");
    let find = |run: &str| {
        std::fs::read_dir(&dir)
            .ok()?
            .flatten()
            .map(|e| e.path())
            .find(|p| {
                p.to_string_lossy().contains(run)
                    && p.extension().is_some_and(|e| e.eq_ignore_ascii_case("mp4"))
            })
    };
    let load = |run: &str| -> Option<Arc<omatrack_core::UnifiedLap>> {
        let recording = Recording::open(find(run)?).ok()?;
        let mut laps = recording.detect_laps();
        let best = fastest_lap_index(&mut laps);
        let lap = laps[best].clone();
        Some(Arc::new(recording.unify_lap(
            lap.start_time,
            lap.end_time,
            &ChannelOverrides::default(),
        )))
    };
    let primary = load("Run4")?;
    let reference = load("Run1")?;
    let comparison = Arc::new(Comparison::new(
        primary.clone(),
        reference.clone(),
        Strategy::Gps,
        Vec::new(),
        0.0,
    ));
    let arc = |v: &[f64]| -> Arc<[f64]> { v.into() };
    let gear = |lap: &omatrack_core::UnifiedLap| -> Arc<[f64]> {
        lap.gear.iter().map(|&g| g as f64).collect()
    };
    let pair = |key: &'static str, title: &'static str, kind, p: Arc<[f64]>, r: Arc<[f64]>| {
        LaneSeries::new(key, title, kind, p).with_reference(Some(r))
    };
    let lanes = vec![
        pair(
            "speed",
            "Speed",
            LaneKind::Line,
            arc(&primary.speed),
            arc(&reference.speed),
        ),
        pair(
            "throttle",
            "Throttle",
            LaneKind::Area,
            arc(&primary.throttle),
            arc(&reference.throttle),
        )
        .with_y_range(YRange::new(0.0, 1.0)),
        pair(
            "brake",
            "Brake",
            LaneKind::Area,
            arc(&primary.brake),
            arc(&reference.brake),
        ),
        pair(
            "gear",
            "Gear",
            LaneKind::Step,
            gear(&primary),
            gear(&reference),
        )
        .with_y_range(YRange::new(0.0, 7.0)),
        pair(
            "steering",
            "Steering",
            LaneKind::Line,
            arc(&primary.steering),
            arc(&reference.steering),
        ),
        pair(
            "g_lat",
            "G lat",
            LaneKind::Line,
            arc(&primary.g_force_lat),
            arc(&reference.g_force_lat),
        ),
        pair(
            "g_long",
            "G long",
            LaneKind::Line,
            arc(&primary.g_force_long),
            arc(&reference.g_force_long),
        ),
        LaneSeries::new("delta", "Δ time", LaneKind::Delta, arc(comparison.delta())),
    ];
    eprintln!(
        "real laps: {} / {} samples, basis {}",
        primary.len(),
        reference.len(),
        comparison.basis()
    );
    let map: Arc<dyn omatrack_trace::FractionMap> = comparison;
    Some(
        TraceScene::new(arc(&primary.distance), arc(&primary.time))
            .with_lanes(lanes)
            .with_map(Some(map)),
    )
}

struct Row {
    label: String,
    geometry: Stats,
    submit: Stats,
    vertices_avg: f64,
    vertices_max: usize,
    lanes: usize,
    paths: usize,
}

fn run(scene: &TraceScene, label: &str, width: f32, height: f32) -> Row {
    let styles = LaneStyles::new();
    let sizing: Vec<_> = scene
        .lanes()
        .iter()
        .map(|lane| styles.get(&lane.key).sizing)
        .collect();
    let layout = layout_lanes(&sizing, LayoutMode::default(), height as f64, 0.0);
    // Each frame builds geometry cold for the new viewport, like Qt's
    // benchmark (which rebuilt the whole scene into scratch builders).
    let mut geometries: Vec<ChannelGeometry> = scene
        .lanes()
        .iter()
        .map(|_| ChannelGeometry::default())
        .collect();
    let mut spreads: Vec<SpreadGeometry> = scene
        .lanes()
        .iter()
        .map(|_| SpreadGeometry::default())
        .collect();
    let mut scratch = Scratch::default();
    let mut geometry_ms = Vec::with_capacity(FRAMES);
    let mut submit_ms = Vec::with_capacity(FRAMES);
    let mut vertices = Vec::with_capacity(FRAMES);
    let mut paths = 0;
    for viewport in sweep() {
        let started = Instant::now();
        for slot in &layout.slots {
            for index in slot.channels() {
                let series = &scene.lanes()[index];
                let style = styles.get(&series.key);
                let input = BuildInput::new(scene, series, viewport, width, slot.height as f32)
                    .with_dpr(DPR)
                    .with_stroke_width(style.stroke_width)
                    .with_fill(style.fill_for(series.kind, &series.key) > 0.0);
                geometries[index].prepare(&input, &mut scratch);
                if series.spread.is_some() {
                    spreads[index].prepare(&input, &mut scratch);
                }
            }
        }
        geometry_ms.push(started.elapsed().as_secs_f64() * 1e3);

        // GPUI's paint_path: translate-clone (ours), scale to device pixels
        // and a clone into the scene (GPUI's).
        let started = Instant::now();
        let mut frame_vertices = 0;
        paths = 0;
        for (slot_ix, slot) in layout.slots.iter().enumerate() {
            for index in slot.channels() {
                let spread = scene.lanes()[index]
                    .spread
                    .is_some()
                    .then(|| spreads[index].buffers())
                    .into_iter()
                    .flatten();
                for buffer in geometries[index].buffers().into_iter().chain(spread) {
                    for path in buffer.translated(point(px(0.), px(slot_ix as f32 * 10.0))) {
                        let scaled = path.scale(DPR);
                        let inserted = scaled.clone();
                        frame_vertices += inserted.vertices.len();
                        paths += 1;
                        std::hint::black_box(inserted);
                    }
                }
            }
        }
        submit_ms.push(started.elapsed().as_secs_f64() * 1e3);
        vertices.push(frame_vertices);
        if std::env::var("TRACE_BENCH_FRAMES").is_ok() {
            eprintln!(
                "{:.6} {:.3} {:.3} {}",
                viewport.span(),
                geometry_ms.last().unwrap(),
                submit_ms.last().unwrap(),
                frame_vertices
            );
        }
    }
    Row {
        label: label.into(),
        geometry: stats(&mut geometry_ms),
        submit: stats(&mut submit_ms),
        vertices_avg: vertices.iter().sum::<usize>() as f64 / vertices.len() as f64,
        vertices_max: vertices.iter().cloned().max().unwrap_or(0),
        lanes: layout.slots.len(),
        paths,
    }
}

fn hover(scene: &TraceScene, height: f32) -> (f64, f64) {
    let styles = LaneStyles::new();
    let sizing: Vec<_> = scene
        .lanes()
        .iter()
        .map(|l| styles.get(&l.key).sizing)
        .collect();
    let layout = layout_lanes(&sizing, LayoutMode::default(), height as f64, 0.0);
    let map = scene.map();
    let frames = 5000;
    let mut samples = Vec::with_capacity(frames);
    let mut sink = 0.0;
    for i in 0..frames {
        let fraction = (i as f64 * 0.618_033_988_75) % 1.0;
        let started = Instant::now();
        // Readouts for every lane plus the crosshair dot positions.
        for slot in &layout.slots {
            for index in slot.channels() {
                let lane = &scene.lanes()[index];
                let r = lane.readout(fraction, map);
                let range = lane.y_range;
                for value in [r.primary, r.reference] {
                    if value.is_finite() {
                        let t = ((value - range.min) / range.span()).clamp(0.0, 1.0);
                        sink += slot.y + 1.0 + (slot.height - 2.0) * (1.0 - t);
                    }
                }
            }
        }
        samples.push(started.elapsed().as_secs_f64() * 1e3);
    }
    std::hint::black_box(sink);
    let avg = samples.iter().sum::<f64>() / samples.len() as f64;
    let worst = samples.iter().cloned().fold(0.0, f64::max);
    (avg, worst)
}

/// Per-cursor-frame cost of the whole stack view in a headless GPUI window:
/// a cursor update, the stack re-render (chrome, readouts, overlay) and one
/// draw, in which GPUI replays the cached static layer (copying its path
/// vertices into the new scene). Measured at the full lap and at a deep zoom
/// to separate the replay cost from the view cost. GPUI's test platform has
/// no GPU and a stub text system: this is CPU shape, not a native frame time.
fn headless_cursor_frames(scene: TraceScene) -> Vec<(String, f64, f64, usize)> {
    use gpui_kit::component::Root;
    use gpui_kit::{AnyWindowHandle, AppContext as _, TestAppContext, size};
    use omatrack_trace::{CursorState, TraceStack, ViewportState};
    let mut cx = TestAppContext::single();
    cx.update(gpui_kit::init);
    let viewport = cx.new(|_| ViewportState::new());
    let cursor = cx.new(|_| CursorState::new());
    let mut stack = None;
    let scene = Arc::new(scene);
    let handle = cx.open_window(size(px(1400.), px(900.)), |window, cx| {
        let view =
            cx.new(|cx| TraceStack::new(scene, viewport.clone(), cursor.clone(), window, cx));
        stack = Some(view.clone());
        Root::new(view, window, cx)
    });
    let stack = stack.unwrap();
    let window: AnyWindowHandle = handle.into();
    let mut rows = Vec::new();
    for (label, view) in [
        ("full lap", Viewport::FULL),
        ("1000x zoom", Viewport::new(0.4, 0.401)),
    ] {
        viewport.update(&mut cx, |v, cx| v.set_viewport(view, cx));
        for _ in 0..3 {
            cx.update_window(window, |_, window, cx| {
                window.refresh();
                window.draw(cx).clear(cx);
            })
            .unwrap();
            cx.run_until_parked();
        }
        let renders_before = cx.update(|cx| stack.read(cx).static_rebuilds(cx));
        let frames = 300;
        let mut samples = Vec::with_capacity(frames);
        for i in 0..frames {
            let fraction = view.start + view.span() * ((i as f64 * 0.618_033_988_75) % 1.0);
            cursor.update(&mut cx, |c, cx| c.set_fraction(Some(fraction), cx));
            cx.run_until_parked();
            let started = Instant::now();
            cx.update_window(window, |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            samples.push(started.elapsed().as_secs_f64() * 1e3);
        }
        let renders_after = cx.update(|cx| stack.read(cx).static_rebuilds(cx));
        let avg = samples.iter().sum::<f64>() / samples.len() as f64;
        let worst = samples.iter().cloned().fold(0.0, f64::max);
        rows.push((
            label.to_string(),
            avg,
            worst,
            renders_after - renders_before,
        ));
    }
    rows
}

fn main() {
    let (scene, source) = match real_scene() {
        Some(scene) => (scene, "real AiM laps (OMATRACK_FIXTURES)"),
        None => (synthetic::scene(), "synthetic 90 s laps"),
    };
    println!(
        "trace_bench: {source}; {} channels × 2 laps; {FRAMES}-frame zoom sweep 1×→10,000×→1×; dpr {DPR}",
        scene.lanes().len()
    );
    let rows = [
        run(
            &scene,
            "1280 lp × 700 (2560 dev px, Qt setup)",
            1280.0,
            700.0,
        ),
        run(
            &scene,
            "2560 lp × 1400 (5120 dev px, stress)",
            2560.0,
            1400.0,
        ),
        run(
            &synthetic::with_session_spread(scene.clone(), CONSISTENCY_LAPS),
            "1280 lp × 700, Consistency (8 session laps + band)",
            1280.0,
            700.0,
        ),
    ];
    println!();
    println!(
        "| workload | lanes | geometry avg ms | geometry p95 ms | geometry worst ms | cold ms | submit avg ms | submit worst ms | vertices avg | vertices max | paths |"
    );
    println!("|---|---|---|---|---|---|---|---|---|---|---|");
    for row in &rows {
        println!(
            "| {} | {} | {:.3} | {:.3} | {:.3} | {:.3} | {:.3} | {:.3} | {:.0} | {} | {} |",
            row.label,
            row.lanes,
            row.geometry.avg,
            row.geometry.p95,
            row.geometry.worst,
            row.geometry.cold,
            row.submit.avg,
            row.submit.worst,
            row.vertices_avg,
            row.vertices_max,
            row.paths
        );
    }
    let (hover_avg, hover_worst) = hover(&scene, 700.0);
    println!();
    println!(
        "hover (overlay readouts + dots, all lanes): avg {hover_avg:.4} ms, worst {hover_worst:.4} ms"
    );
    for (label, avg, worst, renders) in headless_cursor_frames(scene.clone()) {
        println!(
            "headless cursor frame draw, {label} (stack re-render + cached static replay, test platform): avg {avg:.3} ms, worst {worst:.3} ms, static renders over 300 frames: {renders}"
        );
    }
    println!();
    let qt = &rows[0];
    let pass_geometry = qt.geometry.avg <= 4.0 && qt.geometry.worst <= 8.33;
    let pass_hover = hover_avg < 0.1;
    println!(
        "targets (Qt setup row): geometry avg <= 4 ms and worst <= 8.33 ms: {}; hover < 0.1 ms: {}",
        if pass_geometry { "PASS" } else { "FAIL" },
        if pass_hover { "PASS" } else { "FAIL" }
    );
    println!(
        "Qt precedent: 5.92 ms avg / 9.50 ms worst (7 lanes); 8.29 ms median frame, 8-lane 10,000x sweep; hover 0.057 ms"
    );
    if !(pass_geometry && pass_hover) {
        std::process::exit(1);
    }
}
