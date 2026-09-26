//! Map: the Track Atlas centerline with both laps' GPS traces, the primary
//! coloured by the slope of the delta (gaining, losing), and the cursor's
//! P and R positions.
//!
//! The drawing, projection, hover snapping and the static/overlay split
//! belong to [`TrackMap`] (omatrack-trace). This panel owns the data it is
//! given ([`TrackMapData`], rebuilt only when the analysis changes identity)
//! and routes its intent to the shared cursor: hovering previews the
//! cursor's readout position, a click moves the cursor.

use std::sync::Arc;

use gpui_kit::component::{ActiveTheme as _, IconName, Sizable as _, h_flex, v_flex};
use gpui_kit::prelude::FluentBuilder as _;
use gpui_kit::{
    App, AppContext as _, Context, Entity, FocusHandle, InteractiveElement as _, IntoElement,
    ParentElement as _, Render, SharedString, Styled as _, Subscription, TestSupportExt as _,
    Window, div,
};
use omatrack_core::session::{Analysis, CornerSource};
use omatrack_trace::{
    FractionMap, GeoPoint, GpsTrack, MapCorner, Selection, TrackMap, TrackMapData, TrackMapEvent,
};
use omatrack_ui::{LapRole, Swatch};

use crate::panels::{PanelKind, analysis_body, empty_state, panel_body};
use crate::state::AppState;

/// A GPS lap, when it has at least two usable fixes.
fn gps_track(latitude: &[f64], longitude: &[f64]) -> Option<GpsTrack> {
    let usable = latitude
        .iter()
        .zip(longitude)
        .filter(|(lat, lon)| GeoPoint::new(**lon, **lat).is_valid())
        .take(2)
        .count();
    (usable == 2).then(|| GpsTrack::new(Arc::from(latitude), Arc::from(longitude)))
}

/// `track` (the reference lap) resampled onto the primary's `samples`
/// grid through `map`: sample `i` is where the reference was at primary
/// fraction `i / (samples - 1)`.
fn stationed_track(track: &GpsTrack, map: &dyn FractionMap, samples: usize) -> Option<GpsTrack> {
    if samples < 2 {
        return None;
    }
    let last = (samples - 1) as f64;
    let (latitude, longitude): (Vec<f64>, Vec<f64>) = (0..samples)
        .map(|i| {
            track
                .position_at(map.reference_fraction(i as f64 / last))
                .map_or((f64::NAN, f64::NAN), |p| (p.lat, p.lon))
        })
        .unzip();
    gps_track(&latitude, &longitude)
}

/// The share of usable fixes a lap's GPS needs to be drawn as itself;
/// below it the dropouts leave broken fragments off the track, so both laps
/// are placed on the atlas outline instead (and the panels say so).
/// Said wherever a map draws laps placed on the outline.
pub(crate) const OUTLINE_NOTE: &str = "Laps placed on the track outline (GPS dropouts)";

pub(crate) const MIN_GPS_COVERAGE: f64 = 0.9;

/// The share of samples with a usable fix.
fn coverage(latitude: &[f64], longitude: &[f64]) -> f64 {
    if latitude.is_empty() {
        return 0.0;
    }
    let usable = latitude
        .iter()
        .zip(longitude)
        .filter(|(lat, lon)| GeoPoint::new(**lon, **lat).is_valid())
        .count();
    usable as f64 / latitude.len() as f64
}

/// A lap placed on the atlas `centerline`: sample `i` sits at the
/// centerline point at its share of lap distance (`distance[i] / total`,
/// else its share of the samples), interpolated along the centerline's
/// cumulative metres.
fn centerline_track(centerline: &[GeoPoint], distance: &[f64], samples: usize) -> Option<GpsTrack> {
    if centerline.len() < 2 || samples < 2 {
        return None;
    }
    let mut cumulative = Vec::with_capacity(centerline.len());
    let mut total_m = 0.0;
    cumulative.push(0.0);
    for pair in centerline.windows(2) {
        let lat = (pair[0].lat + pair[1].lat).to_radians() * 0.5;
        let dx = (pair[1].lon - pair[0].lon).to_radians() * lat.cos();
        let dy = (pair[1].lat - pair[0].lat).to_radians();
        total_m += dx.hypot(dy) * 6_371_000.0;
        cumulative.push(total_m);
    }
    if total_m <= 0.0 {
        return None;
    }
    let lap_total = distance
        .last()
        .copied()
        .filter(|d| d.is_finite() && *d > 0.0 && distance.len() == samples);
    let last = (samples - 1) as f64;
    let (latitude, longitude): (Vec<f64>, Vec<f64>) = (0..samples)
        .map(|i| {
            let share = match lap_total {
                Some(total) if distance[i].is_finite() => distance[i] / total,
                _ => i as f64 / last,
            };
            let target = share.clamp(0.0, 1.0) * total_m;
            let k = cumulative
                .partition_point(|m| *m <= target)
                .clamp(1, centerline.len() - 1);
            let span = cumulative[k] - cumulative[k - 1];
            let t = if span > 0.0 {
                (target - cumulative[k - 1]) / span
            } else {
                0.0
            };
            let (a, b) = (centerline[k - 1], centerline[k]);
            (a.lat + (b.lat - a.lat) * t, a.lon + (b.lon - a.lon) * t)
        })
        .unzip();
    gps_track(&latitude, &longitude)
}

/// Everything the map draws for an analysis.
pub fn map_data(analysis: &Analysis) -> TrackMapData {
    // Under a LOW-confidence alignment the gain/loss colouring would claim a
    // station-by-station verdict the time share cannot support.
    let delta = (!analysis.delta().is_empty()
        && !crate::workspace::status::analysis_approximate(analysis))
    .then(|| Arc::<[f64]>::from(analysis.delta()));
    map_layers(analysis).with_delta(delta)
}

/// The map's layers without any colouring of the primary: the centerline,
/// both GPS laps, the shared map and the corner labels.
pub(crate) fn map_layers(analysis: &Analysis) -> TrackMapData {
    let primary = analysis.primary();
    let unified = primary.unified();
    let centerline: Vec<GeoPoint> = primary
        .layout()
        .map(|layout| {
            layout
                .centerline
                .iter()
                .copied()
                .map(GeoPoint::from)
                .collect()
        })
        .unwrap_or_default();
    let reference = analysis.reference().map(|lap| lap.unified());
    let reference_coverage = reference.map(|lap| coverage(&lap.gps_lat, &lap.gps_lon));
    let stationed = analysis.corner_source() == CornerSource::Reference;
    // A stationed primary is drawn from the reference's fixes.
    let primary_coverage = if stationed {
        reference_coverage.unwrap_or(0.0)
    } else {
        coverage(&unified.gps_lat, &unified.gps_lon)
    };
    let broken = primary_coverage < MIN_GPS_COVERAGE
        || reference_coverage.is_some_and(|c| c < MIN_GPS_COVERAGE);
    let on_outline = broken && centerline.len() >= 2;
    let (primary_track, reference_track) = if on_outline {
        (
            centerline_track(&centerline, &unified.distance, unified.len()),
            reference.and_then(|lap| centerline_track(&centerline, &lap.distance, lap.len())),
        )
    } else {
        let reference_track = reference.and_then(|lap| gps_track(&lap.gps_lat, &lap.gps_lon));
        let primary_track = if stationed {
            // The primary's own GPS misses the circuit (the core carried the
            // reference's corners over for the same reason): place the
            // primary where the reference was at the same station, through
            // the one map.
            reference_track
                .as_ref()
                .zip(analysis.comparison())
                .and_then(|(track, comparison)| {
                    stationed_track(track, comparison.as_ref(), unified.len())
                })
        } else {
            gps_track(&unified.gps_lat, &unified.gps_lon)
        };
        (primary_track, reference_track)
    };
    let corners = primary_track
        .as_ref()
        .map(|track| {
            analysis
                .corners()
                .iter()
                .enumerate()
                .filter_map(|(ix, zone)| {
                    let middle = (zone.start + zone.end) * 0.5;
                    let position = track.position_near(middle, zone.start, zone.end)?;
                    Some(MapCorner::new(corner_id(ix), zone.name.clone(), position))
                })
                .collect()
        })
        .unwrap_or_default();
    let map = analysis
        .comparison()
        .map(|comparison| comparison.clone() as Arc<dyn FractionMap>);
    TrackMapData::new()
        .with_centerline(centerline)
        .with_primary(primary_track)
        .with_reference(reference_track)
        .with_map(map)
        .with_corners(corners)
        .with_outline_placement(on_outline)
}

/// The map's id of the corner at `ix` in lap order.
pub(crate) fn corner_id(ix: usize) -> u32 {
    u32::try_from(ix + 1).unwrap_or(u32::MAX)
}

pub struct MapPanel {
    app: AppState,
    focus_handle: FocusHandle,
    map: Entity<TrackMap>,
    shown: Option<Arc<Analysis>>,
    followed_focus: Option<Selection>,
    _subscriptions: Vec<Subscription>,
}

impl MapPanel {
    pub fn new(app: AppState, cx: &mut Context<Self>) -> Self {
        let map = cx.new(|cx| TrackMap::new(app.cursor.clone(), cx));
        let subscriptions = vec![
            cx.observe(&app.session, |this, _, cx| this.sync_analysis(cx)),
            cx.subscribe(&map, |this, _, event, cx| this.on_map_event(event, cx)),
            // The map redraws its own overlay on cursor moves; this panel
            // only follows a change of corner focus.
            cx.observe(&app.cursor, |this, _, cx| this.follow_focus(cx)),
        ];
        let mut panel = Self {
            app,
            focus_handle: cx.focus_handle(),
            map,
            shown: None,
            followed_focus: None,
            _subscriptions: subscriptions,
        };
        panel.sync_analysis(cx);
        panel
    }

    /// The track map view.
    pub fn map(&self) -> &Entity<TrackMap> {
        &self.map
    }

    fn sync_analysis(&mut self, cx: &mut Context<Self>) {
        let analysis = self.app.session.read(cx).analysis().cloned();
        let same = match (&analysis, &self.shown) {
            (Some(a), Some(b)) => Arc::ptr_eq(a, b),
            (None, None) => true,
            _ => false,
        };
        if !same {
            let data = Arc::new(
                analysis
                    .as_deref()
                    .map(map_data)
                    .unwrap_or_else(TrackMapData::new),
            );
            self.map.update(cx, |map, cx| {
                map.set_data(data, cx);
                map.set_focused_corner(None, cx);
            });
            self.shown = analysis;
            self.followed_focus = None;
            self.follow_focus(cx);
        }
        cx.notify();
    }

    fn on_map_event(&mut self, event: &TrackMapEvent, cx: &mut Context<Self>) {
        match event {
            TrackMapEvent::MapHover(fraction) => {
                let fraction = *fraction;
                self.app
                    .cursor
                    .update(cx, |cursor, cx| cursor.set_hover(fraction, cx));
            }
            TrackMapEvent::MapClicked(fraction) => {
                let fraction = *fraction;
                self.app
                    .cursor
                    .update(cx, |cursor, cx| cursor.set_fraction(Some(fraction), cx));
            }
            _ => {}
        }
    }

    /// Highlight the focused corner's label on the map.
    fn follow_focus(&mut self, cx: &mut Context<Self>) {
        let focus = self.app.cursor.read(cx).focus();
        if focus == self.followed_focus {
            return;
        }
        self.followed_focus = focus;
        let id = focus.and_then(|focus| {
            let analysis = self.shown.as_ref()?;
            analysis
                .corners()
                .iter()
                .position(|zone| {
                    (zone.start - focus.start).abs() < 1e-9 && (zone.end - focus.end).abs() < 1e-9
                })
                .map(corner_id)
        });
        self.map
            .update(cx, |map, cx| map.set_focused_corner(id, cx));
    }

    fn render_legend(&self, analysis: &Analysis, cx: &App) -> impl IntoElement {
        let theme = cx.theme();
        let entry = |color, label: &'static str| {
            h_flex()
                .gap_1()
                .child(Swatch::new(color).xsmall())
                .child(label)
        };
        let title: SharedString = match analysis.primary().layout() {
            Some(layout) => format!("{} · {}", layout.track_name, layout.layout_name).into(),
            None => "GPS only · no Track Atlas layout".into(),
        };
        let comparing = analysis.reference().is_some() && !analysis.delta().is_empty();
        let approximate = crate::workspace::status::analysis_approximate(analysis);
        let shaded = comparing && !approximate;
        let on_outline = self.map.read(cx).data().is_on_outline();
        v_flex()
            .flex_shrink_0()
            .gap_1()
            .px_2()
            .py_1()
            .border_b_1()
            .border_color(theme.border)
            .text_xs()
            .text_color(theme.muted_foreground)
            .child(div().truncate().child(title))
            .when(on_outline, |this| {
                this.child(
                    div()
                        .id("map-outline-note")
                        .test_support()
                        .truncate()
                        .child(OUTLINE_NOTE),
                )
            })
            .child(
                h_flex()
                    .flex_wrap()
                    .gap_3()
                    .child(entry(LapRole::Primary.color(theme), "Primary"))
                    .children(
                        analysis
                            .reference()
                            .map(|_| entry(LapRole::Reference.color(theme), "Reference")),
                    )
                    .children(shaded.then(|| entry(theme.success, "Gaining")))
                    .children(shaded.then(|| entry(theme.danger, "Losing"))),
            )
    }
}

impl gpui_kit::component::dock::BasePanel for MapPanel {
    fn panel_name(&self) -> &'static str {
        PanelKind::Map.name()
    }

    fn closable(&self, _: &App) -> bool {
        false
    }
}

impl gpui_kit::component::dock::Panel for MapPanel {
    fn tab_name(&self, _: &App) -> Option<SharedString> {
        Some(PanelKind::Map.title().into())
    }

    fn title(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        PanelKind::Map.title()
    }
}

impl gpui_kit::EventEmitter<gpui_kit::component::dock::PanelEvent> for MapPanel {}

impl gpui_kit::Focusable for MapPanel {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for MapPanel {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let root = div()
            .id("map-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full();
        let Some(analysis) = self.shown.clone() else {
            return root
                .child(analysis_body("map-summary", &self.app, cx, |_| {
                    SharedString::default()
                }))
                .into_any_element();
        };
        if self.map.read(cx).data().is_empty() {
            let description = "This lap has no GPS positions and no Track Atlas layout.";
            return root
                .child(panel_body(
                    "map-summary",
                    format!("No map. {description}"),
                    empty_state(IconName::Map, "No map", description),
                    cx,
                ))
                .into_any_element();
        }
        root.child(
            v_flex()
                .size_full()
                .child(self.render_legend(&analysis, cx))
                .child(div().flex_1().min_h_0().child(self.map.clone())),
        )
        .into_any_element()
    }
}

/// This panel's app-wide setup: its dock registration (saved layouts
/// rebuild it by name). Called once from [`crate::panels::init`].
pub fn init(cx: &mut App) {
    crate::panels::register(PanelKind::Map, cx);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn coverage_counts_usable_fixes() {
        let lat = [45.0, f64::NAN, 45.1, f64::NAN];
        let lon = [7.0, f64::NAN, 7.1, f64::NAN];
        assert!((coverage(&lat, &lon) - 0.5).abs() < 1e-12);
        assert_eq!(coverage(&[], &[]), 0.0);
    }

    #[test]
    fn centerline_track_places_samples_by_distance_share() {
        // An L: 0.01° north, then 0.01° east (about equal metres near 1°N).
        let centerline = [
            GeoPoint::new(1.0, 1.0),
            GeoPoint::new(1.0, 1.01),
            GeoPoint::new(1.01, 1.01),
        ];
        let distance = [0.0, 250.0, 500.0, 750.0, 1000.0];
        let track = centerline_track(&centerline, &distance, distance.len()).unwrap();
        assert_eq!(track.len(), 5);
        let start = track.position_at(0.0).unwrap();
        let middle = track.position_at(0.5).unwrap();
        let end = track.position_at(1.0).unwrap();
        assert!((start.lat - 1.0).abs() < 1e-9 && (start.lon - 1.0).abs() < 1e-9);
        assert!((middle.lat - 1.01).abs() < 1e-5 && (middle.lon - 1.0).abs() < 1e-5);
        assert!((end.lat - 1.01).abs() < 1e-9 && (end.lon - 1.01).abs() < 1e-9);
        // Without distance, the share of samples places them.
        let by_index = centerline_track(&centerline, &[], 3).unwrap();
        assert!((by_index.position_at(0.5).unwrap().lat - 1.01).abs() < 1e-5);
        assert!(centerline_track(&centerline[..1], &distance, 5).is_none());
    }
}
