//! Channel mapping: vendor channel names to standard concepts (port of the
//! alias table and matching in `TelemetryEngine.cpp`).

use crate::recording::RawChannel;
use std::collections::BTreeMap;

/// User-selected canonical concept -> source channel name overrides.
/// Missing concepts keep the normal cross-format alias matching.
pub type ChannelOverrides = BTreeMap<String, String>;

/// Concept -> channel index, in concept-name order (the C++ `std::map`).
pub type ChannelMapping = BTreeMap<String, usize>;

/// Canonical concepts and their aliases, most preferred first. Sorted by
/// concept name: this is also the order the C++ `std::map` iterates in, and
/// therefore the order `parse` prints the mapping.
pub const CHANNEL_ALIASES: &[(&str, &[&str])] = &[
    (
        "brake",
        &[
            "brake pressure f",
            "brake pressure fr",
            "brake pressure front",
            "p_f_brake",
            "p_brake_front",
        ],
    ),
    ("brake_pos", &["brake pos"]),
    (
        "clutch",
        &["clutch pos", "clutch position", "clutch pedal", "clutch"],
    ),
    ("damper_fl", &["x_fl_damper", "damper travel fl"]),
    ("damper_fr", &["x_fr_damper", "damper travel fr"]),
    ("damper_rl", &["x_rl_damper", "damper travel rl"]),
    ("damper_rr", &["x_rr_damper", "damper travel rr"]),
    (
        "distance",
        &[
            "lap distance corrected",
            "lap distance",
            "distance_wspd_app",
        ],
    ),
    (
        "driver_id",
        &[
            "driverid",
            "driver_id",
            "driver id",
            "activedriverid",
            "x2lnk_driverid",
        ],
    ),
    (
        "driver_throttle",
        &[
            "driver throttle pos",
            "fbwdrivertps",
            "pps",
            "throttle pedal",
            "pedal pos",
            "accel pedal pos",
            "acc pedal pos",
        ],
    ),
    (
        "fuel",
        &[
            "fuel remaining",
            "fuel remain",
            "fuel level",
            "fuel qty",
            "fuelqty",
            "fuelvol",
            "fuel volume",
            "fuel load",
            "fuelload",
            "ufuel",
            "fuel",
        ],
    ),
    (
        "g_lat",
        &[
            "g force lat",
            "g_force_lat",
            "i_accel_lat",
            "fia_accely",
            "accel_lat",
            "lateral acceleration",
            "latacc",
            "g lat",
        ],
    ),
    ("g_long", &["g force long", "i_accel_long", "fia_accelx"]),
    ("gear", &["gear_pos", "gear", "gearposdisplay"]),
    ("gps_itow", &["gps itow"]),
    ("gps_lat", &["fia_gpslatn", "gps latitude"]),
    ("gps_lon", &["fia_gpslonge", "gps longitude"]),
    ("gps_position_accuracy", &["gps position accuracy"]),
    (
        "gps_speed",
        &["fia_gpsvel", "gps speed", "velocity kmh", "velocity"],
    ),
    ("gps_speed_accuracy", &["gps speed accuracy"]),
    ("gps_week", &["gps week"]),
    (
        "speed",
        &[
            "corr speed",
            "ground speed",
            "wheel speed avg",
            "vehicle speed",
            "aero speed",
            "speed_ref",
            "speed_wspd_app",
            "vehrefspeed",
            "uspeed",
            "speed",
            "gps speed",
            "velocity kmh",
            "velocity",
        ],
    ),
    ("steering", &["steering angle", "steer"]),
    (
        "throttle",
        &[
            "tpsreal",
            "tps",
            "throttle pos",
            "aps",
            "driver throttle pos",
            "throttle pedal",
            "pedal pos",
            "accel pedal pos",
            "acc pedal pos",
            "fbwdrivertps",
            "pps",
        ],
    ),
];

/// Speed unit -> km/h factor.
pub fn speed_unit_factor(unit: &str) -> Option<f64> {
    match unit {
        "m/s" => Some(3.6),
        "km/h" | "kph" | "kmh" | "kmph" => Some(1.0),
        "mph" => Some(1.60934),
        _ => None,
    }
}

/// Brake pressure unit -> bar factor.
pub fn brake_unit_factor(unit: &str) -> Option<f64> {
    match unit {
        "bar" => Some(1.0),
        "psi" => Some(0.0689476),
        "kpa" => Some(0.01),
        "mpa" => Some(10.0),
        "pa" => Some(0.00001),
        _ => None,
    }
}

/// C-locale `isspace`.
fn c_isspace(byte: u8) -> bool {
    matches!(byte, b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r')
}

/// Trim C whitespace and lowercase ASCII (C-locale `tolower`).
pub fn lower_trimmed(value: &str) -> String {
    let bytes = value.as_bytes();
    let mut start = 0;
    let mut end = bytes.len();
    while start < end && c_isspace(bytes[start]) {
        start += 1;
    }
    while end > start && c_isspace(bytes[end - 1]) {
        end -= 1;
    }
    let lowered: Vec<u8> = bytes[start..end]
        .iter()
        .map(|b| b.to_ascii_lowercase())
        .collect();
    String::from_utf8(lowered).unwrap_or_default()
}

/// Lowercase and strip everything but ASCII letters and digits.
pub fn normalize_channel_name(raw: &str) -> String {
    raw.bytes()
        .filter(u8::is_ascii_alphanumeric)
        .map(|b| b.to_ascii_lowercase() as char)
        .collect()
}

/// Score of an already-normalized name against an alias (higher is better):
/// exact > name contains alias > alias contains name.
pub fn score_normalized_channel_match(channel: &str, alias: &str, alias_priority: i32) -> i32 {
    if channel.is_empty() || alias.is_empty() {
        return i32::MIN;
    }
    if channel == alias {
        return 10000 - alias_priority;
    }
    if alias.len() >= 4 && channel.contains(alias) {
        return 7000 - alias_priority;
    }
    if channel.len() >= 4 && alias.contains(channel) {
        return 6000 - alias_priority;
    }
    i32::MIN
}

/// Score how well a channel name matches an alias (higher = better).
pub fn score_channel_match(channel: &str, alias: &str, alias_priority: i32) -> i32 {
    score_normalized_channel_match(
        &normalize_channel_name(channel),
        &normalize_channel_name(alias),
        alias_priority,
    )
}

/// Map canonical concepts to channel indices.
pub fn map_channels(channels: &[RawChannel], overrides: &ChannelOverrides) -> ChannelMapping {
    let mut mapping = ChannelMapping::new();
    let normalized: Vec<String> = channels
        .iter()
        .map(|c| normalize_channel_name(&c.name))
        .collect();

    for (field, aliases) in CHANNEL_ALIASES {
        if let Some(wanted_name) = overrides.get(*field) {
            if let Some(index) = channels
                .iter()
                .position(|c| c.has_samples() && c.name == *wanted_name)
            {
                mapping.insert((*field).to_string(), index);
                continue;
            }
            let wanted = normalize_channel_name(wanted_name);
            if wanted.is_empty() {
                continue;
            }
            if let Some(index) =
                (0..channels.len()).find(|&c| channels[c].has_samples() && normalized[c] == wanted)
            {
                mapping.insert((*field).to_string(), index);
            }
            continue;
        }
        let normalized_aliases: Vec<String> =
            aliases.iter().map(|a| normalize_channel_name(a)).collect();
        let mut best_score = i32::MIN;
        let mut best: Option<usize> = None;
        for (c, channel) in channels.iter().enumerate() {
            if !channel.has_samples() {
                continue;
            }
            if *field == "speed" || *field == "gps_speed" {
                // A name containing "speed" is not evidence of road speed.
                // Known angular/voltage/etc units must not be treated as
                // km/h. Keep unitless CAN aliases and explicit overrides.
                let unit = lower_trimmed(&channel.unit);
                if !unit.is_empty() && speed_unit_factor(&unit).is_none() {
                    continue;
                }
            }
            for (a, alias) in normalized_aliases.iter().enumerate() {
                // Bare catch-all names are exact-only.
                if (alias == "speed" || alias == "velocity" || alias == "fuel")
                    && normalized[c] != *alias
                {
                    continue;
                }
                let score = score_normalized_channel_match(&normalized[c], alias, a as i32);
                if score > best_score {
                    best_score = score;
                    best = Some(c);
                }
            }
        }
        if let Some(best) = best {
            mapping.insert((*field).to_string(), best);
        }
    }
    mapping
}

/// Convert one raw GPS coordinate sample to degrees, east-positive, from the
/// channel's declared unit: `rad`; angular minutes (`min`, `arcmin`,
/// `arcminute`) in the reader's west-positive longitude convention; anything
/// else is already degrees.
pub fn gps_coordinate_degrees(raw: f64, unit: &str, longitude: bool) -> f64 {
    if unit == "rad" {
        return raw * (180.0 / std::f64::consts::PI);
    }
    if unit == "min" || unit == "arcmin" || unit == "arcminute" {
        return if longitude { raw / -60.0 } else { raw / 60.0 };
    }
    raw
}

/// Most frequent positive numeric code in a driver-ID series; ties go to the
/// earlier one. Float32-backed values (sample type code 6) are reduced to
/// their seven significant decimal digits so codes such as 2.1 do not expose
/// binary storage noise. Returns 0 when no positive finite value exists.
///
/// The C++ does the reduction in x87 `long double`; here the decimal is
/// rounded exactly (shortest `{:.6e}` round trip), which is the value that
/// computation approximates.
pub fn dominant_driver_id(values: &[f64], sample_type_code: u32) -> f64 {
    // (count, first index) keyed by the value's bit pattern, iterated in
    // numeric order like std::map<double, ...>.
    let mut counts: Vec<(f64, usize, usize)> = Vec::new();
    let mut index_of: std::collections::HashMap<u64, usize> = std::collections::HashMap::new();
    for (index, &raw) in values.iter().enumerate() {
        let mut candidate = raw;
        if !candidate.is_finite() || candidate <= 0.0 {
            continue;
        }
        if sample_type_code == 6 {
            candidate = format!("{candidate:.6e}").parse().unwrap_or(candidate);
        }
        match index_of.get(&candidate.to_bits()) {
            Some(&slot) => counts[slot].1 += 1,
            None => {
                index_of.insert(candidate.to_bits(), counts.len());
                counts.push((candidate, 1, index));
            }
        }
    }
    counts.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));
    let mut best_id = 0.0;
    let mut best_count = 0usize;
    let mut best_first = usize::MAX;
    for (candidate, count, first) in counts {
        if count > best_count || (count == best_count && first < best_first) {
            best_id = candidate;
            best_count = count;
            best_first = first;
        }
    }
    best_id
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn alias_table_is_in_std_map_order() {
        for pair in CHANNEL_ALIASES.windows(2) {
            assert!(pair[0].0 < pair[1].0, "{} !< {}", pair[0].0, pair[1].0);
        }
    }
}
