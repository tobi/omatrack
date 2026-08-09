---
name: qt-docs
description: >-
  Search and read current official Qt, Qt Creator, and PySide6 documentation.
  Use for Qt API questions, especially exact signals, slots, properties,
  defaults, since-versions, CMake command signatures, Qt 6.7+ APIs, and
  lesser-known modules. Also use whenever the user asks to check, verify, or
  cite official Qt documentation.
---

# Qt documentation lookup

Use the bundled client to query Qt's official documentation MCP service. This
keeps answers tied to current, versioned documentation instead of relying on
model memory.

## Search

Run from the repository root:

```sh
python3 .agents/skills/qt-docs/scripts/qt_docs.py search \
  --query 'QQuickFramebufferObject' \
  --version 6.11.0 \
  --max-results 3
```

Useful optional filters are `--module`, `--product`, `--filter`, and
`--intent`. Use `--keywords` with multiple terms when any-term matching is
more useful than a phrase query. Run `--help` for their accepted values.

Search before claiming that a signal, slot, property, default, or since-version
exists. Prefer an explicit `--version` when the project pins a Qt version.

## Read a result

Search results name the file for a complete page. Pass that filename exactly:

```sh
python3 .agents/skills/qt-docs/scripts/qt_docs.py read \
  qquickframebufferobject.html \
  --version 6.11.0
```

Do not invent filenames. Search first, then read the relevant page when the
snippet does not establish the answer precisely.

## Answering

Distinguish verified documentation from project-specific judgment. Mention the
Qt version when behavior or availability is version-sensitive. Link the
corresponding official `doc.qt.io` page when the result provides or clearly
identifies it.

If the service is unavailable, fall back to `https://doc.qt.io/` and state
that the MCP lookup could not be completed.
