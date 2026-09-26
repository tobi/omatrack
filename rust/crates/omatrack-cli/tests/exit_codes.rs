//! Exit codes and argument handling (byte-level output parity against the
//! C++ oracle lives in rust/parity/run.sh).

#![cfg(test)]

use omatrack_cli::{is_command, main_with, usage};
use std::ffi::OsString;

fn run(args: &[&str]) -> i32 {
    main_with(
        std::iter::once("omatrack-cli")
            .chain(args.iter().copied())
            .map(OsString::from)
            .collect(),
    )
}

#[test]
fn usage_and_argument_errors_exit_2() {
    assert_eq!(run(&[]), 2);
    assert_eq!(run(&["frobnicate"]), 2);
    assert_eq!(run(&["parse"]), 2);
    assert_eq!(run(&["unify", "x.mp4"]), 2);
    assert_eq!(run(&["corners", "x.mp4", "--lap", "3"]), 2);
    assert_eq!(run(&["corners", "x.mp4", "--zone", "0.5"]), 2);
    assert_eq!(
        run(&["corners", "x.mp4", "--bogus", "1", "--zone", "0.1:0.2"]),
        2
    );
    // Documented deviation: the C++ aborted on a non-numeric lap id.
    assert_eq!(
        run(&["corners", "x.mp4", "--lap", "abc", "--zone", "0.1:0.2"]),
        2
    );
    assert_eq!(run(&["--version"]), 0);
}

#[test]
fn open_failures_exit_1() {
    assert_eq!(run(&["parse", "/no/such/file.mp4"]), 1);
    assert_eq!(run(&["parse", "/no/such/file.xyz"]), 1);
    assert_eq!(
        run(&["corners", "/no/such/file.mp4", "--zone", "0.1:0.2"]),
        1
    );
}

#[test]
fn commands_and_usage_text() {
    for command in ["parse", "unify", "corners", "compare"] {
        assert!(is_command(command.as_ref()));
    }
    assert!(!is_command("--version".as_ref()));
    let text = String::from_utf8(usage(b"prog")).unwrap();
    assert!(
        text.starts_with(
            "usage:\n  prog parse <file.pds|file.ld|file.vbo|file.mp4|file.telemetry>\n"
        )
    );
    assert!(text.contains("  prog --version\n\n"));
}
