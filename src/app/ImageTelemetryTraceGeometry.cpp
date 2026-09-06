// origin: PUBLIC — time-axis geometry over the shared immutable series.
#include "ImageTelemetryTraceGeometry.h"
#include "TraceDecimator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace image_trace {
namespace {
using omatrack::inference::ImageTelemetryMaxDurationNs;
using omatrack::inference::ImageTelemetryPeriodNs;
using omatrack::inference::ImageTelemetrySeries;
constexpr double NsPerSecond = 1e9;
constexpr double Unknown = std::numeric_limits<double>::quiet_NaN();

bool validDuration(const ImageTelemetrySeries& series) {
    return series.durationNs > 0 &&
           series.durationNs <= ImageTelemetryMaxDurationNs;
}
std::int64_t nanoseconds(double seconds) {
    return std::int64_t(std::llround(
        std::clamp(seconds, 0.0,
                   double(ImageTelemetryMaxDurationNs) / NsPerSecond) *
        NsPerSecond));
}
std::int64_t cellEnd(const ImageTelemetrySeries& series, std::size_t index) {
    return std::min(series.durationNs,
                    std::int64_t(index + 1) * ImageTelemetryPeriodNs);
}
bool evidence(const ImageTelemetrySeries& series, std::size_t index) {
    if (index >= series.cells.size() ||
        index >= ImageTelemetrySeries::slotCount(series.durationNs))
        return false;
    const auto& slot = series.cells[index];
    if (!slot.visited || !slot.layoutSupported || !slot.presentationPtsNs ||
        !slot.sourcePtsNs)
        return false;
    const auto pts = *slot.presentationPtsNs;
    if (pts < std::int64_t(index) * ImageTelemetryPeriodNs ||
        pts >= cellEnd(series, index))
        return false;
    const auto origin = series.timelineOriginNs;
    if ((origin > 0 &&
         pts > std::numeric_limits<std::int64_t>::max() - origin) ||
        (origin < 0 && pts < std::numeric_limits<std::int64_t>::min() - origin))
        return false;
    return *slot.sourcePtsNs == pts + origin;
}
bool finiteField(const omatrack::inference::ImageTelemetrySlot& slot,
                 std::size_t field) {
    if (field >= 4 || !slot.values[field]) return false;
    const double value = *slot.values[field];
    return std::isfinite(value) && value >= 0 &&
           value <= (field < 2 ? 999 : 100) &&
           (field >= 2 || value == std::floor(value));
}
bool known(const ImageTelemetrySeries& series, std::size_t index,
           std::size_t field) {
    return evidence(series, index) && finiteField(series.cells[index], field);
}
void span(std::vector<Span>& spans, double start, double end, double low,
          double high) {
    start = std::max(start, low);
    end = std::min(end, high);
    if (end <= start) return;
    if (!spans.empty() && std::abs(spans.back().end - start) < 1e-8)
        spans.back().end = end;
    else
        spans.push_back({start, end});
}
void append(Curve& curve, double time, double value) {
    if (!curve.times.empty() && curve.times.back() == time &&
        curve.values.back() == value)
        return;
    curve.times.push_back(time);
    curve.values.push_back(value);
}
void gap(Curve& curve, double time) {
    // One pen-up is enough even for hours of unvisited cells. Keeping the
    // first gap boundary avoids allocating a fake full-resolution channel.
    if (curve.values.empty() || std::isfinite(curve.values.back()))
        append(curve, time, Unknown);
}
// Dense overview: accumulate extrema and masks directly from source cells
// into physical-pixel columns before the shared decimator. This avoids building
// million-vertex temporary channels or emitting one rectangle per tiny gap.
// Any unknown cell makes that field's column a pen-up; surviving observed
// extrema remain actual-PTS dots. Gaps are never averaged into zero or bridged.
void projectDense(const ImageTelemetrySeries& series, double start, double end,
                  int columns, Projection& output) {
    struct Bucket {
        bool bad = false;
        bool visitedUnknown = false;
        bool any = false;
        std::size_t low = 0, high = 0;
    };
    std::array<double, 4> minima{{Unknown, Unknown, Unknown, Unknown}},
        maxima = minima;
    const auto rangeValue = [&](std::size_t field, double value) {
        auto& curve = output.curves[field];
        minima[field] =
            curve.hasValues ? std::min(minima[field], value) : value;
        maxima[field] =
            curve.hasValues ? std::max(maxima[field], value) : value;
        curve.hasValues = true;
    };
    for (auto& curve : output.curves) {
        curve.times.reserve(std::size_t(columns) * 6);
        curve.values.reserve(std::size_t(columns) * 6);
        curve.isolated.reserve(columns * 2);
    }
    output.inspectedSlots = 0;
    for (int column = 0; column < columns; ++column) {
        const double a = start + (end - start) * column / columns;
        const double b = start + (end - start) * (column + 1) / columns;
        const auto aNs = nanoseconds(a), bNs = nanoseconds(b);
        if (bNs <= aNs) continue;
        const auto range = series.slotRange(aNs, bNs);
        output.inspectedSlots += range.second - range.first;
        bool unvisited = false;
        std::array<Bucket, 4> buckets{};
        for (std::size_t index = range.first; index < range.second; ++index) {
            const auto& cell = series.cells[index];
            const bool valid = evidence(series, index);
            unvisited |= !cell.visited;
            for (std::size_t field = 0; field < 4; ++field) {
                auto& bucket = buckets[field];
                if (!valid || !finiteField(cell, field)) {
                    bucket.bad = true;
                    bucket.visitedUnknown |= cell.visited;
                    continue;
                }
                if (*cell.presentationPtsNs < aNs ||
                    *cell.presentationPtsNs >= bNs)
                    continue;
                const double value = *cell.values[field];
                rangeValue(field, value);
                if (!bucket.any)
                    bucket.low = bucket.high = index;
                else {
                    if (value < *series.cells[bucket.low].values[field])
                        bucket.low = index;
                    if (value > *series.cells[bucket.high].values[field])
                        bucket.high = index;
                }
                bucket.any = true;
            }
        }
        if (unvisited) span(output.unvisited, a, b, start, end);
        for (std::size_t field = 0; field < 4; ++field) {
            auto& curve = output.curves[field];
            const auto& bucket = buckets[field];
            if (bucket.visitedUnknown)
                span(output.unknown[field], a, b, start, end);
            const auto firstValue = valueAt(series, field, a);
            const auto lastValue =
                valueAt(series, field, double(bNs - 1) / NsPerSecond);
            const bool broken = bucket.bad || !firstValue || !lastValue;
            if (broken)
                gap(curve, a);
            else {
                rangeValue(field, *firstValue);
                rangeValue(field, *lastValue);
                append(curve, double(aNs) / NsPerSecond, *firstValue);
            }
            if (bucket.any) {
                const std::array<std::size_t, 2> selected{
                    {std::min(bucket.low, bucket.high),
                     std::max(bucket.low, bucket.high)}};
                for (int n = 0; n < 2; ++n) {
                    if (n && selected[0] == selected[1]) continue;
                    const auto index = selected[n];
                    const auto& cell = series.cells[index];
                    const double time =
                        double(*cell.presentationPtsNs) / NsPerSecond;
                    const double value = *cell.values[field];
                    if (broken)
                        curve.isolated.append(QPointF(time, value));
                    else {
                        if (field < 2 && index &&
                            known(series, index - 1, field))
                            append(curve, time,
                                   *series.cells[index - 1].values[field]);
                        append(curve, time, value);
                    }
                }
            }
            if (!broken)
                append(curve, double(bNs - 1) / NsPerSecond, *lastValue);
        }
    }
    for (std::size_t field = 0; field < 2; ++field) {
        auto& curve = output.curves[field];
        if (curve.hasValues) {
            curve.minimum = std::max(0.0, std::floor(minima[field]) - 0.5);
            curve.maximum =
                std::max(curve.minimum + 1.0, std::ceil(maxima[field]) + 0.5);
        }
    }
}
}  // namespace

void project(const ImageTelemetrySeries& series, double start, double end,
             Projection& output, int deviceColumns) {
    output.unvisited.clear();
    output.inspectedSlots = 0;
    for (std::size_t field = 0; field < 4; ++field) {
        auto& curve = output.curves[field];
        curve.times.clear();
        curve.values.clear();
        curve.isolated.clear();
        curve.minimum = 0;
        curve.maximum = field < 2 ? 1 : 100;
        curve.hasValues = false;
        output.unknown[field].clear();
    }
    if (!validDuration(series) || !std::isfinite(start) ||
        !std::isfinite(end) || end <= start)
        return;
    auto range = series.slotRange(nanoseconds(start), nanoseconds(end));
    if (range.first > 0) --range.first;
    range.second = std::min(series.cells.size(), range.second + 1);
    const auto slotLimit = ImageTelemetrySeries::slotCount(series.durationNs);
    range.second = std::min(range.second, slotLimit);
    output.inspectedSlots =
        range.second > range.first ? range.second - range.first : 0;
    deviceColumns = std::clamp(deviceColumns, 2, 32768);
    if (output.inspectedSlots > std::size_t(deviceColumns) * 2) {
        projectDense(series, start, end, deviceColumns, output);
        return;
    }
    std::array<double, 4> low{{Unknown, Unknown, Unknown, Unknown}};
    std::array<double, 4> high = low;
    for (std::size_t index = range.first; index < range.second; ++index) {
        const double cellStart =
            double(std::int64_t(index) * ImageTelemetryPeriodNs) / NsPerSecond;
        const auto endNs = cellEnd(series, index);
        const double cellFinish = double(endNs) / NsPerSecond;
        const auto& slot = series.cells[index];
        if (!slot.visited)
            span(output.unvisited, cellStart, cellFinish, start, end);
        for (std::size_t field = 0; field < 4; ++field) {
            auto& curve = output.curves[field];
            if (!known(series, index, field)) {
                if (slot.visited)
                    span(output.unknown[field], cellStart, cellFinish, start,
                         end);
                gap(curve, cellStart);
                continue;
            }
            const double time = double(*slot.presentationPtsNs) / NsPerSecond;
            const double value = *slot.values[field];
            if (field < 2 && index > range.first &&
                known(series, index - 1, field))
                append(curve, time, *series.cells[index - 1].values[field]);
            append(curve, time, value);
            low[field] = curve.hasValues ? std::min(low[field], value) : value;
            high[field] =
                curve.hasValues ? std::max(high[field], value) : value;
            curve.hasValues = true;
            if ((index == 0 || !known(series, index - 1, field)) &&
                !known(series, index + 1, field))
                curve.isolated.append(QPointF(time, value));
            if (!known(series, index + 1, field)) {
                // A 200ms known cell has a bounded previous-value tail. It
                // stops before the next unknown/unvisited cell, never through
                // it. This is drawing interpolation, not an invented sample.
                if (endNs - 1 > *slot.presentationPtsNs)
                    append(curve, double(endNs - 1) / NsPerSecond, value);
                gap(curve, cellFinish);
            }
        }
    }
    for (std::size_t field = 0; field < 2; ++field) {
        auto& curve = output.curves[field];
        if (curve.hasValues) {
            curve.minimum = std::max(0.0, std::floor(low[field]) - 0.5);
            curve.maximum =
                std::max(curve.minimum + 1.0, std::ceil(high[field]) + 0.5);
        }
    }
}

void path(const Curve& curve, double start, double end, const QRectF& rect,
          double dpr, QVector<QPointF>& output) {
    output.clear();
    if (!curve.hasValues || curve.times.size() < 2 || end <= start) return;
    const auto fraction = [&curve](double time) {
        const auto& times = curve.times;
        const auto next = std::upper_bound(times.begin(), times.end(), time);
        if (next == times.begin()) return 0.0;
        if (next == times.end()) return 1.0;
        const auto index = std::size_t(next - times.begin() - 1);
        const double between =
            (time - times[index]) / (times[index + 1] - times[index]);
        return (double(index) + between) / double(times.size() - 1);
    };
    trace::decimate(curve.values, fraction, start, end - start, rect,
                    curve.minimum, curve.maximum - curve.minimum, dpr,
                    curve.times.front(), curve.times.back(), output);
}

void markers(const Curve& curve, double start, double end, const QRectF& rect,
             double dpr, QVector<QPointF>& output) {
    output.clear();
    if (end <= start || rect.width() <= 0 || rect.height() <= 0) return;
    const int columns =
        std::max(1, int(std::ceil(rect.width() * std::max(1.0, dpr))));
    int column = -1;
    QPointF low, high;
    const auto flush = [&]() {
        if (column < 0) return;
        const auto emitPoint = [&](const QPointF& point) {
            output.append(
                QPointF(rect.left() +
                            (point.x() - start) / (end - start) * rect.width(),
                        rect.bottom() - (point.y() - curve.minimum) /
                                            (curve.maximum - curve.minimum) *
                                            rect.height()));
        };
        if (low.x() <= high.x()) {
            emitPoint(low);
            if (high != low) emitPoint(high);
        } else {
            emitPoint(high);
            emitPoint(low);
        }
    };
    for (const auto& point : curve.isolated) {
        if (point.x() < start || point.x() > end) continue;
        const int next = std::clamp(
            int((point.x() - start) / (end - start) * columns), 0, columns - 1);
        if (next != column) {
            flush();
            column = next;
            low = high = point;
        } else {
            if (point.y() < low.y()) low = point;
            if (point.y() > high.y()) high = point;
        }
    }
    flush();
}

std::optional<double> valueAt(const ImageTelemetrySeries& series,
                              std::size_t field, double seconds) {
    if (!validDuration(series) || !std::isfinite(seconds) || seconds < 0 ||
        seconds >= double(series.durationNs) / NsPerSecond)
        return {};
    const auto ns = nanoseconds(seconds);
    const auto index = std::size_t(ns / ImageTelemetryPeriodNs);
    if (!known(series, index, field)) return {};
    const auto& current = series.cells[index];
    std::size_t first = index, last = index;
    if (ns < *current.presentationPtsNs) {
        if (index == 0 || !known(series, index - 1, field)) return {};
        first = index - 1;
    } else if (known(series, index + 1, field)) {
        last = index + 1;
    }
    const auto& a = series.cells[first];
    if (field < 2 || first == last) return a.values[field];
    const auto& b = series.cells[last];
    const double fraction = double(ns - *a.presentationPtsNs) /
                            double(*b.presentationPtsNs - *a.presentationPtsNs);
    return *a.values[field] + (*b.values[field] - *a.values[field]) * fraction;
}
}  // namespace image_trace
