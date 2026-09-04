# Which checks catch which Qt bugs?

## Recommended layers

| Tool/check | Useful for | Important limit |
| --- | --- | --- |
| **Clazy** | Qt-aware C++ diagnostics: signal/slot mistakes, connection lifetimes, QObject/meta-object conventions, event-filter misuse, implicit-sharing copies. | Uses the active compilation configuration. It cannot inspect Windows-only code when invoked with a Linux compile database. It does not prove arbitrary QObject thread affinity. |
| **clang-tidy** | General C++ bug patterns, static-analyzer paths, suspicious lifetimes and performance mistakes. Already available through `OMATRACK_CLANG_TIDY=ON` / `cpp_tidy`. | Same preprocessor/configuration limitation; the current project configuration is opt-in because existing findings need triage. |
| **Qt 6 qmllint** | QML type/role/property errors, unqualified access, invalid bindings and compiler diagnostics. Already a CMake/CTest gate. | Does not know that an overlay was supposed to persist across restarts, or what a correct filmstrip should look like. |
| **Platform builds** | Missing members, wrong inheritance, platform APIs, ABI and linking errors in each target configuration. CI already builds Linux, Windows and macOS. | Must actually run the relevant configuration and gate releases on its result. A Linux build is not Windows verification. |
| **Unit/native acceptance tests** | Parent-owned lifetime, thread-dependent behavior, focus preservation, viewport geometry, restart persistence, real GL/video behavior. | Need assertions for the product contract and real recordings for integration evidence. |
| **clang-format / qmlformat** | Formatting consistency. Already gates. | Not semantic linters. |

The vendored Qt review scripts are useful heuristic pre-filters, not substitutes
for compiler-backed analysis. The C++ script produced **no findings** for the
v1.8.0 USB event-filter bug.

## Concrete example: v1.8.0 Windows failure

`UsbDeviceChangeFilter` inherited `QAbstractNativeEventFilter` and called
`setParent()`. That interface is not a QObject. Windows CI and the Windows
release build rejected the call, while Linux's compiler and analyzers excluded
the entire class under `Q_OS_WIN`.

The fix gives it a QObject base and keeps the common ownership contract compiled
on all platforms. Unit tests check parent ownership/deletion everywhere; Windows
also tests actual device-arrival/removal payloads. Only the native Windows
message decoding remains conditional.

There already was a Windows CI job. The missing process safeguard was checking
that **the exact main commit had green CI before tagging it**. Use:

```sh
# First commit and push main. Wait for its CI workflow to pass all platforms.
scripts/tag-release.sh 1.8.1 --check
scripts/tag-release.sh 1.8.1
```

The script refuses dirty trees, a version mismatch, a commit other than remote
main, existing tags, or missing/pending/failed CI. It never force-pushes or
replaces a tag. Release asset publication still has its own build/test/checksum
gates.

## Adding Clazy

Clazy is the appropriate additional Qt-specific analyzer. It is **not currently
installed or enabled as a gate** on this workstation; do not claim that it ran.
Start with level 0 and a reviewed subset of level 1, then expand after triaging
findings. The official documentation recommends disabling PCH and ccache for an
analysis build. Use a dedicated compilation database for each target rather
than trying to fake Windows by defining `Q_OS_WIN` on Linux.

Example, once a matching Clang/Clazy toolchain is installed:

```sh
clazy-standalone -checks=level0,connect-3arg-lambda,install-event-filter,qproperty-without-notify \
  -p build-clang src/app/UsbMedia.cpp
```

Qt Creator can run both Clazy and clang-tidy interactively. For ownership or
thread-affinity behavior which static analysis cannot establish, retain the
native tests and runtime Qt warnings rather than treating a clean linter as
proof.

Sources:

- [Clazy documentation and supported checks](https://github.com/KDE/clazy)
- [Clazy event-filter check](https://github.com/KDE/clazy/blob/master/docs/checks/README-install-event-filter.md)
- [Clazy thread-with-slots check](https://github.com/KDE/clazy/blob/master/docs/checks/README-thread-with-slots.md)
- [Qt Creator: Clang-Tidy and Clazy](https://doc.qt.io/qtcreator/creator-clang-tools.html)
