//! `omatrack2 <command>` must behave exactly like `omatrack-cli <command>`:
//! same stdout, stderr and exit code, before any window system starts.

use std::os::unix::process::CommandExt as _;
use std::path::PathBuf;
use std::process::{Command, Output};

/// The `omatrack-cli` binary from the same target directory, built on demand
/// (it belongs to another package, so Cargo does not build it for these
/// tests).
fn omatrack_cli() -> PathBuf {
    let omatrack2 = PathBuf::from(env!("CARGO_BIN_EXE_omatrack2"));
    let path = omatrack2.with_file_name("omatrack-cli");
    if !path.exists() {
        let status = Command::new(env!("CARGO"))
            .args([
                "build",
                "--locked",
                "-p",
                "omatrack-cli",
                "--bin",
                "omatrack-cli",
            ])
            .current_dir(env!("CARGO_MANIFEST_DIR"))
            .status()
            .expect("cargo runs");
        assert!(status.success(), "building omatrack-cli failed");
    }
    path
}

/// Run `binary` with argv[0] fixed, so usage text names the same program.
fn run(binary: &PathBuf, args: &[&str]) -> Output {
    Command::new(binary)
        .arg0("omatrack")
        .args(args)
        .env_remove("WAYLAND_DISPLAY")
        .env_remove("DISPLAY")
        .output()
        .expect("binary runs")
}

#[test]
fn headless_commands_match_omatrack_cli() {
    let cli = omatrack_cli();
    let app = PathBuf::from(env!("CARGO_BIN_EXE_omatrack2"));
    for args in [
        &["parse", "/nonexistent.mp4"][..],
        &["parse", "/nonexistent.xyz"][..],
        &["parse"][..],
        &["corners", "/nonexistent.mp4", "--lap", "3"][..],
    ] {
        let expected = run(&cli, args);
        let actual = run(&app, args);
        assert_eq!(
            actual.status.code(),
            expected.status.code(),
            "{args:?} exit code"
        );
        assert_eq!(actual.stdout, expected.stdout, "{args:?} stdout");
        assert_eq!(actual.stderr, expected.stderr, "{args:?} stderr");
    }
    // The failures really are failures, not an empty run.
    assert_eq!(
        run(&app, &["parse", "/nonexistent.mp4"]).status.code(),
        Some(1)
    );
    assert_eq!(run(&app, &["parse"]).status.code(), Some(2));
}
