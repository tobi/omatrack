# Strict lint policy

Run `scripts/lint.sh` from this directory. It checks every workspace crate,
target (including tests and examples), and feature with `--locked` and
`-D warnings`. Local `scripts/check.sh` and CI use this same command.
`rust-toolchain.toml` pins Rust, Clippy and rustfmt to one release so agents
and CI see the same diagnostics. Upgrade that pin deliberately and review
the new diagnostics; never update dependencies to get around a lint failure.

## Enforced rules

`Cargo.toml` denies Clippy's `all`, `pedantic` and `nursery` groups, with
the explicit exceptions below. Selected restriction lints also reject:

- Production `unwrap`, `expect`, explicit `panic`, and process exit.
- `todo!`, `unimplemented!`, and `dbg!` (including in tests).
- Discarded must-use results and misleading `.ok()` error suppression.
- Undocumented unsafe blocks, `mem::forget`, and inferred `as _` casts.
- Outer `#[allow]` attributes and lint exemptions without a reason.

Rust checks also deny unsafe code by default, unsafe operations hidden
inside unsafe functions, unused must-use values, unreachable public items,
unnecessary qualification, unknown/renamed lints, and Rust 2018 idiom
violations (including hidden lifetimes). Consuming builders carry
`#[must_use]`; ignoring their returned value loses the change.

Every new crate must inherit the workspace lints:

```toml
[lints]
workspace = true
```

## How to resolve a failure

Fix the cause first. Propagate or handle errors, use checked integer
conversions at external boundaries, and remove unused state or allocations.
Do not change numerical operation order, NaN handling, or rounding merely
to satisfy a suggestion: the core has a byte-parity contract.

When an operation is intentional, use a specific `#[expect]` with a reason
on the smallest useful statement, function or boundary:

```rust,ignore
#[expect(
    clippy::let_underscore_must_use,
    reason = "A dropped view needs no deferred update; this weak handle may already be gone."
)]
let _ = view.update(cx, update_view);
```

`unfulfilled_lint_expectations` makes stale exemptions fail. Explain the
invariant or tradeoff, rather than saying that the lint is inconvenient.
Do not add blanket group exemptions, `cfg_attr(test, allow(...))`, or
`--cap-lints` to the gate. Any change to the workspace policy needs its own
reason and review, not just a passing build.

Existing local expectations document numerical port boundaries, deliberate
pixel projections, exact state comparisons, declarative layouts, and weak
UI updates after a view closes. Unsafe exceptions are confined to the
libmpv boundary, pixel-copy kernel, and libc formatting/parsing oracle.

## Deliberate workspace exceptions

| Lint | Reason |
| --- | --- |
| `suboptimal_flops`, `imprecise_flops` | FMA, power and angle rewrites can alter the frozen numerical results. |
| `future_not_send` | GPUI foreground futures contain entities and windows; the background executor enforces `Send` separately. |
| `needless_pass_by_ref_mut` | Framework callbacks use consistent mutable window/context signatures. |
| `missing_const_for_fn` | A `const` public API is a design commitment, not an automatic cleanup. |
| `must_use_candidate` | Annotating every getter adds noise; `return_self_not_must_use` still protects consuming builders. |
| `redundant_pub_crate` | Explicit crate visibility agrees with `unreachable_pub`; the nursery rule recommends the opposite inside private modules. |
| `option_if_let_else` | Explicit branches keep fallbacks and GPUI context borrowing readable. |

The entire restriction group is intentionally not enabled: several of its
rules express mutually exclusive policies.

## Tests

`clippy.toml` permits `unwrap`, `expect` and `panic` in test code so fixture
setup and assertions can fail directly. Integration-test roots declare
`#![cfg(test)]`, which also identifies their shared helpers as test code.
All other lint groups still apply, and examples follow production rules.

Shared integration-test helper modules have a reasoned inner
`allow(dead_code)`: different test crates use different subsets, so an
`expect` would be stale in a target that uses them all. This is not a
general test exemption.

After lint cleanup, run `scripts/check.sh`. Its real-file and parity steps
run when the private fixtures and frozen baseline are available. Report
which steps actually ran, and keep source telemetry and video immutable.
