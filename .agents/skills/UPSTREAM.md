# Vendored agent skills

`qt-cmake-project/`, `qt-cpp-review/`, `qt-qml/`, `qt-qml-profiler/`,
`qt-qml-review/`, `qt-qml-test/`, `qt-qml-test-run/` are copied verbatim from
The Qt Company's official agent-skills repository.

- Upstream: https://github.com/TheQtCompanyRnD/agent-skills
- Docs: https://doc.qt.io/agentictools/
- Pinned commit: `71d6c10da78b9a764468ae11c86ab3bc4ca4921f` ("Bump plugin
  version to 1.6.1", 2026-07-09)
- License: `LicenseRef-Qt-Commercial OR BSD-3-Clause` — see
  `LICENSE.qt-agent-skills`. We use the BSD-3-Clause option; the copyright
  notice is retained in every vendored `LICENSE.txt`.

Not vendored (deliberately): `qt-figma-component-generation`,
`qt-figma-token-extraction` (no Figma workflow here), `qt-ui-design`
(conflicts with the density rules in `AGENTS.md`), `qt-qml-docs`,
`qt-cpp-docs` (this repo does not generate reference docs).

## Refreshing

```sh
git clone --depth 1 https://github.com/TheQtCompanyRnD/agent-skills /tmp/qt-agent-skills
for s in qt-qml qt-qml-review qt-qml-profiler qt-cmake-project \
         qt-cpp-review qt-qml-test qt-qml-test-run; do
  rm -rf ".agents/skills/$s"
  cp -r "/tmp/qt-agent-skills/skills/$s" .agents/skills/
done
cp /tmp/qt-agent-skills/LICENSE .agents/skills/LICENSE.qt-agent-skills
```

Then update the pinned commit above. `racecraft-qt/` is ours — never overwrite
it from upstream.
