//! printf-exact formatting.
//!
//! The headless commands are the acceptance surface, and their output is
//! byte-compared against the C++ oracle. Rust's `{:.3}` rounds like printf
//! for finite values but disagrees on `nan`/`-nan`/`inf` spelling and on
//! width/flag combinations, so every numeric conversion here is delegated to
//! the C library's own `snprintf` with the exact conversion spec. Strings are
//! padded by BYTES, as printf does (`%-40s` of `°` pads one column less than
//! a char count would).

use std::ffi::CString;

/// One printf argument.
#[derive(Debug, Clone, Copy)]
pub enum Arg<'a> {
    /// `%f`, `%e`, `%g` (and their width/precision/flag variants).
    F(f64),
    /// `%d`, `%i`, `%lld`, `%ld`.
    I(i64),
    /// `%zu`, `%u`, `%llu`, `%x`.
    U(u64),
    /// `%s`.
    S(&'a str),
    /// `%c`.
    C(u8),
}

impl From<f64> for Arg<'_> {
    fn from(value: f64) -> Self {
        Arg::F(value)
    }
}
impl From<i32> for Arg<'_> {
    fn from(value: i32) -> Self {
        Arg::I(i64::from(value))
    }
}
impl From<i64> for Arg<'_> {
    fn from(value: i64) -> Self {
        Arg::I(value)
    }
}
impl From<u64> for Arg<'_> {
    fn from(value: u64) -> Self {
        Arg::U(value)
    }
}
impl From<usize> for Arg<'_> {
    fn from(value: usize) -> Self {
        Arg::U(value as u64)
    }
}
impl<'a> From<&'a str> for Arg<'a> {
    fn from(value: &'a str) -> Self {
        Arg::S(value)
    }
}
impl<'a> From<&'a String> for Arg<'a> {
    fn from(value: &'a String) -> Self {
        Arg::S(value.as_str())
    }
}

/// `sprintf(format, args...)` into a `String`.
///
/// Supports the conversions the omatrack C++ code uses: flags `-+ #0`,
/// width, `.precision`, length modifiers `hh h l ll z j t` (ignored: the
/// argument's own type decides), and `d i u x X o f F e E g G s c %`.
/// A mismatched argument panics: format strings here are compile-time
/// constants, so that is a programming error, never input-dependent.
#[expect(
    clippy::cast_possible_wrap,
    clippy::cast_sign_loss,
    reason = "C printf signed/unsigned conversions deliberately preserve the integer bit pattern."
)]
#[expect(
    clippy::too_many_lines,
    reason = "Keep the ported analysis/report stages in source order so numerical and CLI parity remain auditable."
)]
///
/// # Panics
/// Panics for an unsupported, truncated or mismatched format string, or unused
/// arguments. Callers supply programmer-owned formats, never source-file text.
#[expect(
    clippy::panic,
    reason = "Only programmer-owned printf formats reach this libc boundary; a mismatched format is a programming error, not source input."
)]
pub fn sprintf(format: &str, args: &[Arg<'_>]) -> String {
    let mut out = String::with_capacity(format.len() + 16);
    let bytes = format.as_bytes();
    let mut next = 0usize;
    let mut i = 0usize;
    while i < bytes.len() {
        let byte = bytes[i];
        if byte != b'%' {
            // Copy the literal run in one go (keeps UTF-8 sequences intact).
            let start = i;
            while i < bytes.len() && bytes[i] != b'%' {
                i += 1;
            }
            out.push_str(&format[start..i]);
            continue;
        }
        i += 1;
        if i < bytes.len() && bytes[i] == b'%' {
            out.push('%');
            i += 1;
            continue;
        }
        let spec_start = i;
        while i < bytes.len() && matches!(bytes[i], b'-' | b'+' | b' ' | b'#' | b'0') {
            i += 1;
        }
        let flags = &format[spec_start..i];
        let width_start = i;
        while i < bytes.len() && bytes[i].is_ascii_digit() {
            i += 1;
        }
        let width = &format[width_start..i];
        let precision = if i < bytes.len() && bytes[i] == b'.' {
            let precision_start = i + 1;
            i += 1;
            while i < bytes.len() && bytes[i].is_ascii_digit() {
                i += 1;
            }
            Some(&format[precision_start..i])
        } else {
            None
        };
        while i < bytes.len() && matches!(bytes[i], b'h' | b'l' | b'z' | b'j' | b't' | b'L') {
            i += 1;
        }
        let conversion = *bytes
            .get(i)
            .unwrap_or_else(|| panic!("cfmt: truncated conversion in {format:?}"));
        i += 1;
        let arg = *args
            .get(next)
            .unwrap_or_else(|| panic!("cfmt: missing argument {next} for {format:?}"));
        next += 1;
        match conversion {
            b's' => {
                let Arg::S(text) = arg else {
                    panic!("cfmt: %s wants a string in {format:?}");
                };
                let text = match precision {
                    Some(p) => truncate_bytes(text, p.parse().unwrap_or(0)),
                    None => text,
                };
                pad(&mut out, text, flags, width);
            }
            b'c' => {
                let Arg::C(ch) = arg else {
                    panic!("cfmt: %c wants a byte in {format:?}");
                };
                let buffer = [ch];
                pad(
                    &mut out,
                    std::str::from_utf8(&buffer).unwrap_or("?"),
                    flags,
                    width,
                );
            }
            b'f' | b'F' | b'e' | b'E' | b'g' | b'G' | b'a' | b'A' => {
                let Arg::F(value) = arg else {
                    panic!("cfmt: %{} wants a double in {format:?}", conversion as char)
                };
                let spec = c_spec(flags, width, precision, "", conversion);
                out.push_str(&c_double(&spec, value));
            }
            b'd' | b'i' => {
                let value = match arg {
                    Arg::I(v) => v,
                    Arg::U(v) => v as i64,
                    _ => panic!("cfmt: %d wants an integer in {format:?}"),
                };
                let spec = c_spec(flags, width, precision, "ll", conversion);
                out.push_str(&c_signed(&spec, value));
            }
            b'u' | b'x' | b'X' | b'o' => {
                let value = match arg {
                    Arg::U(v) => v,
                    Arg::I(v) => v as u64,
                    _ => panic!("cfmt: %u wants an integer in {format:?}"),
                };
                let spec = c_spec(flags, width, precision, "ll", conversion);
                out.push_str(&c_unsigned(&spec, value));
            }
            other => panic!(
                "cfmt: unsupported conversion %{} in {format:?}",
                other as char
            ),
        }
    }
    assert!(
        next == args.len(),
        "cfmt: {} unused arguments for {format:?}",
        args.len() - next
    );
    out
}

/// `%.{precision}f`, the most common conversion.
pub fn fixed(value: f64, precision: usize) -> String {
    c_double(&format!("%.{precision}f"), value)
}

fn truncate_bytes(text: &str, max: usize) -> &str {
    if text.len() <= max {
        return text;
    }
    let mut end = max;
    while end > 0 && !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

fn pad(out: &mut String, text: &str, flags: &str, width: &str) {
    let width: usize = width.parse().unwrap_or(0);
    let fill = width.saturating_sub(text.len());
    if flags.contains('-') {
        out.push_str(text);
        out.extend(std::iter::repeat_n(' ', fill));
    } else {
        out.extend(std::iter::repeat_n(' ', fill));
        out.push_str(text);
    }
}

fn c_spec(flags: &str, width: &str, precision: Option<&str>, length: &str, conv: u8) -> String {
    let mut spec = String::with_capacity(16);
    spec.push('%');
    spec.push_str(flags);
    spec.push_str(width);
    if let Some(p) = precision {
        spec.push('.');
        spec.push_str(p);
    }
    spec.push_str(length);
    spec.push(conv as char);
    spec
}

#[expect(
    unsafe_code,
    reason = "Use libc snprintf to preserve the frozen CLI's exact float formatting."
)]
#[expect(
    clippy::cast_sign_loss,
    reason = "finish checks that snprintf returned a nonnegative byte count before using it as a buffer length."
)]
#[expect(
    clippy::expect_used,
    reason = "Only programmer-owned printf formats reach this libc boundary; a mismatched format is a programming error, not source input."
)]
fn c_double(spec: &str, value: f64) -> String {
    let spec = CString::new(spec).expect("printf spec has no NUL");
    let mut buffer = [0u8; 512];
    // SAFETY: `buffer` is writable for its full length, `spec` is a valid
    // NUL-terminated format with exactly one double conversion (built by
    // c_spec from a validated conversion character), and the variadic
    // argument is passed as a C double.
    let written = unsafe {
        libc::snprintf(
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            spec.as_ptr(),
            value as libc::c_double,
        )
    };
    finish(&buffer, written, || {
        // Longer than 512 bytes: only huge %f of 1e300-scale values. Retry
        // with an exact-size buffer.
        let mut big = vec![0u8; written as usize + 1];
        // SAFETY: as above, with a buffer sized from the first call.
        unsafe {
            libc::snprintf(big.as_mut_ptr().cast(), big.len(), spec.as_ptr(), value);
        }
        big
    })
}

#[expect(
    unsafe_code,
    reason = "Use libc snprintf with the validated signed-integer format and argument."
)]
#[expect(
    clippy::expect_used,
    clippy::panic,
    reason = "Only programmer-owned printf formats reach this libc boundary; a mismatched format is a programming error, not source input."
)]
fn c_signed(spec: &str, value: i64) -> String {
    let spec = CString::new(spec).expect("printf spec has no NUL");
    let mut buffer = [0u8; 128];
    // SAFETY: one `%lld`-family conversion, argument passed as long long.
    let written = unsafe {
        libc::snprintf(
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            spec.as_ptr(),
            value as libc::c_longlong,
        )
    };
    finish(&buffer, written, || {
        panic!("cfmt: integer conversion overflowed 128 bytes")
    })
}

#[expect(
    unsafe_code,
    reason = "Use libc snprintf with the validated unsigned-integer format and argument."
)]
#[expect(
    clippy::expect_used,
    clippy::panic,
    reason = "Only programmer-owned printf formats reach this libc boundary; a mismatched format is a programming error, not source input."
)]
fn c_unsigned(spec: &str, value: u64) -> String {
    let spec = CString::new(spec).expect("printf spec has no NUL");
    let mut buffer = [0u8; 128];
    // SAFETY: one `%llu`-family conversion, argument passed as unsigned long long.
    let written = unsafe {
        libc::snprintf(
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            spec.as_ptr(),
            value as libc::c_ulonglong,
        )
    };
    finish(&buffer, written, || {
        panic!("cfmt: integer conversion overflowed 128 bytes")
    })
}

#[expect(
    clippy::cast_sign_loss,
    reason = "finish checks that snprintf returned a nonnegative byte count before using it as a buffer length."
)]
fn finish(buffer: &[u8], written: libc::c_int, retry: impl FnOnce() -> Vec<u8>) -> String {
    assert!(written >= 0, "snprintf failed");
    let written = written as usize;
    if written < buffer.len() {
        return String::from_utf8_lossy(&buffer[..written]).into_owned();
    }
    let big = retry();
    String::from_utf8_lossy(&big[..written]).into_owned()
}

/// `std::stoi`: skip leading whitespace, parse the longest decimal integer
/// prefix (`"3x"` is 3). `None` where the C++ would throw
/// (`invalid_argument` / `out_of_range`).
#[expect(
    unsafe_code,
    reason = "Use libc strtol and thread-local errno to preserve C++ prefix parsing."
)]
pub fn stoi(text: &str) -> Option<i32> {
    let c = CString::new(text).ok()?;
    let mut end: *mut libc::c_char = std::ptr::null_mut();
    // SAFETY: `c` is NUL-terminated and outlives the call; `end` receives a
    // pointer into it. errno is thread-local and reset first.
    let (value, consumed, range_error) = unsafe {
        *libc::__errno_location() = 0;
        let value = libc::strtol(c.as_ptr(), &raw mut end, 10);
        let consumed = end.offset_from(c.as_ptr());
        (value, consumed, *libc::__errno_location() == libc::ERANGE)
    };
    if consumed == 0 || range_error {
        return None;
    }
    i32::try_from(value).ok()
}

/// `std::stod`: the longest `strtod` prefix (decimal, hex, inf, nan). `None`
/// where the C++ would throw.
#[expect(
    unsafe_code,
    reason = "Use libc strtod and thread-local errno to preserve C++ prefix parsing."
)]
pub fn stod(text: &str) -> Option<f64> {
    let c = CString::new(text).ok()?;
    let mut end: *mut libc::c_char = std::ptr::null_mut();
    // SAFETY: as in `stoi`.
    let (value, consumed, range_error) = unsafe {
        *libc::__errno_location() = 0;
        let value = libc::strtod(c.as_ptr(), &raw mut end);
        let consumed = end.offset_from(c.as_ptr());
        (value, consumed, *libc::__errno_location() == libc::ERANGE)
    };
    if consumed == 0 || range_error {
        return None;
    }
    Some(value)
}

/// `sprintf!("%.3f %d", a, b)` builds the argument slice from `Into<Arg>`.
#[macro_export]
macro_rules! sprintf {
    ($format:expr $(, $arg:expr)* $(,)?) => {
        $crate::cfmt::sprintf($format, &[$($crate::cfmt::Arg::from($arg)),*])
    };
}

/// Append formatted text to a `String` (`fprintf` into a buffer).
pub fn append(out: &mut String, format: &str, args: &[Arg<'_>]) {
    out.push_str(&sprintf(format, args));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mixes_literals_and_conversions() {
        assert_eq!(
            sprintf(
                "  #%d %s  %8.3fs",
                &[Arg::I(3), Arg::S("1:13.644"), Arg::F(12.5)]
            ),
            "  #3 1:13.644    12.500s"
        );
        assert_eq!(sprintf("%-7s|", &[Arg::S("12m")]), "12m    |");
        assert_eq!(sprintf("%zu%%", &[Arg::U(7)]), "7%");
        assert_eq!(sprintf("%06.3f", &[Arg::F(3.5)]), "03.500");
    }

    #[test]
    fn pads_by_bytes() {
        // "5°" is three bytes: printf pads it to width 4 with ONE space,
        // where a char count would give two.
        assert_eq!(sprintf("%-4s|", &[Arg::S("5°")]), "5° |");
    }

    #[test]
    fn parses_prefixes_like_the_standard_library() {
        assert_eq!(stoi("3x"), Some(3));
        assert_eq!(stoi(" -2"), Some(-2));
        assert_eq!(stoi("x3"), None);
        assert_eq!(stoi("99999999999"), None);
        assert_eq!(stod("0.25:"), Some(0.25));
        assert_eq!(stod(""), None);
    }

    #[test]
    fn spells_non_finite_like_glibc() {
        assert_eq!(fixed(f64::NAN, 3), "nan");
        assert_eq!(fixed(-f64::NAN, 3), "-nan");
        assert_eq!(fixed(f64::INFINITY, 1), "inf");
        assert_eq!(fixed(-0.0, 1), "-0.0");
    }
}
