// origin: PUBLIC — acceptance contract; no recording/model content.
#pragma once
class QQmlApplicationEngine;
class TelemetryStore;
namespace omatrack::autotest {
// OMATRACK_AUTOTEST_IMAGE_SCAN = supported (or 1), native, blank,
// metadata-veto. Reuses OMATRACK_VIDEO, OMATRACK_AUTOTEST and
// OMATRACK_AUTOTEST_IMAGE_BLANK. Requires scratch XDG config/cache and a real
// reviewed model configuration.
bool installImageTelemetryScan(QQmlApplicationEngine& engine,
                               TelemetryStore& store);
}  // namespace omatrack::autotest
