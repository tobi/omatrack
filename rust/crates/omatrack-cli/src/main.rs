//! `omatrack-cli`: test-only entry point for the headless commands that
//! also ship inside `omatrack2`. Built for the parity harness and scripts;
//! never packaged.

fn main() {
    let args: Vec<std::ffi::OsString> = std::env::args_os().collect();
    std::process::exit(omatrack_cli::main_with(args));
}
