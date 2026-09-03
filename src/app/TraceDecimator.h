// Viewport-bounded telemetry paths, independent of scene-graph resources.
#pragma once

#include <QPointF>
#include <QRectF>
#include <QVector>

#include <functional>
#include <vector>

namespace trace {

// A NaN point is a pen-up. Samples are selected in time order, never sorted
// by value. The forward alignment map is evaluated locally in each device
// column: a nonlinear reference must not use an endpoint-only inverse.
// At most two extrema per device column plus clipped run endpoints. Gaps
// cost at most one column. A slope corridor removes only subpixel detail.
void decimate(const std::vector<double>& series,
              const std::function<double(double)>& sourceFraction,
              double xStart, double xSpan, const QRectF& rect, double yMin,
              double ySpan, double dpr, double clipLow, double clipHigh,
              QVector<QPointF>& output);

}  // namespace trace
