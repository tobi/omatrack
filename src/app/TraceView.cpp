#include "TraceView.h"

#include "StoreModels.h"
#include "TelemetryStore.h"
#include "core/TelemetryEngine.h"

#include <QCursor>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QSet>
#include <QQuickWindow>
#include <QSGNode>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace omatrack;
using ChannelSpec = TraceLaneLayout::ChannelSpec;
using Lane = TraceLaneLayout::Lane;
using ChannelRange = TraceLaneLayout::ChannelRange;
using Clamp = TraceLaneLayout::Clamp;
using SpanHit = TraceInteraction::SpanHit;

namespace {

constexpr double kTopPad = 22.0;
constexpr double kBottomPad = 18.0;
constexpr double kMinLabelW = 62.0;
// Height of the corner-marker strip along the bottom of the trace area.
constexpr double kMarkerBand = 26.0;
constexpr double kConsistencyStripHeight = 14.0;
constexpr double kGroupHeaderHeight = 20.0;
constexpr double kSpanTrackHeight = 14.0;
constexpr double kStandardSampleHeight = 72.0;
constexpr int kCursorSamplesPerStep = 1;

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

QColor mixColors(const QColor& from, const QColor& to, double amount) {
    const double factor = std::clamp(amount, 0.0, 1.0);
    return QColor::fromRgbF(
        from.redF() + (to.redF() - from.redF()) * factor,
        from.greenF() + (to.greenF() - from.greenF()) * factor,
        from.blueF() + (to.blueF() - from.blueF()) * factor,
        from.alphaF() + (to.alphaF() - from.alphaF()) * factor);
}

QColor cornerMarkerColor(const QString& key) {
    if (key == QStringLiteral("brake")) return kRed;
    if (key == QStringLiteral("apex")) return kMagenta;
    if (key == QStringLiteral("pickup")) return kGreen;
    return kAccent;
}

}  // namespace

TraceView::TraceView(QQuickItem* parent) : QQuickItem(parent) {
    canvasFont_.setFamily(QStringLiteral("Geist Mono"));
    canvasFont_.setPointSizeF(8.5);
    emptyStateFont_.setFamily(QStringLiteral("Geist Mono"));
    emptyStateFont_.setPointSize(11);
    labelFont_.setFamily(QStringLiteral("Geist Mono"));
    labelFont_.setPointSizeF(8.0);
    labelFont_.setBold(true);
    unitFont_.setFamily(QStringLiteral("Geist Mono"));
    unitFont_.setPointSizeF(7.0);
    valueFont_.setFamily(QStringLiteral("Geist Mono"));
    valueFont_.setPointSizeF(7.0);
    valueFont_.setBold(true);
    markerFont_.setFamily(QStringLiteral("Geist Mono"));
    markerFont_.setPointSizeF(7.0);
    markerFont_.setBold(true);
    pillFont_.setFamily(QStringLiteral("Geist Mono"));
    pillFont_.setPointSizeF(8.5);
    pillFont_.setBold(true);
    setFlag(QQuickItem::ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton |
                            Qt::RightButton);
    setAcceptHoverEvents(true);
    setFlag(QQuickItem::ItemAcceptsInputMethod, false);
    setFocusPolicy(Qt::StrongFocus);

    layout_.setLabelFont(labelFont_);
    layout_.setUnitFont(unitFont_);
    interaction_.setLayout(&layout_);
    layout_.onLaneLayoutChanged = [this]() { emit laneLayoutChanged(); };
    layout_.onLabelWidthChanged = [this]() { emit labelWidthChanged(); };
    layout_.onInvalidateScene = [this]() { invalidateScene(); };

    interaction_.onCursorChangedFromCanvas = [this]() {
        emit cursorChangedFromCanvas();
    };
    interaction_.onCornerEdited = [this]() { emit cornerEdited(); };
    interaction_.onChannelMenuRequested =
        [this](const QString& key, const QString& title, double weight, qreal x,
               qreal y) {
            emit channelMenuRequested(key, title, weight, x, y);
        };
    interaction_.onOverlayChanged = [this]() { emit overlayChanged(); };
    interaction_.onSpanHoverChanged = [this]() { emit spanHoverChanged(); };
    interaction_.onLaneLayoutChanged = [this]() { emit laneLayoutChanged(); };
    interaction_.onInvalidateScene = [this]() { invalidateScene(); };
    interaction_.onSetCursor = [this](Qt::CursorShape shape) {
        setCursor(shape);
    };
    interaction_.onUnsetCursor = [this]() { unsetCursor(); };

    rebuildChannelSpecs();
}

void TraceView::setBackgroundColor(const QColor& color) {
    if (!color.isValid() || backgroundColor_ == color) return;
    backgroundColor_ = color;
    update();
    emit backgroundColorChanged();
}
void TraceView::setFitChannels(bool fit) {
    if (layout_.fitChannels() == fit) return;
    layout_.setFitChannels(fit);
    layout_.setVerticalScroll(0.0);
    emit fitChannelsChanged();
    emit laneLayoutChanged();
    invalidateScene();
}
qreal TraceView::rulerHeight() const { return kTopPad; }

void TraceView::setStore(TelemetryStore* store) {
    if (store_ == store) return;
    if (store_) disconnect(store_, nullptr, this, nullptr);
    store_ = store;
    layout_.setStore(store);
    interaction_.setStore(store);
    if (store_) {
        connect(store_, &TelemetryStore::selectionChanged, this, [this]() {
            interaction_.resetSelection();
            rebuildChannelSpecs();
            invalidateRanges();
        });
        connect(store_, &TelemetryStore::cursorFracChanged, this,
                [this]() { emit overlayChanged(); });
        connect(store_, &TelemetryStore::viewChanged, this,
                &TraceView::invalidateScene);
        connect(store_, &TelemetryStore::cornersChanged, this,
                &TraceView::invalidateScene);
        connect(store_, &TelemetryStore::editingCornersChanged, this,
                &TraceView::invalidateScene);
        connect(store_, &TelemetryStore::cornerFocusChanged, this, [this]() {
            interaction_.resetHover();
            invalidateScene();
        });
        connect(store_, &TelemetryStore::highlightedCornerMarkerChanged, this,
                [this]() { emit overlayChanged(); });
        connect(store_, &TelemetryStore::channelConfigChanged, this, [this]() {
            rebuildChannelSpecs();
            invalidateRanges();
        });
        connect(store_, &TelemetryStore::overlaysChanged, this, [this]() {
            rebuildChannelSpecs();
            invalidateRanges();
        });
        connect(store_, &TelemetryStore::referenceAlignmentChanged, this,
                &TraceView::invalidateScene);
        connect(store_, &TelemetryStore::comparisonSyncStrategyChanged, this,
                &TraceView::invalidateScene);
        connect(store_, &TelemetryStore::traceConfidenceChanged, this,
                [this]() {
                    invalidateRanges();
                    emit laneLayoutChanged();
                });
    }
    invalidateRanges();
    emit storeChanged();
}

void TraceView::invalidateScene() {
    update();
    emit overlayChanged();
}

QSGNode* TraceView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) {
    QSGNode* root = oldNode ? oldNode : new QSGNode;
    builder_.begin(window());
    buildScene(builder_);
    builder_.commit(root);
    // The overlay reads the lane rectangles this build produced.
    QMetaObject::invokeMethod(
        this, [this]() { emit overlayChanged(); }, Qt::QueuedConnection);
    return root;
}

void TraceView::releaseResources() {
    builder_.releaseResources();
    QQuickItem::releaseResources();
}

void TraceView::geometryChange(const QRectF& newGeometry,
                               const QRectF& oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        layout_.setItemSize(width(), height());
        interaction_.setItemSize(width(), height());
        updateLabelWidth();
        invalidateScene();
        emit laneLayoutChanged();
    }
}

QVariantMap TraceView::benchmarkGeometry(int frames) {
    frames = std::clamp(frames, 1, 2000);
    TraceSceneBuilder scratch;
    QElapsedTimer clock;
    clock.start();
    int quads = 0;
    for (int i = 0; i < frames; ++i) {
        // A null window skips texture creation: text is cached across frames
        // in the real renderer, so the measurement is the geometry work.
        scratch.begin(nullptr);
        buildScene(scratch);
        quads = scratch.quadCount();
    }
    const double elapsed = double(clock.nsecsElapsed()) / 1.0e6;
    return QVariantMap{{QStringLiteral("averageMs"), elapsed / frames},
                       {QStringLiteral("quads"), quads},
                       {QStringLiteral("lanes"), layoutLanes().size()}};
}

void TraceView::buildScene(TraceSceneBuilder& builder) {
    cursorLanes_.clear();
    setCursorTop(0.0);
    setCursorBottom(0.0);
    if (width() <= 0.0 || height() <= 0.0) return;
    if (store_) snapshot_ = store_->traceSnapshot();
    layout_.setItemSize(width(), height());
    interaction_.setItemSize(width(), height());
    layout_.setSnapshot(&snapshot_);
    interaction_.setSnapshot(&snapshot_);
    builder.rect(QRectF(0, 0, width(), height()), backgroundColor_);

    const UnifiedLap* primary = snapshot_.primary;
    if (!store_ || !primary || primary->size() < 2) {
        builder.text(QStringLiteral("Select a session to begin"),
                     emptyStateFont_, kDim, QRectF(0, 0, width(), height()),
                     Qt::AlignCenter);
        return;
    }
    const UnifiedLap* compare = snapshot_.compare;

    const QVector<Lane> lanes = layoutLanes();
    if (lanes.isEmpty()) return;
    buildCornerMarkerGuides(builder);
    if (store_->traceConfidenceMode())
        buildConsistencyStrip(
            builder, QRectF(labelWidth(), kTopPad, width() - labelWidth(),
                            kConsistencyStripHeight));

    interaction_.clearSpanHits();
    for (const Lane& lane : lanes) {
        const ChannelSpec& spec = channelSpecs()[lane.spec];
        const QRectF rect(labelWidth(), lane.y, width() - labelWidth(),
                          lane.height);
        if (spec.kind == ChannelSpec::Kind::GroupHeader)
            buildGroupHeader(builder, spec, rect);
        else if (spec.kind == ChannelSpec::Kind::SpanTrack)
            buildSpanTrack(builder, spec, rect);
        else if (spec.key == QStringLiteral("delta"))
            buildDelta(builder, rect);
        else
            buildChannel(builder, spec, rect, primary, compare);
        builder.hLine(lane.y, labelWidth(), width(), 1.0,
                      alpha(kGridStrong, 110));
    }

    const double axisTop = height() - kBottomPad;
    builder.hLine(axisTop, labelWidth(), width(), 1.0, kGridStrong);
    if (!primary->distance.empty()) {
        const int n = int(primary->size());
        const int divisions =
            std::max(2, int((width() - labelWidth()) / 120.0));
        const double origin = primary->distance.front();
        const double totalDistance = primary->distance.back() - origin;
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
            const double absolute = origin + distance;
            const auto it = std::lower_bound(primary->distance.begin(),
                                             primary->distance.end(), absolute);
            int sample =
                std::clamp(int(it - primary->distance.begin()), 0, n - 1);
            if (sample > 0 &&
                std::fabs(primary->distance[sample - 1] - absolute) <
                    std::fabs(primary->distance[sample] - absolute))
                --sample;
            const double fraction = double(sample) / double(n - 1);
            if (fraction < store_->viewStart() || fraction > store_->viewEnd())
                continue;
            const double x = xForFrac(fraction);
            builder.vLine(x, axisTop, axisTop + 4, 1.0, kGridStrong);
            builder.text(QString("%1m").arg(int(distance)), canvasFont_, kMuted,
                         QRectF(x + 3, axisTop + 4, 70, 12),
                         Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    const double traceHeight = std::max(0.0, height() - kTopPad - kBottomPad);
    const QRectF traceRect(labelWidth(), kTopPad, width() - labelWidth(),
                           traceHeight);
    buildCornerZones(builder, traceRect);
    buildCornerFocus(builder, traceRect);
    buildOutOfLap(builder, traceRect);

    setCursorTop(kTopPad - 3);
    setCursorBottom(height() - kBottomPad);
}

qreal TraceView::devicePixelRatio() const {
    return window() ? std::max(1.0, window()->effectiveDevicePixelRatio())
                    : 1.0;
}

int TraceView::deviceColumns(const QRectF& rect) const {
    return std::max(2, int(std::lround(rect.width() * devicePixelRatio())));
}

void TraceView::buildSeries(TraceSceneBuilder& builder,
                            const std::vector<double>* values,
                            const QRectF& rect, const ChannelRange& range,
                            const QColor& color, bool fill, bool alignCompare,
                            double shift, qreal width, double clipLow,
                            double clipHigh) {
    if (!values || values->size() < 2 || rect.width() < 2.0) return;
    const double span = std::max(1.0e-12, range.max - range.min);
    const double viewStart = store_->viewStart();
    const double viewSpan = store_->viewSpan();

    auto sourceFraction = [&](double fraction) {
        const double shifted = std::clamp(fraction - shift, 0.0, 1.0);
        return alignCompare
                   ? snapshot_.compareFractionForPrimaryFraction(shifted)
                   : shifted;
    };

    TraceSceneBuilder::EnvelopeStyle style;
    style.width = width;
    style.fill = fill;
    style.color = color;
    style.fillColor = alpha(color, 42);
    builder.envelopePolyline(*values, sourceFraction, viewStart, viewSpan, rect,
                             range.min, span, style, clipLow, clipHigh);
}

void TraceView::buildConfidenceBand(TraceSceneBuilder& builder,
                                    const TraceConfidenceBand* band,
                                    const QRectF& rect,
                                    const ChannelRange& range) {
    if (!band || !band->valid() || rect.width() < 2.0) return;
    const int valueLast = int(band->lower.size()) - 1;
    const int columns = std::max(2, int(rect.width()));
    const double columnWidth = rect.width() / double(columns);
    const double viewStart = store_->viewStart();
    const double viewSpan = store_->viewSpan();
    const double rangeSpan = std::max(1.0e-12, range.max - range.min);
    const auto toY = [&](double value) {
        return std::clamp(rect.top() + (1.0 - (value - range.min) / rangeSpan) *
                                           rect.height(),
                          rect.top(), rect.bottom());
    };
    const auto interpolate = [valueLast](const std::vector<double>& values,
                                         double position) {
        position = std::clamp(position, 0.0, double(valueLast));
        const int low = std::clamp(int(std::floor(position)), 0, valueLast);
        const int high = std::min(low + 1, valueLast);
        const double first = values[size_t(low)];
        const double second = values[size_t(high)];
        if (!std::isfinite(first) || !std::isfinite(second))
            return std::numeric_limits<double>::quiet_NaN();
        return first + (second - first) * (position - double(low));
    };

    builder.reserveQuads(columns * 4);
    bool hasPrevious = false;
    double previousTop = 0.0;
    double previousBottom = 0.0;
    double previousMedian = 0.0;
    for (int column = 0; column < columns; ++column) {
        const double startFraction =
            viewStart + viewSpan * double(column) / double(columns);
        const double endFraction =
            viewStart + viewSpan * double(column + 1) / double(columns);
        const double centre = (startFraction + endFraction) * 0.5;
        if (centre < 0.0 || centre > 1.0) {
            hasPrevious = false;
            continue;
        }

        double from = std::clamp(startFraction, 0.0, 1.0) * valueLast;
        double to = std::clamp(endFraction, 0.0, 1.0) * valueLast;
        if (to < from) std::swap(from, to);
        const int firstIndex = std::clamp(int(std::floor(from)), 0, valueLast);
        const int lastIndex = std::clamp(int(std::ceil(to)), 0, valueLast);
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        for (int index = firstIndex; index <= lastIndex; ++index) {
            const double lower = band->lower[size_t(index)];
            const double upper = band->upper[size_t(index)];
            if (!std::isfinite(lower) || !std::isfinite(upper)) continue;
            low = std::min(low, lower);
            high = std::max(high, upper);
        }
        const double median = interpolate(band->median, (from + to) * 0.5);
        if (!(low <= high) || !std::isfinite(median)) {
            hasPrevious = false;
            continue;
        }

        const double top = toY(high);
        const double bottom = toY(low);
        const double medianY = toY(median);
        const double spread = std::max(0.0, high - low) / rangeSpan;
        const double heat = std::clamp((spread - 0.015) / 0.16, 0.0, 1.0);
        QColor heatColor = heat < 0.5
                               ? mixColors(kGreen, kOrange, heat * 2.0)
                               : mixColors(kOrange, kRed, (heat - 0.5) * 2.0);
        const double x = rect.left() + columnWidth * column;
        builder.rect(QRectF(x, top, columnWidth, std::max(1.0, bottom - top)),
                     alpha(heatColor, int(std::lround(28.0 + heat * 34.0))));

        double topLine = top;
        double bottomLine = bottom;
        double medianLine = medianY;
        if (hasPrevious) {
            builder.rect(
                QRectF(x, std::min(topLine, previousTop) - 0.5, columnWidth,
                       std::fabs(topLine - previousTop) + 1.0),
                alpha(heatColor, 145));
            builder.rect(QRectF(x, std::min(bottomLine, previousBottom) - 0.5,
                                columnWidth,
                                std::fabs(bottomLine - previousBottom) + 1.0),
                         alpha(heatColor, 145));
            builder.rect(QRectF(x, std::min(medianLine, previousMedian) - 0.5,
                                columnWidth,
                                std::fabs(medianLine - previousMedian) + 1.0),
                         alpha(kForeground, 90));
        } else {
            builder.rect(QRectF(x, topLine - 0.5, columnWidth, 1.0),
                         alpha(heatColor, 145));
            builder.rect(QRectF(x, bottomLine - 0.5, columnWidth, 1.0),
                         alpha(heatColor, 145));
            builder.rect(QRectF(x, medianLine - 0.5, columnWidth, 1.0),
                         alpha(kForeground, 90));
        }
        previousTop = topLine;
        previousBottom = bottomLine;
        previousMedian = medianLine;
        hasPrevious = true;
    }
}
void TraceView::buildConsistencyStrip(TraceSceneBuilder& builder,
                                      const QRectF& rect) {
    builder.vLine(rect.left() - 1, rect.top(), rect.bottom(), 1.0,
                  alpha(kGridStrong, 110));

    const QRectF dataRect = rect.adjusted(1, 1, -1, -1);
    builder.rect(dataRect, alpha(kDim, 55));
    const std::vector<double>& values = store_->traceConsistency();
    if (values.size() < 2 || dataRect.width() < 2.0) return;

    const int valueLast = int(values.size()) - 1;
    const int columns = std::max(2, int(dataRect.width()));
    const double columnWidth = dataRect.width() / double(columns);
    const double viewStart = store_->viewStart();
    const double viewSpan = store_->viewSpan();
    builder.reserveQuads(columns);
    for (int column = 0; column < columns; ++column) {
        const double startFraction =
            viewStart + viewSpan * double(column) / double(columns);
        const double endFraction =
            viewStart + viewSpan * double(column + 1) / double(columns);
        const double centre = (startFraction + endFraction) * 0.5;
        if (centre < 0.0 || centre > 1.0) continue;

        double from = std::clamp(startFraction, 0.0, 1.0) * valueLast;
        double to = std::clamp(endFraction, 0.0, 1.0) * valueLast;
        if (to < from) std::swap(from, to);
        const int firstIndex = std::clamp(int(std::floor(from)), 0, valueLast);
        const int lastIndex = std::clamp(int(std::ceil(to)), 0, valueLast);
        double heat = 0.0;
        bool valid = false;
        for (int index = firstIndex; index <= lastIndex; ++index) {
            const double value = values[size_t(index)];
            if (!std::isfinite(value)) continue;
            heat = std::max(heat, value);
            valid = true;
        }
        if (!valid) continue;

        const QColor color = heat < 0.5
                                 ? mixColors(kGreen, kOrange, heat * 2.0)
                                 : mixColors(kOrange, kRed, (heat - 0.5) * 2.0);
        const double x = dataRect.left() + columnWidth * column;
        builder.rect(QRectF(x, dataRect.top(), columnWidth, dataRect.height()),
                     alpha(color, 220));
    }
}

void TraceView::buildChannel(TraceSceneBuilder& builder,
                             const ChannelSpec& spec, const QRectF& rect,
                             const UnifiedLap* primary,
                             const UnifiedLap* compare) {
    const bool rawChannel = spec.field.startsWith(QStringLiteral("raw:"));
    const bool sidecarChannel =
        spec.field.startsWith(QStringLiteral("sidecar:"));
    const std::vector<double>* primaryData =
        primary ? (sidecarChannel ? store_->overlayChannelData(spec.key)
                   : rawChannel   ? store_->extraChannelData(spec.key, false)
                                  : fieldFor(*primary, spec.field))
                : nullptr;
    const std::vector<double>* compareData =
        compare && spec.field != QStringLiteral("gear") && !sidecarChannel
            ? (rawChannel ? store_->extraChannelData(spec.key, true)
                          : fieldFor(*compare, spec.field))
            : nullptr;

    QColor traceColor(store_->channelColor(spec.key));
    if (!traceColor.isValid()) traceColor = spec.color;
    const ChannelRange& range = rangeFor(spec, primary, compare);

    builder.vLine(rect.left() - 1, rect.top(), rect.bottom(), 1.0,
                  alpha(kGridStrong, 110));

    const QRectF dataRect = rect.adjusted(1, 1, -1, -1);
    const double span = std::max(1.0e-12, range.max - range.min);
    auto toY = [&](double value) {
        return dataRect.top() +
               (1.0 - (value - range.min) / span) * dataRect.height();
    };

    for (int grid = 1; grid < 4; ++grid) {
        const double y = dataRect.top() + dataRect.height() * grid / 4.0;
        builder.hLine(y, dataRect.left(), dataRect.right(), 1.0, kGrid);
    }
    if (range.min < 0 && range.max > 0)
        builder.dashedHLine(toY(0), dataRect.left(), dataRect.right(),
                            kGridStrong);

    // Whatever the viewport shows before lap start / after lap end is the
    // neighbouring lap, drawn faintly so it reads as context, not as data.
    // buildOutOfLap() masks it further and names it.
    if (store_->viewStart() < 0.0) {
        if (const UnifiedLap* previous = snapshot_.neighbourPrev) {
            const std::vector<double>* data = fieldFor(*previous, spec.field);
            buildSeries(builder, data, dataRect, range, alpha(traceColor, 150),
                        false, false, -1.0, 1.2, -1.0, 0.0);
        }
    }
    if (store_->viewEnd() > 1.0) {
        if (const UnifiedLap* next = snapshot_.neighbourNext) {
            const std::vector<double>* data = fieldFor(*next, spec.field);
            buildSeries(builder, data, dataRect, range, alpha(traceColor, 150),
                        false, false, 1.0, 1.2, 1.0, 2.0);
        }
    }
    if (store_->traceConfidenceMode())
        buildConfidenceBand(builder, store_->traceConfidenceBand(spec.field),
                            dataRect, range);
    // Keep the reference trace visually distinct from the active trace. It
    // is drawn first, so the active line still wins at exact overlap points.
    buildSeries(builder, compareData, dataRect, range, alpha(kOrange, 230),
                false, true, 0.0, 2.2);
    buildSeries(builder, primaryData, dataRect, range, traceColor, spec.filled,
                false, 0.0, 1.8);

    if (!range.empty || sidecarChannel)
        cursorLanes_.append(CursorLane{spec.field, dataRect, range.min,
                                       range.max, traceColor, range.gear});
}

void TraceView::buildGroupHeader(TraceSceneBuilder& builder,
                                 const ChannelSpec& spec, const QRectF& rect) {
    if (!overlayGroup(spec.groupId)) return;
    const QRectF band(0.0, rect.top(), width(), rect.height());
    builder.rect(band, alpha(kGridStrong, 70));
    builder.rect(QRectF(0.0, rect.top(), 3.0, rect.height()), kAccent);
}

double TraceView::sidecarValueAt(const QString& key, double fraction) const {
    if (!store_) return std::numeric_limits<double>::quiet_NaN();
    if (const std::vector<double>* data = store_->overlayChannelData(key)) {
        if (data->size() >= 2) {
            const double position =
                std::clamp(fraction, 0.0, 1.0) * double(data->size() - 1);
            const int low = int(std::floor(position));
            const int high = std::min(low + 1, int(data->size()) - 1);
            const double a = (*data)[size_t(low)];
            const double b = (*data)[size_t(high)];
            if (std::isfinite(a) && std::isfinite(b))
                return a + (b - a) * (position - double(low));
            if (std::isfinite(a)) return a;
            if (std::isfinite(b)) return b;
        }
    }
    qint64 lapStartNs = 0;
    qint64 lapEndNs = 0;
    if (!primaryLapWindowNs(&lapStartNs, &lapEndNs))
        return std::numeric_limits<double>::quiet_NaN();
    const UnifiedLap* primary = snapshot_.primary;
    qint64 hostNs = lapStartNs;
    if (primary && primary->time.size() >= 2) {
        const double position =
            std::clamp(fraction, 0.0, 1.0) * double(primary->time.size() - 1);
        const int low = int(std::floor(position));
        const int high = std::min(low + 1, int(primary->time.size()) - 1);
        const double time =
            primary->time[size_t(low)] +
            (primary->time[size_t(high)] - primary->time[size_t(low)]) *
                (position - double(low));
        hostNs = lapStartNs + qint64(std::llround(time * 1e9));
    } else {
        hostNs =
            lapStartNs + qint64(std::llround(std::clamp(fraction, 0.0, 1.0) *
                                             double(lapEndNs - lapStartNs)));
    }
    for (const OverlayGroup& group : store_->overlayGroups()) {
        for (const OverlayChannel& channel : group.channels) {
            if (channel.key != key || !channel.samples ||
                channel.samples->empty() || channel.periodNs <= 0)
                continue;
            const qint64 offset = hostNs - channel.t0HostNs;
            if (offset < 0) return std::numeric_limits<double>::quiet_NaN();
            const double index = double(offset) / double(channel.periodNs);
            const int last = int(channel.samples->size()) - 1;
            if (index < 0.0 || index > double(last))
                return std::numeric_limits<double>::quiet_NaN();
            const int low = int(std::floor(index));
            const int high = std::min(low + 1, last);
            const double a = (*channel.samples)[size_t(low)];
            const double b = (*channel.samples)[size_t(high)];
            if (std::isfinite(a) && std::isfinite(b))
                return a + (b - a) * (index - double(low));
            if (std::isfinite(a)) return a;
            if (std::isfinite(b)) return b;
            return std::numeric_limits<double>::quiet_NaN();
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

void TraceView::buildSpanTrack(TraceSceneBuilder& builder,
                               const ChannelSpec& spec, const QRectF& rect) {
    const OverlayGroup* group = overlayGroup(spec.groupId);
    if (!group) return;
    qint64 lapStartNs = 0;
    qint64 lapEndNs = 0;
    if (!primaryLapWindowNs(&lapStartNs, &lapEndNs)) return;
    qint64 clipStart = lapStartNs;
    qint64 clipEnd = lapEndNs;
    if (snapshot_.videoClipValid) {
        clipStart = std::max(clipStart, snapshot_.videoClipStartNs);
        clipEnd = std::min(clipEnd, snapshot_.videoClipEndNs);
    }
    if (clipEnd <= clipStart) return;
    const double lapSpan = double(lapEndNs - lapStartNs);
    const QRectF dataRect(labelWidth(), rect.top() + 2, width() - labelWidth(),
                          std::max(4.0, rect.height() - 4.0));
    builder.rect(dataRect, alpha(kGrid, 55));
    for (const OverlaySpan& span : group->spans) {
        if (span.name != spec.spanName || !span.visible) continue;
        const qint64 startNs = std::max(span.startHostNs, clipStart);
        const qint64 endNs = std::min(span.endHostNs, clipEnd);
        if (endNs <= startNs) continue;
        const double startFrac =
            std::clamp(double(startNs - lapStartNs) / lapSpan, 0.0, 1.0);
        const double endFrac =
            std::clamp(double(endNs - lapStartNs) / lapSpan, 0.0, 1.0);
        const double left = xForFrac(startFrac);
        const double right = xForFrac(endFrac);
        if (right - left < 1.0) continue;
        const QRectF bar(left, dataRect.top(), right - left, dataRect.height());
        builder.rect(bar, alpha(span.color, 200));
        builder.outline(bar, alpha(span.color, 240));
        const QString title = span.title.isEmpty() ? span.name : span.title;
        if (dataRect.height() >= 12.0 && bar.width() > 36.0 &&
            !title.isEmpty()) {
            builder.text(title, markerFont_, kForeground,
                         bar.adjusted(4, 0, -4, 0),
                         Qt::AlignLeft | Qt::AlignVCenter);
            if (dataRect.height() >= 24.0 && bar.width() > 88.0 &&
                !span.subtitle.isEmpty())
                builder.text(span.subtitle, unitFont_, kMuted,
                             bar.adjusted(4, bar.height() * 0.42, -4, -1),
                             Qt::AlignLeft | Qt::AlignVCenter);
        }
        SpanHit hit;
        hit.rect = bar;
        hit.title = title;
        hit.subtitle = span.subtitle;
        hit.color = span.color;
        hit.meta = span.meta;
        interaction_.addSpanHit(std::move(hit));
    }
}

void TraceView::buildDelta(TraceSceneBuilder& builder, const QRectF& rect) {
    if (!store_->comparing()) return;
    const QVector<double>& delta = *snapshot_.deltaTrace;
    const int n = delta.size();
    if (n < 2) return;

    if (layout_.deltaMaxAbsRef() <= 0.0) {
        layout_.deltaMaxAbsRef() = 0.001;
        for (double value : delta)
            layout_.deltaMaxAbsRef() =
                std::max(layout_.deltaMaxAbsRef(), std::fabs(value));
    }

    const int last = n - 1;
    const int columns = deviceColumns(rect);
    const double columnWidth = rect.width() / double(columns);
    const double viewStart = store_->viewStart();
    const double viewSpan = store_->viewSpan();
    auto toY = [&](double value) {
        return rect.top() + (1.0 - (value + layout_.deltaMaxAbsRef()) /
                                       (2.0 * layout_.deltaMaxAbsRef())) *
                                rect.height();
    };
    const double zeroY = std::clamp(toY(0.0), rect.top(), rect.bottom());

    // One path at every zoom: a per-column band against the zero line plus
    // a single tight stroke through the column centres. The delta is a
    // smooth cumulative trace, so centre interpolation is exact to well
    // under a pixel — no sparse/dense switch, no stretched bars.
    builder.reserveQuads(columns * 2);
    QVector<QPointF> points;
    points.reserve(columns + 1);
    auto flushStroke = [&]() {
        if (points.size() >= 2)
            builder.polyline(points.constData(), points.size(), 1.8,
                             kForeground);
        points.clear();
    };
    for (int column = 0; column < columns; ++column) {
        const double fraction =
            viewStart + viewSpan * (double(column) + 0.5) / double(columns);
        if (fraction < 0.0 || fraction > 1.0) {
            flushStroke();
            continue;
        }
        const double position = fraction * last;
        const int lower = std::clamp(int(std::floor(position)), 0, last);
        const int upper = std::min(lower + 1, last);
        const double value =
            delta[lower] + (delta[upper] - delta[lower]) * (position - lower);
        if (!std::isfinite(value)) {
            flushStroke();
            continue;
        }
        const double y = std::clamp(toY(value), rect.top(), rect.bottom());
        const double x = rect.left() + columnWidth * column;

        const QColor band = alpha(value < 0.0 ? kGreen : kRed, 48);
        builder.rect(
            QRectF(x, std::min(y, zeroY), columnWidth, std::fabs(zeroY - y)),
            band);

        points.append(QPointF(x + columnWidth * 0.5, y));
    }
    flushStroke();

    builder.dashedHLine(zeroY, rect.left(), rect.right(), kGridStrong);
    builder.text(QString("Δ +%1 / -%2")
                     .arg(layout_.deltaMaxAbsRef(), 0, 'f', 3)
                     .arg(layout_.deltaMaxAbsRef(), 0, 'f', 3),
                 canvasFont_, kAccent,
                 QRectF(rect.left(), rect.top() + 2, rect.width() - 8, 14),
                 Qt::AlignRight | Qt::AlignVCenter);
}

void TraceView::buildCornerZones(TraceSceneBuilder& builder,
                                 const QRectF& totalRect) {
    if (!store_ || snapshot_.corners->isEmpty()) return;
    const bool editing = store_->editingCorners();
    const auto& corners = *snapshot_.corners;
    for (const CornerZone& corner : corners) {
        const double x1 = xForFrac(corner.start);
        const double x2 = xForFrac(corner.end);
        if (x2 <= totalRect.left() || x1 >= totalRect.right()) continue;

        // The QML ruler owns corner chrome. Keep the scene-graph range inside
        // the data plot so zoomed, partially visible zones cannot tint labels.
        const double clippedLeft = std::max(x1, totalRect.left());
        const double clippedRight = std::min(x2, totalRect.right());
        builder.rect(QRectF(clippedLeft, totalRect.top(),
                            clippedRight - clippedLeft, totalRect.height()),
                     alpha(kMagenta, editing ? 22 : 8));
        if (x1 >= totalRect.left())
            builder.vLine(x1, totalRect.top(), totalRect.bottom(), 1.0,
                          alpha(kMagenta, editing ? 80 : 28));
        if (x2 <= totalRect.right())
            builder.vLine(x2, totalRect.top(), totalRect.bottom(), 1.0,
                          alpha(kMagenta, editing ? 80 : 28));

        if (editing) {
            if (x1 >= totalRect.left())
                builder.rect(
                    QRectF(x1 - 2, totalRect.top(), 4, totalRect.height()),
                    alpha(kOrange, 220));
            if (x2 <= totalRect.right())
                builder.rect(
                    QRectF(x2 - 2, totalRect.top(), 4, totalRect.height()),
                    alpha(kOrange, 220));
        }
    }
}

// Outside a focused corner the traces stay visible but recede, so the eye
// lands on the zone without losing the approach and exit context.
void TraceView::buildCornerFocus(TraceSceneBuilder& builder,
                                 const QRectF& totalRect) {
    if (!store_) return;
    const int focused = store_->focusedCorner();
    if (focused < 0 || focused >= snapshot_.corners->size()) return;
    const CornerZone& corner = (*snapshot_.corners)[focused];
    const double x1 =
        std::clamp(xForFrac(corner.start), totalRect.left(), totalRect.right());
    const double x2 =
        std::clamp(xForFrac(corner.end), totalRect.left(), totalRect.right());
    const QColor dim = alpha(backgroundColor_, 172);
    builder.rect(QRectF(totalRect.left(), totalRect.top(),
                        x1 - totalRect.left(), totalRect.height()),
                 dim);
    builder.rect(
        QRectF(x2, totalRect.top(), totalRect.right() - x2, totalRect.height()),
        dim);
    builder.vLine(x1, totalRect.top(), totalRect.bottom(), 1.0,
                  alpha(kMagenta, 150));
    builder.vLine(x2, totalRect.top(), totalRect.bottom(), 1.0,
                  alpha(kMagenta, 150));
    // Edge grips: the focused window can be slid or stretched from here.
    builder.rect(QRectF(x1 - 2, 2.0, 4, 17), alpha(kMagenta, 200));
    builder.rect(QRectF(x2 - 2, 2.0, 4, 17), alpha(kMagenta, 200));
}

// A corner near start/finish keeps its place in the left half, so the
// viewport runs off the end of the lap. What lies there is the neighbouring
// lap — worth seeing, but never confusable with the lap under analysis — or
// nothing at all, which reads as black.
void TraceView::buildOutOfLap(TraceSceneBuilder& builder,
                              const QRectF& totalRect) {
    if (!store_) return;
    const QColor empty(0, 0, 0);
    // Enough to push the neighbour behind the lap under analysis,
    // not so much that it disappears.
    const QColor mask = alpha(backgroundColor_, 110);

    const double lapStart = xForFrac(0.0);
    if (lapStart > totalRect.left()) {
        const QRectF region(totalRect.left(), totalRect.top(),
                            lapStart - totalRect.left(), totalRect.height());
        const QString label = store_->neighbourLabel(-1);
        builder.rect(region, label.isEmpty() ? empty : mask);
        builder.vLine(lapStart, totalRect.top(), totalRect.bottom(), 2.0,
                      alpha(kForeground, 160));
        if (!label.isEmpty())
            builder.text(QStringLiteral("« ") + label, markerFont_,
                         alpha(kMuted, 210),
                         QRectF(region.left() + 4, totalRect.top() + 4,
                                std::max(10.0, region.width() - 10.0), 12),
                         Qt::AlignRight | Qt::AlignVCenter);
    }

    const double lapEnd = xForFrac(1.0);
    if (lapEnd < totalRect.right()) {
        const QRectF region(lapEnd, totalRect.top(), totalRect.right() - lapEnd,
                            totalRect.height());
        const QString label = store_->neighbourLabel(1);
        builder.rect(region, label.isEmpty() ? empty : mask);
        builder.vLine(lapEnd, totalRect.top(), totalRect.bottom(), 2.0,
                      alpha(kForeground, 160));
        if (!label.isEmpty())
            builder.text(label + QStringLiteral(" »"), markerFont_,
                         alpha(kMuted, 210),
                         QRectF(region.left() + 6, totalRect.top() + 4,
                                std::max(10.0, region.width() - 10.0), 12),
                         Qt::AlignLeft | Qt::AlignVCenter);
    }
}

void TraceView::buildCursorScene(TraceSceneBuilder& builder) {
    // The cursor path reuses the snapshot captured at the last static build:
    // its compare-fraction map and lap pointers are stable while the cursor
    // moves, so a cursor frame never allocates the std::function. Only the
    // cursor position is read fresh.
    //
    // Stable while the cursor moves — not across a lap swap. The GUI thread
    // may have replaced a UnifiedLap (re-opening the selected file adopts a
    // fresh lap over the loaded one, a session change evicts the cache) and
    // this overlay can be synced before the static view rebuilds, so the
    // cached pointers are checked against the store every frame and the
    // snapshot is retaken only when they differ. A pointer compare is the
    // whole steady-state cost.
    if (store_ && (snapshot_.primary != store_->primaryUnified() ||
                   snapshot_.compare != store_->compareUnified()))
        snapshot_ = store_->traceSnapshot();
    const UnifiedLap* primary = snapshot_.primary;
    if (!primary) return;
    const UnifiedLap* compare = snapshot_.compare;
    const double fraction = store_->cursorFrac();
    const double cursorX = xForFrac(fraction);

    buildSelection(builder);
    builder.vLine(cursorX, cursorTop(), cursorBottom(), 1.0,
                  alpha(kAccent, 190));

    for (const CursorLane& lane : cursorLanes_) {
        const bool sidecar = lane.field.startsWith(QStringLiteral("sidecar:"));
        const std::vector<double>* primaryData =
            sidecar ? nullptr
            : lane.field.startsWith(QStringLiteral("raw:"))
                ? store_->extraChannelData(lane.field, false)
                : fieldFor(*primary, lane.field);
        double value = std::numeric_limits<double>::quiet_NaN();
        if (sidecar)
            value = sidecarValueAt(lane.field, fraction);
        else if (primaryData && !primaryData->empty()) {
            const int sample =
                std::min(int(primaryData->size()) - 1,
                         int(fraction * double(primaryData->size() - 1)));
            value = (*primaryData)[size_t(sample)];
        } else {
            continue;
        }
        const double span = std::max(1.0e-12, lane.max - lane.min);
        auto toY = [&](double yValue) {
            return lane.rect.top() +
                   (1.0 - (yValue - lane.min) / span) * lane.rect.height();
        };

        if (std::isfinite(value))
            builder.dot(QPointF(cursorX, toY(value)), 2.5, lane.color);

        QString valueText;
        if (!std::isfinite(value))
            valueText = QStringLiteral("—");
        else if (lane.field == "speed")
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
        else if (std::fabs(value) >= 100.0)
            valueText = QString::number(value, 'f', 0);
        else if (std::fabs(value) >= 10.0)
            valueText = QString::number(value, 'f', 1);
        else
            valueText = QString::number(value, 'f', 2);

        if (lane.rect.height() >= 12.0) {
            const bool compact = lane.rect.height() < 24.0;
            const QRectF valueRect(
                compact ? labelWidth() - 48.0 : 0.0,
                compact ? lane.rect.top() : lane.rect.top() + 15.0,
                compact ? 42.0 : labelWidth() - 6.0,
                compact ? lane.rect.height() : 12.0);
            builder.rect(valueRect, backgroundColor_);
            builder.text(valueText, valueFont_, lane.color, valueRect,
                         Qt::AlignRight | Qt::AlignVCenter);
        }

        if (compare && !lane.gear &&
            !lane.field.startsWith(QStringLiteral("sidecar:"))) {
            const std::vector<double>* compareData =
                lane.field.startsWith(QStringLiteral("raw:"))
                    ? store_->extraChannelData(lane.field, true)
                    : fieldFor(*compare, lane.field);
            if (!compareData || compareData->empty()) continue;
            const double compareFraction =
                snapshot_.compareFractionForPrimaryFraction(fraction);
            const int compareSample = std::min(
                int(compareData->size()) - 1,
                int(compareFraction * double(compareData->size() - 1)));
            builder.dot(
                QPointF(cursorX, toY((*compareData)[size_t(compareSample)])),
                1.8, alpha(kMuted, 190));
        }
    }

    buildCornerMarkers(builder);
    buildHoveredCornerDelta(builder);
}

void TraceView::buildSelection(TraceSceneBuilder& builder) {
    if (!store_ || interaction_.selectionStart() < 0.0 ||
        interaction_.selectionEnd() < 0.0)
        return;
    const UnifiedLap* primary = snapshot_.primary;
    if (!primary || primary->time.size() < 2) return;

    const double lo =
        std::min(interaction_.selectionStart(), interaction_.selectionEnd());
    const double hi =
        std::max(interaction_.selectionStart(), interaction_.selectionEnd());
    const double x0 = xForFrac(lo);
    const double x1 = xForFrac(hi);
    const QRectF selectionRect(x0, cursorTop(), std::max(1.0, x1 - x0),
                               std::max(1.0, cursorBottom() - cursorTop()));

    builder.rect(selectionRect, alpha(kAccent, 24));
    builder.outline(selectionRect, alpha(kAccent, 150));

    auto sampleStd = [](const std::vector<double>& values, double fraction) {
        const double position =
            std::clamp(fraction, 0.0, 1.0) * double(values.size() - 1);
        const int i0 =
            std::clamp(int(std::floor(position)), 0, int(values.size()) - 1);
        const int i1 = std::min(i0 + 1, int(values.size()) - 1);
        return values[size_t(i0)] +
               (values[size_t(i1)] - values[size_t(i0)]) * (position - i0);
    };
    const double primaryTime =
        sampleStd(primary->time, hi) - sampleStd(primary->time, lo);

    QString label = QString("t %1s").arg(primaryTime, 0, 'f', 3);
    QColor labelColor = kForeground;
    const QVector<double>& delta = *snapshot_.deltaTrace;
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
        label = QString("Δ %1%2s")
                    .arg(regionDelta >= 0.0 ? "+" : "")
                    .arg(regionDelta, 0, 'f', 3);
        labelColor = regionDelta > 0.01    ? kRed
                     : regionDelta < -0.01 ? kGreen
                                           : kForeground;
    }

    const QFontMetricsF metrics(pillFont_);
    const QSizeF textSize = metrics.size(Qt::TextSingleLine, label);
    const qreal pillWidth = textSize.width() + 14;
    const qreal pillHeight = textSize.height() + 8;
    qreal labelX = x1 + 6;
    if (labelX + pillWidth > width()) labelX = x0 - pillWidth - 6;
    labelX = std::clamp(labelX, 2.0, std::max(2.0, width() - pillWidth - 2));
    const QRectF pill(labelX, cursorTop() + 6, pillWidth, pillHeight);
    builder.rect(pill, alpha(backgroundColor_, 238));
    builder.outline(pill, alpha(labelColor, 170));
    builder.text(label, pillFont_, labelColor, pill, Qt::AlignCenter);
}

void TraceView::buildHoveredCornerDelta(TraceSceneBuilder& builder) {
    if (!store_ || !store_->comparing() || interaction_.hoveredCorner() < 0)
        return;
    const auto& corners = *snapshot_.corners;
    if (interaction_.hoveredCorner() >= corners.size()) return;
    const QVector<double>& delta = *snapshot_.deltaTrace;
    if (delta.size() < 2) return;

    const CornerZone& corner = corners[interaction_.hoveredCorner()];
    const int last = int(delta.size()) - 1;
    auto sampleDelta = [&](double fraction) {
        const double position = std::clamp(fraction, 0.0, 1.0) * double(last);
        const int i0 = std::clamp(int(std::floor(position)), 0, last);
        const int i1 = std::min(i0 + 1, last);
        return delta[i0] + (delta[i1] - delta[i0]) * (position - i0);
    };
    const double regionDelta =
        sampleDelta(corner.end) - sampleDelta(corner.start);
    const QString label = QString("%1%2s")
                              .arg(regionDelta >= 0.0 ? "+" : "")
                              .arg(regionDelta, 0, 'f', 3);
    const QColor color = regionDelta > 0.01    ? kRed
                         : regionDelta < -0.01 ? kGreen
                                               : kForeground;

    const double x1 = xForFrac(corner.start);
    const double x2 = xForFrac(corner.end);
    const QFontMetricsF labelMetrics(markerFont_);
    const QSizeF textSize = labelMetrics.size(Qt::TextSingleLine, label);
    const qreal inset = 2.0;
    const qreal bandY = 2.0;
    const qreal bandH = 17.0;
    qreal pillWidth = textSize.width() + 8;
    const qreal inner = std::max(0.0, (x2 - x1) - 2.0 * inset);
    if (pillWidth > inner) pillWidth = inner;
    const QRectF pill(x2 - inset - pillWidth, bandY + 1.0, pillWidth,
                      bandH - 2.0);
    if (pill.width() < 4.0) return;
    builder.rect(pill, alpha(backgroundColor_, 235));
    builder.outline(pill, alpha(color, 170));
    builder.text(label, markerFont_, color, pill, Qt::AlignCenter);
}

// Hover keeps the event name available without redrawing the guide over the
// channel geometry. The guide itself lives in the static scene behind traces.
void TraceView::buildCornerMarkers(TraceSceneBuilder& builder) {
    if (!store_ || store_->focusedCorner() < 0) return;
    const int highlighted = focusedMarkerIndex();
    if (highlighted < 0) return;
    const QVector<CornerMarker>& markers = *snapshot_.markers;
    if (highlighted >= markers.size()) return;

    const CornerMarker& marker = markers[highlighted];
    const QColor color = cornerMarkerColor(marker.key);
    const double top = 2.0;
    const double bottom = height() - kBottomPad;
    auto paintMarker = [&](double fraction, int fade) {
        const double x = xForFrac(fraction);
        if (x < labelWidth() || x > width()) return;
        builder.dashedVLine(x, top, bottom, alpha(color, fade), 1.5, 6.0, 4.0);
        builder.vLine(x, bottom - 10.0, bottom, 2.0, alpha(color, fade));
    };
    paintMarker(marker.fraction, 220);
    if (marker.referenceFraction >= 0.0 &&
        std::fabs(marker.referenceFraction - marker.fraction) >= 0.0005)
        paintMarker(marker.referenceFraction, 150);

    const double x = xForFrac(marker.fraction);
    if (x < labelWidth() || x > width()) return;
    const QFontMetricsF metrics(markerFont_);
    const QSizeF size = metrics.size(Qt::TextSingleLine, marker.label);
    QRectF pill(x + 4, bottom - size.height() - 20, size.width() + 10,
                size.height() + 4);
    if (pill.right() > width()) pill.moveRight(x - 4);
    builder.rect(pill, alpha(backgroundColor_, 235));
    builder.outline(pill, alpha(color, 190));
    builder.text(marker.label, markerFont_, color, pill, Qt::AlignCenter);
}
// Focused-corner events sit behind the channel geometry as dim dashed guides.
// They reach from the corner-label band to the distance axis, so the event is
// easy to track across every lane without obscuring a trace.
void TraceView::buildCornerMarkerGuides(TraceSceneBuilder& builder) {
    if (!store_ || store_->focusedCorner() < 0) return;
    const QVector<CornerMarker>& markers = *snapshot_.markers;
    const double bottom = height() - kBottomPad;
    for (const CornerMarker& marker : markers) {
        const double x = xForFrac(marker.fraction);
        if (x < labelWidth() || x > width()) continue;
        const QColor color = cornerMarkerColor(marker.key);
        builder.dashedVLine(x, 2.0, bottom, alpha(color, 105), 1.0, 5.0, 4.0);

        if (marker.referenceFraction < 0.0) continue;
        const double referenceX = xForFrac(marker.referenceFraction);
        if (referenceX < labelWidth() || referenceX > width() ||
            std::fabs(referenceX - x) < 0.5)
            continue;
        builder.dashedVLine(referenceX, 2.0, bottom, alpha(color, 70), 1.0, 2.0,
                            5.0);
    }
}

// ── hit testing ─────────────────────────────────────────────────────

int TraceView::addCornerAt(double fraction) {
    if (!store_) return -1;
    constexpr double width = 0.04;
    const double start = qBound(0.0, fraction - width * 0.5, 1.0 - width);
    return store_->addCorner(start, start + width);
}

void TraceView::hideChannel(const QString& key) {
    if (!store_) return;
    if (key.startsWith(QStringLiteral("overlay:"))) {
        store_->removeOverlay(key.mid(QStringLiteral("overlay:").size()));
        return;
    }
    store_->setChannelVisible(key, false);
}

void TraceView::showAllStandardChannels() {
    if (!store_) return;
    for (const ChannelSpec& channel : channelSpecs())
        if (channel.kind == ChannelSpec::Kind::Sample &&
            !channel.key.startsWith(QStringLiteral("raw:")) &&
            !channel.key.startsWith(QStringLiteral("sidecar:")))
            store_->setChannelVisible(channel.key, true);
}

// ── interaction ─────────────────────────────────────────────────────

void TraceView::mouseDoubleClickEvent(QMouseEvent* event) {
    interaction_.mouseDoubleClickEvent(event);
}

void TraceView::mousePressEvent(QMouseEvent* event) {
    interaction_.mousePressEvent(event);
}

void TraceView::mouseMoveEvent(QMouseEvent* event) {
    interaction_.mouseMoveEvent(event);
}

void TraceView::mouseReleaseEvent(QMouseEvent* event) {
    interaction_.mouseReleaseEvent(event);
}

void TraceView::wheelEvent(QWheelEvent* event) {
    interaction_.wheelEvent(event);
}

void TraceView::keyPressEvent(QKeyEvent* event) {
    if (!interaction_.keyPressEvent(event)) QQuickItem::keyPressEvent(event);
}

void TraceView::hoverMoveEvent(QHoverEvent* event) {
    interaction_.hoverMoveEvent(event);
}

void TraceView::hoverLeaveEvent(QHoverEvent* event) {
    interaction_.hoverLeaveEvent(event);
    QQuickItem::hoverLeaveEvent(event);
}

// ── cursor overlay item ─────────────────────────────────────────────

TraceCursorOverlay::TraceCursorOverlay(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(QQuickItem::ItemHasContents, true);
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
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

QSGNode* TraceCursorOverlay::updatePaintNode(QSGNode* oldNode,
                                             UpdatePaintNodeData*) {
    QSGNode* root = oldNode ? oldNode : new QSGNode;
    builder_.begin(window());
    if (trace_) trace_->buildCursorScene(builder_);
    builder_.commit(root);
    return root;
}

void TraceCursorOverlay::releaseResources() {
    builder_.releaseResources();
    QQuickItem::releaseResources();
}

QVariantMap TraceCursorOverlay::benchmarkGeometry(int frames) {
    frames = std::clamp(frames, 1, 2000);
    TraceSceneBuilder scratch;
    // The overlay draws into the lane rectangles the trace scene produces,
    // so build that once first or the measurement is of an empty frame.
    if (trace_) {
        scratch.begin(nullptr);
        trace_->buildScene(scratch);
    }
    QElapsedTimer clock;
    clock.start();
    int quads = 0;
    for (int i = 0; i < frames; ++i) {
        scratch.begin(nullptr);
        if (trace_) trace_->buildCursorScene(scratch);
        quads = scratch.quadCount();
    }
    const double elapsed = double(clock.nsecsElapsed()) / 1.0e6;
    return QVariantMap{{QStringLiteral("averageMs"), elapsed / frames},
                       {QStringLiteral("quads"), quads}};
}
