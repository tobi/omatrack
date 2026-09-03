#include "TraceDecimator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace trace {

void decimate(const std::vector<double>& series,
              const std::function<double(double)>& sourceFraction,
              double xStart, double xSpan, const QRectF& rect, double yMin,
              double ySpan, double dpr, double clipLow, double clipHigh,
              QVector<QPointF>& output) {
    output.clear();
    if (series.size() < 2 || rect.width() < 2 || rect.height() <= 0 ||
        !std::isfinite(xStart) || !std::isfinite(xSpan) || xSpan <= 0 ||
        !std::isfinite(yMin) || !std::isfinite(ySpan) || ySpan <= 0)
        return;
    const int last = int(series.size()) - 1;
    const int columns = std::max(2, int(std::ceil(rect.width() * dpr)));
    // Work on a detached buffer once, rather than QList's mutable indexing
    // and refcount check at every point of every channel during a zoom.
    output.resize(columns * 5 + 4);
    QPointF* points = output.data();
    int used = 0;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    auto penUp = [&] {
        if (used && std::isfinite(points[used - 1].x()))
            points[used++] = QPointF(nan, nan);
    };
    auto valueAt = [&](double index) {
        const int lo = int(std::floor(index));
        const double f = index - lo;
        if (f < 1e-10 || lo == last) return series[size_t(lo)];
        return series[size_t(lo)] +
               (series[size_t(lo + 1)] - series[size_t(lo)]) * f;
    };
    auto append = [&](double x, double value) {
        const QPointF point(
            x,
            std::clamp(rect.bottom() - (value - yMin) / ySpan * rect.height(),
                       rect.top(), rect.bottom()));
        if (used && point == points[used - 1]) return;
        if (used >= 2) {
            const QPointF a = points[used - 1] - points[used - 2];
            const QPointF b = point - points[used - 1];
            // Only remove numerically collinear points, never filter peaks.
            if (std::isfinite(a.x()) && QPointF::dotProduct(a, b) >= 0 &&
                std::abs(a.x() * b.y() - a.y() * b.x()) < 1e-9) {
                points[used - 1] = point;
                return;
            }
        }
        points[used++] = point;
    };
    bool open = false;
    double pendingX = 0, pendingValue = 0;
    for (int column = 0; column < columns; ++column) {
        const double start =
            std::max(clipLow, xStart + xSpan * column / columns);
        const double end =
            std::min(clipHigh, xStart + xSpan * (column + 1) / columns);
        if (end <= start) {
            if (open) append(pendingX, pendingValue);
            open = false;
            penUp();
            continue;
        }
        const double from = sourceFraction(start) * last;
        const double to = sourceFraction(end) * last;
        if (!std::isfinite(from) || !std::isfinite(to) || from < 0 ||
            to > last || to < from) {
            if (open) append(pendingX, pendingValue);
            open = false;
            penUp();
            continue;
        }
        const double firstValue = valueAt(from), lastValue = valueAt(to);
        const int first = int(std::ceil(from));
        const int endIndex = std::min(last + 1, int(std::ceil(to)));
        int low = first, high = first;
        bool gap = !std::isfinite(firstValue) || !std::isfinite(lastValue);
        for (int i = first; !gap && i < endIndex; ++i) {
            const double value = series[size_t(i)];
            if (!std::isfinite(value)) {
                gap = true;
                break;
            }
            if (value < series[size_t(low)]) low = i;
            if (value > series[size_t(high)]) high = i;
        }
        if (gap) {
            if (open) append(pendingX, pendingValue);
            open = false;
            penUp();
            continue;
        }
        const double x0 = rect.left() + (start - xStart) / xSpan * rect.width();
        const double x1 = rect.left() + (end - xStart) / xSpan * rect.width();
        if (!open) append(x0, firstValue);
        open = true;
        // Min/max in temporal order. Samples have a stable x,
        // never the column centre. Sparse columns contribute only the actual
        // sample, not a new interpolated vertex and joint at every pixel.
        if (first < endIndex && to > from) {
            std::array<int, 2> selected{std::min(low, high),
                                        std::max(low, high)};
            int previous = -1;
            for (int index : selected) {
                if (index == previous) continue;
                previous = index;
                append(x0 + (index - from) / (to - from) * (x1 - x0),
                       series[size_t(index)]);
            }
        }
        pendingX = x1;
        pendingValue = lastValue;
    }
    if (open) append(pendingX, pendingValue);
    if (used && !std::isfinite(points[used - 1].x())) --used;
    // Subpixel line simplification. A swinging slope corridor is O(points),
    // not recursive Douglas-Peucker. The emitted endpoint can differ from
    // the corridor centre by epsilon as well, so the maximum vertical error
    // is 2*epsilon = 0.1 physical pixel. This removes invisible joints, not
    // telemetry noise or real spikes, and is independent of stroke width.
    const double epsilon = 0.05 / dpr;
    int written = 0, anchor = -1;
    double lowSlope = -std::numeric_limits<double>::infinity();
    double highSlope = std::numeric_limits<double>::infinity();
    QPointF previous;
    for (int i = 0; i < used; ++i) {
        const QPointF point = points[i];
        if (!std::isfinite(point.x())) {
            if (anchor >= 0 && previous != points[anchor])
                points[written++] = previous;
            points[written++] = point;
            anchor = -1;
            continue;
        }
        if (anchor < 0) {
            anchor = written;
            points[written++] = point;
            lowSlope = -std::numeric_limits<double>::infinity();
            highSlope = std::numeric_limits<double>::infinity();
        } else {
            double dx = point.x() - points[anchor].x();
            double dy = point.y() - points[anchor].y();
            const double lo = dx > 0 ? (dy - epsilon) / dx : highSlope + 1;
            const double hi = dx > 0 ? (dy + epsilon) / dx : lowSlope - 1;
            if (lo > highSlope || hi < lowSlope) {
                if (previous != points[anchor]) {
                    anchor = written;
                    points[written++] = previous;
                }
                dx = point.x() - points[anchor].x();
                dy = point.y() - points[anchor].y();
                lowSlope = dx > 0 ? (dy - epsilon) / dx
                                  : -std::numeric_limits<double>::infinity();
                highSlope = dx > 0 ? (dy + epsilon) / dx
                                   : std::numeric_limits<double>::infinity();
            } else {
                lowSlope = std::max(lowSlope, lo);
                highSlope = std::min(highSlope, hi);
            }
        }
        previous = point;
    }
    if (anchor >= 0 && previous != points[anchor]) points[written++] = previous;
    output.resize(written);
}

}  // namespace trace
