//! One module per headless command.

pub mod compare;
pub mod corners;
pub mod parse;
pub mod unify;

use crate::Out;
use omatrack_core::Recording;
use std::ffi::OsStr;
use std::path::Path;

/// Open a recording for analysis, reporting a failure the way the C++ CLI
/// did (`error: omatrack_open: <parser message>` on stderr).
pub(crate) fn open(path: &OsStr) -> Option<Recording> {
    match Recording::open(Path::new(path)) {
        Ok(recording) => Some(recording),
        Err(error) => {
            let mut err = Out::stderr();
            err.str("error: omatrack_open: ").str(&error.0).str("\n");
            err.flush();
            None
        }
    }
}
