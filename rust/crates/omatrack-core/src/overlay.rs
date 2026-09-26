//! The channel-source seam: everything a trace lane can plot comes from a
//! [`ChannelProvider`] as an [`OverlayGroup`] on the lap's 50 Hz grid.
//!
//! Two providers exist: [`StandardChannels`] (the normalized `UnifiedLap`
//! fields, in the trace workspace's default order) and [`SourceChannels`]
//! (opt-in raw vendor channels resampled through the source clock). Further
//! sources plug in as further providers without touching lanes, colours,
//! visibility or the renderer.

use crate::recording::Recording;
use crate::unify::{DEFAULT_SAMPLE_RATE, UnifiedLap};
use std::sync::Arc;

/// One plottable channel a provider offers for a recording.
#[derive(Debug, Clone, PartialEq)]
pub struct ChannelInfo {
    /// Stable key (`speed`, `raw:RPM`, ...): preferences are keyed by it.
    pub key: String,
    pub title: String,
    pub unit: String,
    /// Shown without the user opting in.
    pub default_visible: bool,
    /// Native sample rate, when known (0 for derived channels).
    pub sample_rate_hz: f64,
}

/// One channel resampled onto the lap grid (`values.len() == lap.len()`).
#[derive(Debug, Clone, PartialEq)]
pub struct OverlayChannel {
    pub key: String,
    pub title: String,
    pub unit: String,
    pub values: Arc<[f64]>,
}

/// A provider's channels for one lap.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct OverlayGroup {
    pub provider: String,
    pub title: String,
    pub channels: Vec<OverlayChannel>,
}

impl OverlayGroup {
    pub fn channel(&self, key: &str) -> Option<&OverlayChannel> {
        self.channels.iter().find(|c| c.key == key)
    }
}

/// A provider could not produce the requested channels.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum OverlayError {
    #[error("unknown channel {0}")]
    UnknownChannel(String),
}

/// A source of plottable channels.
pub trait ChannelProvider: Send + Sync {
    fn id(&self) -> &str;
    fn title(&self) -> &str;
    /// What this provider can plot for `recording`.
    fn catalog(&self, recording: &Recording) -> Vec<ChannelInfo>;
    /// The requested channels on `lap`'s grid.
    ///
    /// # Errors
    /// Returns `OverlayError` when the provider cannot supply or resample the requested
    /// channels.
    fn resample(
        &self,
        recording: &Recording,
        lap: &UnifiedLap,
        keys: &[String],
    ) -> Result<OverlayGroup, OverlayError>;
}

/// The normalized `UnifiedLap` fields.
#[derive(Debug, Clone, Copy, Default)]
pub struct StandardChannels;

/// Standard channel keys in trace-workspace order, with title and unit.
pub const STANDARD_CHANNELS: &[(&str, &str, &str)] = &[
    ("speed", "Speed", "km/h"),
    ("throttle", "Throttle", "%"),
    ("brake", "Brake", "bar"),
    ("steering", "Steering", "deg"),
    ("gear", "Gear", ""),
    ("damper_fl", "Damper FL", "mm"),
    ("damper_fr", "Damper FR", "mm"),
    ("damper_rl", "Damper RL", "mm"),
    ("damper_rr", "Damper RR", "mm"),
    ("g_long", "G Long", "g"),
    ("g_lat", "G Lat", "g"),
    ("clutch", "Clutch", "%"),
    ("driver_throttle", "Driver throttle", "%"),
    ("fuel", "Fuel", "l"),
    ("distance", "Distance", "m"),
    ("gps_lat", "GPS latitude", "\u{b0}"),
    ("gps_lon", "GPS longitude", "\u{b0}"),
];

/// Channels visible on a fresh install.
const DEFAULT_VISIBLE: &[&str] = &["speed", "throttle", "brake", "steering", "gear"];

/// The `UnifiedLap` array behind a standard key.
pub fn standard_values(lap: &UnifiedLap, key: &str) -> Option<Vec<f64>> {
    Some(match key {
        "speed" => lap.speed.clone(),
        "throttle" => lap.throttle.clone(),
        "brake" => lap.brake.clone(),
        "steering" => lap.steering.clone(),
        "gear" => lap.gear.iter().map(|g| f64::from(*g)).collect(),
        "damper_fl" => lap.damper_fl.clone(),
        "damper_fr" => lap.damper_fr.clone(),
        "damper_rl" => lap.damper_rl.clone(),
        "damper_rr" => lap.damper_rr.clone(),
        "g_long" => lap.g_force_long.clone(),
        "g_lat" => lap.g_force_lat.clone(),
        "clutch" => lap.clutch.clone(),
        "driver_throttle" => lap.driver_throttle.clone(),
        "fuel" => lap.fuel.clone(),
        "distance" => lap.distance.clone(),
        "gps_lat" => lap.gps_lat.clone(),
        "gps_lon" => lap.gps_lon.clone(),
        _ => return None,
    })
}

impl ChannelProvider for StandardChannels {
    fn id(&self) -> &'static str {
        "standard"
    }
    fn title(&self) -> &'static str {
        "Standard"
    }
    fn catalog(&self, recording: &Recording) -> Vec<ChannelInfo> {
        let mapping = recording.map_channels(&std::collections::BTreeMap::default());
        STANDARD_CHANNELS
            .iter()
            .filter(|(key, _, _)| {
                // Derived or always-present fields, else a mapped source.
                matches!(*key, "speed" | "throttle" | "brake" | "distance" | "gear")
                    || mapping.contains_key(*key)
            })
            .map(|(key, title, unit)| ChannelInfo {
                key: (*key).to_string(),
                title: (*title).to_string(),
                unit: (*unit).to_string(),
                default_visible: DEFAULT_VISIBLE.contains(key),
                sample_rate_hz: f64::from(DEFAULT_SAMPLE_RATE),
            })
            .collect()
    }
    fn resample(
        &self,
        _recording: &Recording,
        lap: &UnifiedLap,
        keys: &[String],
    ) -> Result<OverlayGroup, OverlayError> {
        let mut group = OverlayGroup {
            provider: self.id().to_string(),
            title: self.title().to_string(),
            channels: Vec::with_capacity(keys.len()),
        };
        for key in keys {
            let (_, title, unit) = STANDARD_CHANNELS
                .iter()
                .find(|(k, _, _)| k == key)
                .ok_or_else(|| OverlayError::UnknownChannel(key.clone()))?;
            let values = standard_values(lap, key).unwrap_or_default();
            group.channels.push(OverlayChannel {
                key: key.clone(),
                title: (*title).to_string(),
                unit: (*unit).to_string(),
                values: Arc::from(values),
            });
        }
        Ok(group)
    }
}

/// Raw vendor channels, opt in, keyed `raw:<source name>`.
#[derive(Debug, Clone, Copy, Default)]
pub struct SourceChannels;

/// Key prefix of raw source channels.
pub const RAW_PREFIX: &str = "raw:";

impl ChannelProvider for SourceChannels {
    fn id(&self) -> &'static str {
        "source"
    }
    fn title(&self) -> &'static str {
        "Source channels"
    }
    fn catalog(&self, recording: &Recording) -> Vec<ChannelInfo> {
        recording
            .channels()
            .iter()
            .filter(|channel| channel.has_samples())
            .map(|channel| ChannelInfo {
                key: format!("{RAW_PREFIX}{}", channel.name),
                title: channel.name.clone(),
                unit: channel.unit.clone(),
                default_visible: false,
                sample_rate_hz: channel.frequency_hz,
            })
            .collect()
    }
    /// Sampled through the source clock on the lap's 50 Hz grid; a gap
    /// holds the previous value (the store's `extraChannelData`).
    #[expect(
        clippy::cast_precision_loss,
        reason = "Preserve the C++ port's sample-index widths and rounding at this numerical boundary; verified by parity."
    )]
    fn resample(
        &self,
        recording: &Recording,
        lap: &UnifiedLap,
        keys: &[String],
    ) -> Result<OverlayGroup, OverlayError> {
        let mut group = OverlayGroup {
            provider: self.id().to_string(),
            title: self.title().to_string(),
            channels: Vec::with_capacity(keys.len()),
        };
        for key in keys {
            let name = key
                .strip_prefix(RAW_PREFIX)
                .ok_or_else(|| OverlayError::UnknownChannel(key.clone()))?;
            let index = recording
                .channels()
                .iter()
                .position(|channel| channel.name == name)
                .ok_or_else(|| OverlayError::UnknownChannel(key.clone()))?;
            let channel = &recording.channels()[index];
            let mut values = vec![0.0; lap.len()];
            for sample in 0..lap.len() {
                let time = lap.start_time + sample as f64 / f64::from(lap.sample_rate);
                if let Some(value) = recording.sample_at(index, time, true) {
                    values[sample] = value;
                } else if sample > 0 {
                    values[sample] = values[sample - 1];
                }
            }
            group.channels.push(OverlayChannel {
                key: key.clone(),
                title: channel.name.clone(),
                unit: channel.unit.clone(),
                values: Arc::from(values),
            });
        }
        Ok(group)
    }
}
