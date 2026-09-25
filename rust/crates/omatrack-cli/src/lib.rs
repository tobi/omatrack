//! Headless omatrack commands, shared by `omatrack2` (dispatched before the
//! GPUI application starts) and the test-only `omatrack-cli` binary.
//!
//! ```text
//! parse <file>
//! unify <file> --output <csv>
//! corners <file> [--lap N] [--reference <file>] [--reference-lap N]
//!         --zone <start:end> ...
//! compare <aimd.mp4> <file.telemetry>
//! ```
//!
//! Line-for-line port of the C++ `cli/Headless.cpp`: the output is the
//! acceptance surface and is byte-compared against the C++ oracle
//! (`rust/parity/run.sh`). Exit code: 0 success, 1 failure, 2 usage.
//!
//! Documented deviation: a non-numeric `--lap`/`--reference-lap` or `--zone`
//! value prints usage and exits 2, where `std::stoi`/`std::stod` aborted the
//! C++ process.

mod commands;
mod out;

use std::ffi::OsString;
use std::os::unix::ffi::OsStrExt;

pub use out::Out;

const COMMANDS: [&str; 4] = ["parse", "unify", "corners", "compare"];

/// True when `argument` names a headless command.
pub fn is_command(argument: &std::ffi::OsStr) -> bool {
    COMMANDS.iter().any(|c| c.as_bytes() == argument.as_bytes())
}

/// The command usage for `program`, as printed to stderr.
pub fn usage(program: &[u8]) -> Vec<u8> {
    let mut text = Vec::new();
    let line = |text: &mut Vec<u8>, before: &str, after: &str| {
        text.extend_from_slice(before.as_bytes());
        text.extend_from_slice(program);
        text.extend_from_slice(after.as_bytes());
    };
    line(
        &mut text,
        "usage:\n  ",
        " parse <file.pds|file.ld|file.vbo|file.mp4|file.telemetry>\n",
    );
    line(&mut text, "  ", " unify <file> --output <csv>\n");
    line(
        &mut text,
        "  ",
        " corners <file> [--lap N] [--reference <file>] [--reference-lap N] --zone <start:end> [--zone ...]\n",
    );
    line(&mut text, "  ", " compare <aimd.mp4> <file.telemetry>\n");
    line(&mut text, "  ", " --version\n\n");
    text.extend_from_slice(
        b"unify exports location-bearing GPS fields when available; choose an explicit \
output path and handle it as sensitive data.\n\
corners runs the corner analyzers on the fastest lap (or the lap ids given); zones are lap \
fractions.\n\
compare dumps GPS, main channels, laps, and video-frame sync from an AiM extract against \
its .telemetry companion.\n",
    );
    text
}

/// Print the usage for `program` to stderr.
pub fn print_usage(program: &std::ffi::OsStr) {
    let mut err = Out::stderr();
    err.bytes(&usage(program.as_bytes()));
    err.flush();
}

/// Run `args` (`args[0]` is the command, which must satisfy
/// [`is_command`]) and return the process exit code. Malformed arguments
/// print usage and return 2.
pub fn run(args: &[OsString], program: &std::ffi::OsStr) -> i32 {
    let arg = |i: usize| args[i].as_os_str();
    let command = args.first().map(|a| a.as_bytes()).unwrap_or_default();
    // argc counts the program name too.
    let argc = args.len() + 1;
    match command {
        b"parse" if argc == 3 => return commands::parse::run(arg(1)),
        b"unify" if argc == 5 && arg(2).as_bytes() == b"--output" => {
            return commands::unify::run(arg(1), arg(3));
        }
        b"compare" if argc == 4 => return commands::compare::run(arg(1), arg(2)),
        b"corners" if argc >= 3 => return commands::corners::run_args(args, program),
        _ => {}
    }
    print_usage(program);
    2
}

/// The standalone `omatrack-cli` entry point (`main` of the test binary).
pub fn main_with(args: Vec<OsString>) -> i32 {
    let program = args.first().cloned().unwrap_or_default();
    if args.len() == 2 && (args[1] == "--version" || args[1] == "-V") {
        let mut out = Out::stdout();
        out.str(&format!("omatrack-cli {}\n", env!("CARGO_PKG_VERSION")));
        out.flush();
        return 0;
    }
    if args.len() < 2 || !is_command(&args[1]) {
        print_usage(&program);
        return 2;
    }
    run(&args[1..], &program)
}
