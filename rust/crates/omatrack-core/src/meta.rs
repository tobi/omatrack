//! Cheap recording metadata: filename inference and GPS wall clock.

use crate::num::llround;

/// Session metadata inferred from a (PDS-style) filename stem.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SessionMeta {
    pub date: String,
    pub time: String,
    pub driver_name: String,
    pub driver_tag: String,
    pub vehicle_id: String,
    pub venue: String,
    pub event_name: String,
}

/// Session metadata from a `YYMMDDHHMMSS...` filename stem.
pub fn session_meta_from_filename(stem: &str) -> SessionMeta {
    let mut meta = SessionMeta::default();
    if stem.len() >= 12 {
        let token: String = stem
            .bytes()
            .take_while(u8::is_ascii_digit)
            .take(12)
            .map(char::from)
            .collect();
        if token.len() == 12 {
            let field = |at: usize| -> i32 { token[at..at + 2].parse().unwrap_or(0) };
            let (yy, mm, dd, hh, mi, ss) =
                (field(0), field(2), field(4), field(6), field(8), field(10));
            meta.date = crate::sprintf!("%02d/%02d/%04d", dd, mm, 2000 + yy);
            meta.time = crate::sprintf!("%02d:%02d:%02d", hh, mi, ss);
        }
    }
    meta.event_name = stem.to_string();
    meta
}

/// Unix-epoch nanoseconds at file t = 0 from a GPS week / iTOW sample taken
/// at `file_time_sec`. -1 when the fix is unusable.
pub fn utc_start_ns_from_gps(week: f64, itow_ms: f64, file_time_sec: f64) -> i64 {
    if !week.is_finite()
        || !itow_ms.is_finite()
        || !file_time_sec.is_finite()
        || week < 0.0
        || itow_ms < 0.0
    {
        return -1;
    }
    let mut week_count = llround(week);
    // 10-bit week numbers wrap every 1024 weeks. A 2026 logger that only
    // reports the low bits is in the 2048-3071 era.
    if week_count < 1024 {
        week_count += 2048;
    }
    let itow = llround(itow_ms);
    let file_ns = llround(file_time_sec * 1e9);
    if itow < 0 || file_ns < 0 {
        return -1;
    }
    const GPS_EPOCH_UNIX_SEC: i64 = 315_964_800;
    const LEAP_SECONDS: i64 = 18;
    const NS_PER_SEC: i64 = 1_000_000_000;
    const NS_PER_WEEK: i64 = 604_800 * NS_PER_SEC;
    let gps_ns = week_count
        .wrapping_mul(NS_PER_WEEK)
        .wrapping_add(itow.wrapping_mul(1_000_000));
    let utc_ns = gps_ns
        .wrapping_add(GPS_EPOCH_UNIX_SEC * NS_PER_SEC)
        .wrapping_sub(LEAP_SECONDS * NS_PER_SEC)
        .wrapping_sub(file_ns);
    if utc_ns < 1_000_000_000_000_000_000 {
        return -1;
    }
    utc_ns
}
