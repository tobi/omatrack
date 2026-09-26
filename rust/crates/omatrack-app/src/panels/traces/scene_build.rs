//! The trace scene of an [`Analysis`], built off the UI thread, and the lane
//! styles of `channels.<key>` in `omatrack.yml`.
//!
//! A scene holds every channel the primary lap offers, in workspace order:
//! Δ (with a reference), speed, throttle, brake, gear, steering, RPM, then
//! the remaining provider channels. Which lanes show is a style decision
//! ([`lane_styles`]), so toggling a lane never rebuilds the scene; only a
//! new analysis does.
//!
//! Everything here is plain data and free of GPUI state: [`build`] runs on
//! the background executor, [`lane_styles`] on the UI thread (it reads the
//! configuration only).

use std::collections::HashMap;
use std::sync::Arc;

use gpui_kit::SharedString;
use omatrack_core::events::{EventLap, LapEventKind};
use omatrack_core::overlay::{OverlayChannel, OverlayGroup, STANDARD_CHANNELS, standard_values};
use omatrack_core::session::{
    Analysis, Consistency, LapStripKind, LoadedLap, MarkerKind, analysis_events,
};
use omatrack_core::{Lap, UnifiedLap};
use omatrack_library::Config;
use omatrack_library::config::{ChannelStyle, TraceColorMode};
use omatrack_trace::layout::{LaneSizing, lane_height_boost};
use omatrack_trace::scale::format_distance;
use omatrack_trace::{
    Apex, ColorMode, ComplexBand, CornerBand, EventMark, EventMarkKind, FractionMap, LaneKind,
    LaneSeries, LaneSpread, LaneStyle, LaneStyles, TraceScene,
};

/// Key of the cumulative gap lane (the time delta to the reference).
pub const DELTA_KEY: &str = "delta";

/// Title of the gap lane: the primary's cumulative time delta to the
/// reference, in seconds.
pub const DELTA_TITLE: &str = "Gap to R";

/// Height share of the gap lane under a lap-time-share alignment, percent:
/// slimmer than a verified map's, still tall enough for its legend.
const TIME_SHARE_DELTA_PERCENT: f64 = 16.0;

/// Lanes that lead the stack, in this order, when the lap has them.
const LEADING: &[&str] = &["speed", "throttle", "brake", "gear", "steering"];

/// Channels shown on a fresh install (besides the gap lane): speed, the two
/// pedals and gear. Steering, RPM and the rest of a provider's channels are
/// opt in through `channels.<key>.visible` (the Channels panel).
const DEFAULT_VISIBLE: &[&str] = &["speed", "throttle", "brake", "gear"];

/// Never a lane: the x-axis itself, and raw GPS coordinates.
const NOT_A_LANE: &[&str] = &["distance", "gps_lat", "gps_lon"];

/// An RPM channel of any provider (`rpm`, `raw:RPM`, `raw:Engine RPM`).
fn is_rpm(key: &str) -> bool {
    key.to_ascii_lowercase().contains("rpm")
}

/// The neighbouring primary laps, unified once per primary lap and reused
/// by every later scene of the same lap (a new reference, strategy or
/// offset keeps the primary).
pub struct Neighbours {
    /// The primary lap they belong to (by identity).
    primary: Arc<UnifiedLap>,
    previous: Option<NeighbourLap>,
    next: Option<NeighbourLap>,
}

impl Neighbours {
    fn is_for(&self, lap: &LoadedLap) -> bool {
        Arc::ptr_eq(&self.primary, lap.unified())
    }
}

struct NeighbourLap {
    label: SharedString,
    lap: UnifiedLap,
}

/// One corner band of the scene and the analysis zone it stands for.
#[derive(Clone, Debug, PartialEq)]
pub struct CornerLink {
    /// The band id in the scene (stable within one analysis).
    pub band: u32,
    /// The zone id of the analysis (`FocusCorner` and `tracks.<track>.corners`).
    pub zone: SharedString,
}

/// A scene and what the panel needs to relate it to the analysis.
pub struct BuiltScene {
    pub analysis: Arc<Analysis>,
    pub scene: Arc<TraceScene>,
    pub corners: Vec<CornerLink>,
    pub neighbours: Option<Arc<Neighbours>>,
}

/// The scene of `analysis`. `neighbours` from an earlier scene of the same
/// primary lap are reused; otherwise the neighbouring laps are unified here.
pub fn build(analysis: Arc<Analysis>, neighbours: Option<Arc<Neighbours>>) -> BuiltScene {
    let primary = analysis.primary();
    let reference = analysis.reference();
    let unified = primary.unified();
    let neighbours = match neighbours.filter(|n| n.is_for(primary)) {
        Some(neighbours) => neighbours,
        None => Arc::new(unify_neighbours(primary)),
    };

    let mut lanes = Vec::new();
    if reference.is_some() && !analysis.delta().is_empty() {
        let delta: Arc<[f64]> = Arc::from(analysis.delta());
        lanes.push(LaneSeries::new(DELTA_KEY, DELTA_TITLE, LaneKind::Delta, delta).with_unit("s"));
    }
    for channel in ordered_channels(primary.overlays()) {
        let kind = if channel.key == "gear" {
            LaneKind::Step
        } else {
            LaneKind::Line
        };
        let reference_values = reference.and_then(|lap| find_channel(lap.overlays(), &channel.key));
        let (previous, next) = if is_standard(&channel.key) {
            (
                neighbour_values(neighbours.previous.as_ref(), &channel.key),
                neighbour_values(neighbours.next.as_ref(), &channel.key),
            )
        } else {
            (None, None)
        };
        lanes.push(
            LaneSeries::new(
                channel.key.clone(),
                channel.title.clone(),
                kind,
                channel.values.clone(),
            )
            .with_unit(channel.unit.clone())
            .with_reference(reference_values)
            .with_neighbours(previous, next),
        );
    }

    let corners: Vec<CornerLink> = analysis
        .corners()
        .iter()
        .enumerate()
        .map(|(ix, zone)| CornerLink {
            band: ix as u32 + 1,
            zone: zone.id.clone().into(),
        })
        .collect();
    // A corner's Δt is said only where the map places time loss (on a
    // lap-time map it would be a share of lap time, not the corner's).
    let placed = analysis.time_loss_placed();
    let bands = analysis
        .corners()
        .iter()
        .zip(&corners)
        .map(|(zone, link)| {
            let dt = analysis.row(&zone.id).map(|row| row.dt).filter(|_| placed);
            CornerBand::new(link.band, zone.name.clone(), zone.start, zone.end).with_delta(dt)
        })
        .collect();
    let apexes = apexes(&analysis);
    let complexes = analysis
        .complexes()
        .iter()
        .map(|complex| ComplexBand::new(complex.name.clone(), complex.start, complex.end))
        .collect();
    let map = analysis
        .comparison()
        .map(|comparison| comparison.clone() as Arc<dyn FractionMap>);

    let scene = TraceScene::new(
        Arc::from(unified.distance.as_slice()),
        Arc::from(unified.time.as_slice()),
    )
    .with_lanes(lanes)
    .with_map(map)
    .with_corners(bands, complexes)
    .with_neighbour_labels(
        neighbours.previous.as_ref().map(|lap| lap.label.clone()),
        neighbours.next.as_ref().map(|lap| lap.label.clone()),
    )
    .with_lap_labels(Some(lap_label(primary)), reference.map(lap_label))
    .with_apexes(apexes)
    .with_approximate_delta(crate::workspace::status::analysis_approximate(&analysis))
    .with_time_share_delta(crate::workspace::status::analysis_time_share(&analysis))
    .with_events(event_marks(&analysis));
    BuiltScene {
        analysis,
        scene: Arc::new(scene),
        corners,
        neighbours: Some(neighbours),
    }
}

/// The lap's short label as its lap strip names it (`L6`).
fn lap_label(lap: &LoadedLap) -> SharedString {
    lap.strip()
        .iter()
        .find(|cell| cell.lap_id == lap.lap_id())
        .map(|cell| SharedString::from(cell.label.clone()))
        .unwrap_or_else(|| format!("L{}", lap.lap_id()).into())
}

/// Each corner's apex for the speed lane's callouts: where the primary's
/// apex marker sits, its apex speed and the reference's, from the
/// analysis's corner rows. A zone whose minimum is only its edge
/// (`CornerMetrics::apex_is_local`) gets no callout.
fn apexes(analysis: &Analysis) -> Vec<Apex> {
    analysis
        .rows()
        .iter()
        .filter_map(|row| {
            if !row.primary.apex_is_local() {
                return None;
            }
            let marker = row
                .markers
                .iter()
                .find(|marker| marker.kind == MarkerKind::Apex)?;
            (marker.fraction.is_finite() && row.speeds.apex.is_finite()).then(|| {
                Apex::new(
                    marker.fraction,
                    row.speeds.apex,
                    row.reference_speeds.map(|speeds| speeds.apex),
                )
            })
        })
        .collect()
}

/// `scene` with the primary's session spread on every lane the spread
/// carries (same key, same primary grid). Series are shared, not copied.
pub fn with_session_spread(scene: &TraceScene, consistency: &Consistency) -> TraceScene {
    scene.clone().with_spreads(|key| {
        let channel = consistency.channel(key)?;
        Some(Arc::new(LaneSpread::new(
            channel
                .laps
                .iter()
                .map(|(_, series)| series.clone())
                .collect(),
            channel.min.clone(),
            channel.max.clone(),
        )))
    })
}

/// The driving events of both laps as trace marks (drawn in the Events
/// view): `P · Brake · 1,234 m`, a note as its corner sentence.
fn event_marks(analysis: &Analysis) -> Vec<EventMark> {
    analysis_events(analysis)
        .into_iter()
        .map(|event| {
            let reference = event.lap == EventLap::Reference;
            let kind = match event.kind {
                LapEventKind::BrakeOnset => EventMarkKind::BrakeOnset,
                LapEventKind::LiftOff => EventMarkKind::LiftOff,
                LapEventKind::Upshift => EventMarkKind::Upshift,
                LapEventKind::Downshift => EventMarkKind::Downshift,
                _ => EventMarkKind::Note,
            };
            let label = if kind == EventMarkKind::Note {
                event.label.clone()
            } else {
                let role = if reference { "R" } else { "P" };
                let at = if event.distance.is_finite() {
                    format!(" · {}", format_distance(event.distance, 1.0))
                } else {
                    String::new()
                };
                format!("{role} · {}{at}", event.label)
            };
            let tag = event_tag(&event, kind, reference);
            EventMark::new(kind, event.kind.channel(), reference, event.fraction, label)
                .with_tag(tag)
        })
        .collect()
}

/// The short label drawn beside a primary event's tick: a brake point's
/// offset from the reference's (`+12 m`, later along the track), a
/// shift's new gear (`↓3`). Notes, lifts and the reference's ticks carry
/// none.
fn event_tag(
    event: &omatrack_core::events::LapEvent,
    kind: EventMarkKind,
    reference: bool,
) -> Option<SharedString> {
    if reference {
        return None;
    }
    match kind {
        // A note's tick sits in its corner's column under the ruler label;
        // the note itself is read in the Time lost card.
        EventMarkKind::Note => None,
        EventMarkKind::BrakeOnset => {
            let offset = event.brake_offset?;
            let (text, _) =
                omatrack_ui::format_delta(Some(offset), 0, omatrack_ui::DeltaSense::HigherIsBetter);
            Some(format!("{text} m").into())
        }
        EventMarkKind::Downshift => event.gear.map(|gear| format!("↓{gear}").into()),
        EventMarkKind::Upshift => event.gear.map(|gear| format!("↑{gear}").into()),
        _ => None,
    }
}

/// `trace.color_mode` as the trace crate's colour mode.
pub(crate) fn color_mode(config: &Config) -> ColorMode {
    match config.trace.color_mode() {
        TraceColorMode::Lap => ColorMode::Lap,
        TraceColorMode::Channel => ColorMode::Channel,
    }
}

/// The channels a lap offers, lanes first in [`LEADING`] order, then RPM,
/// then everything else in provider order. Keys are unique (the first
/// provider wins).
fn ordered_channels(groups: &[OverlayGroup]) -> Vec<&OverlayChannel> {
    let mut all: Vec<&OverlayChannel> = Vec::new();
    for channel in groups.iter().flat_map(|group| group.channels.iter()) {
        if NOT_A_LANE.contains(&channel.key.as_str())
            || channel.values.len() < 2
            || all.iter().any(|known| known.key == channel.key)
        {
            continue;
        }
        all.push(channel);
    }
    let rank = |key: &str| -> usize {
        match LEADING.iter().position(|leading| *leading == key) {
            Some(position) => position,
            None if is_rpm(key) => LEADING.len(),
            None => LEADING.len() + 1,
        }
    };
    // Stable: provider order is kept within a rank.
    all.sort_by_key(|channel| rank(&channel.key));
    all
}

/// A normalized `UnifiedLap` field (the neighbouring laps have these only).
fn is_standard(key: &str) -> bool {
    STANDARD_CHANNELS
        .iter()
        .any(|(standard, _, _)| *standard == key)
}

fn find_channel(groups: &[OverlayGroup], key: &str) -> Option<Arc<[f64]>> {
    groups
        .iter()
        .find_map(|group| group.channel(key))
        .map(|channel| channel.values.clone())
}

fn neighbour_values(lap: Option<&NeighbourLap>, key: &str) -> Option<Arc<[f64]>> {
    let values = standard_values(&lap?.lap, key)?;
    (values.len() >= 2).then(|| Arc::from(values))
}

/// Unify the laps either side of the primary, for the out-of-lap view
/// around start/finish. A pit stop is no lap to show; an empty lap is left
/// out rather than drawn as a flat line.
fn unify_neighbours(primary: &LoadedLap) -> Neighbours {
    let label = |lap: &Lap| -> SharedString {
        primary
            .strip()
            .iter()
            .find(|cell| cell.lap_id == lap.id)
            .map(|cell| SharedString::from(cell.label.clone()))
            .unwrap_or_else(|| format!("L{}", lap.id).into())
    };
    let is_pit_stop = |lap: &Lap| {
        primary
            .strip()
            .iter()
            .any(|cell| cell.lap_id == lap.id && cell.kind == LapStripKind::PitStop)
    };
    let unify = |offset: isize| -> Option<NeighbourLap> {
        let lap = primary.neighbour_lap(offset)?;
        if is_pit_stop(lap) {
            return None;
        }
        let unified =
            primary
                .recording()
                .unify_lap(lap.start_time, lap.end_time, primary.overrides());
        (unified.len() >= 2).then(|| NeighbourLap {
            label: label(lap),
            lap: unified,
        })
    };
    Neighbours {
        primary: primary.unified().clone(),
        previous: unify(-1),
        next: unify(1),
    }
}

/// Lane appearance and sizing of one channel, from `channels.<key>`.
///
/// Visibility: an explicit `channels.<key>.visible` wins. Otherwise the gap
/// lane, speed, throttle, brake and gear show, everything else is opt in. Δ also
/// defaults to a pinned lane. Heights and fills come from the library's
/// channel defaults (the same numbers the Channels panel shows).
pub fn lane_style(config: &Config, key: &str, pinned: Option<bool>) -> LaneStyle {
    let style: ChannelStyle = config.channel_style(key);
    let configured = config.channels.get(key);
    let visible = configured
        .and_then(|channel| channel.visible)
        .unwrap_or_else(|| key == DELTA_KEY || DEFAULT_VISIBLE.contains(&key));
    let pinned = pinned.unwrap_or(key == DELTA_KEY);
    let sizing = LaneSizing::default()
        .visible(visible)
        .with_weight(style.weight * lane_height_boost(key))
        .with_height_percent(style.height_percent)
        .combine_with_previous(style.combine_with_previous)
        .pinned(pinned);
    LaneStyle::default()
        .with_color(style.color.as_deref().and_then(user_color))
        .with_reference_color(style.reference_color.as_deref().and_then(user_color))
        .with_stroke_width(style.stroke_width as f32)
        .with_fill_opacity(Some(style.fill_opacity as f32))
        .with_sizing(sizing)
}

/// A colour the user typed into `channels.<key>.color`: user data, not a
/// product colour (the Coding Guides' data exception). Unparsable text is
/// ignored and the theme decides.
fn user_color(text: &str) -> Option<gpui_kit::Hsla> {
    gpui_kit::component::try_parse_color(text.trim()).ok()
}

/// Styles of every lane of `scene`. `pinned` holds this session's pin
/// choices; `weights` a resize draft that overrides the configured weights.
pub fn lane_styles(
    config: &Config,
    scene: &TraceScene,
    pinned: &HashMap<SharedString, bool>,
    weights: Option<&HashMap<SharedString, f64>>,
) -> LaneStyles {
    let mut styles = LaneStyles::new();
    for lane in scene.lanes() {
        let mut style = lane_style(config, &lane.key, pinned.get(&lane.key).copied());
        if let Some(weight) = weights.and_then(|weights| weights.get(&lane.key)) {
            style.sizing.weight = weight * lane_height_boost(&lane.key);
        }
        // A time-share Δ is a ramp, not a verdict: a slim lane unless the
        // user sized it.
        let sized = config
            .channels
            .get(lane.key.as_ref())
            .is_some_and(|channel| channel.height_percent.is_some());
        if lane.key.as_ref() == DELTA_KEY && scene.time_share_delta() && !sized {
            style.sizing.height_percent = TIME_SHARE_DELTA_PERCENT;
        }
        styles.set(lane.key.clone(), style);
    }
    styles
}

/// Whether a lane of `key` shows under `config` (for menus and the palette).
pub fn is_lane_visible(config: &Config, key: &str) -> bool {
    lane_style(config, key, None).sizing.visible
}

#[cfg(test)]
mod tests {
    use super::*;

    fn channel(key: &str) -> OverlayChannel {
        OverlayChannel {
            key: key.to_string(),
            title: key.to_string(),
            unit: String::new(),
            values: Arc::from(vec![0.0, 1.0, 2.0]),
        }
    }

    #[test]
    fn channels_follow_the_workspace_order() {
        let standard = OverlayGroup {
            provider: "standard".into(),
            title: "Standard".into(),
            channels: [
                "speed",
                "throttle",
                "brake",
                "steering",
                "gear",
                "damper_fl",
                "distance",
                "gps_lat",
            ]
            .into_iter()
            .map(channel)
            .collect(),
        };
        let source = OverlayGroup {
            provider: "source".into(),
            title: "Source channels".into(),
            channels: ["raw:Oil Temp", "raw:RPM", "raw:Oil Temp"]
                .into_iter()
                .map(channel)
                .collect(),
        };
        let groups = [standard, source];
        let keys: Vec<&str> = ordered_channels(&groups)
            .iter()
            .map(|channel| channel.key.as_str())
            .collect();
        assert_eq!(
            keys,
            [
                "speed",
                "throttle",
                "brake",
                "gear",
                "steering",
                "raw:RPM",
                "damper_fl",
                "raw:Oil Temp"
            ]
        );
    }

    #[test]
    fn styles_come_from_the_configuration() {
        let config = Config::from_yaml_str(
            "channels:\n  speed:\n    weight: 2\n    color: '#ff0000'\n  damper_fl:\n    visible: true\n  gear:\n    visible: false\n",
        )
        .unwrap();
        let speed = lane_style(&config, "speed", None);
        assert!(speed.sizing.visible);
        assert!((speed.sizing.weight - 2.0 * lane_height_boost("speed")).abs() < 1e-9);
        assert!(speed.color.is_some());
        assert!(!lane_style(&config, "gear", None).sizing.visible);
        assert!(lane_style(&config, "damper_fl", None).sizing.visible);
        assert!(!lane_style(&config, "damper_rr", None).sizing.visible);
        // Steering and RPM are opt in; brake has its own lane.
        assert!(!lane_style(&config, "steering", None).sizing.visible);
        assert!(!lane_style(&config, "raw:Engine RPM", None).sizing.visible);
        assert!(lane_style(&config, "brake", None).sizing.visible);
        assert!(
            !lane_style(&config, "brake", None)
                .sizing
                .combine_with_previous
        );
    }

    #[test]
    fn delta_is_pinned_and_shown_by_default() {
        let config = Config::default();
        let delta = lane_style(&config, DELTA_KEY, None);
        assert!(delta.sizing.visible && delta.sizing.pinned);
        // A first-class lane, above the minimum share.
        assert_eq!(
            delta.sizing.height_percent,
            omatrack_trace::layout::default_height_percent(DELTA_KEY)
        );
        assert!(!lane_style(&config, DELTA_KEY, Some(false)).sizing.pinned);
        let hidden = Config::from_yaml_str("channels:\n  delta:\n    visible: false\n").unwrap();
        assert!(!is_lane_visible(&hidden, DELTA_KEY));
    }

    #[test]
    fn an_unparsable_user_colour_leaves_the_theme_in_charge() {
        let config =
            Config::from_yaml_str("channels:\n  speed:\n    color: 'not a colour'\n").unwrap();
        assert!(lane_style(&config, "speed", None).color.is_none());
    }
}
