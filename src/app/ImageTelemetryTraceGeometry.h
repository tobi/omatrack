// origin: PUBLIC — view projection only; no recording/model content.
#pragma once

#include "inference/ImageTelemetrySeries.h"

#include <QPointF>
#include <QRectF>
#include <QVector>

#include <array>
#include <optional>
#include <vector>

namespace image_trace {

struct Span {
    double start = 0;
    double end = 0;
};
struct Curve {
    // View-only vertices, not another recording clock. Actual PTS anchors,
    // categorical step corners, bounded cell-tail holds, and NaN pen-ups.
    std::vector<double> times;
    std::vector<double> values;
    QVector<QPointF> isolated;  // (actual presentation seconds, value)
    double minimum = 0;
    double maximum = 1;
    bool hasValues = false;
};
struct Projection {
    std::array<Curve, 4> curves;
    std::vector<Span> unvisited;
    std::array<std::vector<Span>, 4> unknown;
    std::size_t inspectedSlots = 0;
};

// Only the intersecting 200ms cells plus clipping neighbours are inspected.
// Reuse output storage between immutable snapshot/viewport changes.
void project(const omatrack::inference::ImageTelemetrySeries& series,
             double startSeconds, double endSeconds, Projection& output,
             int deviceColumns = 2048);

// Uses the same source-ordered, pixel-column min/max decimator as native
// traces.
void path(const Curve& curve, double startSeconds, double endSeconds,
          const QRectF& rect, double dpr, QVector<QPointF>& output);
void markers(const Curve& curve, double startSeconds, double endSeconds,
             const QRectF& rect, double dpr, QVector<QPointF>& output);

// O(1), examining only the cell and its immediate neighbour. A visited cell
// with an absent field stays unknown; no interpolation/hold crosses that cell.
std::optional<double> valueAt(
    const omatrack::inference::ImageTelemetrySeries& series, std::size_t field,
    double seconds);

}  // namespace image_trace
