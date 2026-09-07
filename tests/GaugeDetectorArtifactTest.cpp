#include "app/GaugeDetectorArtifact.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir tmp;
        require(tmp.isValid(), "temporary directory unavailable");
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        auto missing = omatrack::GaugeDetectorArtifact::load(
            tmp.filePath("none.onnx"), cancel);
        require(!missing.detector && !missing.error.isEmpty(),
                "missing detector admitted");
        auto fallback = omatrack::GaugeDetectorArtifact::discover(
            {}, tmp.filePath("none.onnx"), cancel);
        require(!fallback.detector && fallback.error.isEmpty(),
                "absent staged detector must silently use heuristic");
        auto explicitMissing = omatrack::GaugeDetectorArtifact::discover(
            tmp.filePath("selected.onnx"), tmp.filePath("none.onnx"), cancel);
        require(!explicitMissing.detector && !explicitMissing.error.isEmpty(),
                "missing explicit detector needs a rejection notice");
        if (argc == 2) {
            const QString model = QString::fromLocal8Bit(argv[1]);
            const auto dir = QFileInfo(model).dir();
            for (const auto* file :
                 {"gauge-detector.onnx", "metadata.json", "contract.json"})
                require(QFile::copy(dir.filePath(file), tmp.filePath(file)),
                        "artifact copy failed");
            const auto copied = tmp.filePath("gauge-detector.onnx");
            auto valid = omatrack::GaugeDetectorArtifact::load(copied, cancel);
            require(bool(valid.detector) && valid.error.isEmpty() &&
                        !valid.identity.isEmpty(),
                    "valid candidate metadata failed");
            valid.detector.reset();
            auto staged =
                omatrack::GaugeDetectorArtifact::discover({}, copied, cancel);
            require(bool(staged.detector) && staged.error.isEmpty(),
                    "pinned staged V2 was not selected automatically");
            staged.detector.reset();
            auto heuristic = omatrack::GaugeDetectorArtifact::discover(
                "heuristic", copied, cancel);
            require(!heuristic.detector && heuristic.error.isEmpty(),
                    "explicit heuristic loaded staged detector");
            cancel->store(true);
            auto cancelled =
                omatrack::GaugeDetectorArtifact::load(copied, cancel);
            require(!cancelled.detector && !cancelled.error.isEmpty(),
                    "cancel ignored");
            cancel->store(false);
            require(QFile::setPermissions(copied,
                                          QFile::ReadOwner | QFile::WriteOwner),
                    "temporary copied model permissions");
            QFile changed(copied);
            require(changed.open(QIODevice::Append) && changed.write("x") == 1,
                    "tamper failed");
            changed.close();
            auto corrupt =
                omatrack::GaugeDetectorArtifact::load(copied, cancel);
            require(!corrupt.detector && !corrupt.error.isEmpty(),
                    "model tamper accepted");
            auto corruptStaged =
                omatrack::GaugeDetectorArtifact::discover({}, copied, cancel);
            require(!corruptStaged.detector && !corruptStaged.error.isEmpty(),
                    "tampered staged model must fall back with a notice");
        }
        std::cout << "Gauge detector artifact validation PASS\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
