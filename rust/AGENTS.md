# Omatrack 2.0 (Rust/GPUI port) — working rules

This workspace is Omatrack 2.0, the whole application. `../AGENTS.md` is the
product and engineering contract. The headless CLI is regression-tested by
`parity/run.sh` against a frozen, gitignored baseline (`parity/baseline/`,
captured from the retired C++ implementation; `--rebaseline` accepts a
deliberate change).

## Read the GPUI skills first

Any UI, component, state, or window-chrome work in `crates/omatrack-ui` (or
anywhere `gpui_kit::*` is used) is governed by two vendored skills, not
generic Rust/GPUI knowledge from training data:

| Skill | Path | Read before |
|---|---|---|
| `gpui-kit` | `../.agents/skills/gpui-kit/SKILL.md` | Any `gpui-kit` component, state ownership, `Entity<T>` vs `RenderOnce`, actions/events/focus, async, public API, naming, testing |
| `gpui-kit-design-guides` | `../.agents/skills/gpui-kit-design-guides/SKILL.md` | Any visible-surface change: layout, spacing, color, density, interaction states, overlays, motion, copy |

Both are normative (see each `SKILL.md`'s own "Read the Guides First"
section) — read the linked reference files themselves, not this summary, a
similar file elsewhere in the codebase, or training data. The design guide
comes first when a change has a visible surface.

Minimum reading, every time (paths relative to `../.agents/skills/`):

- `gpui-kit/SKILL.md`, then `gpui-kit/references/coding-guides.md`:
  "Architecture at a glance", "Rules for coding agents", "Common failure
  modes" and "Implementation checklist" always; the whole guide for a new
  crate, module or feature; the section for the change otherwise
  (`grep -n '^## '` lists them).
- `gpui-kit/references/conventions.md`, `recipes.md` and `usage.md` when
  choosing or wiring a component.
- `gpui-kit/references/gpui/<topic>.md` for every GPUI mechanism touched:
  `element*.md` (custom trace/video elements), `entity*.md`, `action.md` +
  `focus-handle.md` (keybindings), `async.md` (background work, mpv
  events), `layout-style.md`, `element-id.md`, `test*.md` (headless UI
  integration tests with `#[gpui_kit::test]`).
- `gpui-kit-design-guides/SKILL.md` and its `references/design-guides.md`
  for any visible surface; run its Design review checklist before finishing.
- `../AGENTS.md` sections 6 and 8 for the product rules (corner analyzers,
  trace rendering, read-only test data). `omatrack/SKILL.md` describes the
  retired Qt 1.x app and is design history only.

Never invent a `gpui-kit` API: verify signatures in the registry sources
(`~/.cargo/registry/src/*/gpui-kit-0.6.6`, `gpui-component-0.6.6`,
`gpui-base-0.6.6`). Reports and reviews state which skill files were read.

## Gates

```sh
scripts/check.sh      # fmt, clippy -D warnings, tests (--locked), real_*, parity
./parity/run.sh       # CLI against the frozen baseline: 89 cases, 0 diffs
scripts/screenshot.sh # headless visual check; run unsandboxed
```

Parallel runners use their own `CARGO_TARGET_DIR`; `parity/run.sh` then
writes to `$CARGO_TARGET_DIR/parity-out` (or `$PARITY_OUT`) instead of the
shared `parity/out`.
