#include "GaugeBundleCheck.h"
#include "GaugeDetectorArtifact.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace omatrack {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool inside(const QString& path, const QString& directory) {
    const auto root = QFileInfo(directory).canonicalFilePath();
    if (path.isEmpty() || root.isEmpty()) return false;
#ifdef Q_OS_WIN
    return path.startsWith(root + '/', Qt::CaseInsensitive);
#else
    return path.startsWith(root + '/');
#endif
}
QString hashFile(const QString& path, qint64 expectedSize) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() == expectedSize,
            "Bundle asset missing or size mismatch");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, GaugeDetectorArtifact::HashChunkBytes> buffer;
    qint64 total = 0;
    while (total < expectedSize) {
        const auto count =
            file.read(buffer.data(),
                      std::min<qint64>(buffer.size(), expectedSize - total));
        require(count > 0, "Bundle asset truncated while hashing");
        hash.addData(QByteArrayView(buffer.data(), count));
        total += count;
    }
    require(file.size() == expectedSize && file.atEnd(),
            "Bundle asset changed while hashing");
    return QString::fromLatin1(hash.result().toHex());
}
struct SyntheticFrame {
    std::vector<std::uint8_t> pixels =
        std::vector<std::uint8_t>(1920 * 1080 * 3);
    inference::GaugeRgb24Frame view() const {
        return {pixels.data(), pixels.size(), 1920, 1080, 1920 * 3};
    }
    void paint(int x0, int y0, int x1, int y1,
               std::array<std::uint8_t, 3> color) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                std::copy(color.begin(), color.end(),
                          pixels.begin() + (y * 1920 + x) * 3);
    }
    void profile() {
        // Public synthetic structure fixture, not footage or an accuracy test.
        paint(968, 642, 984, 880, {50, 0, 0});
        paint(1024, 642, 1040, 880, {0, 200, 0});
        paint(1415, 855, 1875, 877, {200, 80, 0});
        paint(1410, 988, 1875, 1006, {200, 80, 0});
        paint(1440, 956, 1460, 981, {255, 255, 255});
    }
};
}  // namespace
int checkGaugeBundle(bool requireBundledRuntime) {
    using namespace inference;
    try {
        const auto application =
            QFileInfo(QCoreApplication::applicationFilePath())
                .canonicalFilePath();
        const auto appDirectory = QFileInfo(application).absolutePath();
        const auto runtime =
            QFileInfo(QString::fromStdString(GaugeReader::runtimeLibraryPath()))
                .canonicalFilePath();
        std::cout << "Omatrack model bundle self-check: synthetic runtime "
                     "integrity, NOT accuracy\n"
                  << "executable=" << application.toStdString() << '\n'
                  << "ort_version=" << GaugeReader::runtimeVersion() << '\n'
                  << "ort_module=" << runtime.toStdString() << '\n';
        require(GaugeReader::runtimeAvailable() && !runtime.isEmpty() &&
                    QFileInfo(runtime).fileName().contains("onnxruntime",
                                                           Qt::CaseInsensitive),
                "Compatible loaded ONNX Runtime module required (not an "
                "executable import thunk)");
        if (requireBundledRuntime) {
#ifdef Q_OS_WIN
            const bool bundled = inside(runtime, appDirectory);
#elif defined(Q_OS_MACOS)
            const bool bundled =
                inside(runtime, QDir(appDirectory).filePath("../Frameworks"));
#else
            const bool bundled =
                inside(runtime, QDir(appDirectory).filePath("../lib")) ||
                inside(runtime, QDir(appDirectory).filePath("../lib64"));
#endif
            require(bundled,
                    "Loaded ONNX Runtime is outside the package; external "
                    "SDK/system runtime cannot satisfy strict check");
        }
        QFile embedded(QStringLiteral(":/omatrack-models/bundle.json"));
        require(
            embedded.open(QIODevice::ReadOnly) && embedded.size() <= 64 * 1024,
            "Embedded app-owned model manifest unavailable");
        const auto bytes = embedded.readAll();
        const auto manifest = QJsonDocument::fromJson(bytes).object();
        require(manifest.value("schema").toString() ==
                    "omatrack-offline-model-bundle-v1",
                "Unknown embedded bundle schema");
        const auto root = QFileInfo(QDir(appDirectory).filePath("models"))
                              .canonicalFilePath();
        require(inside(root, appDirectory),
                "Model directory is missing or escapes the executable tree");
        require(hashFile(QDir(root).filePath("bundle.json"), bytes.size()) ==
                    QString::fromLatin1(QCryptographicHash::hash(
                                            bytes, QCryptographicHash::Sha256)
                                            .toHex()),
                "Package model manifest differs from embedded application "
                "manifest");
        const auto files = manifest.value("files").toArray();
        require(files.size() == 15,
                "Bundle must contain the 15 approved assets");
        const QRegularExpression safePath(QStringLiteral("^[A-Za-z0-9_./-]+$"));
        for (const auto& value : files) {
            const auto file = value.toObject();
            const auto relative = file.value("path").toString();
            require(safePath.match(relative).hasMatch() &&
                        !relative.startsWith('/') && !relative.contains(".."),
                    "Unsafe embedded bundle path");
            const auto path =
                QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
            require(inside(path, root),
                    "Model asset missing or symlink escapes bundle");
            require(hashFile(path, file.value("bytes").toInteger()) ==
                        file.value("sha256").toString(),
                    "Bundle asset SHA256 mismatch");
            std::cout << "verified=" << relative.toStdString() << '\n';
        }
        SyntheticFrame frame;
        frame.profile();
        const bool reviewed =
            GaugeReader::inspectLayout(frame.view()).admission ==
            GaugeAdmission::Supported;
        require(reviewed, "Synthetic structural smoke fixture rejected");
        {
            GaugeReader reader(
                QDir(root).filePath("gauge-reader.onnx").toStdString());
            require(reader.ready(), reader.modelError().c_str());
            const auto result = reader.read(frame.view());
            require(result.error == GaugeError::None &&
                        result.admission == GaugeAdmission::Supported,
                    "Reader tensor/finite inference smoke failed");
            for (const auto* field :
                 {&result.gear, &result.stintLap, &result.brakeFillPct,
                  &result.throttleFillPct})
                require(!field->value || std::isfinite(*field->value),
                        "Reader returned nonfinite value");
            std::cout << "inference=gauge-reader PASS\n";
        }
        for (const auto& candidate : GaugeDetectorArtifact::registry()) {
            const auto path =
                QDir(root).filePath(QStringLiteral("detectors/") +
                                    candidate.id + "/gauge-detector.onnx");
            auto artifact =
                GaugeDetectorArtifact::load(path, {}, reviewed, candidate.id);
            require(bool(artifact.detector), qPrintable(artifact.error));
            const auto result = artifact.detector->detect(frame.view());
            require(result.error == GaugeError::None, result.detail.c_str());
            std::cout << "inference=" << candidate.id
                      << " PASS ms=" << result.latencyMs
                      << " peak_hash_buffer=" << artifact.peakHashBufferBytes
                      << '\n';
        }
        std::cout
            << "MODEL BUNDLE SELF-CHECK PASS: 15 assets, 3 native models, "
            << (requireBundledRuntime ? "packaged" : "reported")
            << " runtime; no GUI/settings/writes/network\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MODEL BUNDLE SELF-CHECK FAIL: " << error.what() << '\n';
        return 1;
    }
}
}  // namespace omatrack
