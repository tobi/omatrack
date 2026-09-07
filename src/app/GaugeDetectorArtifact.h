#pragma once
#include <atomic>
#include <array>
#include "inference/GaugeDetector.h"
#include <QString>
#include <memory>
namespace omatrack {
struct GaugeDetectorCandidate {
    const char* id;
    const char* modelSha256;
    qint64 modelBytes;
    const char* metadataSha256;
    const char* contractSha256;
    const char* trainingScope;
    bool aimOnly;
};
struct GaugeDetectorArtifact {
    using Cancel = std::shared_ptr<std::atomic<bool>>;
    static constexpr qint64 HashChunkBytes = 256 * 1024;
    std::unique_ptr<inference::GaugeDetector> detector;
    const GaugeDetectorCandidate* candidate = nullptr;
    QString identity, error;
    qint64 hashedBytes = 0, peakHashBufferBytes = 0;
    bool policyBlocked = false;
    // This compiled registry, not writable metadata, authorizes each exact
    // model.
    static const std::array<GaugeDetectorCandidate, 2>& registry();
    // Worker-only. Large candidate cannot even construct a session without a
    // current independent image-verified AiM gate. Model hashes are streamed.
    static GaugeDetectorArtifact load(const QString& modelPath, Cancel cancel,
                                      bool reviewedAim = false,
                                      const QString& expectedCandidate = {});
};
struct GaugeDetectorDiscovery {
    inference::GaugeDetectionResult result;
    QString identity, notice, candidateId;
    bool reviewedAim = false, cancelled = false;
    double loadMs = 0;
};
// One router per source/selection, owned exclusively by the serial worker.
// Empty selection routes by current source pixels; "small" and "heuristic"
// are explicit overrides; otherwise selection is a local registered export.
class GaugeDetectorRouter {
public:
    GaugeDetectorRouter(QString selection, QString modelDirectory);
    GaugeDetectorDiscovery detect(const inference::GaugeRgb24Frame& frame,
                                  GaugeDetectorArtifact::Cancel cancel);
    int largeRuns() const { return largeRuns_; }
    int smallRuns() const { return smallRuns_; }

private:
    QString selection_, modelDirectory_;
    GaugeDetectorArtifact tiny_, large_, local_;
    bool tinyChecked_ = false, largeChecked_ = false, localChecked_ = false;
    int largeRuns_ = 0, smallRuns_ = 0;
};
}  // namespace omatrack
