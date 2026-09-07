#pragma once
namespace omatrack {
// Production read-only CLI diagnostic, before any GUI/store/acceptance harness.
// QCoreApplication must exist. No settings, source media, writes or network.
int checkGaugeBundle(bool requireBundledRuntime);
}  // namespace omatrack
