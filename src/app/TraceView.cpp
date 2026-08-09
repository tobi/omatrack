#include "TraceView.h"

#include "TelemetryStore.h"
#include "core/TelemetryEngine.h"

#include <QCursor>
#include <QFont>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

using namespace omatrack;

namespace {

constexpr double kTopPad = 22.0;
constexpr double kBottomPad = 18.0;
constexpr double kLabelW = 62.0;
constexpr int kCursorSamplesPerStep = 1;
constexpr qint64 kHoverFrameMs = 16;

const QColor kGrid("#343f44");
const QColor kGridStrong("#475258");
const QColor kForeground("#d3c6aa");
const QColor kMuted("#9da9a0");
const QColor kDim("#4f585e");
const QColor kAccent("#7fbbb3");
const QColor kGreen("#a7c080");
const QColor kRed("#e67e80");
const QColor kOrange("#e09d7f");
const QColor kMagenta("#d699b6");

QColor alpha(QColor c, int a) {
    c.setAlpha(a);
    return c;
}

constexpr std::array<QRgb, 7> kDriverColors = {
    qRgb(0xef, 0xbe, 0x3f), qRgb(0x40, 0xd1, 0x70), qRgb(0x40, 0x8f, 0xe0),
    qRgb(0xd1, 0x61, 0xd1), qRgb(0xe0, 0x40, 0x40), qRgb(0xbf, 0xbf, 0xbf),
    qRgb(0x40, 0xd1, 0xd1)};

}  // namespace

TraceView::TraceView(QQuickItem* parent) : QQuickPaintedItem(parent) {
    canvasFont_.setFamily(QStringLiteral("Geist Mono"));
    canvasFont_.setPointSizeF(8.5);
    emptyStateFont_.setFamily(QStringLiteral("Geist Mono"));
    emptyStateFont_.setPointSize(11);
    labelFont_.setFamily(QStringLiteral("Geist Mono"));
    labelFont_.setPointSizeF(8.0);
    labelFont_.setBold(true);
    unitFont_.setFamily(QStringLiteral("Geist Mono"));
    unitFont_.setPointSizeF(7.0);
    stickyFont_.setFamily(QStringLiteral("Geist Mono"));
    stickyFont_.setPointSize(6);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton |
                            Qt::RightButton);
    setAcceptHoverEvents(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, false);
    setFocusPolicy(Qt::StrongFocus);
    setAntialiasing(true);
    setOpaquePainting(true);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    rebuildChannelSpecs();
}

void TraceView::setBackgroundColor(const QColor& color) {
    if (!color.isValid() || backgroundColor_ == color) return;
    backgroundColor_ = color;
    update();
    emit backgroundColorChanged();
}

void TraceView::setStore(TelemetryStore* store) {
    if (store_ == store) return;
    if (store_) disconnect(store_, nullptr, this, nullptr);
    store_ = store;
    if (store_) {
        connect(store_, &TelemetryStore::selectionChanged, this, [this]() {
            selectionStart_ = -1.0;
            selectionEnd_ = -1.0;
            selecting_ = false;
            rebuildChannelSpecs();
            invalidateGeometry();
        });
        connect(store_, &TelemetryStore::cursorFracChanged, this,
                [this]() { emit overlayChanged(); });
        connect(store_, &TelemetryStore::viewChanged, this,
                &TraceView::invalidateStaticLayer);
        connect(store_, &TelemetryStore::cornersChanged, this,
                &TraceView::invalidateStaticLayer);
        connect(store_, &TelemetryStore::editingCornersChanged, this,
                &TraceView::invalidateStaticLayer);
        connect(store_, &TelemetryStore::channelHeightChanged, this,
                &TraceView::invalidateStaticLayer);
        connect(store_, &TelemetryStore::channelConfigChanged, this, [this]() {
            rebuildChannelSpecs();
            invalidateGeometry();
        });
        connect(store_, &TelemetryStore::referenceAlignmentChanged, this,
                &TraceView::invalidateStaticLayer);
    }
    invalidateGeometry();
    emit storeChanged();
}

void TraceView::invalidateStaticLayer() {
    update();
    emit overlayChanged();
}

void TraceView::invalidateGeometry() {
    geometryCache_.clear();
    deltaRaster_ = QImage();
    deltaMaxAbs_ = 0.001;
    invalidateStaticLayer();
}

void TraceView::rebuildChannelSpecs() {
    channelSpecs_.clear();
    // ordered like omatrack: delta on top, then main channels
    auto add = [&](const QString& key, const QString& title,
                   const QString& unit, QColor color, Clamp clamp, bool filled,
                   bool dots, const QString& field) {
        ChannelSpec s;
        s.key = key;
        s.title = title;
        s.unit = unit;
        s.color = color;
        s.clamp = clamp;
        s.filled = filled;
        s.showDots = dots;
        s.field = field;
        channelSpecs_.append(s);
    };
    add("delta", "Δ Time", "s", QColor("#83c092"), Clamp{0, 0, true, true},
        true, false, "");
    add("speed", "Speed", "km/h", QColor("#a7c080"), Clamp{0, 0, true, false},
        false, true, "speed");
    add("throttle", "Throttle", "%", QColor("#a7c080"),
        Clamp{0, 1, false, false}, true, false, "throttle");
    add("brake", "Brake", "bar", QColor("#e67e80"), Clamp{0, 0, true, false},
        true, false, "brake");
    add("steering", "Steering", "deg", QColor("#dbbc7f"),
        Clamp{0, 0, true, true}, false, true, "steering");
    add("gear", "Gear", "", QColor("#d699b6"), Clamp{0, 7, false, false}, false,
        true, "gear");
    add("dampers", "Dampers", "mm", QColor("#7fbbb3"), Clamp{0, 0, true, true},
        false, false, "damperFL");
    add("g_long", "G Long", "g", QColor("#e09d7f"), Clamp{0, 0, true, true},
        false, false, "gForceLong");
    add("clutch", "Clutch", "%", QColor("#d3c6aa"), Clamp{0, 1, false, false},
        true, false, "clutch");
    add("driver_throttle", "Driver throttle", "%", QColor("#9da9a0"),
        Clamp{0, 1, false, false}, false, false, "driverThrottle");
    add("gps_lat", "GPS latitude", "°", QColor("#83c092"),
        Clamp{0, 0, true, false}, false, false, "gpsLat");
    add("gps_lon", "GPS longitude", "°", QColor("#e09d7f"),
        Clamp{0, 0, true, false}, false, false, "gpsLon");
    if (store_) {
        for (const QVariant& item : store_->channelSettings()) {
            const QVariantMap row = item.toMap();
            if (!row.value(QStringLiteral("source")).toBool()) continue;
            const QString key = row.value(QStringLiteral("key")).toString();
            if (key.isEmpty()) continue;
            QColor color(store_->channelColor(key));
            if (!color.isValid()) color = QColor("#9da9a0");
            add(key, row.value(QStringLiteral("title")).toString(),
                row.value(QStringLiteral("unit")).toString(), color,
                Clamp{0, 0, true, false}, false, false, key);
        }
    }
}

double TraceView::xForFrac(double frac) const {
    return kLabelW + (frac - store_->viewStart()) / store_->viewSpan() *
                         (width() - kLabelW);
}

double TraceView::fracForX(double x) const {
    double w = std::max(1.0, width() - kLabelW);
    return store_->viewStart() +
           std::clamp((x - kLabelW) / w, 0.0, 1.0) * store_->viewSpan();
}

bool TraceView::isSticky(const QString& key) const {
    return stickyChannels_.contains(key);
}

double TraceView::rowHeightFor(const ChannelSpec& spec) const {
    const double weight = std::max(0.5, store_->channelWeight(spec.key));
    const double speedBoost = spec.key == QStringLiteral("speed") ? 1.35 : 1.0;
    return std::max(48.0,
                    double(store_->channelHeight()) * weight * speedBoost);
}

const std::vector<double>* TraceView::fieldFor(const UnifiedLap& lap,
                                               const QString& field) const {
    if (field == "speed") return &lap.speed;
    if (field == "throttle") return &lap.throttle;
    if (field == "driverThrottle") return &lap.driverThrottle;
    if (field == "brake") return &lap.brake;
    if (field == "clutch") return &lap.clutch;
    if (field == "steering") return &lap.steering;
    if (field == "distance") return &lap.distance;
    if (field == "gForceLong") return &lap.gForceLong;
    if (field == "damperFL") return &lap.damperFL;
    if (field == "damperFR") return &lap.damperFR;
    if (field == "damperRL") return &lap.damperRL;
    if (field == "damperRR") return &lap.damperRR;
    if (field == "gpsLat") return &lap.gpsLat;
    if (field == "gpsLon") return &lap.gpsLon;
    if (field == "gear") {
        // gear is std::vector<int>; expose a converted copy for painting.
        // Alternate buffers so a second fieldFor() for the compare lap can't
        // silently alias the first lap's data.
        int idx = scratchIdx_++;
        auto& buf = scratch_[idx & 1];
        buf.resize(lap.gear.size());
        for (size_t i = 0; i < lap.gear.size(); ++i)
            buf[i] = double(lap.gear[i]);
        return &buf;
    }
    return nullptr;
}

QColor TraceView::colorForDriver() const {
    if (!store_ || !store_->primarySession()) return QColor(0x80, 0x80, 0x80);
    const QString driver = store_->primarySession()->driver().toCaseFolded();
    if (driver.isEmpty()) return QColor(0x80, 0x80, 0x80);
    return QColor::fromRgb(kDriverColors[qHash(driver) % kDriverColors.size()]);
}

// ── paint ───────────────────────────────────────────────────────────

void TraceView::paint(QPainter* painter) {
    paintStatic(painter);
    QMetaObject::invokeMethod(
        this, [this]() { emit overlayChanged(); }, Qt::QueuedConnection);
}

void TraceView::paintStatic(QPainter* painter) {
    cursorLanes_.clear();
    cursorTop_ = 0.0;
    cursorBottom_ = 0.0;
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->fillRect(boundingRect(), backgroundColor_);

    if (!store_) {
        painter->setPen(kDim);
        painter->setFont(emptyStateFont_);
        painter->drawText(boundingRect(), Qt::AlignCenter,
                          QStringLiteral("Select a session to begin"));
        return;
    }

    const UnifiedLap* primary = store_->primaryUnified();
    if (!primary || primary->size() < 2) {
        painter->setPen(kDim);
        painter->setFont(emptyStateFont_);
        painter->drawText(boundingRect(), Qt::AlignCenter,
                          QStringLiteral("Select a session to begin"));
        return;
    }
    const UnifiedLap* compare = store_->compareUnified();

    QVector<int> stickyIdx;
    QVector<int> scrollIdx;
    for (int i = 0; i < channelSpecs_.size(); ++i) {
        const ChannelSpec& spec = channelSpecs_[i];
        if (spec.key == "delta") {
            if (!compare || !store_->channelVisible(spec.key)) continue;
        } else if (!store_->channelVisible(spec.key)) {
            continue;
        }
        (isSticky(spec.key) ? stickyIdx : scrollIdx).append(i);
    }
    if (stickyIdx.isEmpty() && scrollIdx.isEmpty()) return;

    painter->setFont(canvasFont_);

    auto rowHeight = [&](int idx) { return rowHeightFor(channelSpecs_[idx]); };
    double stickyHeight = 0.0;
    for (int idx : stickyIdx) stickyHeight += rowHeight(idx);
    double secondaryContentHeight = 0.0;
    for (int idx : scrollIdx) secondaryContentHeight += rowHeight(idx);

    const double secondaryTop = kTopPad + stickyHeight;
    const double secondaryBottom =
        std::max(secondaryTop, height() - kBottomPad);
    const double secondaryViewport =
        std::max(0.0, secondaryBottom - secondaryTop);
    const double maxScroll =
        std::max(0.0, secondaryContentHeight - secondaryViewport);
    secondaryScroll_ = std::clamp(secondaryScroll_, 0.0, maxScroll);

    auto drawRow = [&](int idx, double y) {
        const ChannelSpec& spec = channelSpecs_[idx];
        const double rowH = rowHeight(idx);
        const QRectF rect(kLabelW, y, width() - kLabelW, rowH);
        if (spec.key == "delta") {
            paintDelta(*painter, rect);
        } else {
            paintChannel(*painter, spec, rect, idx, primary, compare, nullptr,
                         nullptr);
        }
        painter->setPen(QPen(alpha(kGridStrong, 110), 1));
        painter->drawLine(QPointF(kLabelW, y), QPointF(width(), y));
    };

    double y = kTopPad;
    for (int idx : stickyIdx) {
        drawRow(idx, y);
        y += rowHeight(idx);
    }

    if (!scrollIdx.isEmpty() && secondaryViewport > 0.0) {
        painter->save();
        painter->setClipRect(
            QRectF(0, secondaryTop, width(), secondaryViewport));
        y = secondaryTop - secondaryScroll_;
        for (int idx : scrollIdx) {
            drawRow(idx, y);
            y += rowHeight(idx);
        }
        painter->restore();

        if (maxScroll > 0.0) {
            const double trackTop = secondaryTop + 3.0;
            const double trackHeight = std::max(12.0, secondaryViewport - 6.0);
            const double thumbHeight = std::max(
                18.0, trackHeight * secondaryViewport / secondaryContentHeight);
            const double thumbY = trackTop + (trackHeight - thumbHeight) *
                                                 (secondaryScroll_ / maxScroll);
            painter->setPen(Qt::NoPen);
            painter->setBrush(alpha(kMuted, 70));
            painter->drawRoundedRect(
                QRectF(width() - 5, thumbY, 3, thumbHeight), 1.5, 1.5);
        }
    }

    const QRectF axisRect(kLabelW, height() - kBottomPad, width() - kLabelW,
                          kBottomPad);
    painter->setPen(kGridStrong);
    painter->drawLine(QPointF(kLabelW, axisRect.top()),
                      QPointF(width(), axisRect.top()));
    if (!primary->distance.empty()) {
        const int n = int(primary->size());
        const int divisions = std::max(2, int((width() - kLabelW) / 120.0));
        const double totalDistance = primary->distance.back();
        double step = totalDistance / divisions;
        if (step < 50)
            step = 50;
        else if (step < 100)
            step = 100;
        else if (step < 200)
            step = 200;
        else if (step < 500)
            step = 500;
        else
            step = 1000;

        for (double distance = 0; distance <= totalDistance; distance += step) {
            const auto it = std::lower_bound(primary->distance.begin(),
                                             primary->distance.end(), distance);
            int sample =
                std::clamp(int(it - primary->distance.begin()), 0, n - 1);
            if (sample > 0 &&
                std::fabs(primary->distance[sample - 1] - distance) <
                    std::fabs(primary->distance[sample] - distance))
                --sample;
            const double fraction = double(sample) / double(n - 1);
            if (fraction < store_->viewStart() || fraction > store_->viewEnd())
                continue;
            const double x = xForFrac(fraction);
            painter->drawLine(QPointF(x, axisRect.top()),
                              QPointF(x, axisRect.top() + 4));
            painter->setPen(kMuted);
            painter->drawText(QPointF(x + 3, axisRect.top() + 12),
                              QString("%1m").arg(int(distance)));
        }
    }

    const double traceHeight = std::max(0.0, height() - kTopPad - kBottomPad);
    paintCornerZones(*painter,
                     QRectF(kLabelW, kTopPad, width() - kLabelW, traceHeight));

    cursorTop_ = kTopPad - 3;
    cursorBottom_ = height() - kBottomPad;
}

void TraceView::paintCursorOverlay(QPainter* painter) {
    if (!store_ || cursorLanes_.isEmpty()) return;
    const UnifiedLap* primary = store_->primaryUnified();
    if (!primary) return;
    const UnifiedLap* compare = store_->compareUnified();
    const double fraction = store_->cursorFrac();
    const double cursorX = xForFrac(fraction);

    painter->setRenderHint(QPainter::Antialiasing, true);
    paintSelectionOverlay(painter);
    painter->setPen(QPen(alpha(kAccent, 190), 1));
    painter->drawLine(QPointF(cursorX, cursorTop_),
                      QPointF(cursorX, cursorBottom_));

    for (const CursorLane& lane : cursorLanes_) {
        const std::vector<double>* primaryData =
            lane.field.startsWith(QStringLiteral("raw:"))
                ? store_->extraChannelData(lane.field, false)
                : fieldFor(*primary, lane.field);
        if (!primaryData || primaryData->empty()) continue;
        const double span = std::max(1.0e-12, lane.max - lane.min);
        auto toY = [&](double value) {
            return lane.rect.top() +
                   (1.0 - (value - lane.min) / span) * lane.rect.height();
        };
        const int sample =
            std::min(int(primaryData->size()) - 1,
                     int(fraction * double(primaryData->size() - 1)));
        const double value = (*primaryData)[sample];

        painter->setPen(Qt::NoPen);
        painter->setBrush(lane.color);
        painter->drawEllipse(QPointF(cursorX, toY(value)), 2.5, 2.5);

        QString valueText;
        if (lane.field == "speed")
            valueText = QString::number(qRound(value));
        else if (lane.field == "throttle")
            valueText = QString("%1%").arg(qRound(value * 100.0));
        else if (lane.field == "brake")
            valueText = QString::number(value, 'f', 1);
        else if (lane.field == "steering")
            valueText = QString("%1°").arg(qRound(value));
        else if (lane.field == "gear")
            valueText = QString::number(qRound(value));
        else if (lane.field.startsWith("damper"))
            valueText = QString::number(value, 'f', 1);
        else
            valueText = QString::number(value, 'f', 2);

        const QRectF valueRect(0, lane.rect.top() + 15, kLabelW - 6, 12);
        painter->fillRect(valueRect, backgroundColor_);
        QFont valueFont("Geist Mono");
        valueFont.setPointSizeF(7.0);
        valueFont.setBold(true);
        painter->setFont(valueFont);
        painter->setPen(lane.color);
        painter->drawText(valueRect, Qt::AlignRight | Qt::AlignVCenter,
                          valueText);

        if (compare && !lane.gear) {
            const std::vector<double>* compareData =
                lane.field.startsWith(QStringLiteral("raw:"))
                    ? store_->extraChannelData(lane.field, true)
                    : fieldFor(*compare, lane.field);
            const double compareFraction =
                store_->compareFractionForPrimaryFraction(std::clamp(
                    fraction - store_->referenceAlignment(), 0.0, 1.0));
            const int compareSample = std::min(
                int(compareData->size()) - 1,
                int(compareFraction * double(compareData->size() - 1)));
            painter->setPen(Qt::NoPen);
            painter->setBrush(alpha(kMuted, 190));
            painter->drawEllipse(
                QPointF(cursorX, toY((*compareData)[compareSample])), 1.8, 1.8);
        }
    }
}

void TraceView::paintSelectionOverlay(QPainter* painter) {
    if (!store_ || selectionStart_ < 0.0 || selectionEnd_ < 0.0) return;
    const UnifiedLap* primary = store_->primaryUnified();
    if (!primary || primary->time.size() < 2) return;

    const double lo = std::min(selectionStart_, selectionEnd_);
    const double hi = std::max(selectionStart_, selectionEnd_);
    const double x0 = xForFrac(lo);
    const double x1 = xForFrac(hi);
    const QRectF selectionRect(x0, cursorTop_, std::max(1.0, x1 - x0),
                               std::max(1.0, cursorBottom_ - cursorTop_));

    painter->save();
    painter->fillRect(selectionRect, alpha(kAccent, 24));
    painter->setPen(QPen(alpha(kAccent, 150), 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(selectionRect);

    auto sampleStd = [](const std::vector<double>& values, double fraction) {
        const double position =
            std::clamp(fraction, 0.0, 1.0) * double(values.size() - 1);
        const int i0 =
            std::clamp(int(std::floor(position)), 0, int(values.size()) - 1);
        const int i1 = std::min(i0 + 1, int(values.size()) - 1);
        return values[i0] + (values[i1] - values[i0]) * (position - i0);
    };
    const double primaryTime =
        sampleStd(primary->time, hi) - sampleStd(primary->time, lo);

    QString label = QString("t %1s").arg(primaryTime, 0, 'f', 3);
    QColor labelColor = kForeground;
    const QVector<double>& delta = store_->deltaTrace();
    if (store_->comparing() && delta.size() > 1) {
        const int deltaLast = int(delta.size()) - 1;
        auto sampleDelta = [&](double fraction) {
            const double position =
                std::clamp(fraction, 0.0, 1.0) * double(deltaLast);
            const int i0 = std::clamp(int(std::floor(position)), 0, deltaLast);
            const int i1 = std::min(i0 + 1, deltaLast);
            return delta[i0] + (delta[i1] - delta[i0]) * (position - i0);
        };
        const double regionDelta = sampleDelta(hi) - sampleDelta(lo);
        const double referenceTime = primaryTime - regionDelta;
        label = QString("P %1s  R %2s  Δ %3%4s")
                    .arg(primaryTime, 0, 'f', 3)
                    .arg(referenceTime, 0, 'f', 3)
                    .arg(regionDelta >= 0.0 ? "+" : "")
                    .arg(regionDelta, 0, 'f', 3);
        labelColor = regionDelta > 0.01    ? kRed
                     : regionDelta < -0.01 ? kGreen
                                           : kForeground;
    }

    QFont labelFont("Geist Mono");
    labelFont.setPointSizeF(8.5);
    labelFont.setBold(true);
    painter->setFont(labelFont);
    const QFontMetricsF metrics(labelFont);
    const QSizeF textSize = metrics.size(Qt::TextSingleLine, label);
    const qreal pillWidth = textSize.width() + 14;
    const qreal pillHeight = textSize.height() + 8;
    qreal labelX = x1 + 6;
    if (labelX + pillWidth > width()) labelX = x0 - pillWidth - 6;
    labelX = std::clamp(labelX, 2.0, std::max(2.0, width() - pillWidth - 2));
    const QRectF pill(labelX, cursorTop_ + 6, pillWidth, pillHeight);
    painter->setPen(QPen(alpha(labelColor, 170), 1));
    painter->setBrush(alpha(backgroundColor_, 238));
    painter->drawRoundedRect(pill, 4, 4);
    painter->setPen(labelColor);
    painter->drawText(pill, Qt::AlignCenter, label);
    painter->restore();
}

const TraceView::ChannelGeometry& TraceView::geometryFor(
    const ChannelSpec& spec, const UnifiedLap* primary,
    const UnifiedLap* compare) {
    auto cached = geometryCache_.find(spec.key);
    if (cached != geometryCache_.end()) return cached.value();

    ChannelGeometry geometry;
    geometry.min = spec.clamp.min;
    geometry.max = spec.clamp.max;
    geometry.gear = spec.field == "gear";
    geometry.filled = spec.filled;

    const bool rawChannel = spec.field.startsWith(QStringLiteral("raw:"));
    const std::vector<double>* primaryData =
        primary ? (rawChannel ? store_->extraChannelData(spec.key, false)
                              : fieldFor(*primary, spec.field))
                : nullptr;
    const std::vector<double>* compareData =
        compare && !geometry.gear
            ? (rawChannel ? store_->extraChannelData(spec.key, true)
                          : fieldFor(*compare, spec.field))
            : nullptr;

    if (spec.clamp.autoRange) {
        geometry.min = 1e18;
        geometry.max = -1e18;
        auto includeRange = [&](const std::vector<double>* values) {
            if (!values) return;
            for (double value : *values) {
                if (!std::isfinite(value)) continue;
                geometry.min = std::min(geometry.min, value);
                geometry.max = std::max(geometry.max, value);
            }
        };
        includeRange(primaryData);
        includeRange(compareData);
        if (!(geometry.max > geometry.min)) {
            geometry.min = 0.0;
            geometry.max = 1.0;
        }
        if (spec.clamp.symmetric) {
            const double magnitude =
                std::max(std::fabs(geometry.min), std::fabs(geometry.max));
            geometry.min = magnitude < 1e-6 ? -1.0 : -magnitude;
            geometry.max = magnitude < 1e-6 ? 1.0 : magnitude;
        } else {
            const double padding = (geometry.max - geometry.min) * 0.06;
            geometry.min -= padding;
            geometry.max += padding;
        }
    }
    if (!(geometry.max > geometry.min)) {
        geometry.min = 0.0;
        geometry.max = 1.0;
    }
    const double span = geometry.max - geometry.min;

    auto buildPath = [&](const std::vector<double>* values, QPainterPath& path,
                         bool alignCompare) {
        if (!values || values->size() < 2) return;
        const int outputLast = alignCompare && primary
                                   ? int(primary->size()) - 1
                                   : int(values->size()) - 1;
        const int valueLast = int(values->size()) - 1;
        double previousY = 0.0;
        for (int i = 0; i <= outputLast; ++i) {
            const double x = double(i) / double(outputLast);
            const double sourceFraction =
                alignCompare ? store_->compareFractionForPrimaryFraction(x) : x;
            const double sourcePosition = sourceFraction * valueLast;
            const int low =
                std::clamp(int(std::floor(sourcePosition)), 0, valueLast);
            const int high = std::min(low + 1, valueLast);
            const double value =
                (*values)[low] + ((*values)[high] - (*values)[low]) *
                                     (sourcePosition - double(low));
            const double y = 1.0 - (value - geometry.min) / span;
            if (i == 0) {
                path.moveTo(x, y);
            } else if (geometry.gear) {
                path.lineTo(x, previousY);
                path.lineTo(x, y);
            } else {
                path.lineTo(x, y);
            }
            previousY = y;
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        path.setCachingEnabled(true);
#endif
    };
    buildPath(primaryData, geometry.primaryLine, false);
    buildPath(compareData, geometry.compareLine, true);
    if (geometry.filled && !geometry.primaryLine.isEmpty()) {
        geometry.primaryFill = geometry.primaryLine;
        const auto first = geometry.primaryLine.elementAt(0);
        const auto last = geometry.primaryLine.elementAt(
            geometry.primaryLine.elementCount() - 1);
        geometry.primaryFill.lineTo(last.x, 1.0);
        geometry.primaryFill.lineTo(first.x, 1.0);
        geometry.primaryFill.closeSubpath();
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        geometry.primaryFill.setCachingEnabled(true);
#endif
    }

    constexpr int rasterWidth = 4096;
    constexpr int rasterHeight = 128;
    QColor traceColor(store_->channelColor(spec.key));
    if (!traceColor.isValid()) traceColor = spec.color;
    QTransform rasterTransform;
    rasterTransform.scale(rasterWidth - 1, rasterHeight - 1);

    if (!geometry.primaryLine.isEmpty()) {
        geometry.primaryRaster = QImage(rasterWidth, rasterHeight,
                                        QImage::Format_ARGB32_Premultiplied);
        geometry.primaryRaster.fill(Qt::transparent);
        QPainter rasterPainter(&geometry.primaryRaster);
        rasterPainter.setRenderHint(QPainter::Antialiasing, true);
        if (!geometry.primaryFill.isEmpty()) {
            rasterPainter.setPen(Qt::NoPen);
            rasterPainter.setBrush(alpha(traceColor, 46));
            rasterPainter.drawPath(rasterTransform.map(geometry.primaryFill));
        }
        QPen pen(traceColor, 2.2);
        pen.setCosmetic(true);
        rasterPainter.setPen(pen);
        rasterPainter.setBrush(Qt::NoBrush);
        rasterPainter.drawPath(rasterTransform.map(geometry.primaryLine));
    }
    if (!geometry.compareLine.isEmpty()) {
        geometry.compareRaster = QImage(rasterWidth, rasterHeight,
                                        QImage::Format_ARGB32_Premultiplied);
        geometry.compareRaster.fill(Qt::transparent);
        QPainter rasterPainter(&geometry.compareRaster);
        rasterPainter.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(alpha(kMuted, 150), 2.0);
        pen.setCosmetic(true);
        rasterPainter.setPen(pen);
        rasterPainter.setBrush(Qt::NoBrush);
        rasterPainter.drawPath(rasterTransform.map(geometry.compareLine));
    }

    auto inserted = geometryCache_.insert(spec.key, std::move(geometry));
    return inserted.value();
}

void TraceView::paintChannel(QPainter& p, const ChannelSpec& spec,
                             const QRectF& rect, int index,
                             const UnifiedLap* primary,
                             const UnifiedLap* compare,
                             const std::vector<double>* pf,
                             const std::vector<double>* cf) {
    Q_UNUSED(index);
    const bool rawChannel = spec.field.startsWith(QStringLiteral("raw:"));
    const std::vector<double>* primaryData =
        pf ? pf
           : (primary ? (rawChannel ? store_->extraChannelData(spec.key, false)
                                    : fieldFor(*primary, spec.field))
                      : nullptr);
    const std::vector<double>* compareData =
        cf ? cf
           : (compare && spec.field != QStringLiteral("gear")
                  ? (rawChannel ? store_->extraChannelData(spec.key, true)
                                : fieldFor(*compare, spec.field))
                  : nullptr);

    QColor traceColor(store_->channelColor(spec.key));
    if (!traceColor.isValid()) traceColor = spec.color;
    const ChannelGeometry& geometry = geometryFor(spec, primary, compare);

    p.setPen(QPen(alpha(kGridStrong, 110), 1));
    p.drawLine(QPointF(rect.left() - 1, rect.top()),
               QPointF(rect.left() - 1, rect.bottom()));

    if (isSticky(spec.key)) {
        const QRectF stickyButton(3, rect.top() + 4, 12, 12);
        p.setPen(QPen(alpha(isSticky(spec.key) ? traceColor : kMuted,
                            isSticky(spec.key) ? 90 : 35),
                      1));
        p.setBrush(alpha(isSticky(spec.key) ? traceColor : kMuted,
                         isSticky(spec.key) ? 22 : 8));
        p.drawRoundedRect(stickyButton, 2, 2);
        p.setFont(stickyFont_);
        p.setPen(alpha(isSticky(spec.key) ? traceColor : kMuted,
                       isSticky(spec.key) ? 190 : 70));
        p.drawText(stickyButton, Qt::AlignCenter, QStringLiteral("S"));
    }

    p.setFont(labelFont_);
    p.setPen(kMuted);
    p.drawText(QRectF(0, rect.top() + 2, kLabelW - 6, 14),
               Qt::AlignRight | Qt::AlignVCenter, spec.title);
    p.setFont(unitFont_);
    p.setPen(kDim);
    p.drawText(QRectF(0, rect.top() + 15, kLabelW - 6, 12),
               Qt::AlignRight | Qt::AlignVCenter, spec.unit);

    const QRectF dataRect = rect.adjusted(1, 1, -1, -1);
    const double span = geometry.max - geometry.min;
    auto toY = [&](double value) {
        return dataRect.top() +
               (1.0 - (value - geometry.min) / span) * dataRect.height();
    };

    p.setPen(QPen(kGrid, 1));
    for (int grid = 1; grid < 4; ++grid) {
        const double y = dataRect.top() + dataRect.height() * grid / 4.0;
        p.drawLine(QPointF(dataRect.left(), y), QPointF(dataRect.right(), y));
    }
    if (geometry.min < 0 && geometry.max > 0) {
        p.setPen(QPen(kGridStrong, 1, Qt::DashLine));
        const double zeroY = toY(0);
        p.drawLine(QPointF(dataRect.left(), zeroY),
                   QPointF(dataRect.right(), zeroY));
    }

    auto drawRaster = [&](const QImage& image, double horizontalShift) {
        if (image.isNull()) return;
        const double sourceStart =
            (store_->viewStart() - horizontalShift) * image.width();
        const double sourceWidth = store_->viewSpan() * image.width();
        const QRectF requested(sourceStart, 0, sourceWidth, image.height());
        const QRectF available =
            requested.intersected(QRectF(0, 0, image.width(), image.height()));
        if (available.isEmpty() || sourceWidth <= 0.0) return;
        const double targetX =
            dataRect.left() +
            (available.left() - sourceStart) / sourceWidth * dataRect.width();
        const double targetWidth =
            available.width() / sourceWidth * dataRect.width();
        const QRectF target(targetX, dataRect.top(), targetWidth,
                            dataRect.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(target, image, available);
    };

    auto drawAdaptive = [&](const std::vector<double>* values,
                            double horizontalShift, QColor color, bool fill,
                            bool alignCompare) {
        if (!values || values->size() < 2) return;
        const int last = alignCompare && primary ? int(primary->size()) - 1
                                                 : int(values->size()) - 1;
        const int valueLast = int(values->size()) - 1;
        const double visibleStart = store_->viewStart() - horizontalShift;
        const double visibleEnd = store_->viewEnd() - horizontalShift;
        const int first =
            std::clamp(int(std::floor(visibleStart * last)) - 1, 0, last);
        const int finish =
            std::clamp(int(std::ceil(visibleEnd * last)) + 1, 0, last);
        const int pointBudget =
            std::max(96, int(std::ceil(dataRect.width() * 1.5)));
        const int stride = std::max(
            1, int(std::ceil(double(finish - first) / double(pointBudget))));
        QPainterPath path;
        int previous = -1;
        double previousValue = 0.0;
        auto append = [&](int i) {
            const double fraction = double(i) / double(last) + horizontalShift;
            if (fraction < store_->viewStart() - 0.001 ||
                fraction > store_->viewEnd() + 0.001)
                return;
            const double sourceFraction =
                alignCompare ? store_->compareFractionForPrimaryFraction(
                                   double(i) / double(last))
                             : double(i) / double(last);
            const double sourcePosition = sourceFraction * valueLast;
            const int low =
                std::clamp(int(std::floor(sourcePosition)), 0, valueLast);
            const int high = std::min(low + 1, valueLast);
            const double value =
                (*values)[low] + ((*values)[high] - (*values)[low]) *
                                     (sourcePosition - double(low));
            if (!std::isfinite(value)) return;
            const double x =
                dataRect.left() + (fraction - store_->viewStart()) /
                                      store_->viewSpan() * dataRect.width();
            const double y = toY(value);
            if (previous < 0)
                path.moveTo(x, y);
            else if (geometry.gear) {
                path.lineTo(x, toY(previousValue));
                path.lineTo(x, y);
            } else {
                path.lineTo(x, y);
            }
            previous = i;
            previousValue = value;
        };
        append(first);
        for (int i = first + stride; i < finish; i += stride) append(i);
        append(finish);
        if (path.isEmpty()) return;

        if (fill) {
            QPainterPath area = path;
            area.lineTo(path.currentPosition().x(), dataRect.bottom());
            area.lineTo(dataRect.left(), dataRect.bottom());
            area.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(alpha(color, 42));
            p.drawPath(area);
        }
        QPen pen(color, fill ? 1.8 : 1.5);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.drawPath(path);
    };

    // Raster paths are cheap at the overview scale. Once zoomed, rebuild
    // only the visible samples so a small viewport never magnifies a bitmap.
    if (store_->viewSpan() < 0.65) {
        drawAdaptive(compareData, store_->referenceAlignment(),
                     alpha(kMuted, 175), false, true);
        drawAdaptive(primaryData, 0.0, traceColor, spec.filled, false);
    } else {
        drawRaster(geometry.compareRaster, store_->referenceAlignment());
        drawRaster(geometry.primaryRaster, 0.0);
    }

    if (!geometry.primaryLine.isEmpty()) {
        cursorLanes_.append(CursorLane{spec.field, dataRect, geometry.min,
                                       geometry.max, traceColor,
                                       geometry.gear});
    }
}

void TraceView::paintDelta(QPainter& p, const QRectF& rect) {
    if (!store_->comparing()) return;
    if (isSticky(QStringLiteral("delta"))) {
        const QRectF stickyButton(3, rect.top() + 4, 12, 12);
        p.setPen(
            QPen(alpha(isSticky(QStringLiteral("delta")) ? kAccent : kMuted,
                       isSticky(QStringLiteral("delta")) ? 90 : 35),
                 1));
        p.setBrush(alpha(isSticky(QStringLiteral("delta")) ? kAccent : kMuted,
                         isSticky(QStringLiteral("delta")) ? 22 : 8));
        p.drawRoundedRect(stickyButton, 2, 2);
        p.setFont(stickyFont_);
        p.setPen(alpha(isSticky(QStringLiteral("delta")) ? kAccent : kMuted,
                       isSticky(QStringLiteral("delta")) ? 190 : 70));
        p.drawText(stickyButton, Qt::AlignCenter, QStringLiteral("S"));
    }

    const QVector<double>& delta = store_->deltaTrace();
    const int n = delta.size();
    if (n < 2) return;

    p.setFont(labelFont_);
    p.setPen(kMuted);
    p.drawText(QRectF(0, rect.top() + 2, kLabelW - 6, 14),
               Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Δ Time"));
    p.setFont(unitFont_);
    p.setPen(kDim);
    p.drawText(QRectF(0, rect.top() + 15, kLabelW - 6, 12),
               Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("s"));

    constexpr int rasterWidth = 4096;
    constexpr int rasterHeight = 128;
    if (deltaRaster_.isNull()) {
        deltaMaxAbs_ = 0.001;
        for (double value : delta)
            deltaMaxAbs_ = std::max(deltaMaxAbs_, std::fabs(value));

        deltaRaster_ = QImage(rasterWidth, rasterHeight,
                              QImage::Format_ARGB32_Premultiplied);
        deltaRaster_.fill(Qt::transparent);
        auto toY = [&](double value) {
            return (1.0 - (value + deltaMaxAbs_) / (deltaMaxAbs_ * 2.0)) *
                   (rasterHeight - 1);
        };
        QPainterPath ahead;
        QPainterPath behind;
        QPainterPath line;
        line.moveTo(0, toY(delta[0]));
        for (int i = 1; i < n; ++i) {
            const double x0 = double(i - 1) / double(n - 1) * (rasterWidth - 1);
            const double x1 = double(i) / double(n - 1) * (rasterWidth - 1);
            const double y0 = toY(delta[i - 1]);
            const double y1 = toY(delta[i]);
            line.lineTo(x1, y1);
            QPainterPath& band = delta[i] < 0 ? ahead : behind;
            const double zero = toY(0);
            band.moveTo(x0, y0);
            band.lineTo(x1, y1);
            band.lineTo(x1, zero);
            band.lineTo(x0, zero);
            band.closeSubpath();
        }
        QPainter rasterPainter(&deltaRaster_);
        rasterPainter.setRenderHint(QPainter::Antialiasing, true);
        rasterPainter.setPen(Qt::NoPen);
        rasterPainter.setBrush(alpha(kGreen, 48));
        rasterPainter.drawPath(ahead);
        rasterPainter.setBrush(alpha(kRed, 48));
        rasterPainter.drawPath(behind);
        QPen linePen(kForeground, 2.2);
        linePen.setCosmetic(true);
        rasterPainter.setPen(linePen);
        rasterPainter.setBrush(Qt::NoBrush);
        rasterPainter.drawPath(line);
        QPen zeroPen(kGridStrong, 1, Qt::DashLine);
        zeroPen.setCosmetic(true);
        rasterPainter.setPen(zeroPen);
        rasterPainter.drawLine(QPointF(0, toY(0)),
                               QPointF(rasterWidth - 1, toY(0)));
    }

    if (store_->viewSpan() < 0.65) {
        const int last = n - 1;
        const int first = std::clamp(
            int(std::floor(store_->viewStart() * last)) - 1, 0, last);
        const int finish =
            std::clamp(int(std::ceil(store_->viewEnd() * last)) + 1, 0, last);
        const int budget = std::max(96, int(rect.width() * 1.5));
        const int stride =
            std::max(1, int(std::ceil(double(finish - first) / budget)));
        auto yForDelta = [&](double value) {
            return rect.top() +
                   (1.0 - (value + deltaMaxAbs_) / (2.0 * deltaMaxAbs_)) *
                       rect.height();
        };
        QPainterPath line;
        QPainterPath ahead;

        QPainterPath behind;
        int previous = -1;
        auto append = [&](int i) {
            const double fraction = double(i) / double(last);
            if (fraction < store_->viewStart() - 0.001 ||
                fraction > store_->viewEnd() + 0.001)
                return;
            const double x = xForFrac(fraction);
            const double y = yForDelta(delta[i]);
            if (previous < 0)
                line.moveTo(x, y);
            else
                line.lineTo(x, y);
            if (previous >= 0) {
                QPainterPath& band = delta[i] < 0 ? ahead : behind;
                const double x0 = xForFrac(double(previous) / double(last));
                const double y0 = yForDelta(delta[previous]);
                const double zero = yForDelta(0.0);
                band.moveTo(x0, y0);
                band.lineTo(x, y);
                band.lineTo(x, zero);
                band.lineTo(x0, zero);
                band.closeSubpath();
            }
            previous = i;
        };
        append(first);
        for (int i = first + stride; i < finish; i += stride) append(i);
        append(finish);
        p.setPen(Qt::NoPen);
        p.setBrush(alpha(kGreen, 48));
        p.drawPath(ahead);
        p.setBrush(alpha(kRed, 48));
        p.drawPath(behind);
        QPen linePen(kForeground, 1.7);
        linePen.setCosmetic(true);
        p.setPen(linePen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
        QPen zeroPen(kGridStrong, 1, Qt::DashLine);
        zeroPen.setCosmetic(true);
        p.setPen(zeroPen);
        p.drawLine(QPointF(rect.left(), yForDelta(0.0)),
                   QPointF(rect.right(), yForDelta(0.0)));
    } else {
        const QRectF source(store_->viewStart() * deltaRaster_.width(), 0,
                            store_->viewSpan() * deltaRaster_.width(),
                            deltaRaster_.height());
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(rect, deltaRaster_, source);
    }
    p.setPen(kAccent);
    p.drawText(QRectF(rect.left(), rect.top() + 2, rect.width() - 8, 14),
               Qt::AlignRight,
               QString("Δ +%1 / -%2")
                   .arg(deltaMaxAbs_, 0, 'f', 3)
                   .arg(deltaMaxAbs_, 0, 'f', 3));
}

int TraceView::cornerIndexAt(const QPointF& position) const {
    if (!store_ || store_->corners().isEmpty() || position.y() > kTopPad)
        return -1;
    const double fraction = fracForX(position.x());
    for (int i = 0; i < store_->corners().size(); ++i) {
        const CornerZone& corner = store_->corners()[i];
        if (corner.start <= fraction && fraction <= corner.end) return i;
    }
    return -1;
}

void TraceView::paintCornerZones(QPainter& p, const QRectF& totalRect) {
    if (!store_ || store_->corners().isEmpty()) return;
    const bool editing = store_->editingCorners();
    const auto& corners = store_->corners();
    for (const CornerZone& corner : corners) {
        const double x1 = xForFrac(corner.start);
        const double x2 = xForFrac(corner.end);
        const QRectF zone(x1, totalRect.top(), x2 - x1, totalRect.height());

        // Keep the analysis area readable: only a whisper of the corner range
        // crosses the traces. Labels live in the dedicated ruler above them.
        p.fillRect(zone, alpha(kMagenta, editing ? 22 : 8));
        p.setPen(QPen(alpha(kMagenta, editing ? 80 : 28), 1));
        p.drawLine(QPointF(x1, totalRect.top()),
                   QPointF(x1, totalRect.bottom()));
        p.drawLine(QPointF(x2, totalRect.top()),
                   QPointF(x2, totalRect.bottom()));

        const QRectF labelBand(x1, 2, std::max(1.0, x2 - x1), 17);
        p.fillRect(labelBand, alpha(kMagenta, editing ? 64 : 34));
        p.setPen(alpha(kForeground, editing ? 220 : 160));
        QFont labelFont("Geist Mono");
        labelFont.setPointSizeF(7.0);
        labelFont.setBold(editing);
        p.setFont(labelFont);
        p.drawText(labelBand.adjusted(4, 0, -3, 0),
                   Qt::AlignLeft | Qt::AlignVCenter, corner.name);

        if (editing) {
            p.setPen(Qt::NoPen);
            p.setBrush(alpha(kOrange, 220));
            p.drawRect(QRectF(x1 - 2, totalRect.top(), 4, totalRect.height()));
            p.drawRect(QRectF(x2 - 2, totalRect.top(), 4, totalRect.height()));
            p.setBrush(Qt::NoBrush);
        }
    }
}

int TraceView::channelIndexAt(const QPointF& position) const {
    if (!store_) return -1;
    QVector<int> stickyIdx;
    QVector<int> scrollIdx;
    const UnifiedLap* compare = store_->compareUnified();
    for (int i = 0; i < channelSpecs_.size(); ++i) {
        const ChannelSpec& spec = channelSpecs_[i];
        if (spec.key == "delta") {
            if (!compare || !store_->channelVisible(spec.key)) continue;
        } else if (!store_->channelVisible(spec.key)) {
            continue;
        }
        (isSticky(spec.key) ? stickyIdx : scrollIdx).append(i);
    }
    double y = kTopPad;
    for (int idx : stickyIdx) {
        const double rowH = rowHeightFor(channelSpecs_[idx]);
        if (position.y() >= y && position.y() < y + rowH) return idx;
        y += rowH;
    }
    const double secondaryTop = y;
    const double viewport = std::max(0.0, height() - kBottomPad - secondaryTop);
    y = secondaryTop - secondaryScroll_;
    for (int idx : scrollIdx) {
        const double rowH = rowHeightFor(channelSpecs_[idx]);
        if (position.y() >= y && position.y() < y + rowH &&
            position.y() < secondaryTop + viewport)
            return idx;
        y += rowH;
    }
    return -1;
}

// Menus are Material popups owned by QML: this is a QGuiApplication, so a
// QWidget-based QMenu cannot be constructed here.
void TraceView::showCornerMenu(const QPointF& position) {
    if (!store_) return;
    const double fraction = fracForX(position.x());
    int cornerIndex = -1;
    for (int index = 0; index < store_->corners().size(); ++index) {
        const CornerZone& corner = store_->corners()[index];
        if (corner.start <= fraction && fraction <= corner.end) {
            cornerIndex = index;
            break;
        }
    }
    emit cornerMenuRequested(
        cornerIndex,
        cornerIndex >= 0 ? store_->corners()[cornerIndex].name : QString(),
        fraction, position.x(), position.y());
}

void TraceView::showChannelMenu(const QPointF& position) {
    const int index = channelIndexAt(position);
    if (index < 0 || index >= channelSpecs_.size()) return;
    const ChannelSpec& spec = channelSpecs_[index];
    emit channelMenuRequested(spec.key, spec.title, isSticky(spec.key),
                              position.x(), position.y());
}

int TraceView::addCornerAt(double fraction) {
    if (!store_) return -1;
    constexpr double width = 0.04;
    const double start = qBound(0.0, fraction - width * 0.5, 1.0 - width);
    return store_->addCorner(start, start + width);
}

void TraceView::toggleSticky(const QString& key) {
    if (isSticky(key))
        stickyChannels_.remove(key);
    else
        stickyChannels_.insert(key);
    invalidateStaticLayer();
}

void TraceView::unpinAllChannels() {
    stickyChannels_.clear();
    secondaryScroll_ = 0.0;
    invalidateStaticLayer();
}

void TraceView::hideChannel(const QString& key) {
    if (!store_) return;
    stickyChannels_.remove(key);
    store_->setChannelVisible(key, false);
}

void TraceView::showAllStandardChannels() {
    if (!store_) return;
    for (const ChannelSpec& channel : channelSpecs_)
        if (!channel.key.startsWith(QStringLiteral("raw:")))
            store_->setChannelVisible(channel.key, true);
}
// ── interaction ─────────────────────────────────────────────────────

void TraceView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    panning_ = false;
    selecting_ = true;
    selectionStart_ = fracForX(event->position().x());
    selectionEnd_ = selectionStart_;
    store_->setCursorFrac(selectionStart_);
    emit cursorChangedFromCanvas();
    setCursor(Qt::CrossCursor);
    emit overlayChanged();
    event->accept();
}

void TraceView::mousePressEvent(QMouseEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    const double x = event->position().x();
    const double fraction = fracForX(x);
    if (event->button() == Qt::RightButton) {
        if (store_->editingCorners())
            showCornerMenu(event->position());
        else
            showChannelMenu(event->position());
        event->accept();
        return;
    }

    if (event->button() == Qt::LeftButton) {
        const int cornerIndex = cornerIndexAt(event->position());
        if (cornerIndex >= 0) {
            emit cornerActivated(cornerIndex);
            event->accept();
            return;
        }
    }

    // A click commits the range that was started with a double-click.
    if (selecting_) {
        selectionEnd_ = fraction;
        selecting_ = false;
        store_->setCursorFrac(fraction);
        emit cursorChangedFromCanvas();
        unsetCursor();
        emit overlayChanged();
        event->accept();
        return;
    }

    // Corner editing takes precedence over navigation.
    if (event->button() == Qt::LeftButton && store_->editingCorners() &&
        !store_->corners().isEmpty()) {
        for (int i = 0; i < store_->corners().size(); ++i) {
            const CornerZone& corner = store_->corners()[i];
            const double x1 = xForFrac(corner.start);
            const double x2 = xForFrac(corner.end);
            if (std::fabs(x - x1) < 5) {
                dragCorner_ = i;
                dragCornerMove_ = false;
                dragging_ = true;
                break;
            }
            if (std::fabs(x - x2) < 5) {
                dragCorner_ = i;
                dragCornerMove_ = false;
                dragging_ = true;
                break;
            }
            if (corner.start <= fraction && fraction <= corner.end) {
                dragCorner_ = i;
                dragCornerMove_ = true;
                dragStartFrac_ = fraction - corner.start;
                dragging_ = true;
                break;
            }
        }
        if (dragging_) {
            setCursor(Qt::SizeHorCursor);
            event->accept();
            return;
        }
    }

    // Normal dragging remains navigation/panning.
    selectionStart_ = -1.0;
    selectionEnd_ = -1.0;
    store_->setCursorFrac(fraction);
    emit cursorChangedFromCanvas();
    panning_ = true;
    lastPanFrac_ = fraction;
    setCursor(Qt::ClosedHandCursor);
    emit overlayChanged();
    event->accept();
}

void TraceView::mouseMoveEvent(QMouseEvent* event) {
    if (!store_) return;
    const double x = event->position().x();
    if (dragging_ && dragCorner_ >= 0) {
        const double fraction = fracForX(x);
        const auto& corners = store_->corners();
        if (dragCorner_ >= corners.size()) {
            dragging_ = false;
            return;
        }
        const double start = corners[dragCorner_].start;
        const double end = corners[dragCorner_].end;
        if (!dragCornerMove_) {
            const double dx1 = std::fabs(x - xForFrac(start));
            const double dx2 = std::fabs(x - xForFrac(end));
            if (dx1 < dx2)
                store_->updateCorner(dragCorner_, qBound(0.0, fraction, end),
                                     end);
            else
                store_->updateCorner(dragCorner_, start,
                                     qBound(start, fraction, 1.0));
        } else {
            const double width = end - start;
            const double nextStart =
                qBound(0.0, fraction - dragStartFrac_, 1.0 - width);
            store_->updateCorner(dragCorner_, nextStart, nextStart + width);
        }
        emit cornerEdited();
        return;
    }
    if (selecting_) {
        selectionEnd_ = fracForX(x);
        store_->setCursorFrac(selectionEnd_);
        emit cursorChangedFromCanvas();
        emit overlayChanged();
        return;
    }
    if (panning_) {
        const double fraction = fracForX(x);
        store_->pan(lastPanFrac_ - fraction);
        lastPanFrac_ = fraction;
        return;
    }
    store_->setCursorFrac(fracForX(x));
    emit cursorChangedFromCanvas();
}

void TraceView::mouseReleaseEvent(QMouseEvent* event) {
    Q_UNUSED(event);
    if (dragging_ && dragCorner_ >= 0 && store_) store_->saveCorners();
    dragging_ = false;
    panning_ = false;
    dragCorner_ = -1;
    if (selecting_)
        setCursor(Qt::CrossCursor);
    else
        unsetCursor();
    emit overlayChanged();
}

void TraceView::wheelEvent(QWheelEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    double delta = event->angleDelta().y();
    if (delta == 0.0) delta = event->pixelDelta().y();
    if (delta == 0.0) return;

    const bool forceZoom = event->modifiers() & Qt::ControlModifier;
    const bool scrollChannels =
        !forceZoom && ((event->modifiers() & Qt::ShiftModifier) ||
                       event->position().x() < kLabelW);
    if (scrollChannels) {
        secondaryScroll_ -= delta / 120.0 * 80.0;
        invalidateStaticLayer();
    } else {
        const double anchor = fracForX(event->position().x());
        store_->zoomAt(anchor, std::pow(0.8, delta / 120.0));
    }
    event->accept();
}

void TraceView::keyPressEvent(QKeyEvent* event) {
    if (!store_) return;
    int steps = 0;
    switch (event->key()) {
        case Qt::Key_Left: steps = -kCursorSamplesPerStep; break;
        case Qt::Key_Right: steps = kCursorSamplesPerStep; break;
        case Qt::Key_Home:
            event->accept();
            store_->jumpToFraction(0.0);
            return;
        case Qt::Key_End:
            event->accept();
            store_->jumpToFraction(1.0);
            return;
        default: QQuickPaintedItem::keyPressEvent(event); return;
    }
    store_->moveCursorSteps(steps);
    emit cursorChangedFromCanvas();
    event->accept();
}

void TraceView::hoverMoveEvent(QHoverEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    if (cursorTimer_.elapsed() < kHoverFrameMs) {
        event->accept();
        return;
    }
    cursorTimer_.restart();
    const double fraction = fracForX(event->position().x());
    if (selecting_) {
        selectionEnd_ = fraction;
        emit overlayChanged();
    }
    store_->setCursorFrac(fraction);
    emit cursorChangedFromCanvas();
    event->accept();
}

bool TraceView::event(QEvent* event) {
    if (event->type() == QEvent::HoverEnter ||
        event->type() == QEvent::HoverLeave)
        return QQuickPaintedItem::event(event);
    return QQuickPaintedItem::event(event);
}

TraceCursorOverlay::TraceCursorOverlay(QQuickItem* parent)
    : QQuickPaintedItem(parent) {
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
    setAntialiasing(true);
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
}

void TraceCursorOverlay::setTrace(TraceView* trace) {
    if (trace_ == trace) return;
    if (trace_) disconnect(trace_, nullptr, this, nullptr);
    trace_ = trace;
    if (trace_) {
        connect(trace_, &TraceView::overlayChanged, this,
                [this]() { update(); });
    }
    update();
    emit traceChanged();
}

void TraceCursorOverlay::paint(QPainter* painter) {
    painter->setCompositionMode(QPainter::CompositionMode_Source);
    painter->fillRect(boundingRect(), Qt::transparent);
    painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (trace_) trace_->paintCursorOverlay(painter);
}
