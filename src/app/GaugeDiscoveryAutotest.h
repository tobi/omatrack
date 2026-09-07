#pragma once
class QQmlApplicationEngine;
class TelemetryStore;
namespace omatrack::autotest {
bool installGaugeDiscovery(QQmlApplicationEngine& engine,
                           TelemetryStore& store);
}
