//! `compare <aimd.mp4> <file.telemetry>`: side-by-side source dump.

use crate::Out;
use omatrack_core::report::compare_telemetry_sources;
use std::ffi::OsStr;

pub fn run(left_path: &OsStr, right_path: &OsStr) -> i32 {
    let Some(left) = super::open(left_path) else {
        return 1;
    };
    let Some(right) = super::open(right_path) else {
        return 1;
    };
    let report = compare_telemetry_sources(&left, &right, "aimd", "telemetry");
    Out::stdout().str(&report).flush();
    0
}
