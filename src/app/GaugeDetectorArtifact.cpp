#include "GaugeDetectorArtifact.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <stdexcept>

namespace omatrack {
namespace {
QByteArray readBounded(const QString& path, qint64 maximum,
                       GaugeDetectorArtifact::Cancel cancel) {
    if (cancel && cancel->load())
        throw std::runtime_error("Detector load cancelled");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
        file.size() > maximum)
        throw std::runtime_error(
            "Missing, empty or oversized detector artifact");
    auto bytes = file.read(maximum + 1);
    if (bytes.size() != file.size() || bytes.size() > maximum)
        throw std::runtime_error("Detector artifact changed while reading");
    return bytes;
}
QString hash(const QByteArray& bytes) {
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}  // namespace
GaugeDetectorArtifact GaugeDetectorArtifact::discover(const QString& selection,
                                                      const QString& stagedPath,
                                                      Cancel cancel) {
    if (selection == QStringLiteral("heuristic")) return {};
    if (!selection.isEmpty()) return load(selection, cancel);
    if (!QFileInfo::exists(stagedPath)) return {};
    auto result = load(stagedPath, cancel);
    // Only this independently reviewed experimental artifact may auto-propose.
    // A larger/new candidate requires an explicit selection and contract
    // review.
    if (result.detector && !result.identity.startsWith(QStringLiteral(
                               "experimental-center-v1:"
                               "99bdd483402444e8c799452d9885c1184980a41f2cd80d4"
                               "af4d7a6b3ae00abc6:"))) {
        result.detector.reset();
        result.error =
            QStringLiteral("Staged detector is not the pinned V2 candidate");
    }
    return result;
}
GaugeDetectorArtifact GaugeDetectorArtifact::load(const QString& path,
                                                  Cancel cancel) {
    GaugeDetectorArtifact result;
    try {
        const QFileInfo info(path);
        const auto metadataBytes = readBounded(
            info.dir().filePath("metadata.json"), 64 * 1024, cancel);
        const auto metadata = QJsonDocument::fromJson(metadataBytes).object();
        const auto contractBytes = readBounded(
            info.dir().filePath("contract.json"), 32 * 1024, cancel);
        const auto contract = QJsonDocument::fromJson(contractBytes).object();
        require(metadata.value("schema").toString() ==
                        inference::GaugeDetectorContract &&
                    contract.value("schema").toString() ==
                        inference::GaugeDetectorContract,
                "Unsupported detector contract schema");
        require(
            hash(contractBytes) == inference::GaugeDetectorContractSha256 &&
                metadata.value("contract_sha256").toString() ==
                    inference::GaugeDetectorContractSha256 &&
                metadata.value("contract_file").toString() == "contract.json",
            "Detector contract hash mismatch");
        require(contract.value("training_scope_version").toString() ==
                    inference::GaugeDetectorTrainingScope,
                "Detector training scope is not the reviewed experimental "
                "candidate");
        for (const auto* key : {"input", "outputs", "decode", "limitations"})
            require(metadata.value(key) == contract.value(key),
                    "Detector metadata disagrees with pinned contract");
        require(metadata.value("status").toString() ==
                    "experimental_candidate_not_independently_accepted",
                "Detector status is not the explicit experimental candidate "
                "contract");
        const auto bytes = readBounded(path, 8 * 1024 * 1024, cancel);
        const auto modelHash = hash(bytes);
        require(metadata.value("model_sha256").toString() == modelHash &&
                    metadata.value("model_bytes").toInteger() == bytes.size() &&
                    metadata.value("model_file").toString() == info.fileName(),
                "Detector model size/name/SHA256 mismatch");
        result.detector =
            std::make_unique<inference::GaugeDetector>(path.toStdString());
        require(result.detector->ready(),
                result.detector->modelError().c_str());
        require(hash(readBounded(path, 8 * 1024 * 1024, cancel)) == modelHash &&
                    readBounded(info.dir().filePath("metadata.json"), 64 * 1024,
                                cancel) == metadataBytes,
                "Detector artifacts changed during load");
        result.identity = QStringLiteral("experimental-center-v1:") +
                          modelHash + ":" + hash(metadataBytes);
    } catch (const std::exception& e) {
        result.detector.reset();
        result.error = QString::fromUtf8(e.what());
    }
    return result;
}
}  // namespace omatrack
