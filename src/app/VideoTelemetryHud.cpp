#include "VideoTelemetryHud.h"

#include "TelemetryStore.h"
#include "core/TelemetryEngine.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

double sample(const std::vector<double>& values, double fraction) {
    if (values.empty()) return 0.0;
    const double position =
        std::clamp(fraction, 0.0, 1.0) * double(values.size() - 1);
    const size_t low = size_t(std::floor(position));
    const size_t high = std::min(low + 1, values.size() - 1);
    return values[low] +
           (values[high] - values[low]) * (position - double(low));
}

int sampleGear(const std::vector<int>& values, double fraction) {
    if (values.empty()) return 0;
    return values[size_t(std::clamp(fraction, 0.0, 1.0) *
                         double(values.size() - 1))];
}

QColor withAlpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

}  // namespace

VideoTelemetryHud::VideoTelemetryHud(QQuickItem* parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    connect(this, &VideoTelemetryHud::paletteChanged, this,
            [this]() { update(); });
}

void VideoTelemetryHud::setStore(TelemetryStore* store) {
    if (store_ == store) return;
    if (store_) disconnect(store_, nullptr, this, nullptr);
    store_ = store;
    if (store_) {
        connect(store_, &TelemetryStore::cursorFracChanged, this,
                [this]() { update(); });
        connect(store_, &TelemetryStore::selectionChanged, this,
                [this]() { update(); });
        connect(store_, &TelemetryStore::referenceAlignmentChanged, this,
                [this]() { update(); });
    }
    update();
    emit storeChanged();
}

void VideoTelemetryHud::paint(QPainter* painter) {
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->fillRect(boundingRect(), backgroundColor_);
    if (!store_) return;

    const omatrack::UnifiedLap* primary = store_->primaryUnified();
    if (!primary || primary->size() < 2) return;
    const omatrack::UnifiedLap* compare = store_->compareUnified();
    const double cursor = store_->cursorFrac();
    const double compareCursor =
        compare ? store_->compareFractionForPrimaryFraction(std::clamp(
                      cursor - store_->referenceAlignment(), 0.0, 1.0))
                : 0.0;

    const qreal sx = width() / 1000.0;
    const qreal sy = height() / 260.0;
    painter->scale(sx, sy);

    const QRectF graph(70, 28, 590, 204);
    painter->fillRect(graph, QColor(20, 22, 22, 225));
    painter->setPen(QPen(QColor(255, 255, 255, 16), 1));
    for (int i = 1; i < 4; ++i)
        painter->drawLine(graph.left(), graph.top() + graph.height() * i / 4.0,
                          graph.right(),
                          graph.top() + graph.height() * i / 4.0);

    QFont labelFont(monoFontFamily_);
    labelFont.setBold(true);
    labelFont.setPixelSize(16);
    painter->setFont(labelFont);
    painter->setPen(foregroundColor_);
    painter->save();
    painter->translate(28, 210);
    painter->rotate(-90);
    painter->drawText(QRectF(0, 0, 180, 24), Qt::AlignCenter, "TELEMETRY");
    painter->restore();

    const double primaryDuration =
        primary->time.empty() ? 1.0 : std::max(1.0, primary->time.back());
    const double halfWindow = std::min(0.18, 4.0 / primaryDuration);
    auto drawTrace = [&](const std::vector<double>& values, double maximum,
                         const QColor& color, bool reference) {
        if (values.size() < 2) return;
        constexpr int points = 180;
        QPainterPath path;
        for (int i = 0; i < points; ++i) {
            const double local = double(i) / double(points - 1);
            const double primaryFraction =
                std::clamp(cursor + (local * 2.0 - 1.0) * halfWindow, 0.0, 1.0);
            const double fraction =
                reference
                    ? store_->compareFractionForPrimaryFraction(std::clamp(
                          primaryFraction - store_->referenceAlignment(), 0.0,
                          1.0))
                    : primaryFraction;
            const double value =
                std::clamp(sample(values, fraction) / maximum, 0.0, 1.0);
            const QPointF point(graph.left() + local * graph.width(),
                                graph.bottom() - value * graph.height());
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        QPen pen(color, reference ? 3.0 : 5.0, Qt::SolidLine, Qt::RoundCap,
                 Qt::RoundJoin);
        if (reference) pen.setDashPattern({4.0, 3.0});
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path);
    };

    const double primaryBrakeMax =
        primary->brake.empty()
            ? 1.0
            : *std::max_element(primary->brake.begin(), primary->brake.end());
    const double compareBrakeMax =
        !compare || compare->brake.empty()
            ? 1.0
            : *std::max_element(compare->brake.begin(), compare->brake.end());
    const double brakeMax = std::max({1.0, primaryBrakeMax, compareBrakeMax});
    if (compare) {
        drawTrace(compare->throttle, 1.0, withAlpha(compareColor_, 210), true);
        drawTrace(compare->brake, brakeMax, withAlpha(compareColor_, 150),
                  true);
    }
    drawTrace(primary->throttle, 1.0, primaryColor_, false);
    drawTrace(primary->brake, brakeMax, brakeColor_, false);

    painter->setPen(QPen(withAlpha(foregroundColor_, 150), 1));
    painter->drawLine(graph.center().x(), graph.top(), graph.center().x(),
                      graph.bottom());

    const double throttle = sample(primary->throttle, cursor);
    const double brake = sample(primary->brake, cursor) / brakeMax;
    const double clutch = sample(primary->clutch, cursor);
    const std::array<double, 3> pedals = {brake, clutch, throttle};
    const std::array<QColor, 3> pedalColors = {brakeColor_, mutedColor_,
                                               primaryColor_};
    const std::array<QString, 3> pedalNames = {
        QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("T")};
    for (int i = 0; i < 3; ++i) {
        const QRectF bar(682 + i * 48, 55, 34, 175);
        painter->fillRect(bar, QColor(35, 35, 35, 245));
        const QRectF fill(bar.left() + 3,
                          bar.bottom() - 3 - pedals[i] * (bar.height() - 6),
                          bar.width() - 6, pedals[i] * (bar.height() - 6));
        painter->fillRect(fill, withAlpha(pedalColors[i], 220));
        painter->setPen(foregroundColor_);
        painter->drawText(QRectF(bar.left(), 28, bar.width(), 22),
                          Qt::AlignCenter, pedalNames[i]);
    }

    const QPointF dialCenter(910, 132);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(35, 35, 35, 245));
    painter->drawEllipse(dialCenter, 76, 76);
    QFont gearFont(monoFontFamily_);
    gearFont.setBold(true);
    gearFont.setPixelSize(58);
    painter->setFont(gearFont);
    painter->setPen(foregroundColor_);
    painter->drawText(QRectF(850, 66, 120, 74), Qt::AlignCenter,
                      QString::number(sampleGear(primary->gear, cursor)));
    QFont valueFont(monoFontFamily_);
    valueFont.setBold(true);
    valueFont.setPixelSize(17);
    painter->setFont(valueFont);
    painter->drawText(QRectF(850, 139, 120, 27), Qt::AlignCenter, "km/h");
    valueFont.setPixelSize(25);
    painter->setFont(valueFont);
    painter->drawText(QRectF(850, 166, 120, 35), Qt::AlignCenter,
                      QString::number(qRound(sample(primary->speed, cursor))));

    if (compare) {
        QFont compareFont(monoFontFamily_);
        compareFont.setBold(true);
        compareFont.setPixelSize(12);
        painter->setFont(compareFont);
        painter->setPen(compareColor_);
        painter->drawText(
            QRectF(842, 214, 136, 24), Qt::AlignCenter,
            QStringLiteral("REF %1 · G%2")
                .arg(qRound(sample(compare->speed, compareCursor)))
                .arg(sampleGear(compare->gear, compareCursor)));
    }
}
