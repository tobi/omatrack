# Contributing to Omatrack

## Before changing code

Open an issue for broad product or architecture changes. Small fixes with a
clear scope can go directly to a pull request. [AGENTS.md](AGENTS.md) is the
product and engineering contract: read the section for the crate you touch.

Boundaries:

1. Vendor decoding belongs upstream in
   [`motorsport-telemetry-rs`](https://github.com/tobi/motorsport-telemetry-rs);
   Omatrack takes it through a pinned revision in `rust/Cargo.toml`.
2. Cross-format normalization, laps, alignment, delta and corner analysis
   belong in `omatrack-core` (no GPUI). The UI never branches on file format.
3. Track identity, geometry and corners come from
   [Track Atlas](https://github.com/tobi/track-atlas). Contribute
   authoritative metadata upstream, then bump the pin.
4. UI uses gpui-kit components and theme tokens; first-party elements only for
   hot telemetry drawing.

## Gates

From `rust/`, every step must pass:

```sh
scripts/check.sh
```

It runs `cargo fmt --all --check`, `cargo clippy --workspace --all-targets
--locked -- -D warnings`, `cargo test --workspace --locked`, the `#[ignore]`d
`real_*` tests when `OMATRACK_FIXTURES` (or the default fixture folder) is
present, and the CLI regression against the parity baseline when it exists.
Additionally, for the relevant changes:

```sh
cargo run --release -p omatrack-trace --example trace_bench   # renderer changes
cargo build --release --locked -p omatrack-app               # release build
scripts/screenshot.sh                                         # headless visual check (target/shot/shot.png)
```

`scripts/screenshot.sh` renders the app in a headless Wayland compositor via
`nix shell` (cage + grim); run it outside any sandbox. It is not a substitute
for checking native visuals and frame time on a real display.

### Lockfile

The GPUI stack is yanked from the crates.io index. Always pass `--locked` and
never run `cargo update`; `rust/scripts/seed-cargo-lock.sh` documents how the
lockfile was seeded. Parallel runners use their own
`CARGO_TARGET_DIR=rust/target-<slug>`.

### Parity baseline

`rust/parity/run.sh` runs the headless CLI over 89 cases and `diff -r`s
stdout, stderr, CSVs and exit codes against a frozen baseline in
`rust/parity/baseline/` (gitignored: it holds GPS), captured from the last
byte-identical run of the retired C++ implementation. It must report 0 diffs.
A deliberate analytical change is accepted with `parity/run.sh --rebaseline`,
with the reason in the commit message.

## Verification expectations

- Parser pin bumps: parse a copied real fixture for every affected format.
- Normalization or lap changes: run `omatrack2 unify` on a copied fixture and
  check sample counts, units, distance monotonicity and physical plausibility.
- Comparison changes: exercise laps of different durations and with missing
  or degraded GPS.
- Corner checks: run `omatrack2 corners` on two real laps of one track and
  read the notes; a check that fires everywhere is noise.
- UI changes: a headless GPUI test (`#[gpui_kit::test]`) driving real actions.
- Renderer changes: before/after `trace_bench` numbers against the 8.33 ms
  design target and 16.67 ms hard ceiling.

## Telemetry is read-only

Never modify, rename or delete source telemetry or onboard video, and never
write beside it. Real-file tests read fixtures only; derived files belong in
the target directory or `$TMPDIR`. Do not commit telemetry, video, parity
output or CSV exports (they contain GPS).

## Commits and pull requests

- Logical steps with subject `<scope>: <summary>`, where scope is one of
  `core`, `cli`, `library`, `trace`, `mpv-player`, `ui`, `app`, `rust`,
  `docs`; a short body with what, why and gate results for behaviour changes.
- Stage explicit paths (`git add <paths>`), never `git add -A` or `.`.
- Pull requests state the problem and chosen behaviour, the exact
  verification commands, before/after timing for renderer hot paths, and
  screenshots for visible changes with private telemetry details removed.
