#include "VideoTelemetryHud.h"

#include "TelemetryStore.h"
#include "core/TelemetryEngine.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
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

QColor mixColors(const QColor& from, const QColor& to, double amount) {
    const double factor = std::clamp(amount, 0.0, 1.0);
    return QColor::fromRgbF(
        from.redF() + (to.redF() - from.redF()) * factor,
        from.greenF() + (to.greenF() - from.greenF()) * factor,
        from.blueF() + (to.blueF() - from.blueF()) * factor,
        from.alphaF() + (to.alphaF() - from.alphaF()) * factor);
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
    const qreal sy = height() / 210.0;
    painter->scale(sx, sy);

    constexpr qreal bandTop = 35.0;
    constexpr qreal bandBottom = 175.0;
    const QPointF dialCenter(902, 105);
    constexpr qreal dialRadius = 70.0;
    painter->fillRect(QRectF(0, bandTop, dialCenter.x(), bandBottom - bandTop),
                      backgroundColor_);

    const QRectF graph(52, 39, 630, 132);
    painter->fillRect(graph, QColor(20, 22, 22, 225));
    painter->setPen(QPen(QColor(255, 255, 255, 16), 1));
    for (int i = 1; i < 4; ++i)
        painter->drawLine(graph.left(), graph.top() + graph.height() * i / 4.0,
                          graph.right(),
                          graph.top() + graph.height() * i / 4.0);

    QFont labelFont(monoFontFamily_);
    labelFont.setBold(true);
    labelFont.setPixelSize(12);
    painter->setFont(labelFont);
    painter->setPen(foregroundColor_);
    painter->save();
    painter->translate(24, 170);
    painter->rotate(-90);
    painter->drawText(QRectF(0, 0, 130, 20), Qt::AlignCenter, "TELEMETRY");
    painter->restore();

    const double primaryDuration =
        primary->time.empty() ? 1.0 : std::max(1.0, primary->time.back());
    const double halfWindow = std::min(0.18, 4.0 / primaryDuration);
    constexpr double currentMarkerFraction = 0.86;
    auto drawTrace = [&](const std::vector<double>& values, double maximum,
                         const QColor& color, qreal lineWidth, bool reference) {
        if (values.size() < 2) return;
        constexpr int points = 180;
        QPainterPath path;
        for (int i = 0; i < points; ++i) {
            const double local = double(i) / double(points - 1);
            const double primaryFraction = std::clamp(
                cursor + (local - currentMarkerFraction) * 2.0 * halfWindow,
                0.0, 1.0);
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
        QPen pen(color, lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        if (reference) pen.setDashPattern({4.0, 4.0});
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
        drawTrace(compare->throttle, 1.0, withAlpha(compareColor_, 55), 1.1,
                  true);
        drawTrace(compare->brake, brakeMax, withAlpha(compareColor_, 42), 1.1,
                  true);
    }
    drawTrace(primary->throttle, 1.0, throttleColor_, 2.5, false);
    drawTrace(primary->brake, brakeMax, brakeColor_, 2.5, false);

    painter->setPen(QPen(withAlpha(foregroundColor_, 150), 1));
    const qreal currentMarkerX =
        graph.left() + graph.width() * currentMarkerFraction;
    painter->drawLine(currentMarkerX, graph.top(), currentMarkerX,
                      graph.bottom());

    const double throttle = sample(primary->throttle, cursor);
    const double brake = sample(primary->brake, cursor) / brakeMax;
    const double pedals[] = {brake, throttle};
    const QColor pedalColors[] = {brakeColor_, throttleColor_};
    for (int i = 0; i < 2; ++i) {
        const QRectF bar(714 + i * 30, bandTop, 24, bandBottom - bandTop);
        painter->fillRect(bar, QColor(35, 35, 35, 245));
        const QRectF fill(bar.left() + 2,
                          bar.bottom() - 2 - pedals[i] * (bar.height() - 4),
                          bar.width() - 4, pedals[i] * (bar.height() - 4));
        painter->fillRect(fill, withAlpha(pedalColors[i], 220));
    }

    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(35, 35, 35, 245));
    painter->drawEllipse(dialCenter, dialRadius, dialRadius);
    painter->setPen(QPen(withAlpha(mutedColor_, 80), 2));
    painter->setBrush(Qt::NoBrush);
    painter->drawEllipse(dialCenter, dialRadius, dialRadius);

    constexpr double pi = 3.14159265358979323846;
    auto steeringDirection = [](double steering) {
        constexpr double degreesToRadians = pi / 180.0;
        const double angle =
            std::clamp(steering, -180.0, 180.0) * degreesToRadians;
        return QPointF(-std::sin(angle), -std::cos(angle));
    };
    auto drawSteeringMarker = [&](const QPointF& point, const QPointF& radial,
                                  const QColor& color, qreal radialSize,
                                  qreal tangentSize) {
        const QPointF tangent(-radial.y(), radial.x());
        QPainterPath marker;
        marker.moveTo(point + radial * radialSize);
        marker.lineTo(point + tangent * tangentSize);
        marker.lineTo(point - radial * radialSize);
        marker.lineTo(point - tangent * tangentSize);
        marker.closeSubpath();
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawPath(marker);
    };
    const double primarySteering = sample(primary->steering, cursor);
    const QPointF primarySteeringDirection = steeringDirection(primarySteering);
    const QPointF primarySteeringPoint =
        dialCenter + primarySteeringDirection * dialRadius;
    if (compare) {
        const double referenceSteering =
            sample(compare->steering, compareCursor);
        const QPointF referenceSteeringDirection =
            steeringDirection(referenceSteering);
        const QPointF referenceSteeringPoint =
            dialCenter + referenceSteeringDirection * dialRadius;
        const double steeringSweep =
            std::remainder(primarySteering - referenceSteering, 360.0);
        const int arcSegments =
            std::max(1, int(std::ceil(std::abs(steeringSweep) / 5.0)));
        QPainterPath steeringArc;
        steeringArc.moveTo(referenceSteeringPoint);
        for (int segment = 1; segment <= arcSegments; ++segment) {
            const double steeringAlongArc =
                referenceSteering +
                steeringSweep * double(segment) / double(arcSegments);
            steeringArc.lineTo(
                dialCenter + steeringDirection(steeringAlongArc) * dialRadius);
        }
        painter->setPen(QPen(withAlpha(compareColor_, 65), 1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(steeringArc);
        drawSteeringMarker(referenceSteeringPoint, referenceSteeringDirection,
                           withAlpha(compareColor_, 210), 5.0, 3.5);
    }
    drawSteeringMarker(primarySteeringPoint, primarySteeringDirection,
                       steeringColor_, 8.0, 6.0);

    const int primaryGear = sampleGear(primary->gear, cursor);
    const int referenceGear =
        compare ? sampleGear(compare->gear, compareCursor) : primaryGear;
    const int gearDelta = primaryGear - referenceGear;
    const QColor gearColor = gearDelta < 0   ? brakeColor_
                             : gearDelta > 0 ? throttleColor_
                                             : foregroundColor_;
    QFont gearFont(monoFontFamily_);
    gearFont.setBold(true);
    gearFont.setPixelSize(52);
    painter->setFont(gearFont);
    painter->setPen(gearColor);
    painter->drawText(QRectF(842, 42, 120, 55), Qt::AlignCenter,
                      QString::number(primaryGear));
    if (gearDelta != 0) {
        constexpr qreal triangleCenterX = 944.0;
        constexpr qreal triangleCenterY = 73.0;
        constexpr qreal triangleHalfWidth = 6.0;
        constexpr qreal triangleHalfHeight = 5.0;
        QPainterPath triangle;
        if (gearDelta > 0) {
            triangle.moveTo(triangleCenterX,
                            triangleCenterY - triangleHalfHeight);
            triangle.lineTo(triangleCenterX + triangleHalfWidth,
                            triangleCenterY + triangleHalfHeight);
            triangle.lineTo(triangleCenterX - triangleHalfWidth,
                            triangleCenterY + triangleHalfHeight);
        } else {
            triangle.moveTo(triangleCenterX - triangleHalfWidth,
                            triangleCenterY - triangleHalfHeight);
            triangle.lineTo(triangleCenterX + triangleHalfWidth,
                            triangleCenterY - triangleHalfHeight);
            triangle.lineTo(triangleCenterX,
                            triangleCenterY + triangleHalfHeight);
        }
        triangle.closeSubpath();
        painter->setPen(Qt::NoPen);
        painter->setBrush(gearColor);
        painter->drawPath(triangle);
    }
    QFont valueFont(monoFontFamily_);
    valueFont.setBold(true);
    valueFont.setPixelSize(17);
    painter->setFont(valueFont);
    painter->setPen(foregroundColor_);
    painter->drawText(QRectF(842, 98, 120, 20), Qt::AlignCenter, "km/h");
    valueFont.setPixelSize(23);
    painter->setFont(valueFont);
    const double primarySpeed = sample(primary->speed, cursor);
    const double referenceSpeed =
        compare ? sample(compare->speed, compareCursor) : primarySpeed;
    const double speedDelta = primarySpeed - referenceSpeed;
    QColor speedColor = mutedColor_;
    if (compare && std::abs(speedDelta) > 1.0) {
        constexpr double fullColorDifference = 5.0;
        const double colorMix =
            (std::abs(speedDelta) - 1.0) / (fullColorDifference - 1.0);
        speedColor = mixColors(mutedColor_,
                               speedDelta > 0.0 ? throttleColor_ : brakeColor_,
                               colorMix);
    }
    painter->setPen(speedColor);
    painter->drawText(QRectF(842, 118, 120, 29), Qt::AlignCenter,
                      QString::number(qRound(primarySpeed)));

    if (compare) {
        const QVector<double>& deltaTrace = store_->deltaTrace();
        if (!deltaTrace.isEmpty()) {
            const double position =
                std::clamp(cursor, 0.0, 1.0) * double(deltaTrace.size() - 1);
            const qsizetype low = qsizetype(std::floor(position));
            const qsizetype high = std::min(low + 1, deltaTrace.size() - 1);
            const double timeDelta =
                deltaTrace[low] +
                (deltaTrace[high] - deltaTrace[low]) * (position - double(low));
            const QColor deltaColor = timeDelta > 0.01    ? brakeColor_
                                      : timeDelta < -0.01 ? throttleColor_
                                                          : mutedColor_;
            QFont deltaFont(monoFontFamily_);
            deltaFont.setBold(true);
            deltaFont.setPixelSize(12);
            painter->setFont(deltaFont);
            painter->setPen(deltaColor);
            painter->drawText(
                QRectF(834, 150, 136, 18), Qt::AlignCenter,
                QStringLiteral("ΔT %1%2s")
                    .arg(timeDelta >= 0.0 ? QStringLiteral("+") : QString())
                    .arg(timeDelta, 0, 'f', 3));
        }
    }
}
