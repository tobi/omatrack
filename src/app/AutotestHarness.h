#pragma once
class QQmlApplicationEngine;
class TelemetryStore;
namespace racecraft::autotest {
// Returns true when RACECRAFT_AUTOTEST is set and the harness has been armed.
bool install(QQmlApplicationEngine& engine, TelemetryStore& store);
}  // namespace racecraft::autotest
