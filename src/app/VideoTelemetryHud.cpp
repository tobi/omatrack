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
        // Cursor movement only retraces, never rebuilds the cached
        // compare-fraction map / brake peak, so it must not dirty the snapshot.
        connect(store_, &TelemetryStore::cursorFracChanged, this,
                [this]() { update(); });
        connect(store_, &TelemetryStore::selectionChanged, this, [this]() {
            snapshotDirty_ = true;
            update();
        });
        connect(store_, &TelemetryStore::referenceAlignmentChanged, this,
                [this]() {
                    snapshotDirty_ = true;
                    update();
                });
        connect(store_, &TelemetryStore::comparisonSyncStrategyChanged, this,
                [this]() {
                    snapshotDirty_ = true;
                    update();
                });
        connect(store_, &TelemetryStore::lapLoadingChanged, this, [this]() {
            snapshotDirty_ = true;
            update();
        });
        connect(store_, &TelemetryStore::overlayStyleChanged, this,
                [this]() { update(); });
    }
    snapshotDirty_ = true;
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

    // Signals mark the snapshot dirty, but a lap object can also be replaced
    // underneath an unchanged selection (re-adopting a loaded lap). The
    // pointer compare catches that before a freed lap is read.
    if (snapshotDirty_ || snapshot_.primary != store_->primaryUnified() ||
        snapshot_.compare != store_->compareUnified()) {
        snapshot_ = store_->traceSnapshot();
        snapshotDirty_ = false;
        brakeMaxDirty_ = true;
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

        const omatrack::UnifiedLap* compare = snapshot_.compare;
        const double lapCursor = store_->cursorFrac();
        double cursor = lapCursor;
        if (haveHud && std::isfinite(mediaTime_) && hud->duration > 0.0) {
            const auto telemetryTime =
                store_->primarySession()
                    ? store_->primarySession()->videoTelemetryTime(mediaTime_)
                    : std::nullopt;
            if (telemetryTime)
                cursor = std::clamp(*telemetryTime / hud->duration, 0.0, 1.0);
        }
        const double compareCursor =
            compare ? snapshot_.compareFractionForPrimaryFraction(lapCursor)
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

        // The strip is a window of track progress, not time. X is lap
        // fraction around the current station so both cars answer "what
        // was the pedal here?" and a slower sector cannot stretch one
        // trace past the other.
        constexpr double currentMarkerFraction = 0.86;
        constexpr double kWindowFrac = 0.10;

        const auto finiteMax = [](const std::vector<double>& values) {
            double peak = 0.0;
            for (double value : values)
                if (std::isfinite(value)) peak = std::max(peak, value);
            return peak;
        };

        const bool haveLap = primary && primary->size() >= 2;
        const std::vector<double>& primaryThrottle = haveLap ? primary->throttle
                                                     : haveHud
                                                         ? hud->throttle
                                                         : primary->throttle;
        const std::vector<double>& primaryBrake = haveLap   ? primary->brake
                                                  : haveHud ? hud->brake
                                                            : primary->brake;
        const std::vector<double>& primarySteer = haveLap   ? primary->steering
                                                  : haveHud ? hud->steering
                                                            : primary->steering;
        const std::vector<double>& primarySpd = haveLap   ? primary->speed
                                                : haveHud ? hud->speed
                                                          : primary->speed;
        const auto samplePrimaryGear = [&](double fraction) {
            if (haveLap) return sampleGear(primary->gear, fraction);
            if (haveHud) return int(std::lround(sample(hud->gear, fraction)));
            return sampleGear(primary->gear, fraction);
        };
        const double nowFrac = haveLap ? lapCursor : cursor;

        // Brake scale and throttle scale are scans over full arrays; cache
        // them per selection (refreshed when the snapshot is) rather than
        // every frame.
        if (brakeMaxDirty_) {
            cachedBrakeMax_ =
                std::max({1.0, finiteMax(primaryBrake),
                          compare ? finiteMax(compare->brake) : 0.0});
            cachedThrottleScale_ =
                finiteMax(primaryThrottle) > 1.5 ? 100.0 : 1.0;
            brakeMaxDirty_ = false;
        }
        const double brakeMax = cachedBrakeMax_;
        const double throttleScale = cachedThrottleScale_;

        // The strip is a window of track progress, not time. X is lap
        // fraction around the current station so both cars answer "what
        // was the pedal here?" and a slower sector cannot stretch one
        // trace past the other.
        const double windowStart =
            lapCursor - currentMarkerFraction * kWindowFrac;
        const QRectF graphRect = R(graph);
        const auto drawTrace = [&](const std::vector<double>& values,
                                   double maximum, bool reference, qreal width,
                                   const QColor& color) {
            if (values.size() < 2 || !primary || primary->size() < 2 ||
                maximum <= 0.0)
                return;
            auto sourceFraction = [&](double viewportFrac) -> double {
                const double pf = std::clamp(viewportFrac, 0.0, 1.0);
                return reference
                           ? snapshot_.compareFractionForPrimaryFraction(pf)
                           : pf;
            };
            TraceSceneBuilder::EnvelopeStyle style;
            style.width = width;
            style.color = color;
            builder_.envelopePolyline(values, sourceFraction, windowStart,
                                      kWindowFrac, graphRect, 0.0, maximum,
                                      style, 0.0, 1.0);
        };

        const bool whiteRef = store_ && store_->overlayRefWhite();
        const QString refStyle =
            store_ ? store_->overlayRefStyle() : QStringLiteral("dashed");
        const QColor refColor =
            whiteRef ? QColor(Qt::white)
                     : QColor(store_ && !store_->overlayRefColor().isEmpty()
                                  ? store_->overlayRefColor()
                                  : QStringLiteral("#e09d7f"));
        const qreal refWidth = whiteRef ? 2.4 * s : 1.5 * s;
        auto refDash = TraceSceneBuilder::EnvelopeStyle::Dash::Dashed;
        if (refStyle == QLatin1String("dotted"))
            refDash = TraceSceneBuilder::EnvelopeStyle::Dash::Dotted;
        else if (refStyle == QLatin1String("solid"))
            refDash = TraceSceneBuilder::EnvelopeStyle::Dash::Solid;
        const auto drawStyled =
            [&](const std::vector<double>& values, double maximum,
                bool reference, qreal width, const QColor& color,
                TraceSceneBuilder::EnvelopeStyle::Dash dash) {
                if (values.size() < 2 || !primary || primary->size() < 2 ||
                    maximum <= 0.0)
                    return;
                auto sourceFraction = [&](double viewportFrac) -> double {
                    const double pf = std::clamp(viewportFrac, 0.0, 1.0);
                    return reference
                               ? snapshot_.compareFractionForPrimaryFraction(pf)
                               : pf;
                };
                TraceSceneBuilder::EnvelopeStyle style;
                style.width = width;
                style.color = color;
                style.dash = dash;
                builder_.envelopePolyline(values, sourceFraction, windowStart,
                                          kWindowFrac, graphRect, 0.0, maximum,
                                          style, 0.0, 1.0);
            };
        if (compare) {
            drawStyled(compare->throttle, 1.0, true, refWidth,
                       withAlpha(whiteRef ? QColor(Qt::white) : refColor, 200),
                       refDash);
            drawStyled(compare->brake, brakeMax, true, refWidth,
                       withAlpha(whiteRef ? QColor(Qt::white) : refColor, 200),
                       refDash);
        }
        drawTrace(primaryThrottle, throttleScale, false, 2.5 * s,
                  throttleColor_);
        drawTrace(primaryBrake, brakeMax, false, 2.5 * s, brakeColor_);

        // Current-position marker (vertical rule).
        const qreal currentMarkerX =
            graph.left() + graph.width() * currentMarkerFraction;
        builder_.vLine(currentMarkerX * s, graph.top() * s, graph.bottom() * s,
                       1.0 * s, withAlpha(foregroundColor_, 150));

        // Brake / throttle pedal bars.
        const double throttle =
            sample(primaryThrottle, nowFrac) / throttleScale;
        const double brake = sample(primaryBrake, nowFrac) / brakeMax;
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

        // Steering wheel: 10 px black rim, dark disc for the readouts.
        const QPointF dialCenterPx = P(dialCenter.x(), dialCenter.y());
        constexpr qreal ringWidth = 10.0;
        const qreal innerRadius = dialRadius - ringWidth;
        builder_.dot(dialCenterPx, dialRadius * s, QColor(0, 0, 0, 255));
        builder_.dot(dialCenterPx, innerRadius * s, QColor(35, 35, 35, 245));

        constexpr double pi = 3.14159265358979323846;
        const auto steeringDirection = [](double steering) {
            constexpr double degreesToRadians = pi / 180.0;
            const double angle =
                std::clamp(steering, -180.0, 180.0) * degreesToRadians;
            return QPointF(-std::sin(angle), -std::cos(angle));
        };
        // Scanline-fill a convex polygon with 1px device rows; the only way to
        // produce a filled non-rect shape (rim notch / gear arrow) with the
        // quad-only builder. MSAA antialiases the stair-stepped edges.
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
        // A 5×10 (primary) or narrower (reference) notch sitting in the rim.
        const auto drawRingNotch = [&](double steering, const QColor& color,
                                       qreal tangentWidth) {
            const QPointF radial = steeringDirection(steering);
            const QPointF tangent(-radial.y(), radial.x());
            const QPointF outer = dialCenterPx + radial * (dialRadius * s);
            const QPointF inner = dialCenterPx + radial * (innerRadius * s);
            const qreal half = 0.5 * tangentWidth * s;
            convexScratch_.clear();
            convexScratch_.reserve(4);
            convexScratch_.append(outer + tangent * half);
            convexScratch_.append(outer - tangent * half);
            convexScratch_.append(inner - tangent * half);
            convexScratch_.append(inner + tangent * half);
            fillConvex(convexScratch_, color);
        };

        const double primarySteering = sample(primarySteer, nowFrac);
        if (compare) {
            const double referenceSteering =
                sample(compare->steering, compareCursor);
            drawRingNotch(referenceSteering, QColor(150, 150, 150, 230), 3.0);
        }
        drawRingNotch(primarySteering, QColor(255, 255, 255, 255), 5.0);

        // Gear + speed, centred on the wheel. The shift triangle sits
        // immediately beside the digit, not out on the rim.
        const int primaryGear = samplePrimaryGear(nowFrac);
        const int referenceGear =
            compare ? sampleGear(compare->gear, compareCursor) : primaryGear;
        const int gearDelta = primaryGear - referenceGear;
        const QColor gearColor = gearDelta < 0   ? brakeColor_
                                 : gearDelta > 0 ? throttleColor_
                                                 : foregroundColor_;
        constexpr qreal dialCx = 902.0;
        {
            QFont gearFont(monoFontFamily_);
            gearFont.setBold(true);
            gearFont.setPixelSize(int(std::lround(50.0 * s)));
            builder_.text(QString::number(primaryGear), gearFont, gearColor,
                          R(QRectF(dialCx - 28.0, 64.0, 56.0, 50.0)),
                          Qt::AlignCenter);
        }
        if (gearDelta != 0) {
            constexpr qreal triangleCenterX = dialCx + 26.0;
            constexpr qreal triangleCenterY = 89.0;
            constexpr qreal triangleHalfWidth = 5.0;
            constexpr qreal triangleHalfHeight = 4.5;
            convexScratch_.clear();
            convexScratch_.reserve(3);
            if (gearDelta > 0) {
                convexScratch_.append(
                    P(triangleCenterX, triangleCenterY - triangleHalfHeight));
                convexScratch_.append(P(triangleCenterX + triangleHalfWidth,
                                        triangleCenterY + triangleHalfHeight));
                convexScratch_.append(P(triangleCenterX - triangleHalfWidth,
                                        triangleCenterY + triangleHalfHeight));
            } else {
                convexScratch_.append(P(triangleCenterX - triangleHalfWidth,
                                        triangleCenterY - triangleHalfHeight));
                convexScratch_.append(P(triangleCenterX + triangleHalfWidth,
                                        triangleCenterY - triangleHalfHeight));
                convexScratch_.append(
                    P(triangleCenterX, triangleCenterY + triangleHalfHeight));
            }
            fillConvex(convexScratch_, gearColor);
        }

        {
            QFont kmhFont(monoFontFamily_);
            kmhFont.setBold(true);
            kmhFont.setPixelSize(int(std::lround(14.0 * s)));
            builder_.text(QStringLiteral("km/h"), kmhFont, foregroundColor_,
                          R(QRectF(dialCx - 36.0, 112.0, 72.0, 16.0)),
                          Qt::AlignCenter);
        }
        const double primarySpeed = sample(primarySpd, nowFrac);
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
            speedFont.setPixelSize(int(std::lround(24.0 * s)));
            builder_.text(
                QString::number(qRound(primarySpeed)), speedFont, speedColor,
                R(QRectF(dialCx - 40.0, 126.0, 80.0, 28.0)), Qt::AlignCenter);
        }
    }

    builder_.commit(root);
    return root;
}

void VideoTelemetryHud::releaseResources() { builder_.releaseResources(); }
