//! Derives the converter generation's upstream revision from the workspace
//! manifest, exactly as the Qt bridge's build.rs reads its own: the pinned
//! `motorsport-telemetry-rs` rev is the one string that changes whenever any
//! decoder or the native writer changes.

use std::fs;
use std::path::Path;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let manifest_dir = std::env::var("CARGO_MANIFEST_DIR")?;
    let manifest = Path::new(&manifest_dir).join("../../Cargo.toml");
    println!("cargo:rerun-if-changed={}", manifest.display());
    let text = fs::read_to_string(&manifest)?;
    let rev = text
        .lines()
        .filter(|line| line.contains("motorsport-telemetry-rs"))
        .find_map(|line| {
            let start = line.find("rev = \"")? + "rev = \"".len();
            let end = line[start..].find('"')? + start;
            Some(line[start..end].to_string())
        })
        .ok_or("workspace Cargo.toml must pin motorsport-telemetry-rs by rev")?;
    if rev.len() < 12 {
        return Err("pinned rev too short to identify a commit".into());
    }
    println!("cargo:rustc-env=OMATRACK_UPSTREAM_REV={rev}");
    Ok(())
}
