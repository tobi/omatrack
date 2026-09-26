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
use gpui_kit::{
    App, AppContext as _, Context, Entity, FocusHandle, InteractiveElement as _, IntoElement,
    ParentElement as _, Render, SharedString, Styled as _, Subscription, TestSupportExt as _,
    Window, div,
};
use omatrack_core::session::Analysis;
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

/// Everything the map draws for an analysis.
pub fn map_data(analysis: &Analysis) -> TrackMapData {
    let primary = analysis.primary();
    let unified = primary.unified();
    let primary_track = gps_track(&unified.gps_lat, &unified.gps_lon);
    let reference_track = analysis
        .reference()
        .and_then(|lap| gps_track(&lap.unified().gps_lat, &lap.unified().gps_lon));
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
    let corners = primary_track
        .as_ref()
        .map(|track| {
            analysis
                .corners()
                .iter()
                .enumerate()
                .filter_map(|(ix, zone)| {
                    let position = track.position_at((zone.start + zone.end) * 0.5)?;
                    Some(MapCorner::new(corner_id(ix), zone.name.clone(), position))
                })
                .collect()
        })
        .unwrap_or_default();
    let delta = (!analysis.delta().is_empty()).then(|| Arc::<[f64]>::from(analysis.delta()));
    let map = analysis
        .comparison()
        .map(|comparison| comparison.clone() as Arc<dyn FractionMap>);
    TrackMapData::new()
        .with_centerline(centerline)
        .with_primary(primary_track)
        .with_reference(reference_track)
        .with_delta(delta)
        .with_map(map)
        .with_corners(corners)
}

/// The map's id of the corner at `ix` in lap order.
fn corner_id(ix: usize) -> u32 {
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
                    .children(comparing.then(|| entry(theme.success, "Gaining")))
                    .children(comparing.then(|| entry(theme.danger, "Losing"))),
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
