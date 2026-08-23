// Headless commands shared by `omatrack` (dispatched before Qt starts) and
// the test-only `omatrack-cli` binary. Qt-free: links omatrack_core only.
//
//   parse <file>
//   unify <file> --output <csv>
//   corners <file> [--lap N] [--reference <file>] [--reference-lap N]
//           --zone <start:end> ...
//   compare <aimd.mp4> <file.telemetry>
//
// Exit code is the acceptance signal: 0 = success, non-zero = failure.
#pragma once

namespace omatrack::headless {

/// True when `argument` names a headless command (`parse`, `unify`, …).
bool isCommand(const char* argument);

/// Prints the command usage for `program` to stderr.
void printUsage(const char* program);

/// Runs `argv[1..]` as a headless command. `argv[1]` must satisfy
/// isCommand(); malformed arguments print usage and return 2.
int run(int argc, char** argv, const char* program);

}  // namespace omatrack::headless
