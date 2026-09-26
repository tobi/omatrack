//! C++ `<algorithm>` / `<cmath>` semantics that differ from Rust's.
//!
//! `std::min`/`std::max` are comparisons, not IEEE minNum/maxNum: with a NaN
//! operand they return whichever argument the comparison selects, whereas
//! `f64::min` ignores the NaN. The ported analysis keeps the C++ behaviour so
//! both implementations agree bit-for-bit on the same (sometimes NaN) data.

/// `std::min(a, b)`: `(b < a) ? b : a`.
#[inline]
pub fn min(a: f64, b: f64) -> f64 {
    if b < a { b } else { a }
}

/// `std::max(a, b)`: `(a < b) ? b : a`.
#[inline]
pub fn max(a: f64, b: f64) -> f64 {
    if a < b { b } else { a }
}

/// `std::clamp(v, lo, hi)`: `v < lo ? lo : hi < v ? hi : v` (NaN passes).
#[inline]
pub fn clamp(v: f64, lo: f64, hi: f64) -> f64 {
    if v < lo {
        lo
    } else if hi < v {
        hi
    } else {
        v
    }
}

/// `std::llround` exactly as glibc on x86-64: half away from zero, and `LLONG_MIN` for
/// NaN, infinities and out-of-range values (the hardware conversion result).
///
/// Rust's saturating `as` would disagree on those.
#[inline]
#[expect(
    clippy::cast_possible_truncation,
    reason = "Reproduce the documented C++ integer conversion, including rounding and overflow bit patterns."
)]
pub fn llround(v: f64) -> i64 {
    #[expect(
        clippy::cast_precision_loss,
        reason = "Reproduce the documented C++ integer conversion, including rounding and overflow bit patterns."
    )]
    const LOW: f64 = i64::MIN as f64;

    let r = v.round();
    if r.is_nan() || !(LOW..-LOW).contains(&r) {
        return i64::MIN;
    }
    r as i64
}

/// `int(std::llround(v))`: the long long truncated to 32 bits.
#[inline]
#[expect(
    clippy::cast_possible_truncation,
    reason = "Reproduce the documented C++ integer conversion, including rounding and overflow bit patterns."
)]
pub fn llround_i32(v: f64) -> i32 {
    llround(v) as i32
}

/// Integer `std::clamp`.
#[inline]
pub fn clamp_i(v: i64, lo: i64, hi: i64) -> i64 {
    if v < lo {
        lo
    } else if hi < v {
        hi
    } else {
        v
    }
}

/// `int(x)` of a double as x86-64 compiles it (`cvttsd2si`): truncation
/// toward zero, `INT_MIN` for NaN and out-of-range values.
#[inline]
#[expect(
    clippy::cast_possible_truncation,
    reason = "Reproduce the documented C++ integer conversion, including rounding and overflow bit patterns."
)]
pub fn trunc_i32(v: f64) -> i32 {
    if v.is_nan() || v <= -2_147_483_649.0 || v >= 2_147_483_648.0 {
        return i32::MIN;
    }
    v as i32
}

/// Median at `len / 2` of a sorted copy (the `sort` + `v[n/2]` idiom).
pub fn upper_median(values: &mut [f64]) -> f64 {
    values.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    values[values.len() / 2]
}
