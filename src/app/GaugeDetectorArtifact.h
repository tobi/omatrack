#pragma once
#include <atomic>
#include "inference/GaugeDetector.h"
#include <QString>
#include <memory>
namespace omatrack {
struct GaugeDetectorArtifact {
    using Cancel = std::shared_ptr<std::atomic<bool>>;
    std::unique_ptr<inference::GaugeDetector> detector;
    QString identity, error;
    // Worker-only: empty selection tries the staged local candidate,
    // "heuristic" disables it, otherwise selection is a trusted local path. No
    // downloads.
    static GaugeDetectorArtifact discover(const QString& selection,
                                          const QString& stagedPath,
                                          Cancel cancel);
    // Valid metadata/hash is compatibility, not accuracy approval.
    static GaugeDetectorArtifact load(const QString& modelPath, Cancel cancel);
};
}  // namespace omatrack
