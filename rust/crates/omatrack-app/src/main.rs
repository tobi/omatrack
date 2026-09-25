//! `omatrack2`: the Omatrack 2.0 workstation.
//!
//! A headless command (`parse`, `unify`, `corners`, `compare`) as the first
//! argument runs exactly as `omatrack-cli` would and exits before any window
//! system is touched; anything else starts the GPUI application.

fn main() {
    let args: Vec<std::ffi::OsString> = std::env::args_os().collect();
    if let Some(command) = args.get(1)
        && omatrack_cli::is_command(command)
    {
        std::process::exit(omatrack_cli::run(&args[1..], &args[0]));
    }
    omatrack_app::run();
}
