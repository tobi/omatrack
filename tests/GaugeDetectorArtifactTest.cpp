#include "app/GaugeDetectorArtifact.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <algorithm>

using namespace omatrack;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Frame {
    std::vector<std::uint8_t> bytes =
        std::vector<std::uint8_t>(1920 * 1080 * 3);
    inference::GaugeRgb24Frame view() const {
        return {bytes.data(), bytes.size(), 1920, 1080, 1920 * 3};
    }
    void paint(int x0, int y0, int x1, int y1,
               std::array<std::uint8_t, 3> color) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                std::copy(color.begin(), color.end(),
                          bytes.begin() + (y * 1920 + x) * 3);
    }
    void profile() {
        paint(968, 642, 984, 880, {50, 0, 0});
        paint(1024, 642, 1040, 880, {0, 200, 0});
        paint(1415, 855, 1875, 877, {200, 80, 0});
        paint(1410, 988, 1875, 1006, {200, 80, 0});
        paint(1440, 956, 1460, 981, {255, 255, 255});
    }
};
void copyCompanions(const QString& model, const QString& destination,
                    bool copyModel) {
    require(QDir().mkpath(destination), "create copy directory");
    const auto dir = QFileInfo(model).dir();
    for (const auto* name :
         {"metadata.json", "contract.json", "gauge-detector.onnx"}) {
        if (!copyModel && QString::fromLatin1(name) == "gauge-detector.onnx")
            continue;
        require(
            QFile::copy(dir.filePath(name), QDir(destination).filePath(name)),
            "copy immutable artifact for tamper test");
    }
}
void bundleTests(const QString& root) {
    require(inference::GaugeDetector::runtimeAvailable(),
            "offline bundle smoke requires native ORT");
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    for (const auto& candidate : GaugeDetectorArtifact::registry()) {
        const auto path =
            QDir(root).filePath(QStringLiteral("detectors/") + candidate.id +
                                "/gauge-detector.onnx");
        auto valid =
            GaugeDetectorArtifact::load(path, cancel, true, candidate.id);
        require(valid.detector && valid.error.isEmpty(),
                qPrintable(valid.error));
        require(valid.hashedBytes == 2 * candidate.modelBytes &&
                    valid.peakHashBufferBytes <=
                        GaugeDetectorArtifact::HashChunkBytes,
                "model validation must stream both passes within fixed chunk "
                "bound");
        std::cout << "registered candidate " << candidate.id
                  << " bytes=" << candidate.modelBytes
                  << " hashed=" << valid.hashedBytes
                  << " peak_hash_buffer=" << valid.peakHashBufferBytes << '\n';
        valid.detector.reset();
        if (candidate.aimOnly) {
            auto gated = GaugeDetectorArtifact::load(path, cancel, false);
            require(!gated.detector && gated.policyBlocked &&
                        gated.hashedBytes == 0,
                    "large model loaded without image gate");
            auto future = std::async(std::launch::async, [&] {
                return GaugeDetectorArtifact::load(path, cancel, true);
            });
            require(future.wait_for(std::chrono::milliseconds(1)) ==
                        std::future_status::timeout,
                    "large load finished before cancellation test could start");
            cancel->store(true);
            auto stopped = future.get();
            require(!stopped.detector && stopped.error.contains("cancelled"),
                    "in-flight model hash/load cancellation ignored");
            cancel->store(false);
        }
        QTemporaryDir temporary;
        copyCompanions(path, temporary.path(), true);
        const auto copied = temporary.filePath("gauge-detector.onnx");
        require(
            QFile::setPermissions(copied, QFile::ReadOwner | QFile::WriteOwner),
            "make temporary copy writable");
        QFile file(copied);
        require(file.open(QIODevice::ReadWrite), "open copied artifact");
        require(file.resize(candidate.modelBytes + 1),
                "oversize copied artifact");
        file.close();
        auto oversized = GaugeDetectorArtifact::load(copied, cancel, true);
        require(!oversized.detector && oversized.hashedBytes == 0,
                "oversized model was read or loaded");
        require(file.open(QIODevice::ReadWrite) &&
                    file.resize(candidate.modelBytes) && file.seek(16),
                "restore tamper size");
        auto byte = file.read(1);
        require(byte.size() == 1 && file.seek(16), "read tamper byte");
        byte[0] = char(byte[0] ^ 1);
        require(file.write(byte) == 1, "tamper model");
        file.close();
        auto corrupt = GaugeDetectorArtifact::load(copied, cancel, true);
        require(!corrupt.detector && corrupt.error.contains("SHA256") &&
                    corrupt.hashedBytes == candidate.modelBytes,
                "same-size corruption bypassed streamed SHA before ORT load");
        const auto contractPath = temporary.filePath("contract.json");
        require(QFile::setPermissions(contractPath,
                                      QFile::ReadOwner | QFile::WriteOwner),
                "temporary contract permissions");
        QFile contract(contractPath);
        require(contract.open(QIODevice::Append) && contract.write("x") == 1,
                "tamper contract");
        contract.close();
        auto badContract = GaugeDetectorArtifact::load(copied, cancel, true);
        require(!badContract.detector && badContract.hashedBytes == 0 &&
                    badContract.error.contains("contract"),
                "contract tamper was not rejected before model I/O");
        const auto metadataPath = temporary.filePath("metadata.json");
        require(QFile::setPermissions(metadataPath,
                                      QFile::ReadOwner | QFile::WriteOwner),
                "temporary metadata permissions");
        QFile metadata(metadataPath);
        require(metadata.open(QIODevice::Append) && metadata.write("x") == 1,
                "tamper metadata");
        metadata.close();
        auto badMetadata = GaugeDetectorArtifact::load(copied, cancel, true);
        require(!badMetadata.detector && badMetadata.hashedBytes == 0 &&
                    badMetadata.error.contains("approved"),
                "unregistered metadata was not rejected before model I/O");
    }
    Frame blank, aim;
    aim.profile();
    require(inference::GaugeReader::inspectLayout(aim.view()).admission ==
                inference::GaugeAdmission::Supported,
            "synthetic routing gate fixture (not model accuracy)");
    {
        GaugeDetectorRouter router({}, root);
        auto general = router.detect(blank.view(), cancel);
        require(general.candidateId == "tiny-v2" && router.largeRuns() == 0 &&
                    !general.reviewedAim,
                "unknown-layout auto routing ran large instead of tiny");
        auto specialized = router.detect(aim.view(), cancel);
        require(specialized.candidateId == "aim-large-v1" &&
                    router.largeRuns() == 1 && specialized.reviewedAim,
                "image-verified AiM did not route to large");
        general = router.detect(blank.view(), cancel);
        require(general.candidateId == "tiny-v2" && router.largeRuns() == 1,
                "warm large detector ran after current frame lost AiM gate");
    }
    {
        GaugeDetectorRouter interrupted({}, root);
        auto future = std::async(std::launch::async, [&] {
            return interrupted.detect(aim.view(), cancel);
        });
        require(future.wait_for(std::chrono::milliseconds(1)) ==
                    std::future_status::timeout,
                "router cancellation test did not start");
        cancel->store(true);
        require(future.get().cancelled, "router did not report cancellation");
        cancel->store(false);
        require(interrupted.detect(aim.view(), cancel).candidateId ==
                    "aim-large-v1",
                "cancelled model initialization permanently downgraded source "
                "routing");
    }
    GaugeDetectorRouter small("small", root), heuristic("heuristic", root);
    require(small.detect(aim.view(), cancel).candidateId == "tiny-v2" &&
                small.largeRuns() == 0,
            "explicit small override ignored");
    require(heuristic.detect(aim.view(), cancel).candidateId.isEmpty(),
            "heuristic override loaded model");
    const auto largePath =
        QDir(root).filePath("detectors/aim-large-v1/gauge-detector.onnx");
    {
        GaugeDetectorRouter local(largePath, root);
        require(local.detect(blank.view(), cancel).candidateId == "tiny-v2" &&
                    local.largeRuns() == 0,
                "explicit local path bypassed large source-pixel gate");
        require(local.detect(aim.view(), cancel).candidateId == "aim-large-v1",
                "local large did not activate on later verified image");
    }
    QTemporaryDir missing;
    copyCompanions(QDir(root).filePath("detectors/tiny-v2/gauge-detector.onnx"),
                   missing.filePath("detectors/tiny-v2"), true);
    copyCompanions(largePath, missing.filePath("detectors/aim-large-v1"),
                   false);
    GaugeDetectorRouter fallback({}, missing.path());
    require(fallback.detect(aim.view(), cancel).candidateId == "tiny-v2",
            "missing large did not fall back to tiny");
    QFile bad(missing.filePath("detectors/aim-large-v1/gauge-detector.onnx"));
    require(bad.open(QIODevice::WriteOnly) && bad.write("corrupt") == 7,
            "create corrupt large");
    bad.close();
    GaugeDetectorRouter tampered({}, missing.path());
    require(tampered.detect(aim.view(), cancel).candidateId == "tiny-v2",
            "corrupt large did not fall back to tiny");
    inference::GaugeReader reader(
        QDir(root).filePath("gauge-reader.onnx").toStdString());
    require(reader.ready() && reader.read(blank.view()).admission ==
                                  inference::GaugeAdmission::Rejected,
            "offline incumbent reader failed load/blank rejection");
    std::cout
        << "Offline bundle registry/routing/tamper/cancellation/buffer-bound "
           "smoke PASS (synthetic routing, not accuracy)\n";
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir tmp;
        require(tmp.isValid(), "temporary directory unavailable");
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        auto missing =
            GaugeDetectorArtifact::load(tmp.filePath("none.onnx"), cancel);
        require(!missing.detector && !missing.error.isEmpty(),
                "missing detector admitted");
        cancel->store(true);
        auto stopped =
            GaugeDetectorArtifact::load(tmp.filePath("none.onnx"), cancel);
        require(!stopped.detector && stopped.error.contains("cancelled"),
                "pre-load cancellation ignored");
        cancel->store(false);
        Frame blank;
        GaugeDetectorRouter fallback({}, tmp.path());
        require(fallback.detect(blank.view(), cancel).candidateId.isEmpty() &&
                    fallback.largeRuns() == 0,
                "absent bundle did not use heuristic");
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--bundle")
            bundleTests(QString::fromLocal8Bit(argv[2]));
        else if (argc == 2) {
            auto real = GaugeDetectorArtifact::load(
                QString::fromLocal8Bit(argv[1]), cancel, true);
            require(real.detector && real.error.isEmpty(),
                    qPrintable(real.error));
        } else
            require(argc == 1,
                    "usage: gauge-detector-artifact-test [model.onnx | "
                    "--bundle DIR]");
        std::cout << "Gauge detector artifact validation PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
