// origin: PUBLIC — worker-only image-derived recording cache, not source
// conversion.
#pragma once

#include "inference/ImageTelemetrySeries.h"

#include <QString>
#include <atomic>
#include <memory>

namespace omatrack {

class ImageTelemetryCache {
public:
    using Cancel = std::shared_ptr<std::atomic<bool>>;
    enum class Status {
        Ready,
        Miss,
        Partial,
        Complete,
        Stale,
        Invalid,
        Cancelled,
        Error
    };
    struct Result {
        Status status = Status::Error;
        inference::ImageTelemetrySnapshot series;
        QString path;
        QString error;
        bool ok() const {
            return status == Status::Ready || status == Status::Partial ||
                   status == Status::Complete;
        }
    };

    // Every method doing disk/hash/codec work is synchronous and WORKER ONLY.
    // Empty root uses the app cache's image-telemetry/v1 directory. An explicit
    // root is useful for isolated tests; never pass a source recording
    // directory.
    explicit ImageTelemetryCache(QString root = {});
    // Establish stable file identities and the model content SHA256, with
    // before/after identity checks. Does not imply native telemetry absence:
    // false is valid for a load-only probe, but cannot be saved.
    Result prepare(const QString& sourcePath, const QString& modelPath,
                   std::int64_t durationNs, std::int64_t timelineOriginNs,
                   bool nativeTelemetryAbsent, const Cancel& cancel = {}) const;
    // expected supplies freshly prepared identities and exact duration. The
    // stored source origin is restored on hit (no decoder/ORT needed); the
    // prepare() origin can be zero until a decoder has established it on miss.
    // Identical model bytes relocated elsewhere reuse the same cache key.
    // A hit requires stored native_telemetry=absent and returns that verified
    // flag, bound to the unchanged source, in the immutable snapshot.
    Result load(const inference::ImageTelemetrySeries& expected,
                const Cancel& cancel = {}) const;
    // Atomically replace one standard zstd MTJ .telemetry, never source/model.
    // All cells and provenance are in that one file; no per-recording sidecar.
    // Holds a nonblocking per-cache lock across read/monotonic union/publish;
    // unknown never erases known, conflicting known values/PTS are rejected.
    // Revalidates identities before publish. Completion is derived from
    // coverage. Returns series only when the union added coverage or changed
    // revision; adopt that immutable result. An ordinary save avoids copying
    // into Result.
    Result save(const inference::ImageTelemetrySeries& series,
                const Cancel& cancel = {}) const;
    QString pathFor(const inference::ImageTelemetrySeries& series) const;

private:
    QString root_;
};

}  // namespace omatrack
