# Image-model management UI acceptance

`ImageModelManagementAutotest.h/.cpp` provides finite, acceptance-only checks of
the real preferences UI, Store bindings and `omatrack.yml` persistence. No footage,
private model, inferred sample or HTTP response is embedded in this suite.

The optional download smoke uses the public
[tobil/omatrack-telemetry-reader](https://huggingface.co/tobil/omatrack-telemetry-reader)
repository anonymously, through the application manager. It does not download a
model through a separate test client or bypass the consent action.

## Entry point and modes

Compile the implementation only when `OMATRACK_ENABLE_AUTOTEST_HARNESS` is enabled.
The bootstrap selects it before the scan and generic harnesses:

```cpp
if (!omatrack::autotest::installImageModelManagement(engine, *store) &&
    !omatrack::autotest::installImageTelemetryScan(engine, *store))
    omatrack::autotest::install(engine, *store);
```

The installer returns false when `OMATRACK_AUTOTEST_IMAGE_MODEL` is empty.
Otherwise it runs exactly one mode:

| Mode | Input preferences | Checks |
| --- | --- | --- |
| `fresh` | No image preference keys | Extraction and management default off, updates default on, model path empty; zero model transport attempts and activations while idle; actual extraction checkbox toggles and restores the default; all four image settings persist through the normal debounced writer |
| `existing` | Explicit extraction on, updates off, caller-supplied local model path; management absent/off | Explicit choices and model path survive startup and idle without model network requests; actual extraction checkbox toggles and restores the original state; persistence matches; local model bytes remain unchanged |
| `download` | Same initial defaults as `fresh`, empty scratch model directories | Zero transport attempts before consent; real enabled **Enable & download reader** button turns on management/extraction; public transfer and `modelActivated` update the actual Store path; independent SHA256 and native GaugeReader loading pass; the selected path and all image preferences persist |

The manager's C++ `networkRequestCount()` is monotonic and increments immediately
before a model transport chain starts. Zero therefore establishes **no transport
attempt**, not merely that a fast request finished before a `busy` check. It does
not count every individual redirect. The suite observes the production instance
named `imageModelManager`; it does not replace its services or inject a catalog.

Each non-download mode watches that counter throughout the run. Download mode
holds the default state for at least two seconds before sending mouse events to
the actual QML consent control. Controls must be enabled, visible and inside the
preferences window. Events are delivered directly to the controls on an isolated
display; this is not a claim about hardware-pointer delivery or desktop focus.

## Required isolation

The documented runner targets **Linux**. It requires a fresh, explicitly marked
scratch directory and verifies Qt's actual application-data/cache locations. It
fails closed if the platform does not honor the isolated locations; setting XDG
variables alone is not a portable Windows/macOS sandbox.

| Environment | Required value |
| --- | --- |
| `OMATRACK_AUTOTEST_IMAGE_MODEL` | `fresh`, `existing` or `download` |
| `OMATRACK_AUTOTEST_IMAGE_MODEL_ROOT` | Absolute, existing scratch directory without symlink aliases |
| `HOME` | `<root>/home` |
| `XDG_CONFIG_HOME` | `<root>/config` |
| `XDG_CACHE_HOME` | `<root>/cache` |
| `XDG_DATA_HOME` | `<root>/data` |
| `XDG_RUNTIME_DIR` | `<root>/runtime` |
| `OMATRACK_AUTOTEST` | A new `.png` path beneath the scratch root |
| `OMATRACK_AUTOTEST_IMAGE_MODEL_LOCAL` | Existing mode only: absolute path to a readable local model fixture; read-only |
| `OMATRACK_AUTOTEST_IMAGE_MODEL_SHA256` | Download mode: expected public model SHA256, supplied independently of the downloaded bytes |

The root must contain `.image-model-management-test` with exactly
`omatrack-image-model-acceptance-v1` and a trailing newline. All HOME/XDG child
directories must exist. Model data/cache directories must initially contain no
ONNX files, so the download smoke cannot accidentally pass using an old cached
model. The ordinary `OMATRACK_VIDEO` input must be unset, and the workspace must
have no open video/session.

**Set isolation before launching the application.** The installer runs after
normal application initialization; its guard cannot undo startup work performed
with incorrect environment variables. Do not point these flags at a developer's
normal preferences or model-install directories. Inputs and screenshots are
private test artifacts, not public attachments.

## Example runner

Use the [isolated development helper](VIDEO_BUILD_ENVIRONMENT.md) after an
acceptance build has been prepared. This example creates only scratch test state;
JSON mapping syntax is valid YAML and safely quotes paths containing spaces.

```sh
# Select one mode per fresh invocation. Existing mode also needs LOCAL_MODEL.
export MODE=fresh
# export MODE=existing LOCAL_MODEL=/path/to/local/gauge-reader.onnx
# export MODE=download

# Public v1.0.0 model identity. If the public release changes, review its manifest
# and supply that release's expected hash rather than weakening verification.
export OMATRACK_AUTOTEST_IMAGE_MODEL_SHA256=97029f70068f4ec276b3d6bc28810763275806f579d91ddd4701b544af392147

scripts/setup-video-dev.sh xvfb bash -eu -c '
  root=$(dirname "$HOME")
  export OMATRACK_AUTOTEST_IMAGE_MODEL_ROOT="$root"
  export OMATRACK_AUTOTEST_IMAGE_MODEL="$MODE"
  export OMATRACK_AUTOTEST="$root/model-management.png"
  unset OMATRACK_VIDEO OMATRACK_AUTOTEST_IMAGE_SCAN OMATRACK_AUTOTEST_IMAGE_MODEL_LOCAL
  if [ "$MODE" = existing ]; then
    export OMATRACK_AUTOTEST_IMAGE_MODEL_LOCAL="$LOCAL_MODEL"
  fi
  uv run --no-project --no-config python - <<"PY"
import json
import os
from pathlib import Path
root = Path(os.environ["OMATRACK_AUTOTEST_IMAGE_MODEL_ROOT"])
(root / ".image-model-management-test").write_text("omatrack-image-model-acceptance-v1\n")
document = {"locations": [], "updates": {"check": False}}
if os.environ["MODE"] == "existing":
    document["video"] = {
        "image_telemetry": True,
        "image_model": os.environ["OMATRACK_AUTOTEST_IMAGE_MODEL_LOCAL"],
        "image_model_updates": False,
    }
config = Path(os.environ["XDG_CONFIG_HOME"]) / "omatrack" / "omatrack.yml"
config.parent.mkdir(parents=True, exist_ok=True)
config.write_text(json.dumps(document, indent=2) + "\n")
PY
  exec timeout 200 "$1" --new-instance --mute
' -- "$PWD/build-video-dev/model-acceptance/omatrack"
```

Substitute the actual acceptance executable. The normal application build must
contain ONNX Runtime for `download`: its final verification loads the downloaded
model through `GaugeReader`, not a fake verifier. Fresh and existing preference
checks do not require inference or a video decoder. An existing-mode local model
is only read and hashed, never loaded for inference or rewritten.

The expected download hash intentionally pins the smoke result. A newer public
model can make an old expected hash fail; update it only after reviewing the
published release. A network failure is a test failure, not evidence of success.
No Hugging Face token or account is required or requested.

## Finite execution and evidence

A 50 ms GUI timer drives the phases. Normal phases have a 20-second deadline,
public downloading has a 120-second deadline and the whole run is bounded to
180 seconds. There are no nested event loops, sleeps or direct calls to private
manager work functions.

Filesystem validation, YAML reads, model hashing/loading and PNG writing run on
an `AsyncJob` worker. Persistence is checked by rereading the actual YAML file,
not by examining only in-memory Store values or forcing a synchronous save.
The suite polls that file at most four times per second while awaiting the
normal debounced writer. Existing-mode model hashes are compared before and
after the UI changes.

On success, a preferences screenshot is created exclusively beneath the scratch
root; an existing file is never overwritten. Console receipts contain mode,
aggregate transport/activation counts and elapsed time, not source paths, tokens,
model bytes or inferred readings. Exit status is nonzero on any failed assertion.

## Verified UI smoke

**All six runs passed:** two independent runs of each mode, each with fresh
isolated HOME/XDG state. The diagnostic repeat explicitly confirmed Qt Quick
**OpenGL 4.5** for both application and preferences windows under authenticated
Xvfb with Mesa llvmpipe. This is real OpenGL functional validation, not a
hardware-acceleration or high-refresh performance claim.

| Mode | Passing runs | Model transport attempts per run | Activations per run | Elapsed range |
| --- | ---: | ---: | ---: | ---: |
| Fresh defaults | 2/2 | 0 | 0 | 2,825–2,894 ms |
| Existing local model | 2/2 | 0 | 0 | 2,900–2,960 ms |
| Explicit public download | 2/2 | 3 | 1 | 3,496–3,504 ms |

Both download runs observed zero model transport attempts before the actual
consent action, then activated public version **1.0.0**. Independent hashing
matched the expected public SHA256 in the recipe, and the downloaded file loaded
through native `GaugeReader`. The supplied existing-model fixture retained its
original hash after all runs.

The actual on-disk YAML was checked during the runs and again after exit:

- Fresh: extraction off, management off, updates on, empty explicit model path.
- Existing: explicit extraction on, management off, updates off and the original
  local-model selection retained.
- Download: extraction, management and updates on, with the verified managed
  model selected.

Final screenshots for all three modes were inspected. The appended preferences
section, consent explanation, checkbox states and local-model control were
legible without overlapping controls. The downloaded state displayed version
1.0.0 and successful activation. Screenshots remain private because runtime
configuration/model paths are visible; no private artifact link is published
here.

Elapsed times include startup, the deliberate pre-consent hold, UI actions and
persistence checks; download times also include network transfer and native model
validation. They are small smoke-test observations, not throughput guarantees.

## Boundaries

This is the initial UI/preferences and real-public-download smoke, not a second
HTTP protocol test suite. Manager unit tests must separately cover cancellation,
stale completions after opting out or choosing a custom path, update discovery,
compatibility rejection, deferred activation while video is active, and explicit
`applyPending()` overriding that block. This smoke does **not** claim to automate
the native local-file picker, change a custom model during an in-flight transfer,
or exercise the active-video Apply button.

These results establish the exercised preferences and valid-public-catalog
flows. Malformed-manifest rejection and other protocol cases require their own
unit-test results; the UI smoke does not substitute for them or certify later
implementation changes. This document contains no footage, private model
contents or machine-specific receipts.
