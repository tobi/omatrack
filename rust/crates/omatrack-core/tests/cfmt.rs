//! cfmt against the C library it must match byte for byte: random specs and
//! values, exact ties, non-finite spellings and byte-width string padding.
#![allow(unsafe_code)]

use omatrack_core::cfmt::{Arg, fixed, sprintf};
use std::ffi::CString;

fn c_double(spec: &str, value: f64) -> String {
    let spec = CString::new(spec).unwrap();
    let mut buffer = vec![0u8; 1024];
    // SAFETY: one double conversion, buffer large enough for these values.
    let n = unsafe {
        libc::snprintf(
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            spec.as_ptr(),
            value,
        )
    };
    String::from_utf8(buffer[..n as usize].to_vec()).unwrap()
}

fn c_long_long(spec: &str, value: i64) -> String {
    let spec = CString::new(spec).unwrap();
    let mut buffer = vec![0u8; 128];
    // SAFETY: one long long conversion.
    let n = unsafe {
        libc::snprintf(
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            spec.as_ptr(),
            value as libc::c_longlong,
        )
    };
    String::from_utf8(buffer[..n as usize].to_vec()).unwrap()
}

/// xorshift64*: deterministic, dependency-free.
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }
    fn pick<'a, T>(&mut self, items: &'a [T]) -> &'a T {
        &items[(self.next() % items.len() as u64) as usize]
    }
    fn value(&mut self) -> f64 {
        match self.next() % 6 {
            // Arbitrary bit patterns (incl. subnormals, huge values).
            0 => f64::from_bits(self.next()),
            // Telemetry-scale values.
            1 => (self.next() % 2_000_000) as f64 / 1000.0 - 1000.0,
            // Exact decimal ties at 0-4 places.
            2 => {
                let places = self.next() % 5;
                let scale = 10f64.powi(places as i32);
                ((self.next() % 20_000) as f64 + 0.5) / scale
                    * if self.next().is_multiple_of(2) {
                        1.0
                    } else {
                        -1.0
                    }
            }
            // Binary ties: k/2^m are exact, so %.Nf hits true halfway cases.
            3 => (self.next() % 4096) as f64 / 2f64.powi((self.next() % 12) as i32),
            4 => *self.pick(&[
                0.0,
                -0.0,
                f64::NAN,
                -f64::NAN,
                f64::INFINITY,
                f64::NEG_INFINITY,
            ]),
            _ => (self.next() as f64) * 1e-300,
        }
    }
}

#[test]
fn random_double_specs_match_snprintf() {
    let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
    let flags = ["", "-", "0", "+", " ", "-0", "+0", "#"];
    for _ in 0..40_000 {
        let value = rng.value();
        if value.is_finite() && value.abs() > 1e200 {
            continue;
        }
        let flag = rng.pick(&flags);
        let width = if rng.next().is_multiple_of(3) {
            String::new()
        } else {
            (rng.next() % 16).to_string()
        };
        let precision = match rng.next() % 4 {
            0 => String::new(),
            _ => format!(".{}", rng.next() % 10),
        };
        let conversion = rng.pick(&["f", "e", "g", "f", "f"]);
        let spec = format!("%{flag}{width}{precision}{conversion}");
        assert_eq!(
            sprintf(&spec, &[Arg::F(value)]),
            c_double(&spec, value),
            "{spec} of {value:e}"
        );
    }
}

#[test]
fn exact_ties_round_like_glibc() {
    // Exactly representable halves: glibc rounds half to even on the exact
    // binary value; 2.675 is below the tie in binary and rounds down.
    for (value, precision, want) in [
        (0.125, 2, "0.12"),
        (0.375, 2, "0.38"),
        (2.5, 0, "2"),
        (3.5, 0, "4"),
        (-0.5, 0, "-0"),
        (2.675, 2, "2.67"),
        (1.0005, 3, "1.000"),
    ] {
        assert_eq!(fixed(value, precision), want, "{value} %.{precision}f");
        assert_eq!(
            fixed(value, precision),
            c_double(&format!("%.{precision}f"), value)
        );
    }
}

#[test]
fn integers_match_snprintf() {
    let mut rng = Rng(42);
    for _ in 0..5_000 {
        let value = rng.next() as i64 >> (rng.next() % 63);
        let spec = format!("%{}{}lld", rng.pick(&["", "-", "0", "+"]), rng.next() % 12);
        assert_eq!(
            sprintf(&spec, &[Arg::I(value)]),
            c_long_long(&spec, value),
            "{spec}"
        );
    }
    assert_eq!(sprintf("%zu", &[Arg::U(u64::MAX)]), "18446744073709551615");
    assert_eq!(sprintf("%d", &[Arg::I(-7)]), "-7");
}

#[test]
fn strings_pad_by_bytes() {
    assert_eq!(sprintf("[%-7s]", &[Arg::S("12m")]), "[12m    ]");
    assert_eq!(sprintf("[%7s]", &[Arg::S("12m")]), "[    12m]");
    assert_eq!(sprintf("[%-4s]", &[Arg::S("°C")]), "[°C ]");
    assert_eq!(sprintf("[%-2s]", &[Arg::S("longer")]), "[longer]");
    assert_eq!(sprintf("[%.3s]", &[Arg::S("abcdef")]), "[abc]");
    assert_eq!(sprintf("%.0f°", &[Arg::F(49.6)]), "50°");
}
