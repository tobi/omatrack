#include "VideoTelemetryHud.h"

#include "TelemetryStore.h"
#include "core/TelemetryEngine.h"

#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QSGNode>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

double sample(const std::vector<double>& values, double fraction) {
    if (values.empty()) return 0.0;
    const double position =
        std::clamp(fraction, 0.0, 1.0) * double(values.size() - 1);
    const size_t low = size_t(std::floor(position));
    const size_t high = std::min(low + 1, values.size() - 1);
    const double value =
        values[low] + (values[high] - values[low]) * (position - double(low));
    return std::isfinite(value) ? value : 0.0;
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

VideoTelemetryHud::VideoTelemetryHud(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(QQuickItem::ItemHasContents, true);
    connect(this, &VideoTelemetryHud::paletteChanged, this,
            [this]() { update(); });
    connect(this, &QQuickItem::visibleChanged, this, [this]() {
        if (isVisible()) update();
    });
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
        connect(store_, &TelemetryStore::lapLoadingChanged, this,
                [this]() { update(); });
    }
    update();
    emit storeChanged();
}

void VideoTelemetryHud::setMediaTime(double mediaTime) {
    if (qFuzzyCompare(mediaTime_ + 1.0, mediaTime + 1.0)) return;
    mediaTime_ = mediaTime;
    emit mediaTimeChanged();
    update();
}

QSGNode* VideoTelemetryHud::updatePaintNode(QSGNode* oldNode,
                                            QQuickItem::UpdatePaintNodeData*) {
    QSGNode* root = oldNode ? oldNode : new QSGNode;
    builder_.begin(window());

    if (!store_ || width() <= 0.0 || height() <= 0.0) {
        builder_.commit(root);
        return root;
    }

    const VideoHudSeries* hud = store_->primaryVideoHud();
    const omatrack::UnifiedLap* primary = store_->primaryUnified();
    const bool haveHud = hud && !hud->empty();
    if (haveHud || (primary && primary->size() >= 2)) {
        // The QML overlay is aspect-locked to 1000:210 (VideoTelemetryOverlay
        // aspectRatio 0.21), so the old painter scale(sx, sy) was uniform.
        const qreal s = width() / 1000.0;
        const auto P = [s](qreal x, qreal y) { return QPointF(x * s, y * s); };
        const auto R = [s](const QRectF& r) {
            return QRectF(r.x() * s, r.y() * s, r.width() * s, r.height() * s);
        };

        const omatrack::UnifiedLap* compare = store_->compareUnified();
        const double lapCursor = store_->cursorFrac();
        double cursor = lapCursor;
        if (haveHud && std::isfinite(mediaTime_) && hud->duration > 0.0) {
            const double offset = store_->primarySession()
                                      ? store_->primarySession()
                                            ->videoPresentationOffsetSec()
                                            .value_or(0.0)
                                      : 0.0;
            cursor =
                std::clamp((mediaTime_ - offset) / hud->duration, 0.0, 1.0);
        }
        const double compareCursor =
            compare ? store_->compareFractionForPrimaryFraction(std::clamp(
                          lapCursor - store_->referenceAlignment(), 0.0, 1.0))
                    : 0.0;

        constexpr qreal bandTop = 35.0;
        constexpr qreal bandBottom = 175.0;
        const QPointF dialCenter(902, 105);
        constexpr qreal dialRadius = 70.0;

        // Background band behind the graph + pedal/dial cluster.
        builder_.rect(
            R(QRectF(0, bandTop, dialCenter.x(), bandBottom - bandTop)),
            backgroundColor_);

        const QRectF graph(52, 39, 630, 132);
        builder_.rect(R(graph), QColor(20, 22, 22, 225));
        // Three quarter grid rules (1 px logical -> s px).
        for (int i = 1; i < 4; ++i) {
            const qreal y = graph.top() + graph.height() * i / 4.0;
            builder_.hLine(y * s, graph.left() * s, graph.right() * s, 1.0 * s,
                           QColor(255, 255, 255, 16));
        }

        // "TELEMETRY" label, rotated -90° on the left margin exactly as the
        // painter drew it: the text quad carries a transform node.
        {
            QFont labelFont(monoFontFamily_);
            labelFont.setBold(true);
            labelFont.setPixelSize(int(std::lround(12.0 * s)));
            builder_.rotatedText(QStringLiteral("TELEMETRY"), labelFont,
                                 foregroundColor_, QPointF(34.0 * s, 105.0 * s),
                                 Qt::AlignHCenter, -90.0);
        }

        const double primaryDuration =
            haveHud ? std::max(1.0, hud->duration)
            : !primary || primary->time.empty()
                ? 1.0
                : std::max(1.0, primary->time.back());
        const double halfWindow = std::min(0.18, 4.0 / primaryDuration);
        constexpr double currentMarkerFraction = 0.86;

        // Build the 180-point trace polyline in device space.
        const auto tracePoints = [&](const std::vector<double>& values,
                                     double maximum, bool reference) {
            QVector<QPointF> pts;
            if (values.size() < 2) return pts;
            constexpr int points = 180;
            pts.reserve(points);
            for (int i = 0; i < points; ++i) {
                const double local = double(i) / double(points - 1);
                const double primaryFraction = std::clamp(
                    cursor + (local - currentMarkerFraction) * 2.0 * halfWindow,
                    0.0, 1.0);
                const double fraction =
                    reference
                        ? store_->compareFractionForPrimaryFraction(std::clamp(
                              primaryFraction - store_->referenceAlignment(),
                              0.0, 1.0))
                        : primaryFraction;
                const double value =
                    std::clamp(sample(values, fraction) / maximum, 0.0, 1.0);
                pts.append(P(graph.left() + local * graph.width(),
                             graph.bottom() - value * graph.height()));
            }
            return pts;
        };

        // Emulate the painter's dash pattern along a device-space polyline.
        // Carry pattern state between segments instead of repeatedly using
        // fmod(cumulativeLength, period): a rounded phase at the pattern
        // boundary can produce a sub-ULP step and hang the render thread.
        const auto dashedPolyline = [&](const QVector<QPointF>& pts,
                                        qreal width, const QColor& color,
                                        qreal dashOn, qreal dashOff) {
            if (pts.size() < 2 || dashOn <= 0.0) return;
            if (dashOff <= 0.0) {
                builder_.polyline(pts.constData(), pts.size(), width, color);
                return;
            }

            bool drawing = true;
            qreal patternRemaining = dashOn;
            QVector<QPointF> run;
            for (int i = 1; i < pts.size(); ++i) {
                const QPointF a = pts[i - 1];
                const QPointF b = pts[i];
                const qreal segLen = std::hypot(b.x() - a.x(), b.y() - a.y());
                if (segLen < 1.0e-9) continue;

                const qreal tolerance =
                    std::numeric_limits<qreal>::epsilon() *
                    std::max({1.0, segLen, dashOn, dashOff}) * 16.0;
                qreal t = 0.0;
                while (segLen - t > tolerance) {
                    const qreal step = std::min(patternRemaining, segLen - t);
                    if (drawing) {
                        if (run.isEmpty())
                            run.append(a + (b - a) * (t / segLen));
                        run.append(a + (b - a) * ((t + step) / segLen));
                    }
                    t += step;
                    patternRemaining -= step;

                    if (patternRemaining <= tolerance) {
                        if (drawing && !run.isEmpty()) {
                            builder_.polyline(run.constData(), run.size(),
                                              width, color);
                            run.clear();
                        }
                        drawing = !drawing;
                        patternRemaining = drawing ? dashOn : dashOff;
                    }
                }
            }
            if (!run.isEmpty())
                builder_.polyline(run.constData(), run.size(), width, color);
        };

        const std::vector<double>& primaryThrottle =
            haveHud ? hud->throttle : primary->throttle;
        const std::vector<double>& primaryBrake =
            haveHud ? hud->brake : primary->brake;
        const std::vector<double>& primarySteer =
            haveHud ? hud->steering : primary->steering;
        const std::vector<double>& primarySpd =
            haveHud ? hud->speed : primary->speed;
        const auto samplePrimaryGear = [&](double fraction) {
            if (haveHud) return int(std::lround(sample(hud->gear, fraction)));
            return sampleGear(primary->gear, fraction);
        };

        const auto finiteMax = [](const std::vector<double>& values) {
            double peak = 0.0;
            for (double value : values)
                if (std::isfinite(value)) peak = std::max(peak, value);
            return peak;
        };
        const double brakeMax =
            std::max({1.0, finiteMax(primaryBrake),
                      compare ? finiteMax(compare->brake) : 0.0});

        if (compare) {
            const auto throttlePts = tracePoints(compare->throttle, 1.0, true);
            if (throttlePts.size() >= 2)
                dashedPolyline(throttlePts, 1.1 * s,
                               withAlpha(compareColor_, 55), 4.0 * s, 4.0 * s);
            const auto brakePts = tracePoints(compare->brake, brakeMax, true);
            if (brakePts.size() >= 2)
                dashedPolyline(brakePts, 1.1 * s, withAlpha(compareColor_, 42),
                               4.0 * s, 4.0 * s);
        }
        const double throttleScale =
            finiteMax(primaryThrottle) > 1.5 ? 100.0 : 1.0;
        const auto primaryThrottlePts =
            tracePoints(primaryThrottle, throttleScale, false);
        if (primaryThrottlePts.size() >= 2)
            builder_.polyline(primaryThrottlePts.constData(),
                              primaryThrottlePts.size(), 2.5 * s,
                              throttleColor_);
        const auto primaryBrakePts = tracePoints(primaryBrake, brakeMax, false);
        if (primaryBrakePts.size() >= 2)
            builder_.polyline(primaryBrakePts.constData(),
                              primaryBrakePts.size(), 2.5 * s, brakeColor_);

        // Current-position marker (vertical rule).
        const qreal currentMarkerX =
            graph.left() + graph.width() * currentMarkerFraction;
        builder_.vLine(currentMarkerX * s, graph.top() * s, graph.bottom() * s,
                       1.0 * s, withAlpha(foregroundColor_, 150));

        // Brake / throttle pedal bars.
        const double throttle = sample(primaryThrottle, cursor) / throttleScale;
        const double brake = sample(primaryBrake, cursor) / brakeMax;
        const double pedals[] = {brake, throttle};
        const QColor pedalColors[] = {brakeColor_, throttleColor_};
        for (int i = 0; i < 2; ++i) {
            const QRectF bar(714 + i * 30, bandTop, 24, bandBottom - bandTop);
            builder_.rect(R(bar), QColor(35, 35, 35, 245));
            const QRectF fill(bar.left() + 2,
                              bar.bottom() - 2 - pedals[i] * (bar.height() - 4),
                              bar.width() - 4, pedals[i] * (bar.height() - 4));
            builder_.rect(R(fill), withAlpha(pedalColors[i], 220));
        }

        // Steering dial: filled disc then ring outline.
        const QPointF dialCenterPx = P(dialCenter.x(), dialCenter.y());
        builder_.dot(dialCenterPx, dialRadius * s, QColor(35, 35, 35, 245));
        {
            const qreal r = dialRadius * s;
            QVector<QPointF> ring;
            const int segs = 64;
            ring.reserve(segs + 1);
            for (int i = 0; i <= segs; ++i) {
                const double ang = 2.0 * M_PI * double(i) / double(segs);
                ring.append(QPointF(dialCenterPx.x() + std::cos(ang) * r,
                                    dialCenterPx.y() + std::sin(ang) * r));
            }
            builder_.polyline(ring.constData(), ring.size(), 2.0 * s,
                              withAlpha(mutedColor_, 80));
        }

        constexpr double pi = 3.14159265358979323846;
        const auto steeringDirection = [](double steering) {
            constexpr double degreesToRadians = pi / 180.0;
            const double angle =
                std::clamp(steering, -180.0, 180.0) * degreesToRadians;
            return QPointF(-std::sin(angle), -std::cos(angle));
        };
        // Scanline-fill a convex polygon with 1px device rows; the only way to
        // produce a filled non-rect shape (diamond marker / gear arrow) with
        // the quad-only builder. MSAA antialiases the stair-stepped edges.
        const auto fillConvex = [&](const QVector<QPointF>& verts,
                                    const QColor& color) {
            if (verts.size() < 3) return;
            qreal minY = verts[0].y();
            qreal maxY = verts[0].y();
            for (const QPointF& p : verts) {
                minY = std::min(minY, p.y());
                maxY = std::max(maxY, p.y());
            }
            const int top = int(std::floor(minY));
            const int bottom = int(std::ceil(maxY));
            for (int row = top; row < bottom; ++row) {
                const qreal y = qreal(row) + 0.5;
                qreal xMin = std::numeric_limits<qreal>::max();
                qreal xMax = std::numeric_limits<qreal>::lowest();
                for (int i = 0, n = verts.size(); i < n; ++i) {
                    const QPointF& a = verts[i];
                    const QPointF& b = verts[(i + 1) % n];
                    if ((a.y() <= y && b.y() > y) ||
                        (b.y() <= y && a.y() > y)) {
                        const qreal t = (y - a.y()) / (b.y() - a.y());
                        const qreal x = a.x() + t * (b.x() - a.x());
                        xMin = std::min(xMin, x);
                        xMax = std::max(xMax, x);
                    }
                }
                if (xMax > xMin) builder_.hLine(y, xMin, xMax, 1.0, color);
            }
        };
        const auto drawSteeringMarker =
            [&](const QPointF& point, const QPointF& radial,
                const QColor& color, qreal radialSize, qreal tangentSize) {
                const QPointF tangent(-radial.y(), radial.x());
                const qreal rs = radialSize * s;
                const qreal ts = tangentSize * s;
                QVector<QPointF> verts;
                verts.reserve(4);
                verts.append(point + radial * rs);
                verts.append(point + tangent * ts);
                verts.append(point - radial * rs);
                verts.append(point - tangent * ts);
                fillConvex(verts, color);
            };

        const double primarySteering = sample(primarySteer, cursor);
        const QPointF primarySteeringDirection =
            steeringDirection(primarySteering);
        const QPointF primarySteeringPoint =
            dialCenterPx + primarySteeringDirection * (dialRadius * s);
        if (compare) {
            const double referenceSteering =
                sample(compare->steering, compareCursor);
            const QPointF referenceSteeringDirection =
                steeringDirection(referenceSteering);
            const QPointF referenceSteeringPoint =
                dialCenterPx + referenceSteeringDirection * (dialRadius * s);
            const double steeringSweep =
                std::remainder(primarySteering - referenceSteering, 360.0);
            const int arcSegments =
                std::max(1, int(std::ceil(std::abs(steeringSweep) / 5.0)));
            QVector<QPointF> arc;
            arc.reserve(arcSegments + 1);
            arc.append(referenceSteeringPoint);
            for (int segment = 1; segment <= arcSegments; ++segment) {
                const double steeringAlongArc =
                    referenceSteering +
                    steeringSweep * double(segment) / double(arcSegments);
                arc.append(dialCenterPx + steeringDirection(steeringAlongArc) *
                                              (dialRadius * s));
            }
            builder_.polyline(arc.constData(), arc.size(), 1.5 * s,
                              withAlpha(compareColor_, 65));
            drawSteeringMarker(referenceSteeringPoint,
                               referenceSteeringDirection,
                               withAlpha(compareColor_, 210), 5.0, 3.5);
        }
        drawSteeringMarker(primarySteeringPoint, primarySteeringDirection,
                           steeringColor_, 8.0, 6.0);

        // Gear readout + delta arrow.
        const int primaryGear = samplePrimaryGear(cursor);
        const int referenceGear =
            compare ? sampleGear(compare->gear, compareCursor) : primaryGear;
        const int gearDelta = primaryGear - referenceGear;
        const QColor gearColor = gearDelta < 0   ? brakeColor_
                                 : gearDelta > 0 ? throttleColor_
                                                 : foregroundColor_;
        {
            QFont gearFont(monoFontFamily_);
            gearFont.setBold(true);
            gearFont.setPixelSize(int(std::lround(52.0 * s)));
            builder_.text(QString::number(primaryGear), gearFont, gearColor,
                          R(QRectF(842, 42, 120, 55)), Qt::AlignCenter);
        }
        if (gearDelta != 0) {
            constexpr qreal triangleCenterX = 944.0;
            constexpr qreal triangleCenterY = 73.0;
            constexpr qreal triangleHalfWidth = 6.0;
            constexpr qreal triangleHalfHeight = 5.0;
            QVector<QPointF> tri;
            tri.reserve(3);
            if (gearDelta > 0) {
                tri.append(
                    P(triangleCenterX, triangleCenterY - triangleHalfHeight));
                tri.append(P(triangleCenterX + triangleHalfWidth,
                             triangleCenterY + triangleHalfHeight));
                tri.append(P(triangleCenterX - triangleHalfWidth,
                             triangleCenterY + triangleHalfHeight));
            } else {
                tri.append(P(triangleCenterX - triangleHalfWidth,
                             triangleCenterY - triangleHalfHeight));
                tri.append(P(triangleCenterX + triangleHalfWidth,
                             triangleCenterY - triangleHalfHeight));
                tri.append(
                    P(triangleCenterX, triangleCenterY + triangleHalfHeight));
            }
            fillConvex(tri, gearColor);
        }

        // Speed readout.
        {
            QFont kmhFont(monoFontFamily_);
            kmhFont.setBold(true);
            kmhFont.setPixelSize(int(std::lround(17.0 * s)));
            builder_.text(QStringLiteral("km/h"), kmhFont, foregroundColor_,
                          R(QRectF(842, 98, 120, 20)), Qt::AlignCenter);
        }
        const double primarySpeed = sample(primarySpd, cursor);
        const double referenceSpeed =
            compare ? sample(compare->speed, compareCursor) : primarySpeed;
        const double speedDelta = primarySpeed - referenceSpeed;
        QColor speedColor = mutedColor_;
        if (compare && std::abs(speedDelta) > 1.0) {
            constexpr double fullColorDifference = 5.0;
            const double colorMix =
                (std::abs(speedDelta) - 1.0) / (fullColorDifference - 1.0);
            speedColor = mixColors(
                mutedColor_, speedDelta > 0.0 ? throttleColor_ : brakeColor_,
                colorMix);
        }
        {
            QFont speedFont(monoFontFamily_);
            speedFont.setBold(true);
            speedFont.setPixelSize(int(std::lround(23.0 * s)));
            builder_.text(QString::number(qRound(primarySpeed)), speedFont,
                          speedColor, R(QRectF(842, 118, 120, 29)),
                          Qt::AlignCenter);
        }

        // Reference time-delta readout.
        if (compare) {
            const QVector<double>& deltaTrace = store_->deltaTrace();
            if (!deltaTrace.isEmpty()) {
                const double position = std::clamp(cursor, 0.0, 1.0) *
                                        double(deltaTrace.size() - 1);
                const qsizetype low = qsizetype(std::floor(position));
                const qsizetype high = std::min(low + 1, deltaTrace.size() - 1);
                const double timeDelta =
                    deltaTrace[low] + (deltaTrace[high] - deltaTrace[low]) *
                                          (position - double(low));
                const QColor deltaColor = timeDelta > 0.01    ? brakeColor_
                                          : timeDelta < -0.01 ? throttleColor_
                                                              : mutedColor_;
                QFont deltaFont(monoFontFamily_);
                deltaFont.setBold(true);
                deltaFont.setPixelSize(int(std::lround(12.0 * s)));
                builder_.text(
                    QStringLiteral("ΔT %1%2s")
                        .arg(timeDelta >= 0.0 ? QStringLiteral("+") : QString())
                        .arg(timeDelta, 0, 'f', 3),
                    deltaFont, deltaColor, R(QRectF(834, 150, 136, 18)),
                    Qt::AlignCenter);
            }
        }
    }

    builder_.commit(root);
    return root;
}

void VideoTelemetryHud::releaseResources() { builder_.releaseResources(); }
