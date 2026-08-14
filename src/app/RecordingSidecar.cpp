#include "RecordingSidecar.h"
#include "VerboseLog.h"

#include <QDir>
#include <QFileInfo>

namespace omatrack {
namespace {

bool isTelemetryFile(const QString& path) {
    return QFileInfo(path).suffix().compare(QStringLiteral("telemetry"),
                                            Qt::CaseInsensitive) == 0;
}

}  // namespace

QString nativeCompanionPath(const QString& sourcePath) {
    const QFileInfo source(sourcePath);
    if (isTelemetryFile(sourcePath)) return source.absoluteFilePath();
    return source.dir().filePath(QLatin1Char('.') + source.fileName() +
                                 QStringLiteral(".telemetry"));
}

QString nativeCompanionRelativePath(const QString& sourceRelativePath) {
    if (sourceRelativePath.isEmpty()) return {};
    const QFileInfo source(sourceRelativePath);
    if (isTelemetryFile(sourceRelativePath)) return sourceRelativePath;
    const QString fileName =
        QLatin1Char('.') + source.fileName() + QStringLiteral(".telemetry");
    const QString directory = source.path();
    return directory == QStringLiteral(".")
               ? fileName
               : QDir(directory).filePath(fileName);
}

std::optional<RecordingSidecar> readRecordingSidecar(const QString& videoPath,
                                                     const QString& cacheRoot) {
    Q_UNUSED(cacheRoot);
    const QString companion = nativeCompanionPath(videoPath);
    if (isTelemetryFile(videoPath)) {
        RecordingSidecar result;
        result.path = companion;
        result.videoPath = QFileInfo(videoPath).absoluteFilePath();
        result.telemetryPath = companion;
        result.supported = QFileInfo(companion).isFile();
        return result;
    }
    if (!QFileInfo(companion).isFile()) {
        qCInfo(lcIo).noquote() << "sidecar miss" << displayPath(videoPath);
        return std::nullopt;
    }
    RecordingSidecar result;
    result.path = companion;
    result.videoPath = QFileInfo(videoPath).absoluteFilePath();
    result.telemetryPath = QFileInfo(companion).absoluteFilePath();
    result.supported = QFileInfo(companion).size() > 0;
    qCInfo(lcIo).noquote() << "sidecar read" << displayPath(companion)
                           << (result.supported ? "telemetry" : "empty");
    return result;
}

}  // namespace omatrack
