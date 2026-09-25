//! Clearly stopped time inside a lap, for the lap strip's view-only width
//! projection (port of `TelemetrySource::stoppedDuration`). Never a catalog
//! scalar: lap times, classification, cursor fractions and video timing are
//! untouched.

use crate::mapping::{ChannelOverrides, lower_trimmed, speed_unit_factor};
use crate::recording::Recording;

impl Recording {
    /// Seconds of positive stop evidence (finite speed <= 1 km/h) in
    /// `[start, end]`, probed on the SOURCE clock at about 4 Hz (at most
    /// 4096 bins). Gaps and unknown samples are not assumed stopped; `None`
    /// when nothing was observed or speed is not mapped.
    pub fn stopped_duration(
        &self,
        start_time: f64,
        end_time: f64,
        overrides: &ChannelOverrides,
    ) -> Option<f64> {
        if !start_time.is_finite()
            || !end_time.is_finite()
            || start_time < 0.0
            || end_time <= start_time
            || end_time > i64::MAX as f64 / 1e9
        {
            return None;
        }
        let mapping = self.map_channels(overrides);
        let &speed = mapping.get("speed")?;
        let unit = lower_trimmed(&self.channels()[speed].unit);
        let conversion = speed_unit_factor(&unit);
        if !unit.is_empty() && conversion.is_none() {
            return None;
        }
        let factor = conversion.unwrap_or(1.0);
        let duration = end_time - start_time;
        let bins = 4096.0_f64.min((duration * 4.0).ceil()) as i32;
        let step = duration / f64::from(bins.max(1));
        let mut stopped = 0.0;
        let mut observed = false;
        for i in 0..bins {
            let Some(value) =
                self.sample_at(speed, start_time + (f64::from(i) + 0.5) * step, false)
            else {
                continue;
            };
            let value = value * factor;
            if !value.is_finite() || value < 0.0 || value > 540.0 {
                continue;
            }
            observed = true;
            if value <= 1.0 {
                stopped += step;
            }
        }
        observed.then(|| crate::num::min(stopped, duration))
    }
}
