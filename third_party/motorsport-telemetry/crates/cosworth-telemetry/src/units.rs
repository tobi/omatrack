//! Channel unit resolution for Pi/Cosworth PDS files.
//!
//! # Why this module exists
//!
//! An earlier revision guessed units from channel names and was confidently
//! wrong on real ORECA 07 data: `STEER` was reported as `deg` while holding
//! radians (-2.58..1.88), `X_FL_DAMPER` as `mm` while holding metres
//! (-0.0104..0.0182), `P_F_BRAKE` as `pa` for values up to 7.5e6, and
//! `I_ACCEL_LONG` as `g` while holding m/s^2 (-29.8 = -3.0 g).
//!
//! It turns out none of that guessing is necessary. PDS channel definition
//! records carry a **quantity code**: a small integer naming the channel's
//! physical dimension. Pi/Cosworth loggers store sample values in SI base
//! units for that dimension, which Cosworth's own Pi Toolbox Advanced Maths
//! guide documents ("reads in the channel values in SI units", with `Unit.Mps`
//! / `Unit.Mps_2` enums). So the quantity code determines the unit exactly.
//!
//! Crucially the quantity code is present even in Pi Toolbox *exports*, which
//! strip the human-readable unit string. That is why unit resolution is driven
//! by the code rather than by the string.
//!
//! # Evidence
//!
//! The code -> unit table below was derived by cross-referencing 1115 channels
//! in a native Cosworth MQ12Di ECU log (which declares both a quantity code
//! and a unit string) against 31 channels in a Pi Toolbox export of the same
//! car (which declares only the code). In the native log the mapping is 1:1
//! with no contradictions: every one of the 50 channels with code 4 declared
//! `K`, all 33 with code 7 declared `rad`, all 25 with code 9 declared `Pa`,
//! and so on.
//!
//! # Version robustness
//!
//! Field offsets within the definition record are **not** hardcoded. PDS
//! record layouts vary between logger firmware and Toolbox versions, so
//! [`DefLayout::detect`] locates the quantity and unit-string fields by
//! scanning candidate offsets and scoring how well they agree with each other
//! and with the known table. A layout is only accepted when the evidence
//! supports it; otherwise units degrade to [`UnitSource::Unknown`] rather than
//! being invented.

use motorsport_telemetry_core::UnitSource;

/// A physical dimension a PDS channel can carry.
///
/// Values are the on-disk quantity codes. Codes observed in real files are
/// named; unobserved codes resolve to [`Quantity::Unknown`] so a future
/// firmware revision cannot silently produce a wrong unit.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Quantity {
    /// Counters, flags, alarms, gear positions, ratios: no dimension.
    Dimensionless,
    Length,
    Volume,
    Speed,
    Temperature,
    Time,
    Angle,
    AngularVelocity,
    Pressure,
    Acceleration,
    Voltage,
    Current,
    Mass,
    Force,
    Torque,
    /// Per-unit ratio (`pp1`): dimensionless but conventionally displayed.
    PerUnit,
    /// Absolute wall-clock time in seconds.
    AbsoluteTime,
    /// Rate of change of an angular velocity (rad/s^2).
    AngularAcceleration,
    /// A code we have not seen in a real file.
    Unknown(u32),
}

impl Quantity {
    /// Map an on-disk quantity code to a dimension.
    ///
    /// Codes are from a native MQ12Di log cross-checked against a Toolbox
    /// export; see the module docs for provenance.
    pub fn from_code(code: u32) -> Self {
        match code {
            0 => Self::Dimensionless,
            1 => Self::Length,
            2 => Self::Volume,
            3 => Self::Speed,
            4 => Self::Temperature,
            6 => Self::Time,
            7 => Self::Angle,
            8 => Self::AngularVelocity,
            9 => Self::Pressure,
            10 => Self::Acceleration,
            11 => Self::Voltage,
            12 => Self::Current,
            13 => Self::Mass,
            14 => Self::Force,
            15 => Self::Torque,
            16 => Self::PerUnit,
            20 => Self::AbsoluteTime,
            23 => Self::AngularAcceleration,
            other => Self::Unknown(other),
        }
    }

    /// The SI unit PDS uses to store this dimension.
    ///
    /// Returns `None` for dimensionless channels and unrecognised codes: there
    /// is no unit to report, and inventing one is exactly the bug this module
    /// exists to prevent.
    pub fn si_unit(self) -> Option<&'static str> {
        Some(match self {
            Self::Length => "m",
            Self::Volume => "m^3",
            Self::Speed => "m/s",
            Self::Temperature => "K",
            Self::Time | Self::AbsoluteTime => "s",
            Self::Angle => "rad",
            Self::AngularVelocity => "rad/s",
            Self::AngularAcceleration => "rad/s^2",
            Self::Pressure => "Pa",
            Self::Acceleration => "m/s^2",
            Self::Voltage => "V",
            Self::Current => "A",
            Self::Mass => "kg",
            Self::Force => "N",
            Self::Torque => "N.m",
            // Dimensionless, per-unit ratios and unknown codes have no unit.
            Self::Dimensionless | Self::PerUnit | Self::Unknown(_) => return None,
        })
    }

    /// Whether a declared unit string is consistent with this dimension.
    ///
    /// Used to score candidate field offsets during layout detection, so it
    /// accepts the spellings real files use rather than requiring an exact
    /// match against [`Self::si_unit`].
    fn accepts(self, unit: &str) -> bool {
        let lower = unit.to_ascii_lowercase();
        let text = lower.trim();
        match self {
            Self::Length => matches!(text, "m" | "mm" | "cm" | "km"),
            Self::Volume => matches!(text, "m^3" | "l" | "litre" | "liter" | "ml" | "cc"),
            Self::Speed => matches!(text, "m/s" | "km/h" | "kph" | "mph"),
            Self::Temperature => matches!(text, "k" | "c" | "degc" | "f"),
            Self::Time => matches!(text, "s" | "sec" | "ms" | "us" | "min"),
            Self::AbsoluteTime => matches!(text, "s" | "sec" | "date" | "time"),
            Self::Angle => matches!(text, "rad" | "deg" | "degree" | "degrees"),
            Self::AngularVelocity => matches!(text, "rad/s" | "rpm" | "deg/s"),
            Self::AngularAcceleration => matches!(text, "rad/s^2" | "rad/s2" | "deg/s^2"),
            Self::Pressure => matches!(text, "pa" | "kpa" | "bar" | "mbar" | "psi"),
            Self::Acceleration => matches!(text, "m/s^2" | "m/s2" | "g" | "mps_2"),
            Self::Voltage => matches!(text, "v" | "mv" | "volt" | "volts"),
            Self::Current => matches!(text, "a" | "ma" | "amp" | "amps"),
            Self::Mass => matches!(text, "kg" | "g" | "lb"),
            Self::Force => matches!(text, "n" | "kn" | "lbf"),
            Self::Torque => matches!(text, "nm" | "n.m" | "n-m" | "lbft"),
            Self::PerUnit => matches!(text, "pp1" | "%" | "ratio" | ""),
            // Dimensionless channels legitimately carry marker text.
            Self::Dimensionless => text.is_empty() || is_dimensionless_marker(text),
            Self::Unknown(_) => true,
        }
    }
}

/// Text that appears in the unit field but names no physical dimension.
///
/// Real logs use these as display hints. Treating them as convertible units
/// would let downstream code "convert" a flag or a raw ADC count.
pub fn is_dimensionless_marker(unit: &str) -> bool {
    matches!(
        unit.to_ascii_lowercase().trim(),
        "raw"
            | "flag"
            | "flags"
            | "cnt"
            | "count"
            | "code"
            | "pp1"
            | "laps"
            | "lambda"
            | "bit"
            | "bits"
            | "state"
            | "driver"
            | "user defined"
            | "defined"
            | "none"
            | "n/a"
    )
}

/// Byte offsets of the unit-related fields inside a channel definition record.
///
/// Discovered per file by [`Self::detect`] rather than hardcoded, because
/// record layouts differ between logger firmware and Toolbox versions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DefLayout {
    /// Offset of the u32 quantity code, when found.
    pub quantity: Option<usize>,
    /// Offset of the UTF-16LE unit string, when found.
    pub unit: Option<usize>,
}

impl DefLayout {
    /// Nothing found: callers fall back to [`UnitSource::Unknown`].
    pub const NONE: Self = Self {
        quantity: None,
        unit: None,
    };

    /// Offset of the unit string in every PDS definition record observed so far
    /// (native MQ12Di logs and Pi Toolbox exports alike). Used only as a
    /// tie-breaker when a unit string cannot be corroborated by a quantity code.
    const CANONICAL_UNIT_OFFSET: usize = 0x90;

    /// True when every record's u32 at `offset` decodes to a dimension we know.
    fn codes_all_recognised(records: &[&[u8]], offset: usize) -> bool {
        let mut recognised = 0usize;
        let mut total = 0usize;
        for record in records {
            if let Some(code) = u32_field(record, offset) {
                total += 1;
                if !matches!(Quantity::from_code(code), Quantity::Unknown(_)) {
                    recognised += 1;
                }
            }
        }
        total > 0 && recognised == total
    }

    /// Locate the quantity-code and unit-string fields in a set of records.
    ///
    /// `records` are the raw definition records; `name_offset` is where the
    /// channel name lives so it is not mistaken for a unit.
    ///
    /// Detection strategy:
    ///
    /// 1. Find every 4-aligned offset holding a plausible UTF-16LE unit
    ///    string, and every offset holding small u32 values that look like
    ///    quantity codes.
    /// 2. Score each (quantity, unit) offset pair by how often the decoded
    ///    code's dimension accepts the decoded string across all records.
    /// 3. Accept the best pair only if agreement is overwhelming. A wrong
    ///    offset produces near-zero agreement, so this rejects itself.
    ///
    /// This is what makes the reader version-robust: the offsets are evidence,
    /// not assumptions.
    pub fn detect(records: &[&[u8]], name_offset: usize) -> Self {
        if records.is_empty() {
            return Self::NONE;
        }
        let record_len = records[0].len();

        // Candidate unit-string offsets: 4-aligned, decode as short printable
        // UTF-16LE in a decent number of records, and not the name field.
        let mut unit_candidates: Vec<(usize, usize)> = Vec::new();
        for offset in (0..record_len.saturating_sub(4)).step_by(4) {
            if offset == name_offset {
                continue;
            }
            let mut hits = 0usize;
            for record in records {
                match utf16_field(record, offset, 24) {
                    Some(text) if !text.is_empty() && text.len() <= 12 => hits += 1,
                    _ => {}
                }
            }
            if hits > 0 {
                unit_candidates.push((offset, hits));
            }
        }
        // Strings spill into the following words, so prefer the earliest
        // offset of each run: a real field starts where the text starts.
        unit_candidates.sort_by_key(|&(offset, _)| offset);
        let snapshot = unit_candidates.clone();
        unit_candidates.retain(|&(offset, hits)| {
            // Drop an offset if the preceding word already covers more records
            // (i.e. this is the tail of a longer string).
            !snapshot
                .iter()
                .any(|&(other, other_hits)| other + 4 == offset && other_hits >= hits)
        });

        // Candidate quantity offsets: u32 values that stay small.
        let mut quantity_candidates: Vec<usize> = Vec::new();
        for offset in (0..record_len.saturating_sub(4)).step_by(4) {
            let mut distinct = std::collections::BTreeSet::new();
            let mut ok = true;
            for record in records {
                match u32_field(record, offset) {
                    Some(value) if value <= 64 => {
                        distinct.insert(value);
                    }
                    _ => {
                        ok = false;
                        break;
                    }
                }
            }
            // Require some variation: an all-zero word carries no information.
            if ok && distinct.len() >= 2 {
                quantity_candidates.push(offset);
            }
        }

        // Score pairs by agreement between code and string.
        let mut best: Option<(usize, usize, usize, usize)> = None; // (score, checked, q, u)
        for &quantity in &quantity_candidates {
            for &(unit, _) in &unit_candidates {
                if unit == quantity {
                    continue;
                }
                let mut agree = 0usize;
                let mut checked = 0usize;
                for record in records {
                    let (Some(code), Some(text)) =
                        (u32_field(record, quantity), utf16_field(record, unit, 24))
                    else {
                        continue;
                    };
                    if text.is_empty() {
                        continue;
                    }
                    checked += 1;
                    if Quantity::from_code(code).accepts(&text) {
                        agree += 1;
                    }
                }
                // Need real evidence, not one lucky record.
                if checked >= 4
                    && agree * 100 >= checked * 90
                    && best.is_none_or(|(best_agree, _, _, _)| agree > best_agree)
                {
                    best = Some((agree, checked, quantity, unit));
                }
            }
        }

        if let Some((_, _, quantity, unit)) = best {
            return Self {
                quantity: Some(quantity),
                unit: Some(unit),
            };
        }

        // A declared unit string stands on its own. Pairing it with a quantity
        // code is the strongest evidence, but a file can carry unit strings with
        // no usable quantity field (or have too few channels for the pairing
        // test), and in that case the string is still what the file says.
        //
        // Require the candidate to appear at the *canonical* offset: a real unit
        // field is at a fixed place in the record, so accepting an arbitrary
        // offset here would risk reading some other string as a unit.
        if let Some(&(unit, _)) = unit_candidates
            .iter()
            .find(|&&(offset, _)| offset == Self::CANONICAL_UNIT_OFFSET)
        {
            // Still take a quantity offset if one is credible, so channels
            // without a string can fall back to their dimension.
            let quantity = quantity_candidates
                .iter()
                .copied()
                .find(|&offset| offset != unit && Self::codes_all_recognised(records, offset));
            return Self {
                quantity,
                unit: Some(unit),
            };
        }

        // No confirmed pairing. Fall back to a quantity offset whose codes are
        // all ones we recognise; this is how Toolbox exports (which strip the
        // unit string) still get correct units.
        //
        // This requires real evidence. Several unrelated words in a definition
        // record hold small integers, so with only a handful of channels a
        // spurious offset can look exactly like a quantity field. Reporting
        // `unknown` is correct when the file cannot tell us; inventing a
        // dimension from a coincidence is the failure mode this module exists
        // to prevent.
        const MIN_RECORDS_FOR_FALLBACK: usize = 8;
        if records.len() < MIN_RECORDS_FOR_FALLBACK {
            return Self::NONE;
        }
        let quantity = quantity_candidates
            .into_iter()
            .find(|&offset| Self::codes_all_recognised(records, offset));
        Self {
            quantity,
            unit: None,
        }
    }

    /// Resolve one channel's unit using this layout.
    ///
    /// Precedence: a unit string the file declares wins; otherwise the
    /// quantity code's SI unit is used; otherwise the unit is unknown.
    pub fn resolve(&self, record: &[u8]) -> (String, UnitSource) {
        let quantity = self
            .quantity
            .and_then(|offset| u32_field(record, offset))
            .map(Quantity::from_code);

        if let Some(declared) = self
            .unit
            .and_then(|offset| utf16_field(record, offset, 24))
            .filter(|text| !text.is_empty())
        {
            // Marker text is kept for display but is not a convertible unit.
            if is_dimensionless_marker(&declared) {
                return (declared, UnitSource::SpecDefault);
            }
            return (declared, UnitSource::Declared);
        }

        match quantity.and_then(Quantity::si_unit) {
            Some(unit) => (unit.to_owned(), UnitSource::SpecDefault),
            None => (String::new(), UnitSource::Unknown),
        }
    }
}

/// Read a little-endian u32, or `None` if the record is too short.
fn u32_field(record: &[u8], offset: usize) -> Option<u32> {
    let bytes = record.get(offset..offset + 4)?;
    Some(u32::from_le_bytes(bytes.try_into().ok()?))
}

/// Decode a NUL-terminated UTF-16LE field.
///
/// Returns `None` when the bytes are not a clean printable ASCII-range
/// UTF-16LE string, so binary fields are never mistaken for units.
fn utf16_field(record: &[u8], offset: usize, max_bytes: usize) -> Option<String> {
    let end = (offset + max_bytes).min(record.len());
    let slice = record.get(offset..end)?;
    let mut text = String::new();
    for pair in slice.chunks_exact(2) {
        let code = u16::from_le_bytes([pair[0], pair[1]]);
        if code == 0 {
            return Some(text.trim().to_owned());
        }
        // Units are short printable ASCII in every file observed.
        if !(0x20..0x7f).contains(&code) {
            return None;
        }
        text.push(code as u8 as char);
    }
    Some(text.trim().to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a synthetic definition record with the native MQ12Di layout:
    /// quantity u32 at 0x88, UTF-16LE unit string at 0x90.
    fn record(name: &str, quantity: u32, unit: &str) -> Vec<u8> {
        let mut buffer = vec![0u8; 0x130];
        for (index, code) in name.encode_utf16().enumerate() {
            buffer[8 + index * 2..10 + index * 2].copy_from_slice(&code.to_le_bytes());
        }
        buffer[0x88..0x8c].copy_from_slice(&quantity.to_le_bytes());
        for (index, code) in unit.encode_utf16().enumerate() {
            buffer[0x90 + index * 2..0x92 + index * 2].copy_from_slice(&code.to_le_bytes());
        }
        buffer
    }

    /// Channels from the real MQ12Di log, with their real codes and units.
    fn native_records() -> Vec<Vec<u8>> {
        vec![
            record("A_ign_base", 7, "rad"),
            record("RPM", 8, "rad/s"),
            record("ACT", 4, "K"),
            record("BAP", 9, "Pa"),
            record("Aero_Speed_Pitot", 3, "m/s"),
            record("Battery Voltage", 11, "V"),
            record("CH_AC_Compressor_Curr", 12, "A"),
            record("Coupling Time", 6, "s"),
            record("Fuel Remaining", 2, ""),
            record("Alarm 0", 0, ""),
            record("BBias", 16, "pp1"),
            record("Damper_FL", 1, "m"),
        ]
    }

    #[test]
    fn detects_native_layout_from_code_and_string_agreement() {
        let owned = native_records();
        let records: Vec<&[u8]> = owned.iter().map(|r| r.as_slice()).collect();
        let layout = DefLayout::detect(&records, 8);
        assert_eq!(layout.quantity, Some(0x88), "quantity offset");
        assert_eq!(layout.unit, Some(0x90), "unit string offset");
    }

    #[test]
    fn declared_units_win_over_the_quantity_code() {
        let owned = native_records();
        let records: Vec<&[u8]> = owned.iter().map(|r| r.as_slice()).collect();
        let layout = DefLayout::detect(&records, 8);
        // A file declaring km/h for a speed channel keeps km/h, not m/s.
        let declared = record("GPS Speed", 3, "km/h");
        let (unit, source) = layout.resolve(&declared);
        assert_eq!(unit, "km/h");
        assert_eq!(source, UnitSource::Declared);
    }

    #[test]
    fn marker_text_is_never_reported_as_a_real_unit() {
        let owned = native_records();
        let records: Vec<&[u8]> = owned.iter().map(|r| r.as_slice()).collect();
        let layout = DefLayout::detect(&records, 8);
        for marker in ["raw", "flag", "cnt", "pp1", "user defined"] {
            let (unit, source) = layout.resolve(&record("X", 0, marker));
            assert_eq!(unit, marker);
            assert_eq!(
                source,
                UnitSource::SpecDefault,
                "{marker} must not be Declared"
            );
        }
    }

    /// A Pi Toolbox export: quantity codes present, unit strings stripped.
    /// This is the case that used to produce fabricated units.
    #[test]
    fn export_without_unit_strings_still_resolves_si_units() {
        let owned: Vec<Vec<u8>> = vec![
            record("STEER", 7, ""),
            record("X_FL_DAMPER", 1, ""),
            record("P_F_BRAKE", 9, ""),
            record("I_ACCEL_LONG", 10, ""),
            record("Speed_Ref", 3, ""),
            record("Global Time", 6, ""),
            record("gear", 0, ""),
            record("Lap Number", 0, ""),
        ];
        let records: Vec<&[u8]> = owned.iter().map(|r| r.as_slice()).collect();
        let layout = DefLayout::detect(&records, 8);
        assert_eq!(layout.quantity, Some(0x88));
        assert_eq!(layout.unit, None, "export declares no unit strings");

        // These are exactly the channels the old name-guessing got wrong.
        let expected = [
            ("STEER", 7, "rad", UnitSource::SpecDefault),
            ("X_FL_DAMPER", 1, "m", UnitSource::SpecDefault),
            ("P_F_BRAKE", 9, "Pa", UnitSource::SpecDefault),
            ("I_ACCEL_LONG", 10, "m/s^2", UnitSource::SpecDefault),
            ("Speed_Ref", 3, "m/s", UnitSource::SpecDefault),
            ("Global Time", 6, "s", UnitSource::SpecDefault),
            // Dimensionless channels must stay unitless.
            ("gear", 0, "", UnitSource::Unknown),
        ];
        for (name, code, unit, source) in expected {
            let (got_unit, got_source) = layout.resolve(&record(name, code, ""));
            assert_eq!(got_unit, unit, "{name} unit");
            assert_eq!(got_source, source, "{name} provenance");
        }
    }

    /// The specific regression: never report degrees for a radian channel.
    #[test]
    fn never_reports_the_old_wrong_units() {
        let owned: Vec<Vec<u8>> = vec![
            record("STEER", 7, ""),
            record("X_FL_DAMPER", 1, ""),
            record("P_F_BRAKE", 9, ""),
            record("I_ACCEL_LONG", 10, ""),
        ];
        let records: Vec<&[u8]> = owned.iter().map(|r| r.as_slice()).collect();
        let layout = DefLayout::detect(&records, 8);
        for (name, code, forbidden) in [
            ("STEER", 7u32, "deg"),
            ("X_FL_DAMPER", 1, "mm"),
            ("P_F_BRAKE", 9, "pa"),
            ("I_ACCEL_LONG", 10, "g"),
        ] {
            let (unit, _) = layout.resolve(&record(name, code, ""));
            assert_ne!(unit, forbidden, "{name} regressed to a wrong unit");
        }
    }

    /// A layout with the fields somewhere else entirely must still be found,
    /// which is the whole point of detecting instead of hardcoding.
    #[test]
    fn detects_a_relocated_layout() {
        fn relocated(quantity: u32, unit: &str) -> Vec<u8> {
            let mut buffer = vec![0u8; 0x80];
            buffer[0x30..0x34].copy_from_slice(&quantity.to_le_bytes());
            for (index, code) in unit.encode_utf16().enumerate() {
                buffer[0x50 + index * 2..0x52 + index * 2].copy_from_slice(&code.to_le_bytes());
            }
            buffer
        }
        let owned = [
            relocated(7, "rad"),
            relocated(4, "K"),
            relocated(9, "Pa"),
            relocated(3, "m/s"),
            relocated(11, "V"),
            relocated(8, "rad/s"),
        ];
        let records: Vec<&[u8]> = owned.iter().map(|r| r.as_slice()).collect();
        let layout = DefLayout::detect(&records, 8);
        assert_eq!(layout.quantity, Some(0x30));
        assert_eq!(layout.unit, Some(0x50));
    }

    #[test]
    fn refuses_to_guess_a_quantity_offset_from_too_few_records() {
        // Two records is not evidence. Unrelated words in a definition record
        // hold small integers, so a tiny file can accidentally look like it has
        // a quantity field; reporting `unknown` beats inventing a dimension.
        let owned: Vec<Vec<u8>> = (0..2u32)
            .map(|seed| {
                let mut buffer = record("Speed", 0, "");
                buffer[0x40..0x44].copy_from_slice(&(seed + 1).to_le_bytes());
                buffer[0x58..0x5c].copy_from_slice(&(seed + 3).to_le_bytes());
                buffer
            })
            .collect();
        let records: Vec<&[u8]> = owned.iter().map(|r| r.as_slice()).collect();
        let layout = DefLayout::detect(&records, 8);
        assert_eq!(layout.quantity, None);
        assert_eq!(layout.unit, None);
        let (unit, source) = layout.resolve(records[0]);
        assert!(unit.is_empty(), "expected no unit, got {unit:?}");
        assert_eq!(source, UnitSource::Unknown);
    }

    /// Garbage must not yield a confident layout.
    #[test]
    fn refuses_to_invent_a_layout_from_noise() {
        let owned: Vec<Vec<u8>> = (0..8u8)
            .map(|seed| {
                (0..0x100)
                    .map(|index| seed.wrapping_mul(31).wrapping_add(index as u8) | 0x80)
                    .collect()
            })
            .collect();
        let records: Vec<&[u8]> = owned.iter().map(|r| r.as_slice()).collect();
        let layout = DefLayout::detect(&records, 8);
        assert_eq!(layout.unit, None, "noise must not produce a unit field");
    }

    #[test]
    fn unknown_codes_never_produce_a_unit() {
        assert_eq!(Quantity::from_code(9999), Quantity::Unknown(9999));
        assert_eq!(Quantity::Unknown(9999).si_unit(), None);
        // Dimensionless and per-unit ratios are unitless by definition.
        assert_eq!(Quantity::Dimensionless.si_unit(), None);
        assert_eq!(Quantity::PerUnit.si_unit(), None);
    }

    #[test]
    fn every_named_quantity_has_an_si_unit() {
        for code in [1u32, 2, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 20, 23] {
            let quantity = Quantity::from_code(code);
            assert!(
                !matches!(quantity, Quantity::Unknown(_)),
                "code {code} should be named"
            );
            assert!(
                quantity.si_unit().is_some(),
                "code {code} ({quantity:?}) should have an SI unit"
            );
        }
    }
}
