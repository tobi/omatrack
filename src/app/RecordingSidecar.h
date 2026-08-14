#pragma once

#include <QString>

#include <optional>

namespace omatrack {

/// Hidden native recording next to any source: `foo.pds` →
/// `.foo.pds.telemetry`. A path that is already `.telemetry` is returned
/// unchanged.
QString nativeCompanionPath(const QString& sourcePath);

/// Server-relative form: `event/foo.mp4` → `event/.foo.mp4.telemetry`.
QString nativeCompanionRelativePath(const QString& sourceRelativePath);

struct RecordingSidecar {
    QString path;
    QString videoPath;
    QString telemetryPath;
    bool supported = false;
};

/// Hidden `.telemetry` beside a video, if present.
std::optional<RecordingSidecar> readRecordingSidecar(
    const QString& videoPath, const QString& cacheRoot = {});

inline QString recordingTelemetryPath(const QString& videoPath) {
    return nativeCompanionPath(videoPath);
}

inline QString recordingTelemetryRelativePath(
    const QString& videoRelativePath) {
    return nativeCompanionRelativePath(videoRelativePath);
}

}  // namespace omatrack
