#include "RecordingSidecar.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

namespace omatrack {
namespace {

QString linkedPath(const QJsonValue& value) {
    if (value.isString()) return value.toString().trimmed();
    if (!value.isObject()) return {};
    const QJsonObject object = value.toObject();
    return object.value(QStringLiteral("path")).toString().trimmed();
}

QString resolveLink(const QString& sidecarPath, const QString& link,
                    const QString& cacheRoot) {
    if (link.isEmpty()) return {};
    const QUrl url(link, QUrl::StrictMode);
    QString path;
    if (url.isLocalFile())
        path = url.toLocalFile();
    else if (!url.scheme().isEmpty())
        return {};
    else
        path = link;

    const QFileInfo linked(path);
    const QString absolute = QDir::cleanPath(
        linked.isAbsolute()
            ? linked.absoluteFilePath()
            : QFileInfo(sidecarPath).dir().absoluteFilePath(path));
    if (!cacheRoot.isEmpty()) {
        const QString root =
            QDir::cleanPath(QFileInfo(cacheRoot).absoluteFilePath());
        if (absolute != root && !absolute.startsWith(root + QDir::separator()))
            return {};
    }
    return absolute;
}

QString hiddenCompanion(const QString& videoPath, const QString& suffix) {
    const QFileInfo video(videoPath);
    return video.dir().filePath(QLatin1Char('.') + video.fileName() + suffix);
}

QString hiddenCompanionRelative(const QString& videoRelativePath,
                                const QString& suffix) {
    if (videoRelativePath.isEmpty()) return {};
    const QFileInfo video(videoRelativePath);
    const QString fileName = QLatin1Char('.') + video.fileName() + suffix;
    const QString directory = video.path();
    return directory == QStringLiteral(".")
               ? fileName
               : QDir(directory).filePath(fileName);
}

QString inferHiddenMotec(const QString& videoPath) {
    const QFileInfo video(videoPath);
    for (const QString& candidate :
         {hiddenCompanion(videoPath, QStringLiteral(".ld")),
          video.dir().filePath(video.completeBaseName() +
                               QStringLiteral(".ld"))}) {
        if (QFileInfo(candidate).isFile())
            return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

}  // namespace

QString recordingSidecarPath(const QString& videoPath) {
    return hiddenCompanion(videoPath, QStringLiteral(".json"));
}

QString recordingTelemetryPath(const QString& videoPath) {
    return hiddenCompanion(videoPath, QStringLiteral(".ld"));
}

QString recordingSidecarRelativePath(const QString& videoRelativePath) {
    return hiddenCompanionRelative(videoRelativePath, QStringLiteral(".json"));
}

QString recordingTelemetryRelativePath(const QString& videoRelativePath) {
    return hiddenCompanionRelative(videoRelativePath, QStringLiteral(".ld"));
}

std::optional<RecordingSidecar> readRecordingSidecar(const QString& videoPath,
                                                     const QString& cacheRoot) {
    const QString sidecarPath = recordingSidecarPath(videoPath);
    const bool sidecarExists = QFileInfo(sidecarPath).isFile();
    RecordingSidecar result;
    result.videoPath = QFileInfo(videoPath).absoluteFilePath();

    if (sidecarExists) {
        QFile file(sidecarPath);
        if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
        QJsonParseError error;
        const QJsonDocument document =
            QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return std::nullopt;

        const QJsonObject root = document.object();
        if (root.value(QStringLiteral("schema")).toString() !=
            QStringLiteral("omatrack.recording/1"))
            return std::nullopt;
        const QJsonObject session =
            root.value(QStringLiteral("session")).toObject();

        result.path = sidecarPath;
        result.sourceEtag = root.value(QStringLiteral("source"))
                                .toObject()
                                .value(QStringLiteral("etag"))
                                .toString();
        result.session = session;
        result.supported =
            root.contains(QStringLiteral("supported"))
                ? root.value(QStringLiteral("supported")).toBool()
                : !session.isEmpty();

        const QString explicitVideo =
            linkedPath(root.value(QStringLiteral("video")));
        result.videoPath =
            explicitVideo.isEmpty()
                ? QFileInfo(videoPath).absoluteFilePath()
                : resolveLink(sidecarPath, explicitVideo, cacheRoot);
        if (result.videoPath.isEmpty()) return std::nullopt;

        const QString explicitTelemetry =
            linkedPath(root.value(QStringLiteral("telemetry")));
        if (!explicitTelemetry.isEmpty()) {
            result.telemetryPath =
                resolveLink(sidecarPath, explicitTelemetry, cacheRoot);
        }
    }

    if (result.telemetryPath.isEmpty())
        result.telemetryPath = inferHiddenMotec(videoPath);
    if (!result.telemetryPath.isEmpty() &&
        !QFileInfo(result.telemetryPath).isFile())
        result.telemetryPath.clear();

    if (!sidecarExists) {
        if (result.telemetryPath.isEmpty()) return std::nullopt;
        result.supported = true;
    }
    return result;
}

}  // namespace omatrack
