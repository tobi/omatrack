#pragma once

#include <QJsonObject>
#include <QString>

#include <optional>

namespace omatrack {

struct RecordingSidecar {
    QString path;
    QString videoPath;
    QString telemetryPath;
    QString sourceEtag;
    QJsonObject session;
    bool supported = false;
};

/// Hidden metadata next to `videoPath`: `foo.mp4` -> `.foo.mp4.json`.
QString recordingSidecarPath(const QString& videoPath);

/// Hidden MoTeC companion next to `videoPath`: `foo.mp4` -> `.foo.mp4.ld`.
QString recordingTelemetryPath(const QString& videoPath);

/// Reads the hidden recording sidecar before either media or telemetry is
/// opened. Relative links are resolved beside the sidecar. When `cacheRoot` is
/// set, linked files must stay inside that remote connection's cache. A video
/// with no JSON but a hidden `.ld` still returns a sidecar that points at it.
std::optional<RecordingSidecar> readRecordingSidecar(
    const QString& videoPath, const QString& cacheRoot = {});

/// Server-relative form used by the remote cache and uploader.
QString recordingSidecarRelativePath(const QString& videoRelativePath);

/// Server-relative hidden MoTeC companion: `event/foo.mp4` ->
/// `event/.foo.mp4.ld`.
QString recordingTelemetryRelativePath(const QString& videoRelativePath);

}  // namespace omatrack
