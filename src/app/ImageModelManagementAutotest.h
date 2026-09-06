// Acceptance-only UI/preferences checks. No media or model contents embedded.
#pragma once

class QQmlApplicationEngine;
class TelemetryStore;

namespace omatrack::autotest {
// OMATRACK_AUTOTEST_IMAGE_MODEL = fresh | existing | download.
// Requires an explicitly marked scratch root and isolated HOME/XDG paths;
// see docs/IMAGE_MODEL_MANAGEMENT_ACCEPTANCE.md. Only download may use HF,
// and only after exercising the actual consent button.
bool installImageModelManagement(QQmlApplicationEngine& engine,
                                 TelemetryStore& store);
}  // namespace omatrack::autotest
