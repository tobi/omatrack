//! omatrack-trace: trace math and the GPUI trace workspace.
//!
//! # Modules
//!
//! Pure (no GPUI, unit-tested as math):
//! - [`decimate`]: 1:1 port of `TraceDecimator` (per-device-column temporal
//!   min/max, NaN pen-ups, per-column forward map, slope corridor).
//! - [`scale`]: the viewport in lap fraction (zoom floor 1e-7), zoom/pan,
//!   corner focus placement, Distance/Time ticks.
//! - [`layout`]: `TraceLaneSizing` and lane layout (FIT/manual, 20 px
//!   minimum, combined lanes, overlay-group adjacency, resize borrowing,
//!   pinned region above the scroll region).
//! - [`interaction`]: the gesture state machine of `TraceInteraction`.
//! - [`mesh`]: non-overlapping triangle meshes (strokes, baseline fills,
//!   min–max bands).
//!
//! GPUI:
//! - [`palette`]: trace colours from `cx.theme()` tokens.
//! - [`scene`]: the immutable, `Arc`-shared [`TraceScene`] and [`FractionMap`].
//! - [`state`]: the shared [`ViewportState`] and [`CursorState`] entities.
//! - [`lanes`]: per-channel geometry cached as GPUI paths.
//! - [`static_layer`]: the cached static view (grids, masks, paths, and the
//!   optional [`TraceLayers`]: session spread behind the lanes, event ticks).
//! - `overlay` (private): cursor, hover, selection, focus dimming, corner
//!   zones and the pointer surface.
//! - [`axis`]: the shared x-axis row.
//! - [`stack`]: [`TraceStack`], the composed workspace view.
//! - [`corner_ruler`]: [`CornerRuler`], corner bands and complex brackets
//!   on the shared viewport, with click-to-focus and edge editing.
//! - [`track_map`]: [`TrackMap`], the atlas centerline and both GPS laps,
//!   the primary coloured by the delta's slope, with cursor dots.
//! - [`damper_strip`]: [`DamperStrip`], the manual damper alignment tool.
//! - [`telemetry_hud`]: [`TelemetryHud`], the fullscreen video telemetry band
//!   (progress window, pedals, steering dial, gear, speed, gap).
//! - [`synthetic`]: deterministic laps for tests and `examples/trace_bench`.
//!
//! # Rendering contract (`docs/TRACE_RENDERING.md`)
//!
//! - Decimation keeps source-ordered extrema per device column; a
//!   non-finite sample lifts the pen even mid-column; the reference is
//!   placed through the shared map evaluated per column. Buffers are
//!   caller-owned and reused: nothing allocates after warm-up.
//! - Strokes are logical-pixel width with shared joins (miter limit 2) and
//!   no caps. Fills are non-overlapping trapezoids with a linear gradient
//!   from the peak alpha to transparent at the baseline; Δ fills diverge
//!   into gain (success) and loss (danger). GPUI rasterizes path triangles
//!   with MSAA and premultiplied blending, so overlap would double-blend;
//!   strokes are opaque (tokens pre-mixed against the background).
//! - Paint order is primary area → reference outline → primary outline.
//! - Static and overlay are separate passes: the static layer is an entity
//!   embedded with `Entity::cached`, notified only on scene, viewport,
//!   layout or style change. It never reads [`CursorState`]. Geometry keys
//!   never include a colour. GPUI refreshes the whole window on a theme or
//!   focus change; that repaints the static layer from its cache without
//!   rebuilding geometry.
//! - GPUI paths are non-indexed `Vec<PathVertex>` drawn with 32-bit vertex
//!   indices, so the Qt scene graph's 65,535-vertex cliff does not exist;
//!   paths are still chunked (see below).
//!
//! # Reuse of gpui-component's chart/plot primitives
//!
//! Every decision below was made against the full-session 50 Hz workload of
//! `examples/trace_bench.rs`.
//!
//! - `plot::Plot` lifecycle: followed in idiom, not implemented. Lanes build
//!   geometry once per shape key ([`lanes::ChannelGeometry::prepare`]) and
//!   paint it relative to the frame origin ([`lanes::ChannelGeometry::paint`]),
//!   the `Plot` prepaint/paint split. The stack itself is an entity (shared
//!   viewport/cursor, gestures, focus), which `Plot`'s value-element shape
//!   cannot hold.
//! - `plot::PathCache` / `ShapeKey`: the translate-at-paint idea is reused
//!   ([`lanes::PathBuffer::translated`]); the type is not. `PathCache::get`
//!   replaces its path on every rebuild (no capacity reuse across a zoom
//!   sweep) and `ShapeKey` hashes projected points, O(vertices) per frame.
//!   Omatrack keys on the inputs instead (scene generation, viewport, lane
//!   size, dpr, stroke width, fill).
//! - `plot::shape::Line` / `Area` and `PathBuilder`: never used for traces.
//!   They skip non-finite points instead of lifting the pen, and tessellate
//!   through lyon with 16-bit indices.
//! - `plot::scale::ScaleLinear`: not reused. The horizontal scale is a
//!   viewport in lap fraction with a 1e-7 floor and a nonlinear reference
//!   map evaluated per device column; the vertical mapping is part of the
//!   1:1 decimator port.
//! - `plot::PlotAxis` / `Grid`: the idiom is reused (ticks computed once per
//!   viewport, grid lines at the tick positions, text labels), but ticks
//!   come from lap distance/time arrays, so the code is omatrack's own.
//! - `plot::tooltip::CrossLine` / `Dot`: not reused. They are `div` trees laid
//!   out every frame; the overlay paints the same crosshair and dots as
//!   quads in one element, keeping a cursor frame free of layout.
//!
//! # Path chunking
//!
//! `Window::paint_path` copies each path twice (device scale, scene insert)
//! on top of the translate-copy a cached path needs. At the full-lap zoom a
//! 16-series frame is ~230k vertices; paths are therefore split into chunks
//! of [`lanes::CHUNK_VERTICES`] so those copies recycle heap memory instead
//! of page-faulting fresh mappings. Each fill chunk carries the same pinned
//! vertical extent, so the gradient stays continuous across chunks.
pub mod axis;
pub mod corner_ruler;
pub mod damper_strip;
pub mod decimate;
pub mod interaction;
mod label;
pub mod lanes;
pub mod layout;
pub mod mesh;
mod overlay;
pub mod palette;
pub mod scale;
pub mod scene;
pub mod stack;
pub mod state;
pub mod static_layer;
pub mod synthetic;
pub mod telemetry_hud;
pub mod track_map;

pub use corner_ruler::{CornerRuler, CornerRulerEvent};
pub use damper_strip::{DamperStrip, DamperStripData, DamperStripEvent};
pub use palette::ColorMode;
pub use scale::{Viewport, XAxis};
pub use scene::{
    Apex, ComplexBand, CornerBand, EventMark, EventMarkKind, FractionMap, LaneKind, LaneSeries,
    LaneSpread, LaneStyle, LaneStyles, Readout, TraceLayers, TraceScene, YRange,
};
pub use stack::{CHROME_REMS, GUTTER_REMS, LEGEND_REMS, TraceEvent, TraceStack};
pub use state::{CursorState, Selection, ViewportState};
pub use static_layer::{StaticStats, TraceStaticView};
pub use telemetry_hud::{TelemetryHud, TelemetryHudBuffers, TelemetryHudColors, TelemetryHudData};
pub use track_map::{GeoPoint, GpsTrack, MapCorner, TrackMap, TrackMapData, TrackMapEvent};
