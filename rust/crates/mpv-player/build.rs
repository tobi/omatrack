//! Links libmpv through pkg-config. The client API 2.5 (mpv 0.41) is the
//! floor: the software render API and `MPV_RENDER_PARAM_SW_*` are used.
fn main() -> Result<(), String> {
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");
    pkg_config::Config::new()
        .atleast_version("2.5")
        .probe("mpv")
        .map_err(|error| {
            format!("libmpv >= 2.5 development files are required (pkg-config mpv): {error}")
        })?;
    Ok(())
}
