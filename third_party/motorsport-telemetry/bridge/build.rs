//! Derives the converter generation from this crate's own manifest.
//!
//! Normalized `.telemetry` caches are keyed by source identity (ETag / BLAKE3)
//! *and* by the generation of the converter that produced them, so a
//! decoder fix upstream never leaves a stale normalization in place. The
//! generation is the pinned `motorsport-telemetry-rs` revision, which is the
//! one string that changes whenever any decoder or the native writer changes.

use std::fs;
use std::path::Path;

fn main() {
    let manifest_dir = std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let manifest = Path::new(&manifest_dir).join("Cargo.toml");
    println!("cargo:rerun-if-changed={}", manifest.display());
    let text = fs::read_to_string(&manifest).expect("read bridge Cargo.toml");
    let rev = text
        .lines()
        .filter(|line| line.contains("motorsport-telemetry-rs"))
        .find_map(|line| {
            let start = line.find("rev = \"")? + "rev = \"".len();
            let end = line[start..].find('"')? + start;
            Some(line[start..end].to_string())
        })
        .expect("bridge Cargo.toml pins motorsport-telemetry-rs by rev");
    assert!(rev.len() >= 12, "pinned rev too short to identify a commit");
    println!("cargo:rustc-env=OMATRACK_UPSTREAM_REV={rev}");
}
