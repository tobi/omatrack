//! Derives the converter generation's upstream revision from the workspace
//! manifest, exactly as the Qt bridge's build.rs reads its own: the pinned
//! `motorsport-telemetry-rs` rev is the one string that changes whenever any
//! decoder or the native writer changes.

use std::fs;
use std::path::Path;

fn main() {
    let manifest_dir = std::env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR");
    let manifest = Path::new(&manifest_dir).join("../../Cargo.toml");
    println!("cargo:rerun-if-changed={}", manifest.display());
    let text = fs::read_to_string(&manifest).expect("read workspace Cargo.toml");
    let rev = text
        .lines()
        .filter(|line| line.contains("motorsport-telemetry-rs"))
        .find_map(|line| {
            let start = line.find("rev = \"")? + "rev = \"".len();
            let end = line[start..].find('"')? + start;
            Some(line[start..end].to_string())
        })
        .expect("workspace Cargo.toml pins motorsport-telemetry-rs by rev");
    assert!(rev.len() >= 12, "pinned rev too short to identify a commit");
    println!("cargo:rustc-env=OMATRACK_UPSTREAM_REV={rev}");
}
