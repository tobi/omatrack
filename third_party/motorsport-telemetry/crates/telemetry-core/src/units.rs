//! A global unit registry shared by every telemetry format.
//!
//! # Why this exists
//!
//! Each logging system spells units its own way. Across a single MQ12Di log and
//! a set of MoTeC files we observe `s` and `sec` for seconds, `m/s` alongside
//! `mm` and `deg`, and marker text like `raw`, `flag`, `pp1` and `Driver` that
//! labels a channel without naming a physical dimension.
//!
//! The total vocabulary is small: 1115 channels in a native Cosworth log use
//! only 29 distinct unit strings. That makes a registry practical, and a
//! registry buys three things a per-format string cannot:
//!
//! 1. **Normalisation.** `sec` and `s` resolve to the same canonical unit, so
//!    channels from different systems become comparable.
//! 2. **Dimensional safety.** Every unit knows its dimension, so converting
//!    m/s to bar can be *rejected* instead of silently multiplying.
//! 3. **Composable conversion.** Storing each unit as a factor (and offset)
//!    relative to its dimension's SI base unit means any unit converts to any
//!    other unit of the same dimension, without an N² table of pairs.
//!
//! # Model
//!
//! Every unit is `value_in_si = value * factor + offset`. Conversion between
//! two units of the same dimension is therefore
//! `to_value = (from_value * from.factor + from.offset - to.offset) / to.factor`.
//!
//! Affine units (°C, °F) need the offset; everything else has `offset == 0`.
//! Keeping one affine rule rather than special-casing temperature is what makes
//! `convert` total over same-dimension pairs.
//!
//! # Dimensionless is not a free pass
//!
//! Marker strings (`raw`, `flag`, `Driver`) map to [`Dimension::Dimensionless`]
//! but are *not* mutually convertible with ratios like `%`. A gear position and
//! a percentage are both "unitless" and converting between them is meaningless,
//! so markers get their own [`Dimension::Marker`]. This is the distinction that
//! stops "unitless" becoming a hole in the type system.

use std::fmt;

/// A physical dimension. Two units are convertible if and only if they share
/// one of these.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Dimension {
    Length,
    Speed,
    Acceleration,
    Angle,
    AngularVelocity,
    AngularAcceleration,
    Pressure,
    Temperature,
    Time,
    Frequency,
    Force,
    Torque,
    Energy,
    Power,
    Voltage,
    Current,
    Resistance,
    Mass,
    Volume,
    VolumetricFlow,
    MassFlow,
    Ratio,
    /// A pure count or index: gear number, lap count, error code.
    Count,
    /// Label text that names no physical quantity (`raw`, `flag`, `Driver`).
    /// Deliberately not convertible to anything, including [`Self::Ratio`].
    Marker,
    /// Signal strength expressed logarithmically.
    Logarithmic,
}

impl Dimension {
    /// The SI (or conventional) base unit every factor is relative to.
    pub fn base_unit(self) -> &'static str {
        match self {
            Self::Length => "m",
            Self::Speed => "m/s",
            Self::Acceleration => "m/s^2",
            Self::Angle => "rad",
            Self::AngularVelocity => "rad/s",
            Self::AngularAcceleration => "rad/s^2",
            Self::Pressure => "Pa",
            Self::Temperature => "K",
            Self::Time => "s",
            Self::Frequency => "Hz",
            Self::Force => "N",
            Self::Torque => "Nm",
            Self::Energy => "J",
            Self::Power => "W",
            Self::Voltage => "V",
            Self::Current => "A",
            Self::Resistance => "ohm",
            Self::Mass => "kg",
            Self::Volume => "m^3",
            Self::VolumetricFlow => "m^3/s",
            Self::MassFlow => "kg/s",
            Self::Ratio => "ratio",
            Self::Count => "count",
            Self::Marker => "marker",
            Self::Logarithmic => "dBm",
        }
    }

    /// Stable lowercase name for SQL output.
    pub fn name(self) -> &'static str {
        match self {
            Self::Length => "length",
            Self::Speed => "speed",
            Self::Acceleration => "acceleration",
            Self::Angle => "angle",
            Self::AngularVelocity => "angular_velocity",
            Self::AngularAcceleration => "angular_acceleration",
            Self::Pressure => "pressure",
            Self::Temperature => "temperature",
            Self::Time => "time",
            Self::Frequency => "frequency",
            Self::Force => "force",
            Self::Torque => "torque",
            Self::Energy => "energy",
            Self::Power => "power",
            Self::Voltage => "voltage",
            Self::Current => "current",
            Self::Resistance => "resistance",
            Self::Mass => "mass",
            Self::Volume => "volume",
            Self::VolumetricFlow => "volumetric_flow",
            Self::MassFlow => "mass_flow",
            Self::Ratio => "ratio",
            Self::Count => "count",
            Self::Marker => "marker",
            Self::Logarithmic => "logarithmic",
        }
    }

    /// True when converting between two units of this dimension is meaningful.
    ///
    /// Markers and counts are self-consistent labels, not scales: converting
    /// "gear 3" or `flag` into anything else is a category error.
    pub fn is_convertible(self) -> bool {
        !matches!(self, Self::Marker | Self::Count)
    }
}

impl fmt::Display for Dimension {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

/// One entry in the registry.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct UnitDef {
    /// Canonical spelling, e.g. `m/s`.
    pub canonical: &'static str,
    pub dimension: Dimension,
    /// `value_in_base = value * factor + offset`.
    pub factor: f64,
    pub offset: f64,
    /// Alternate spellings seen in real files.
    pub aliases: &'static [&'static str],
}

impl UnitDef {
    /// Convert a value in this unit to its dimension's base unit.
    pub fn to_base(&self, value: f64) -> f64 {
        value * self.factor + self.offset
    }

    /// Convert a value expressed in the base unit into this unit.
    pub fn from_base(&self, base: f64) -> f64 {
        (base - self.offset) / self.factor
    }
}

const DEG_PER_RAD: f64 = 180.0 / std::f64::consts::PI;
const G_TO_MPS2: f64 = 9.806_65;

/// The registry.
///
/// Every unit observed in Cosworth PDS, MoTeC LD and VBO files appears here,
/// plus the display units teams commonly convert to.
pub static UNITS: &[UnitDef] = &[
    // ── Length ─────────────────────────────────────────────────────────
    u(
        "m",
        Dimension::Length,
        1.0,
        &["metre", "meter", "metres", "meters"],
    ),
    u(
        "mm",
        Dimension::Length,
        0.001,
        &["millimetre", "millimeter"],
    ),
    u("cm", Dimension::Length, 0.01, &[]),
    u("km", Dimension::Length, 1000.0, &[]),
    u("in", Dimension::Length, 0.0254, &["inch", "inches"]),
    u("ft", Dimension::Length, 0.3048, &["feet", "foot"]),
    u("mile", Dimension::Length, 1609.344, &["mi", "miles"]),
    // ── Speed ──────────────────────────────────────────────────────────
    u("m/s", Dimension::Speed, 1.0, &["mps", "ms-1", "m.s^-1"]),
    u(
        "km/h",
        Dimension::Speed,
        1.0 / 3.6,
        &["kph", "kmh", "kmph", "km/hr"],
    ),
    u("mph", Dimension::Speed, 0.447_04, &["mi/h"]),
    u(
        "kn",
        Dimension::Speed,
        0.514_444_444_444_444_4,
        &["knot", "knots"],
    ),
    // ── Acceleration ───────────────────────────────────────────────────
    u(
        "m/s^2",
        Dimension::Acceleration,
        1.0,
        &["m/s2", "mps2", "m/s²"],
    ),
    u("g", Dimension::Acceleration, G_TO_MPS2, &["G", "g-force"]),
    // ── Angle ──────────────────────────────────────────────────────────
    u("rad", Dimension::Angle, 1.0, &["radian", "radians"]),
    u(
        "deg",
        Dimension::Angle,
        1.0 / DEG_PER_RAD,
        &["degree", "degrees", "°"],
    ),
    u(
        "rev",
        Dimension::Angle,
        std::f64::consts::TAU,
        &["revolution"],
    ),
    // ── Angular velocity ───────────────────────────────────────────────
    u(
        "rad/s",
        Dimension::AngularVelocity,
        1.0,
        &["rads", "rad/sec"],
    ),
    u(
        "rpm",
        Dimension::AngularVelocity,
        std::f64::consts::TAU / 60.0,
        &["RPM", "rev/min", "r/min"],
    ),
    u(
        "deg/s",
        Dimension::AngularVelocity,
        1.0 / DEG_PER_RAD,
        &["dps", "deg/sec"],
    ),
    // ── Angular acceleration ───────────────────────────────────────────
    u("rad/s^2", Dimension::AngularAcceleration, 1.0, &["rad/s2"]),
    u(
        "deg/s^2",
        Dimension::AngularAcceleration,
        1.0 / DEG_PER_RAD,
        &["deg/s2"],
    ),
    // ── Pressure ───────────────────────────────────────────────────────
    u("Pa", Dimension::Pressure, 1.0, &["pascal", "pa"]),
    u("kPa", Dimension::Pressure, 1_000.0, &["kpa"]),
    u("MPa", Dimension::Pressure, 1_000_000.0, &["mpa"]),
    u("bar", Dimension::Pressure, 100_000.0, &["Bar"]),
    u("mbar", Dimension::Pressure, 100.0, &["millibar", "hPa"]),
    u(
        "psi",
        Dimension::Pressure,
        6_894.757_293_168_361,
        &["PSI", "lbf/in^2"],
    ),
    u("atm", Dimension::Pressure, 101_325.0, &[]),
    u("inHg", Dimension::Pressure, 3_386.388_640_341, &["inhg"]),
    // ── Temperature (affine) ───────────────────────────────────────────
    u("K", Dimension::Temperature, 1.0, &["kelvin"]),
    UnitDef {
        canonical: "C",
        dimension: Dimension::Temperature,
        factor: 1.0,
        offset: 273.15,
        aliases: &["degC", "°C", "celsius", "Celsius", "deg C"],
    },
    UnitDef {
        canonical: "F",
        dimension: Dimension::Temperature,
        factor: 5.0 / 9.0,
        offset: 273.15 - 32.0 * 5.0 / 9.0,
        aliases: &["degF", "°F", "fahrenheit", "Fahrenheit", "deg F"],
    },
    // ── Time ───────────────────────────────────────────────────────────
    u(
        "s",
        Dimension::Time,
        1.0,
        &["sec", "secs", "second", "seconds"],
    ),
    u("ms", Dimension::Time, 0.001, &["millisecond", "msec"]),
    u("us", Dimension::Time, 1e-6, &["microsecond", "usec", "µs"]),
    u("ns", Dimension::Time, 1e-9, &["nanosecond", "nsec"]),
    u("min", Dimension::Time, 60.0, &["minute", "minutes"]),
    u("h", Dimension::Time, 3_600.0, &["hr", "hour", "hours"]),
    // ── Frequency ──────────────────────────────────────────────────────
    u("Hz", Dimension::Frequency, 1.0, &["hz", "hertz"]),
    u("kHz", Dimension::Frequency, 1_000.0, &["khz"]),
    // ── Force / torque / energy / power ────────────────────────────────
    u("N", Dimension::Force, 1.0, &["newton"]),
    u("kN", Dimension::Force, 1_000.0, &["kilonewton"]),
    u("lbf", Dimension::Force, 4.448_221_615_260_5, &[]),
    u(
        "Nm",
        Dimension::Torque,
        1.0,
        &["N.m", "N*m", "newton-metre"],
    ),
    u(
        "lbft",
        Dimension::Torque,
        1.355_817_948_331_400_4,
        &["lb-ft", "ft-lb", "ftlb"],
    ),
    u("J", Dimension::Energy, 1.0, &["joule"]),
    u("kJ", Dimension::Energy, 1_000.0, &["kilojoule"]),
    u("MJ", Dimension::Energy, 1_000_000.0, &["megajoule"]),
    u("kWh", Dimension::Energy, 3_600_000.0, &["kwh"]),
    u("W", Dimension::Power, 1.0, &["watt"]),
    u("kW", Dimension::Power, 1_000.0, &["kw"]),
    u(
        "hp",
        Dimension::Power,
        745.699_871_582_270_2,
        &["HP", "bhp"],
    ),
    u("PS", Dimension::Power, 735.498_75, &["ps"]),
    // ── Electrical ─────────────────────────────────────────────────────
    u("V", Dimension::Voltage, 1.0, &["volt", "volts"]),
    u("mV", Dimension::Voltage, 0.001, &["millivolt"]),
    u("A", Dimension::Current, 1.0, &["amp", "amps", "ampere"]),
    u("mA", Dimension::Current, 0.001, &["milliamp"]),
    u("ohm", Dimension::Resistance, 1.0, &["Ohm", "Ω"]),
    // ── Mass ───────────────────────────────────────────────────────────
    u("kg", Dimension::Mass, 1.0, &["kilogram", "kilogramme"]),
    u("g_mass", Dimension::Mass, 0.001, &["gram", "gramme"]),
    u("lb", Dimension::Mass, 0.453_592_37, &["lbs", "pound"]),
    // ── Volume and flow ────────────────────────────────────────────────
    u("m^3", Dimension::Volume, 1.0, &["m3", "cubic metre"]),
    u(
        "L",
        Dimension::Volume,
        0.001,
        &["l", "litre", "liter", "litres", "liters"],
    ),
    u("mL", Dimension::Volume, 1e-6, &["ml", "millilitre"]),
    u("gal", Dimension::Volume, 0.003_785_411_784, &["gallon"]),
    u("m^3/s", Dimension::VolumetricFlow, 1.0, &["m3/s"]),
    u("L/s", Dimension::VolumetricFlow, 0.001, &["l/s"]),
    u(
        "L/h",
        Dimension::VolumetricFlow,
        0.001 / 3_600.0,
        &["l/h", "L/hr"],
    ),
    u("kg/s", Dimension::MassFlow, 1.0, &[]),
    u("kg/h", Dimension::MassFlow, 1.0 / 3_600.0, &["kg/hr"]),
    // ── Ratios ─────────────────────────────────────────────────────────
    u(
        "ratio",
        Dimension::Ratio,
        1.0,
        &["fraction", "Lambda", "lambda"],
    ),
    u("%", Dimension::Ratio, 0.01, &["percent", "pct"]),
    u("%/s", Dimension::Ratio, 0.01, &["pct/s"]),
    // Cosworth reports some rates per ECU period rather than per second. The
    // period length is configuration-dependent, so this is kept distinct from
    // %/s instead of being silently equated to it.
    u(
        "%/Period",
        Dimension::Ratio,
        0.01,
        &["%/period", "pct/period"],
    ),
    // ── Counts ─────────────────────────────────────────────────────────
    u(
        "count",
        Dimension::Count,
        1.0,
        &["cnt", "counts", "num", "number"],
    ),
    u("laps", Dimension::Count, 1.0, &["lap"]),
    u("gear", Dimension::Count, 1.0, &[]),
    u("code", Dimension::Count, 1.0, &["error_code"]),
    // ── Markers ────────────────────────────────────────────────────────
    u("marker", Dimension::Marker, 1.0, &[]),
    u("raw", Dimension::Marker, 1.0, &["Raw", "RAW"]),
    u(
        "flag",
        Dimension::Marker,
        1.0,
        &["Flag", "flags", "bool", "boolean"],
    ),
    u(
        "state",
        Dimension::Marker,
        1.0,
        &["status", "State", "Status", "mode"],
    ),
    u("pp1", Dimension::Marker, 1.0, &[]),
    u("Driver", Dimension::Marker, 1.0, &["driver"]),
    u(
        "defined",
        Dimension::Marker,
        1.0,
        &["user defined", "User Defined", "user-defined"],
    ),
    u("text", Dimension::Marker, 1.0, &["string", "label"]),
    // ── Logarithmic ────────────────────────────────────────────────────
    u("dBm", Dimension::Logarithmic, 1.0, &["dbm"]),
    u("dB", Dimension::Logarithmic, 1.0, &["db"]),
];

/// Shorthand for a purely multiplicative unit.
const fn u(
    canonical: &'static str,
    dimension: Dimension,
    factor: f64,
    aliases: &'static [&'static str],
) -> UnitDef {
    UnitDef {
        canonical,
        dimension,
        factor,
        offset: 0.0,
        aliases,
    }
}

/// Look up a unit by canonical name or alias.
///
/// Matching is exact first, then case-insensitive, so `RPM` and `rpm` both
/// resolve while distinct units that differ only by case (`g` acceleration vs
/// `G`) keep their exact meaning.
pub fn lookup(unit: &str) -> Option<&'static UnitDef> {
    let trimmed = unit.trim();
    if trimmed.is_empty() {
        return None;
    }
    UNITS
        .iter()
        .find(|def| def.canonical == trimmed || def.aliases.contains(&trimmed))
        .or_else(|| {
            UNITS.iter().find(|def| {
                def.canonical.eq_ignore_ascii_case(trimmed)
                    || def
                        .aliases
                        .iter()
                        .any(|alias| alias.eq_ignore_ascii_case(trimmed))
            })
        })
}

/// Canonical spelling for a unit string, or `None` if unrecognised.
pub fn normalize(unit: &str) -> Option<&'static str> {
    lookup(unit).map(|def| def.canonical)
}

/// Why a conversion could not be performed.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConvertError {
    /// The unit string is not in the registry.
    UnknownUnit(String),
    /// Both units are known but measure different things.
    DimensionMismatch {
        from: String,
        from_dimension: Dimension,
        to: String,
        to_dimension: Dimension,
    },
    /// The dimension is a label or count, so scaling it is meaningless.
    NotConvertible { unit: String, dimension: Dimension },
}

impl fmt::Display for ConvertError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnknownUnit(unit) => write!(
                f,
                "unknown unit '{unit}': not in the telemetry unit registry \
                 (see telemetry_units())"
            ),
            Self::DimensionMismatch {
                from,
                from_dimension,
                to,
                to_dimension,
            } => write!(
                f,
                "cannot convert '{from}' ({from_dimension}) to '{to}' \
                 ({to_dimension}): different physical dimensions"
            ),
            Self::NotConvertible { unit, dimension } => write!(
                f,
                "'{unit}' is a {dimension}, which has no scale to convert along"
            ),
        }
    }
}

impl std::error::Error for ConvertError {}

/// Convert `value` from one unit to another.
///
/// Succeeds only when both units are known, share a dimension, and that
/// dimension is a real scale. Anything else is an error rather than a silently
/// wrong number.
pub fn convert(value: f64, from: &str, to: &str) -> Result<f64, ConvertError> {
    let from_def = lookup(from).ok_or_else(|| ConvertError::UnknownUnit(from.to_owned()))?;
    let to_def = lookup(to).ok_or_else(|| ConvertError::UnknownUnit(to.to_owned()))?;

    if from_def.dimension != to_def.dimension {
        return Err(ConvertError::DimensionMismatch {
            from: from_def.canonical.to_owned(),
            from_dimension: from_def.dimension,
            to: to_def.canonical.to_owned(),
            to_dimension: to_def.dimension,
        });
    }
    if !from_def.dimension.is_convertible() {
        // Identity is still fine: converting 'gear' to 'gear' asks for nothing.
        if from_def.canonical == to_def.canonical {
            return Ok(value);
        }
        return Err(ConvertError::NotConvertible {
            unit: from_def.canonical.to_owned(),
            dimension: from_def.dimension,
        });
    }
    Ok(to_def.from_base(from_def.to_base(value)))
}

/// True when `from` can be converted to `to`.
pub fn can_convert(from: &str, to: &str) -> bool {
    convert(1.0, from, to).is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Round-trip every unit through its base unit. A factor typo shows up here.
    #[test]
    fn every_unit_round_trips_through_its_base() {
        for def in UNITS {
            let value = 12.345_f64;
            let back = def.from_base(def.to_base(value));
            assert!(
                (back - value).abs() < 1e-9,
                "{} failed round-trip: {value} -> {back}",
                def.canonical
            );
        }
    }

    /// Every pair of units in a convertible dimension must convert both ways.
    #[test]
    fn all_same_dimension_pairs_convert_symmetrically() {
        for from in UNITS {
            for to in UNITS {
                if from.dimension != to.dimension || !from.dimension.is_convertible() {
                    continue;
                }
                let value = 7.5_f64;
                let forward = convert(value, from.canonical, to.canonical)
                    .unwrap_or_else(|e| panic!("{} -> {}: {e}", from.canonical, to.canonical));
                let back = convert(forward, to.canonical, from.canonical)
                    .unwrap_or_else(|e| panic!("{} -> {}: {e}", to.canonical, from.canonical));
                assert!(
                    (back - value).abs() < 1e-9,
                    "{} <-> {} not symmetric: {value} -> {forward} -> {back}",
                    from.canonical,
                    to.canonical
                );
            }
        }
    }

    /// Canonical names must be unique, and no alias may collide with another
    /// unit's canonical name — otherwise lookup would be ambiguous.
    #[test]
    fn registry_has_no_ambiguous_names() {
        let mut seen = std::collections::HashSet::new();
        for def in UNITS {
            assert!(
                seen.insert(def.canonical),
                "duplicate canonical unit {}",
                def.canonical
            );
        }
        for def in UNITS {
            for alias in def.aliases {
                assert!(
                    !seen.contains(alias),
                    "alias '{}' of {} collides with a canonical unit",
                    alias,
                    def.canonical
                );
                let owners: Vec<_> = UNITS
                    .iter()
                    .filter(|other| other.aliases.contains(alias))
                    .map(|other| other.canonical)
                    .collect();
                assert_eq!(
                    owners.len(),
                    1,
                    "alias '{alias}' claimed by multiple units: {owners:?}"
                );
            }
        }
    }

    /// Known physical conversions, checked against reference values.
    #[test]
    fn matches_reference_conversions() {
        let cases: &[(f64, &str, &str, f64)] = &[
            (74.75, "m/s", "km/h", 269.1),
            (100.0, "km/h", "mph", 62.137_119_223_733_39),
            (-29.8, "m/s^2", "g", -3.038_754_314_674_226_6),
            (1.0, "rad", "deg", 57.295_779_513_082_32),
            (7_515_500.0, "Pa", "bar", 75.155),
            (1.0, "bar", "psi", 14.503_773_800_721_814),
            (373.15, "K", "C", 100.0),
            (32.0, "F", "C", 0.0),
            (212.0, "F", "C", 100.0),
            (0.0, "C", "F", 32.0),
            (1000.0, "rpm", "rad/s", 104.719_755_119_659_8),
            (0.0182, "m", "mm", 18.2),
            (1.0, "MJ", "kWh", 0.277_777_777_777_777_8),
            (1.0, "ratio", "%", 100.0),
        ];
        for &(value, from, to, expected) in cases {
            let got = convert(value, from, to).expect("conversion should succeed");
            assert!(
                (got - expected).abs() < 1e-6,
                "{value} {from} -> {to}: expected {expected}, got {got}"
            );
        }
    }

    /// Nonsensical conversions must be refused, not silently scaled.
    #[test]
    fn refuses_nonsensical_conversions() {
        assert!(matches!(
            convert(1.0, "m/s", "bar"),
            Err(ConvertError::DimensionMismatch { .. })
        ));
        assert!(matches!(
            convert(1.0, "K", "m"),
            Err(ConvertError::DimensionMismatch { .. })
        ));
        assert!(matches!(
            convert(1.0, "rad", "s"),
            Err(ConvertError::DimensionMismatch { .. })
        ));
        // A gear position is not a percentage, even though both are "unitless".
        assert!(matches!(
            convert(3.0, "gear", "%"),
            Err(ConvertError::DimensionMismatch { .. })
        ));
        // Markers have no scale at all.
        assert!(matches!(
            convert(1.0, "flag", "raw"),
            Err(ConvertError::NotConvertible { .. })
        ));
        assert!(matches!(
            convert(1.0, "m/s", "furlongs/fortnight"),
            Err(ConvertError::UnknownUnit(_))
        ));
        // Identity on a non-scale unit is still allowed.
        assert_eq!(convert(3.0, "gear", "gear"), Ok(3.0));
    }

    /// Alias normalisation is what makes cross-system comparison work.
    #[test]
    fn normalizes_real_world_spellings() {
        assert_eq!(normalize("sec"), Some("s"));
        assert_eq!(normalize("s"), Some("s"));
        assert_eq!(normalize("RPM"), Some("rpm"));
        assert_eq!(normalize("kph"), Some("km/h"));
        assert_eq!(normalize("KM/H"), Some("km/h"));
        assert_eq!(normalize("°C"), Some("C"));
        assert_eq!(normalize("m/s²"), Some("m/s^2"));
        assert_eq!(normalize("Lambda"), Some("ratio"));
        assert_eq!(normalize("  deg  "), Some("deg"));
        assert_eq!(normalize(""), None);
        assert_eq!(normalize("wat"), None);
    }

    /// Units seen in real Cosworth, MoTeC and VBO files must all resolve.
    #[test]
    fn covers_every_unit_observed_in_real_files() {
        // From a native MQ12Di log (1115 channels) and MoTeC LD exports.
        let observed = [
            "raw",
            "K",
            "V",
            "A",
            "rad",
            "pp1",
            "Pa",
            "m/s",
            "s",
            "m",
            "m^3",
            "rad/s",
            "flag",
            "N",
            "m/s^2",
            "Lambda",
            "%/s",
            "cnt",
            "MJ",
            "rad/s^2",
            "Nm",
            "sec",
            "laps",
            "code",
            "Driver",
            "dBm",
            "kg",
            "g",
            "deg",
            "mm",
            "defined",
            "km/h",
            "user defined",
        ];
        for unit in observed {
            assert!(
                lookup(unit).is_some(),
                "unit '{unit}' observed in real files is missing from the registry"
            );
        }
    }
}
