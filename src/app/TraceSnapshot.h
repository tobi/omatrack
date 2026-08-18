// One immutable view of the telemetry state a renderer needs for a frame.
//
// TraceView and VideoTelemetryHud used to reach into TelemetryStore through
// `friend` declarations to call private helpers
// (compareFractionForPrimaryFraction, videoClipWindowNs, …).
// TelemetryStore::traceSnapshot() now publishes those through this plain struct
// instead, so the renderers depend only on the store's public API plus this
// snapshot. The snapshot is short-lived: it is captured at the start of a
// static scene build and reused by the lightweight cursor overlay path, whose
// only per-frame input is the public cursorFrac().
//
// Pointers alias store-owned state and are valid for the frame that requested
// the snapshot; the std::function captures the store and is stable for as long
// as the selection / sync strategy is unchanged.

#pragma once

#include "TelemetryStore.h"        // CornerZone, CornerMarker
#include "core/TelemetryEngine.h"  // UnifiedLap

#include <QVector>

#include <functional>

struct TraceSnapshot {
    const omatrack::UnifiedLap* primary = nullptr;
    const omatrack::UnifiedLap* compare = nullptr;

    /// Primary−reference per-sample Δ-time (empty when no compare).
    const QVector<double>* deltaTrace = nullptr;

    double viewStart = 0.0;
    double viewEnd = 1.0;
    double cursorFrac = 0.5;

    const QVector<CornerZone>* corners = nullptr;
    const QVector<CornerMarker>* markers = nullptr;

    /// Neighbouring laps that frame a corner viewport running past
    /// start/finish.
    const omatrack::UnifiedLap* neighbourPrev = nullptr;
    const omatrack::UnifiedLap* neighbourNext = nullptr;

    /// File-relative window of the open video, for clipping overlay spans.
    bool videoClipValid = false;
    qint64 videoClipStartNs = 0;
    qint64 videoClipEndNs = 0;

    /// Maps a primary-lap fraction onto the compare lap's own fraction axis.
    /// Returns the input unchanged when there is no compare lap.
    std::function<double(double)> compareFractionForPrimaryFraction;

    double viewSpan() const { return viewEnd - viewStart; }
};
