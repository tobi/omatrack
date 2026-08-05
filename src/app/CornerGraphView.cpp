#include "CornerGraphView.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPanelGap = 8.0;
constexpr int kPanelCount = 3;

// Shared vertical range across both laps so a comparison reads as a
// difference in driving, not a difference in scale.
std::pair<double, double> sharedRange(const std::vector<double>& a,
                                      const std::vector<double>& b,
                                      bool symmetric) {
    double low = std::numeric_limits<double>::infinity();
    double high = -std::numeric_limits<double>::infinity();
    for (const std::vector<double>* values : {&a, &b})
        for (double value : *values) {
            low = std::min(low, value);
            high = std::max(high, value);
        }
    if (!std::isfinite(low) || !std::isfinite(high) || high <= low) {
        low = 0.0;
        high = 1.0;
    }
    if (symmetric) {
        const double magnitude =
            std::max({std::fabs(low), std::fabs(high), 1.0});
        low = -magnitude;
        high = magnitude;
    }
    return {low, high};
}

}  // namespace

CornerGraphView::CornerGraphView(QQuickItem* parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(true);
}

void CornerGraphView::setStore(TelemetryStore* store) {
    if (store_ == store) return;
    if (store_) disconnect(store_, nullptr, this, nullptr);
    store_ = store;
    if (store_) {
        connect(store_, &TelemetryStore::selectionChanged, this,
                &QQuickItem::update);
        connect(store_, &TelemetryStore::cornersChanged, this,
                &QQuickItem::update);
    }
    emit storeChanged();
    update();
}

void CornerGraphView::setCornerIndex(int index) {
    if (cornerIndex_ == index) return;
    cornerIndex_ = index;
    emit cornerIndexChanged();
    update();
}

void CornerGraphView::paintPanel(QPainter* painter, const Panel& panel,
                                 const QString& label) const {
    const double w = width();
    painter->fillRect(QRectF(0.0, panel.top, w, panel.height),
                      backgroundColor_);
    painter->setPen(QPen(gridColor_, 1.0));
    for (int line = 1; line < 4; ++line) {
        const double y = panel.top + panel.height * line / 4.0;
        painter->drawLine(QPointF(0.0, y), QPointF(w, y));
    }
    QFont font(monoFontFamily_);
    font.setPixelSize(10);
    painter->setFont(font);
    painter->setPen(labelColor_);
    painter->drawText(QPointF(7.0, panel.top + 13.0), label);
}

void CornerGraphView::paintSeries(QPainter* painter, const Panel& panel,
                                  const std::vector<double>& values, double low,
                                  double high, const QColor& color,
                                  double lineWidth) const {
    if (values.size() < 2) return;
    const double w = width();
    const double span = std::max(1e-6, high - low);
    const double last = double(values.size() - 1);

    QPainterPath path;
    for (size_t i = 0; i < values.size(); ++i) {
        const double x = double(i) / last * w;
        const double y =
            panel.top + panel.height - (values[i] - low) / span * panel.height;
        if (i == 0)
            path.moveTo(x, y);
        else
            path.lineTo(x, y);
    }
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(color, lineWidth));
    painter->drawPath(path);
}

void CornerGraphView::paintAnnotation(QPainter* painter, double position,
                                      const QString& label, const QColor& color,
                                      double labelY, bool dashed) const {
    if (!std::isfinite(position)) return;
    const double w = width();
    const double h = height();
    const double x = std::clamp(position, 0.0, 1.0) * w;

    QPen pen(color, dashed ? 1.0 : 1.4);
    if (dashed) pen.setDashPattern({4.0, 4.0});
    painter->setPen(pen);
    painter->drawLine(QPointF(x, 0.0), QPointF(x, h));

    if (label.isEmpty()) return;
    QFont font(monoFontFamily_);
    font.setPixelSize(9);
    font.setBold(true);
    painter->setFont(font);
    painter->setPen(color);
    painter->drawText(QPointF(std::min(w - 58.0, x + 4.0), labelY), label);
}

void CornerGraphView::paint(QPainter* painter) {
    if (!store_) return;
    const CornerGraph* graph = store_->cornerGraph(cornerIndex_);
    if (!graph || !graph->valid()) return;

    const double w = width();
    const double h = height();
    if (w <= 1.0 || h <= 1.0) return;

    const double panelHeight =
        (h - kPanelGap * (kPanelCount - 1)) / kPanelCount;
    const Panel speedPanel{0.0, panelHeight};
    const Panel pedalPanel{panelHeight + kPanelGap, panelHeight};
    const Panel steeringPanel{(panelHeight + kPanelGap) * 2.0, panelHeight};

    paintPanel(painter, speedPanel, QStringLiteral("SPEED"));
    paintPanel(painter, pedalPanel, QStringLiteral("THROTTLE / BRAKE"));
    paintPanel(painter, steeringPanel, QStringLiteral("STEERING"));

    const CornerGraphSeries& primary = graph->primary;
    const CornerGraphSeries& compare = graph->compare;

    const auto speedRange = sharedRange(primary.speed, compare.speed, false);
    paintSeries(painter, speedPanel, compare.speed, speedRange.first,
                speedRange.second, compareColor_, 1.2);
    paintSeries(painter, speedPanel, primary.speed, speedRange.first,
                speedRange.second, speedColor_, 1.8);

    paintSeries(painter, pedalPanel, compare.throttle, 0.0, 1.0, compareColor_,
                1.1);
    paintSeries(painter, pedalPanel, primary.throttle, 0.0, 1.0, throttleColor_,
                1.7);
    const double brakeMax = std::max({primary.maxBrake, compare.maxBrake, 1.0});
    paintSeries(painter, pedalPanel, compare.brake, 0.0, brakeMax,
                compareBrakeColor_, 1.1);
    paintSeries(painter, pedalPanel, primary.brake, 0.0, brakeMax, brakeColor_,
                1.7);

    const auto steeringRange =
        sharedRange(primary.steering, compare.steering, true);
    paintSeries(painter, steeringPanel, compare.steering, steeringRange.first,
                steeringRange.second, compareColor_, 1.1);
    paintSeries(painter, steeringPanel, primary.steering, steeringRange.first,
                steeringRange.second, steeringColor_, 1.7);

    // Approach and exit stay visible but recessed; the selected zone keeps full
    // contrast between its boundary lines.
    const double zoneStart = std::clamp(graph->zoneStart, 0.0, 1.0) * w;
    const double zoneEnd = std::clamp(graph->zoneEnd, 0.0, 1.0) * w;
    if (zoneStart > 0.0)
        painter->fillRect(QRectF(0.0, 0.0, zoneStart, h), dimColor_);
    if (zoneEnd < w)
        painter->fillRect(QRectF(zoneEnd, 0.0, w - zoneEnd, h), dimColor_);

    painter->save();
    painter->setOpacity(0.55);
    painter->setPen(QPen(turnInColor_, 1.0));
    painter->drawLine(QPointF(zoneStart, 0.0), QPointF(zoneStart, h));
    painter->drawLine(QPointF(zoneEnd, 0.0), QPointF(zoneEnd, h));
    painter->restore();

    QFont caption(monoFontFamily_);
    caption.setPixelSize(9);
    painter->setFont(caption);
    painter->setPen(labelColor_);
    painter->drawText(QPointF(7.0, h - 6.0),
                      QStringLiteral("%1m window · %2m zone")
                          .arg(std::llround(graph->windowMeters))
                          .arg(std::llround(graph->zoneMeters)));

    if (graph->hasCompare()) {
        paintAnnotation(painter, graph->compareTurnIn, QString(), compareColor_,
                        0.0, true);
        paintAnnotation(painter, graph->compareApex, QString(), compareColor_,
                        0.0, true);
        paintAnnotation(painter, graph->comparePickup, QString(), compareColor_,
                        0.0, true);
    }
    paintAnnotation(painter, graph->turnIn, QStringLiteral("TURN-IN"),
                    turnInColor_, 30.0, false);
    paintAnnotation(painter, graph->apex, QStringLiteral("APEX"), apexColor_,
                    42.0, false);
    paintAnnotation(painter, graph->pickup, QStringLiteral("PICKUP"),
                    pickupColor_, 54.0, false);
}
