#pragma once
class QQmlApplicationEngine;
class TelemetryStore;
namespace omatrack::autotest {
// Returns true when OMATRACK_AUTOTEST is set and the harness has been armed.
bool install(QQmlApplicationEngine& engine, TelemetryStore& store);
// Focused same-session/fullscreen filmstrip check; false unless requested.
bool installFilmstrip(QQmlApplicationEngine& engine, TelemetryStore& store);
// Synthetic-mount USB preview/copy check; false unless requested.
bool installUsbCopy(QQmlApplicationEngine& engine, TelemetryStore& store);
// Focused corner buttons/shortcuts and interruptible viewport transitions.
bool installCornerNavigation(QQmlApplicationEngine& engine,
                             TelemetryStore& store);
// Session header, fixed filmstrip bookends, and persistent fullscreen HUD.
bool installSessionChrome(QQmlApplicationEngine& engine, TelemetryStore& store);
// Trace-height draft, drag, cancellation and persistence checks.
bool installTraceResize(QQmlApplicationEngine& engine, TelemetryStore& store);
// Native frame-paced zoom, stroke/fill settings and screenshots.
bool installTraceRendering(QQmlApplicationEngine& engine,
                           TelemetryStore& store);
// Lua plugin round trip on a copied GPS recording; false unless requested.
bool installPlugin(QQmlApplicationEngine& engine, TelemetryStore& store);
}  // namespace omatrack::autotest
