#include "GaugeDetectorArtifact.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <stdexcept>
#include <algorithm>
#include <utility>

namespace omatrack {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void checkCancel(const GaugeDetectorArtifact::Cancel& cancel) {
    require(!cancel || !cancel->load(), "Detector load cancelled");
}
QByteArray readBounded(const QString& path, qint64 maximum,
                       const GaugeDetectorArtifact::Cancel& cancel) {
    checkCancel(cancel);
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() > 0 &&
                file.size() <= maximum,
            "Missing, empty or oversized detector companion");
    auto bytes = file.read(maximum + 1);
    require(bytes.size() == file.size() && bytes.size() <= maximum,
            "Detector companion changed while reading");
    checkCancel(cancel);
    return bytes;
}
QString hash(const QByteArray& bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
void verifyModel(const QString& path, const GaugeDetectorCandidate& candidate,
                 const GaugeDetectorArtifact::Cancel& cancel,
                 GaugeDetectorArtifact& receipt) {
    checkCancel(cancel);
    QFile file(path);
    require(
        file.open(QIODevice::ReadOnly) && file.size() == candidate.modelBytes,
        "Detector model size does not match registered candidate");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    std::array<char, GaugeDetectorArtifact::HashChunkBytes> buffer;
    qint64 total = 0;
    while (total < candidate.modelBytes) {
        checkCancel(cancel);
        const auto count = file.read(
            buffer.data(),
            std::min<qint64>(buffer.size(), candidate.modelBytes - total));
        require(count > 0, "Detector model truncated while hashing");
        receipt.peakHashBufferBytes =
            std::max(receipt.peakHashBufferBytes, count);
        receipt.hashedBytes += count;
        digest.addData(QByteArrayView(buffer.data(), count));
        total += count;
    }
    checkCancel(cancel);
    require(file.size() == candidate.modelBytes && file.atEnd(),
            "Detector model changed while hashing");
    require(QString::fromLatin1(digest.result().toHex()) ==
                QLatin1String(candidate.modelSha256),
            "Detector model SHA256 does not match registered candidate");
}
}  // namespace
const std::array<GaugeDetectorCandidate, 2>& GaugeDetectorArtifact::registry() {
    static constexpr std::array<GaugeDetectorCandidate, 2> candidates{
        {{"tiny-v2",
          "99bdd483402444e8c799452d9885c1184980a41f2cd80d4af4d7a6b3ae00abc6",
          966419,
          "44af06b2619f3a8e084eec836245cc70038bc8cce3cbe80fddd6b3d397d6e970",
          "87a4cec9e0466250b635e561c74be81f2ce5f15fbd79804a1be639c3719a69d3",
          "pilot-v2-mil-strong4-backgrounds", false},
         {"aim-large-v1",
          "92bbc4a61395c3fea309986986cbd41ef78d1ab735ecaa755a7e57c0c1fcb8c1",
          112025875,
          "92b755bbb91b7991f564369c57b9545207bfca8f3f083d1b68d30c6abd2d99d4",
          "bfd0df6a83f714e00a5b92e441bf7c45ae777899a6976eee2353f728c7080645",
          "convnext-tiny-large-v1-strong28-partial-mil", true}}};
    return candidates;
}
GaugeDetectorArtifact GaugeDetectorArtifact::load(
    const QString& path, Cancel cancel, bool reviewedAim,
    const QString& expectedCandidate) {
    GaugeDetectorArtifact result;
    try {
        const QFileInfo info(path);
        const auto metadataBytes = readBounded(
            info.dir().filePath("metadata.json"), 64 * 1024, cancel);
        const auto metadata = QJsonDocument::fromJson(metadataBytes).object();
        const auto metadataHash = hash(metadataBytes);
        for (const auto& candidate : registry())
            if (metadataHash == QLatin1String(candidate.metadataSha256))
                result.candidate = &candidate;
        require(result.candidate,
                "Detector metadata is not an approved candidate");
        const auto& candidate = *result.candidate;
        require(expectedCandidate.isEmpty() ||
                    expectedCandidate == QLatin1String(candidate.id),
                "Detector path contains the wrong registered candidate");
        if (candidate.aimOnly && !reviewedAim) {
            result.policyBlocked = true;
            throw std::runtime_error(
                "Large detector requires current image-verified orange AiM "
                "layout");
        }
        const auto contractBytes = readBounded(
            info.dir().filePath("contract.json"), 32 * 1024, cancel);
        const auto contract = QJsonDocument::fromJson(contractBytes).object();
        require(metadata.value("schema").toString() ==
                        inference::GaugeDetectorContract &&
                    contract.value("schema").toString() ==
                        inference::GaugeDetectorContract,
                "Unsupported detector tensor contract");
        require(
            hash(contractBytes) == QLatin1String(candidate.contractSha256) &&
                metadata.value("contract_sha256").toString() ==
                    QLatin1String(candidate.contractSha256) &&
                metadata.value("contract_file").toString() == "contract.json" &&
                contract.value("training_scope_version").toString() ==
                    QLatin1String(candidate.trainingScope),
            "Detector contract/scope does not match registered candidate");
        for (const auto* key : {"input", "outputs", "decode", "limitations"})
            require(metadata.value(key) == contract.value(key),
                    "Detector metadata disagrees with contract");
        require(metadata.value("status").toString() ==
                    "experimental_candidate_not_independently_accepted",
                "Unexpected detector export status");
        require(metadata.value("model_sha256").toString() ==
                        QLatin1String(candidate.modelSha256) &&
                    metadata.value("model_bytes").toInteger() ==
                        candidate.modelBytes &&
                    metadata.value("model_file").toString() == info.fileName(),
                "Detector model metadata does not match registered candidate");
        verifyModel(path, candidate, cancel, result);
        result.detector =
            std::make_unique<inference::GaugeDetector>(path.toStdString());
        checkCancel(cancel);
        require(result.detector->ready(),
                result.detector->modelError().c_str());
        verifyModel(path, candidate, cancel, result);
        require(readBounded(info.dir().filePath("metadata.json"), 64 * 1024,
                            cancel) == metadataBytes &&
                    readBounded(info.dir().filePath("contract.json"), 32 * 1024,
                                cancel) == contractBytes,
                "Detector companions changed during load");
        result.identity = QStringLiteral("experimental-route-v1:") +
                          candidate.id + ":" + candidate.modelSha256 + ":" +
                          candidate.metadataSha256 + ":" +
                          candidate.contractSha256;
    } catch (const std::exception& error) {
        result.detector.reset();
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

GaugeDetectorRouter::GaugeDetectorRouter(QString selection,
                                         QString modelDirectory)
    : selection_(std::move(selection)),
      modelDirectory_(std::move(modelDirectory)) {}
GaugeDetectorDiscovery GaugeDetectorRouter::detect(
    const inference::GaugeRgb24Frame& frame,
    GaugeDetectorArtifact::Cancel cancel) {
    GaugeDetectorDiscovery output;
    const auto layout = inference::GaugeReader::inspectLayout(frame);
    output.reviewedAim =
        layout.admission == inference::GaugeAdmission::Supported;
    output.identity = QStringLiteral("orange-structure-heuristic-v1");
    output.notice =
        QStringLiteral("Heuristic fallback (fixed reviewed layout)");
    if (layout.error != inference::GaugeError::None) {
        output.result.error = layout.error;
        output.result.detail = layout.detail;
        return output;
    }
    if (selection_ == "heuristic") return output;
    auto ensure = [&](GaugeDetectorArtifact& artifact, bool& checked,
                      const QString& path, const QString& expected) {
        if (checked && !(artifact.policyBlocked && output.reviewedAim)) return;
        QElapsedTimer timer;
        timer.start();
        artifact = GaugeDetectorArtifact::load(path, cancel, output.reviewedAim,
                                               expected);
        output.loadMs += timer.nsecsElapsed() / 1e6;
        // A seek/source cancellation is not a permanently broken model.
        // Retry initialization on the next admitted frame with a fresh token.
        checked = !cancel || !cancel->load();
    };
    auto run = [&](GaugeDetectorArtifact& artifact) {
        if (cancel && cancel->load()) {
            output.cancelled = true;
            return false;
        }
        if (!artifact.detector) return false;
        // Recheck for EVERY frame, even when the large session is already warm.
        if (artifact.candidate->aimOnly && !output.reviewedAim) return false;
        if (artifact.candidate->aimOnly)
            ++largeRuns_;
        else
            ++smallRuns_;
        output.result = artifact.detector->detect(frame);
        if (cancel && cancel->load()) {
            output.cancelled = true;
            return false;
        }
        if (output.result.error != inference::GaugeError::None) return false;
        output.identity = artifact.identity;
        output.candidateId = QString::fromLatin1(artifact.candidate->id);
        output.notice =
            artifact.candidate->aimOnly
                ? QStringLiteral(
                      "EXPERIMENTAL AiM detector (large) · current image "
                      "verified; proposals are not reader approval")
                : QStringLiteral(
                      "EXPERIMENTAL general detector (tiny V2) · proposals are "
                      "not reader approval");
        return true;
    };
    QString reason;
    if (!selection_.isEmpty() && selection_ != "small") {
        ensure(local_, localChecked_, selection_, {});
        if (run(local_)) return output;
        reason = local_.error.isEmpty()
                     ? QStringLiteral("Local detector failed")
                     : local_.error;
    } else if (selection_.isEmpty() && output.reviewedAim) {
        ensure(large_, largeChecked_,
               QDir(modelDirectory_)
                   .filePath("detectors/aim-large-v1/gauge-detector.onnx"),
               "aim-large-v1");
        if (run(large_)) return output;
        reason = large_.error.isEmpty() ? QStringLiteral("AiM detector failed")
                                        : large_.error;
    }
    if (output.cancelled || (cancel && cancel->load())) {
        output.cancelled = true;
        return output;
    }
    ensure(
        tiny_, tinyChecked_,
        QDir(modelDirectory_).filePath("detectors/tiny-v2/gauge-detector.onnx"),
        "tiny-v2");
    if (run(tiny_)) {
        if (!reason.isEmpty())
            output.notice += QStringLiteral(" · small fallback: ") + reason;
        return output;
    }
    output.notice =
        QStringLiteral("Detector unavailable/failed · heuristic fallback");
    if (!tiny_.error.isEmpty())
        output.notice += QStringLiteral(" · ") + tiny_.error;
    output.identity = QStringLiteral("orange-structure-heuristic-v1");
    output.result.detections.clear();
    return output;
}
}  // namespace omatrack
