#include "TraceView.h"

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

namespace {

constexpr double kTopPad = 22.0;
constexpr double kBottomPad = 18.0;
constexpr double kMinLabelW = 62.0;
// Height of the corner-marker strip along the bottom of the trace area.
constexpr double kMarkerBand = 26.0;
constexpr double kConsistencyStripHeight = 14.0;
constexpr double kGroupHeaderHeight = 20.0;
constexpr double kSpanTrackHeight = 14.0;
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

QColor mixColors(const QColor& from, const QColor& to, double amount) {
    const double factor = std::clamp(amount, 0.0, 1.0);
    return QColor::fromRgbF(
        from.redF() + (to.redF() - from.redF()) * factor,
        from.greenF() + (to.greenF() - from.greenF()) * factor,
        from.blueF() + (to.blueF() - from.blueF()) * factor,
        from.alphaF() + (to.alphaF() - from.alphaF()) * factor);
}

// The scene graph has no dashed-line primitive; a dash is just a short quad.
void dashedHLine(TraceSceneBuilder& builder, double y, double left,
                 double right, const QColor& color) {
    constexpr double kDash = 4.0;
    constexpr double kGap = 4.0;
    for (double x = left; x < right; x += kDash + kGap)
        builder.hLine(y, x, std::min(x + kDash, right), 1.0, color);
}
void dashedVLine(TraceSceneBuilder& builder, double x, double top,
                 double bottom, double dash, double gap, double width,
                 const QColor& color) {
    for (double y = top; y < bottom; y += dash + gap)
        builder.vLine(x, y, std::min(y + dash, bottom), width, color);
}

QColor cornerMarkerColor(const QString& key) {
    if (key == QStringLiteral("brake")) return kRed;
    if (key == QStringLiteral("apex")) return kMagenta;
    if (key == QStringLiteral("pickup")) return kGreen;
    return kAccent;
}

void outline(TraceSceneBuilder& builder, const QRectF& rect,
             const QColor& color) {
    builder.hLine(rect.top(), rect.left(), rect.right(), 1.0, color);
    builder.hLine(rect.bottom(), rect.left(), rect.right(), 1.0, color);
    builder.vLine(rect.left(), rect.top(), rect.bottom(), 1.0, color);
    builder.vLine(rect.right(), rect.top(), rect.bottom(), 1.0, color);
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
    rebuildChannelSpecs();
}

void TraceView::setBackgroundColor(const QColor& color) {
    if (!color.isValid() || backgroundColor_ == color) return;
    backgroundColor_ = color;
    update();
    emit backgroundColorChanged();
}
qreal TraceView::rulerHeight() const { return kTopPad; }

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
            hoveredMarker_ = -1;
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

void TraceView::invalidateRanges() {
    rangeCache_.clear();
    deltaMaxAbs_ = 0.0;
    invalidateScene();
}

void TraceView::rebuildChannelSpecs() {
    channelSpecs_.clear();
    // ordered like omatrack: delta on top, then main channels
    auto add = [&](const QString& key, const QString& title,
                   const QString& unit, QColor color, Clamp clamp, bool filled,
                   const QString& field) {
        ChannelSpec s;
        s.key = key;
        s.title = title;
        s.unit = unit;
        s.color = color;
        s.clamp = clamp;
        s.filled = filled;
        s.field = field;
        channelSpecs_.append(s);
    };
    add("delta", "Δ Time", "s", QColor("#83c092"), Clamp{0, 0, true, true},
        true, "");
    add("speed", "Speed", "km/h", QColor("#a7c080"), Clamp{0, 0, true, false},
        false, "speed");
    add("throttle", "Throttle", "%", QColor("#a7c080"),
        Clamp{0, 1, false, false}, true, "throttle");
    add("brake", "Brake", "bar", QColor("#e67e80"), Clamp{0, 0, true, false},
        true, "brake");
    add("steering", "Steering", "deg", QColor("#dbbc7f"),
        Clamp{0, 0, true, true}, false, "steering");
    add("gear", "Gear", "", QColor("#d699b6"), Clamp{0, 7, false, false}, false,
        "gear");
    add("dampers", "Dampers", "mm", QColor("#7fbbb3"), Clamp{0, 0, true, true},
        false, "damperFL");
    add("g_long", "G Long", "g", QColor("#e09d7f"), Clamp{0, 0, true, true},
        false, "gForceLong");
    add("clutch", "Clutch", "%", QColor("#d3c6aa"), Clamp{0, 1, false, false},
        true, "clutch");
    add("driver_throttle", "Driver throttle", "%", QColor("#9da9a0"),
        Clamp{0, 1, false, false}, false, "driverThrottle");
    add("gps_lat", "GPS latitude", "°", QColor("#83c092"),
        Clamp{0, 0, true, false}, false, "gpsLat");
    add("gps_lon", "GPS longitude", "°", QColor("#e09d7f"),
        Clamp{0, 0, true, false}, false, "gpsLon");
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
                Clamp{0, 0, true, false}, false, key);
        }
        for (const OverlayGroup& group : store_->overlayGroups()) {
            ChannelSpec header;
            header.key = QStringLiteral("overlay:") + group.id;
            header.title = group.name;
            header.kind = ChannelSpec::Kind::GroupHeader;
            header.groupId = group.id;
            header.color = kAccent;
            channelSpecs_.append(header);
            for (const OverlaySpanLane& lane : group.spanLanes) {
                ChannelSpec spans;
                spans.key = lane.key;
                spans.title = lane.name;
                spans.kind = ChannelSpec::Kind::SpanTrack;
                spans.groupId = group.id;
                spans.spanName = lane.name;
                spans.color = kAccent;
                channelSpecs_.append(spans);
            }
            for (const OverlayChannel& channel : group.channels) {
                QColor color(store_->channelColor(channel.key));
                if (!color.isValid()) color = QColor("#9da9a0");
                ChannelSpec spec;
                spec.key = channel.key;
                spec.title = channel.name;
                spec.unit = channel.unit;
                spec.color = color;
                spec.clamp = Clamp{0, 0, true, false};
                spec.field = channel.key;
                spec.groupId = group.id;
                channelSpecs_.append(spec);
            }
        }
    }
    updateLabelWidth();
    emit laneLayoutChanged();
}

void TraceView::updateLabelWidth() {
    QFontMetricsF titleMetrics(labelFont_);
    QFontMetricsF unitMetrics(unitFont_);
    double widest = kMinLabelW - 8.0;
    for (const ChannelSpec& spec : channelSpecs_) {
        if (spec.kind == ChannelSpec::Kind::GroupHeader) continue;
        if (!spec.title.isEmpty())
            widest =
                std::max(widest, titleMetrics.horizontalAdvance(spec.title));
        if (!spec.unit.isEmpty())
            widest = std::max(widest, unitMetrics.horizontalAdvance(spec.unit));
    }
    double next = std::ceil(widest + 10.0);
    if (width() > 200.0)
        next = std::min(next, std::max(kMinLabelW, width() * 0.5));
    if (std::fabs(next - labelWidth_) < 0.5) return;
    labelWidth_ = next;
    emit labelWidthChanged();
}

double TraceView::xForFrac(double frac) const {
    return labelWidth() + (frac - store_->viewStart()) / store_->viewSpan() *
                              (width() - labelWidth());
}

double TraceView::fracForX(double x) const {
    double w = std::max(1.0, width() - labelWidth());
    return store_->viewStart() +
           std::clamp((x - labelWidth()) / w, 0.0, 1.0) * store_->viewSpan();
}

double TraceView::laneWeightFor(const ChannelSpec& spec) const {
    if (spec.kind != ChannelSpec::Kind::Sample) return 0.0;
    const double weight = std::max(0.25, store_->channelWeight(spec.key));
    // Speed is the lane a driver reads first; it keeps a share premium.
    const double speedBoost = spec.key == QStringLiteral("speed") ? 1.35 : 1.0;
    return weight * speedBoost;
}

// Every visible lane is laid out as its weight share of the item height, so
// the workspace shows all of them at once and never scrolls vertically.
QVector<TraceView::Lane> TraceView::layoutLanes() const {
    QVector<Lane> lanes;
    if (!store_) return lanes;
    const UnifiedLap* compare = store_->compareUnified();
    double totalWeight = 0.0;
    QSet<QString> collapsed;
    for (const OverlayGroup& group : store_->overlayGroups()) {
        if (!group.expanded) collapsed.insert(group.id);
    }
    for (int i = 0; i < channelSpecs_.size(); ++i) {
        const ChannelSpec& spec = channelSpecs_[i];
        if (spec.key == QStringLiteral("delta") && !compare) continue;
        if (spec.kind == ChannelSpec::Kind::GroupHeader) {
            // headers stay visible so a collapsed folder can be reopened
        } else if (!spec.groupId.isEmpty() &&
                   collapsed.contains(spec.groupId)) {
            continue;
        } else if (spec.kind == ChannelSpec::Kind::SpanTrack) {
            if (!store_->channelVisible(spec.key)) continue;
            const OverlayGroup* group = overlayGroup(spec.groupId);
            bool any = false;
            if (group) {
                for (const OverlaySpan& span : group->spans) {
                    if (span.name == spec.spanName &&
                        overlaySpanVisibleOnLap(span)) {
                        any = true;
                        break;
                    }
                }
            }
            if (!any) continue;
        } else if (spec.kind == ChannelSpec::Kind::Sample &&
                   !store_->channelVisible(spec.key)) {
            continue;
        }
        Lane lane;
        lane.spec = i;
        if (spec.kind == ChannelSpec::Kind::GroupHeader)
            lane.height = kGroupHeaderHeight;
        else if (spec.kind == ChannelSpec::Kind::SpanTrack)
            lane.height = kSpanTrackHeight;
        else {
            lane.height = laneWeightFor(spec);
            totalWeight += lane.height;
        }
        lanes.append(lane);
    }
    if (lanes.isEmpty()) return lanes;

    const double consistencyHeight =
        store_->traceConfidenceMode() ? kConsistencyStripHeight : 0.0;
    const double available =
        std::max(0.0, height() - kTopPad - kBottomPad - consistencyHeight);
    double preferredFixed = 0.0;
    for (const Lane& lane : lanes) {
        const ChannelSpec& spec = channelSpecs_[lane.spec];
        if (spec.kind != ChannelSpec::Kind::Sample)
            preferredFixed += lane.height;
    }
    const double fixedBudget = totalWeight > 0.0 ? available * 0.35 : available;
    const double fixedScale = preferredFixed > 0.0
                                  ? std::min(1.0, fixedBudget / preferredFixed)
                                  : 1.0;
    const double weightedAvailable =
        std::max(0.0, available - preferredFixed * fixedScale);

    double y = kTopPad + consistencyHeight;
    for (Lane& lane : lanes) {
        const ChannelSpec& spec = channelSpecs_[lane.spec];
        lane.y = y;
        if (spec.kind == ChannelSpec::Kind::Sample)
            lane.height = totalWeight > 0.0
                              ? weightedAvailable * lane.height / totalWeight
                              : 0.0;
        else
            lane.height *= fixedScale;
        y += lane.height;
    }
    return lanes;
}
QVariantList TraceView::laneRows() const {
    QVariantList rows;
    for (const Lane& lane : layoutLanes()) {
        const ChannelSpec& spec = channelSpecs_[lane.spec];
        QString kind = QStringLiteral("sample");
        if (spec.kind == ChannelSpec::Kind::GroupHeader)
            kind = QStringLiteral("group");
        else if (spec.kind == ChannelSpec::Kind::SpanTrack)
            kind = QStringLiteral("span");

        QVariantMap row{{QStringLiteral("key"), spec.key},
                        {QStringLiteral("kind"), kind},
                        {QStringLiteral("title"), spec.title},
                        {QStringLiteral("unit"), spec.unit},
                        {QStringLiteral("color"), spec.color},
                        {QStringLiteral("y"), lane.y},
                        {QStringLiteral("height"), lane.height}};
        if (spec.kind == ChannelSpec::Kind::GroupHeader) {
            if (const OverlayGroup* group = overlayGroup(spec.groupId)) {
                QVariantList chromeRows;
                for (const OverlayChrome& chrome : group->chrome) {
                    chromeRows.append(
                        QVariantMap{{QStringLiteral("kind"), chrome.kind},
                                    {QStringLiteral("text"), chrome.text},
                                    {QStringLiteral("label"), chrome.label},
                                    {QStringLiteral("value"), chrome.value}});
                }
                row.insert(QStringLiteral("expanded"), group->expanded);
                row.insert(QStringLiteral("chrome"), chromeRows);
            }
        }
        rows.append(row);
    }
    return rows;
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

// ── scene graph ─────────────────────────────────────────────────────

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
    cursorTop_ = 0.0;
    cursorBottom_ = 0.0;
    if (width() <= 0.0 || height() <= 0.0) return;
    builder.rect(QRectF(0, 0, width(), height()), backgroundColor_);

    const UnifiedLap* primary = store_ ? store_->primaryUnified() : nullptr;
    if (!store_ || !primary || primary->size() < 2) {
        builder.text(QStringLiteral("Select a session to begin"),
                     emptyStateFont_, kDim, QRectF(0, 0, width(), height()),
                     Qt::AlignCenter);
        return;
    }
    const UnifiedLap* compare = store_->compareUnified();

    const QVector<Lane> lanes = layoutLanes();
    if (lanes.isEmpty()) return;
    buildCornerMarkerGuides(builder);
    if (store_->traceConfidenceMode())
        buildConsistencyStrip(
            builder, QRectF(labelWidth(), kTopPad, width() - labelWidth(),
                            kConsistencyStripHeight));

    spanHits_.clear();
    hoveredSpan_ = -1;
    for (const Lane& lane : lanes) {
        const ChannelSpec& spec = channelSpecs_[lane.spec];
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

    cursorTop_ = kTopPad - 3;
    cursorBottom_ = height() - kBottomPad;
}

const TraceView::ChannelRange& TraceView::rangeFor(const ChannelSpec& spec,
                                                   const UnifiedLap* primary,
                                                   const UnifiedLap* compare) {
    auto cached = rangeCache_.find(spec.key);
    if (cached != rangeCache_.end()) return cached.value();

    ChannelRange range;
    range.min = spec.clamp.min;
    range.max = spec.clamp.max;
    range.gear = spec.field == QStringLiteral("gear");

    const bool rawChannel = spec.field.startsWith(QStringLiteral("raw:"));
    const bool sidecarChannel =
        spec.field.startsWith(QStringLiteral("sidecar:"));
    const std::vector<double>* primaryData =
        primary ? (sidecarChannel ? store_->overlayChannelData(spec.key)
                   : rawChannel   ? store_->extraChannelData(spec.key, false)
                                  : fieldFor(*primary, spec.field))
                : nullptr;
    const std::vector<double>* compareData =
        compare && !range.gear && !sidecarChannel
            ? (rawChannel ? store_->extraChannelData(spec.key, true)
                          : fieldFor(*compare, spec.field))
            : nullptr;
    range.empty = !primaryData || primaryData->size() < 2;

    if (spec.clamp.autoRange) {
        range.min = 1e18;
        range.max = -1e18;
        auto includeRange = [&](const std::vector<double>* values) {
            if (!values) return;
            for (double value : *values) {
                if (!std::isfinite(value)) continue;
                range.min = std::min(range.min, value);
                range.max = std::max(range.max, value);
            }
        };
        includeRange(primaryData);
        includeRange(compareData);
        if (store_->traceConfidenceMode()) {
            if (const TraceConfidenceBand* band =
                    store_->traceConfidenceBand(spec.field)) {
                includeRange(&band->lower);
                includeRange(&band->upper);
            }
        }
        if (!(range.max > range.min)) {
            range.min = 0.0;
            range.max = 1.0;
        }
        if (spec.clamp.symmetric) {
            const double magnitude =
                std::max(std::fabs(range.min), std::fabs(range.max));
            range.min = magnitude < 1e-6 ? -1.0 : -magnitude;
            range.max = magnitude < 1e-6 ? 1.0 : magnitude;
        } else {
            const double padding = (range.max - range.min) * 0.06;
            range.min -= padding;
            range.max += padding;
        }
    }
    if (!(range.max > range.min)) {
        range.min = 0.0;
        range.max = 1.0;
    }
    auto inserted = rangeCache_.insert(spec.key, range);
    return inserted.value();
}

qreal TraceView::devicePixelRatio() const {
    return window() ? std::max(1.0, window()->effectiveDevicePixelRatio())
                    : 1.0;
}

int TraceView::deviceColumns(const QRectF& rect) const {
    return std::max(2, int(std::lround(rect.width() * devicePixelRatio())));
}

void TraceView::emitSeriesStroke(TraceSceneBuilder& builder,
                                 const QVector<QPointF>& points,
                                 const QRectF& rect, const QColor& color,
                                 bool fill, qreal width) {
    if (points.size() < 2) return;
    if (fill) {
        const QColor fillColor = alpha(color, 42);
        builder.reserveQuads(points.size());
        for (int i = 1; i < points.size(); ++i) {
            const QPointF& from = points[i - 1];
            const QPointF& to = points[i];
            const qreal top = std::min(from.y(), to.y());
            if (top >= rect.bottom()) continue;
            builder.fillQuad(from, to, QPointF(to.x(), rect.bottom()),
                             QPointF(from.x(), rect.bottom()), fillColor);
        }
    }
    // A slightly wider, dimmer stroke under the core line hides the hard
    // quad edge that 4× MSAA alone still leaves on a steep slope.
    builder.polyline(points.constData(), points.size(), width + 0.9,
                     alpha(color, 80));
    builder.polyline(points.constData(), points.size(), width, color);
}

void TraceView::buildSeries(TraceSceneBuilder& builder,
                            const std::vector<double>* values,
                            const QRectF& rect, const ChannelRange& range,
                            const QColor& color, bool fill, bool alignCompare,
                            double shift, qreal width, double clipLow,
                            double clipHigh) {
    if (!values || values->size() < 2 || rect.width() < 2.0) return;
    const int valueLast = int(values->size()) - 1;
    const double span = std::max(1.0e-12, range.max - range.min);
    const double viewStart = store_->viewStart();
    const double viewSpan = store_->viewSpan();
    const qreal dpr = devicePixelRatio();
    const int columns = deviceColumns(rect);
    const double columnWidth = rect.width() / double(columns);
    const QColor fillColor = alpha(color, 42);

    auto toY = [&](double value) {
        return rect.top() + (1.0 - (value - range.min) / span) * rect.height();
    };
    auto sourceFraction = [&](double fraction) {
        const double shifted = std::clamp(fraction - shift, 0.0, 1.0);
        return alignCompare ? store_->compareFractionForPrimaryFraction(shifted)
                            : shifted;
    };
    auto viewX = [&](double fraction) {
        return rect.left() + (fraction - viewStart) / viewSpan * rect.width();
    };

    const double samplesPerPixel =
        (viewSpan * double(valueLast)) / std::max(1.0, rect.width() * dpr);
    if (samplesPerPixel < 1.25) {
        QVector<QPointF> points;
        auto flush = [&]() {
            emitSeriesStroke(builder, points, rect, color, fill, width);
            points.clear();
        };
        if (!alignCompare) {
            const double viewLo = std::max(clipLow, viewStart);
            const double viewHi = std::min(clipHigh, viewStart + viewSpan);
            const int first = std::clamp(
                int(std::floor((viewLo - shift) * double(valueLast))) - 1, 0,
                valueLast);
            const int last = std::clamp(
                int(std::ceil((viewHi - shift) * double(valueLast))) + 1, 0,
                valueLast);
            points.reserve(last - first + 1);
            for (int i = first; i <= last; ++i) {
                const double viewFrac = double(i) / double(valueLast) + shift;
                if (viewFrac < clipLow || viewFrac > clipHigh) {
                    flush();
                    continue;
                }
                const double value = (*values)[size_t(i)];
                if (!std::isfinite(value)) {
                    flush();
                    continue;
                }
                points.append(
                    QPointF(viewX(viewFrac),
                            std::clamp(toY(value), rect.top(), rect.bottom())));
            }
        } else {
            points.reserve(columns);
            for (int column = 0; column < columns; ++column) {
                const double fraction = viewStart + viewSpan *
                                                        (double(column) + 0.5) /
                                                        double(columns);
                if (fraction < clipLow || fraction > clipHigh) {
                    flush();
                    continue;
                }
                const double position = sourceFraction(fraction) * valueLast;
                const int lower =
                    std::clamp(int(std::floor(position)), 0, valueLast);
                const int upper = std::min(lower + 1, valueLast);
                const double value =
                    (*values)[size_t(lower)] +
                    ((*values)[size_t(upper)] - (*values)[size_t(lower)]) *
                        (position - double(lower));
                if (!std::isfinite(value)) {
                    flush();
                    continue;
                }
                points.append(
                    QPointF(rect.left() + columnWidth * (double(column) + 0.5),
                            std::clamp(toY(value), rect.top(), rect.bottom())));
            }
        }
        flush();
        return;
    }

    builder.reserveQuads(columns * (fill ? 2 : 1));
    bool hasPrevious = false;
    double previousTop = 0.0;
    double previousBottom = 0.0;
    for (int column = 0; column < columns; ++column) {
        const double startFraction =
            viewStart + viewSpan * double(column) / double(columns);
        const double endFraction =
            viewStart + viewSpan * double(column + 1) / double(columns);
        // The viewport may run past the lap: a series only owns the columns
        // inside its own fraction window, and the clamp in sourceFraction
        // would otherwise smear its first and last sample across the rest.
        const double centre = (startFraction + endFraction) * 0.5;
        if (centre < clipLow || centre > clipHigh) {
            hasPrevious = false;
            continue;
        }
        double from = sourceFraction(startFraction) * valueLast;
        double to = sourceFraction(endFraction) * valueLast;
        if (to < from) std::swap(from, to);

        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        const int firstIndex = std::clamp(int(std::floor(from)), 0, valueLast);
        const int lastIndex = std::clamp(int(std::ceil(to)), 0, valueLast);
        if (lastIndex - firstIndex >= 2) {
            for (int i = firstIndex; i <= lastIndex; ++i) {
                const double value = (*values)[size_t(i)];
                if (!std::isfinite(value)) continue;
                low = std::min(low, value);
                high = std::max(high, value);
            }
        } else {
            const int lower = firstIndex;
            const int upper = std::min(lower + 1, valueLast);
            const double value =
                (*values)[size_t(lower)] +
                ((*values)[size_t(upper)] - (*values)[size_t(lower)]) *
                    (from - double(lower));
            if (std::isfinite(value)) low = high = value;
        }
        if (low > high) {
            hasPrevious = false;
            continue;
        }

        double top = std::clamp(toY(high), rect.top(), rect.bottom());
        double bottom = std::clamp(toY(low), rect.top(), rect.bottom());
        if (hasPrevious) {
            if (bottom < previousTop) bottom = previousTop;
            if (top > previousBottom) top = previousBottom;
        }
        const double x = rect.left() + columnWidth * column;
        const qreal half = std::max(0.5 / dpr, width * 0.5);

        if (fill && bottom < rect.bottom())
            builder.rect(QRectF(x, top, columnWidth, rect.bottom() - top),
                         fillColor);
        builder.rect(
            QRectF(x, top - half, columnWidth, (bottom - top) + half * 2.0),
            color);

        previousTop = top;
        previousBottom = bottom;
        hasPrevious = true;
    }
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
        dashedHLine(builder, toY(0), dataRect.left(), dataRect.right(),
                    kGridStrong);

    // Whatever the viewport shows before lap start / after lap end is the
    // neighbouring lap, drawn faintly so it reads as context, not as data.
    // buildOutOfLap() masks it further and names it.
    if (store_->viewStart() < 0.0) {
        if (const UnifiedLap* previous = store_->neighbourUnified(-1)) {
            const std::vector<double>* data = fieldFor(*previous, spec.field);
            buildSeries(builder, data, dataRect, range, alpha(traceColor, 150),
                        false, false, -1.0, 1.2, -1.0, 0.0);
        }
    }
    if (store_->viewEnd() > 1.0) {
        if (const UnifiedLap* next = store_->neighbourUnified(1)) {
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
                false, true, store_->referenceAlignment(), 2.2);
    buildSeries(builder, primaryData, dataRect, range, traceColor, spec.filled,
                false, 0.0, 1.8);

    if (!range.empty || sidecarChannel)
        cursorLanes_.append(CursorLane{spec.field, dataRect, range.min,
                                       range.max, traceColor, range.gear});
}

bool TraceView::primaryLapWindowNs(qint64* startNs, qint64* endNs) const {
    if (!startNs || !endNs || !store_ || !store_->primarySession())
        return false;
    const int lapId = store_->primaryLapIndex();
    for (const LapEntry& lap : store_->primarySession()->laps()) {
        if (lap.lapId != lapId || !(lap.endTime > lap.startTime)) continue;
        *startNs = qint64(std::llround(lap.startTime * 1e9));
        *endNs = qint64(std::llround(lap.endTime * 1e9));
        return *endNs > *startNs;
    }
    return false;
}

void TraceView::buildGroupHeader(TraceSceneBuilder& builder,
                                 const ChannelSpec& spec, const QRectF& rect) {
    if (!overlayGroup(spec.groupId)) return;
    const QRectF band(0.0, rect.top(), width(), rect.height());
    builder.rect(band, alpha(kGridStrong, 70));
    builder.rect(QRectF(0.0, rect.top(), 3.0, rect.height()), kAccent);
}

const OverlayGroup* TraceView::overlayGroup(const QString& id) const {
    if (!store_) return nullptr;
    for (const OverlayGroup& group : store_->overlayGroups()) {
        if (group.id == id) return &group;
    }
    return nullptr;
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
    const UnifiedLap* primary = store_->primaryUnified();
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

bool TraceView::overlaySpanVisibleOnLap(const OverlaySpan& span) const {
    if (!span.visible) return false;
    qint64 lapStartNs = 0;
    qint64 lapEndNs = 0;
    if (!primaryLapWindowNs(&lapStartNs, &lapEndNs)) return false;
    qint64 clipStart = lapStartNs;
    qint64 clipEnd = lapEndNs;
    qint64 videoStart = 0;
    qint64 videoEnd = 0;
    if (store_->videoClipWindowNs(&videoStart, &videoEnd)) {
        clipStart = std::max(clipStart, videoStart);
        clipEnd = std::min(clipEnd, videoEnd);
    }
    return span.startHostNs < clipEnd && span.endHostNs > clipStart;
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
    qint64 videoStart = 0;
    qint64 videoEnd = 0;
    if (store_->videoClipWindowNs(&videoStart, &videoEnd)) {
        clipStart = std::max(clipStart, videoStart);
        clipEnd = std::min(clipEnd, videoEnd);
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
        outline(builder, bar, alpha(span.color, 240));
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
        spanHits_.append(std::move(hit));
    }
}

void TraceView::buildDelta(TraceSceneBuilder& builder, const QRectF& rect) {
    if (!store_->comparing()) return;
    const QVector<double>& delta = store_->deltaTrace();
    const int n = delta.size();
    if (n < 2) return;

    if (deltaMaxAbs_ <= 0.0) {
        deltaMaxAbs_ = 0.001;
        for (double value : delta)
            deltaMaxAbs_ = std::max(deltaMaxAbs_, std::fabs(value));
    }

    const int last = n - 1;
    const int columns = deviceColumns(rect);
    const double columnWidth = rect.width() / double(columns);
    const double viewStart = store_->viewStart();
    const double viewSpan = store_->viewSpan();
    const qreal dpr = devicePixelRatio();
    const double samplesPerPixel =
        (viewSpan * double(last)) / std::max(1.0, rect.width() * dpr);
    auto toY = [&](double value) {
        return rect.top() +
               (1.0 - (value + deltaMaxAbs_) / (2.0 * deltaMaxAbs_)) *
                   rect.height();
    };
    const double zeroY = std::clamp(toY(0.0), rect.top(), rect.bottom());

    if (samplesPerPixel < 1.25) {
        QVector<QPointF> points;
        const int first =
            std::clamp(int(std::floor(viewStart * last)) - 1, 0, last);
        const int lastIndex = std::clamp(
            int(std::ceil((viewStart + viewSpan) * last)) + 1, 0, last);
        points.reserve(lastIndex - first + 1);
        for (int i = first; i <= lastIndex; ++i) {
            const double fraction = double(i) / double(last);
            const double y =
                std::clamp(toY(delta[i]), rect.top(), rect.bottom());
            const double x =
                rect.left() + (fraction - viewStart) / viewSpan * rect.width();
            points.append(QPointF(x, y));
        }
        if (points.size() >= 2) {
            builder.reserveQuads(points.size());
            for (int i = 1; i < points.size(); ++i) {
                const QPointF& from = points[i - 1];
                const QPointF& to = points[i];
                const double midY = 0.5 * (from.y() + to.y());
                const QColor band = alpha(midY > zeroY ? kGreen : kRed, 48);
                builder.fillQuad(from, to, QPointF(to.x(), zeroY),
                                 QPointF(from.x(), zeroY), band);
            }
            builder.polyline(points.constData(), points.size(), 1.8 + 0.9,
                             alpha(kForeground, 80));
            builder.polyline(points.constData(), points.size(), 1.8,
                             kForeground);
        }
    } else {
        builder.reserveQuads(columns * 2);
        bool hasPrevious = false;
        double previousY = 0.0;
        for (int column = 0; column < columns; ++column) {
            const double fraction =
                viewStart + viewSpan * double(column) / double(columns);
            if (fraction < 0.0 || fraction > 1.0) {
                hasPrevious = false;
                continue;
            }
            const double position = std::clamp(fraction, 0.0, 1.0) * last;
            const int lower = std::clamp(int(std::floor(position)), 0, last);
            const int upper = std::min(lower + 1, last);
            const double value = delta[lower] + (delta[upper] - delta[lower]) *
                                                    (position - lower);
            const double y = std::clamp(toY(value), rect.top(), rect.bottom());
            const double x = rect.left() + columnWidth * column;

            const QColor band = alpha(value < 0.0 ? kGreen : kRed, 48);
            builder.rect(QRectF(x, std::min(y, zeroY), columnWidth,
                                std::fabs(zeroY - y)),
                         band);

            double top = y;
            double bottom = y;
            if (hasPrevious) {
                top = std::min(top, previousY);
                bottom = std::max(bottom, previousY);
            }
            const qreal half = std::max(0.5 / dpr, 0.85);
            builder.rect(
                QRectF(x, top - half, columnWidth, bottom - top + half * 2.0),
                kForeground);
            previousY = y;
            hasPrevious = true;
        }
    }

    dashedHLine(builder, zeroY, rect.left(), rect.right(), kGridStrong);
    builder.text(QString("Δ +%1 / -%2")
                     .arg(deltaMaxAbs_, 0, 'f', 3)
                     .arg(deltaMaxAbs_, 0, 'f', 3),
                 canvasFont_, kAccent,
                 QRectF(rect.left(), rect.top() + 2, rect.width() - 8, 14),
                 Qt::AlignRight | Qt::AlignVCenter);
}

void TraceView::buildCornerZones(TraceSceneBuilder& builder,
                                 const QRectF& totalRect) {
    if (!store_ || store_->corners().isEmpty()) return;
    const bool editing = store_->editingCorners();
    const auto& corners = store_->corners();
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
    if (focused < 0 || focused >= store_->corners().size()) return;
    const CornerZone& corner = store_->corners()[focused];
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
    if (!store_ || cursorLanes_.isEmpty()) return;
    const UnifiedLap* primary = store_->primaryUnified();
    if (!primary) return;
    const UnifiedLap* compare = store_->compareUnified();
    const double fraction = store_->cursorFrac();
    const double cursorX = xForFrac(fraction);

    buildSelection(builder);
    builder.vLine(cursorX, cursorTop_, cursorBottom_, 1.0, alpha(kAccent, 190));

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
                store_->compareFractionForPrimaryFraction(std::clamp(
                    fraction - store_->referenceAlignment(), 0.0, 1.0));
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
    if (!store_ || selectionStart_ < 0.0 || selectionEnd_ < 0.0) return;
    const UnifiedLap* primary = store_->primaryUnified();
    if (!primary || primary->time.size() < 2) return;

    const double lo = std::min(selectionStart_, selectionEnd_);
    const double hi = std::max(selectionStart_, selectionEnd_);
    const double x0 = xForFrac(lo);
    const double x1 = xForFrac(hi);
    const QRectF selectionRect(x0, cursorTop_, std::max(1.0, x1 - x0),
                               std::max(1.0, cursorBottom_ - cursorTop_));

    builder.rect(selectionRect, alpha(kAccent, 24));
    outline(builder, selectionRect, alpha(kAccent, 150));

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
    const QRectF pill(labelX, cursorTop_ + 6, pillWidth, pillHeight);
    builder.rect(pill, alpha(backgroundColor_, 238));
    outline(builder, pill, alpha(labelColor, 170));
    builder.text(label, pillFont_, labelColor, pill, Qt::AlignCenter);
}

void TraceView::buildHoveredCornerDelta(TraceSceneBuilder& builder) {
    if (!store_ || !store_->comparing() || hoveredCorner_ < 0) return;
    const auto& corners = store_->corners();
    if (hoveredCorner_ >= corners.size()) return;
    const QVector<double>& delta = store_->deltaTrace();
    if (delta.size() < 2) return;

    const CornerZone& corner = corners[hoveredCorner_];
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
    outline(builder, pill, alpha(color, 170));
    builder.text(label, markerFont_, color, pill, Qt::AlignCenter);
}

// Hover keeps the event name available without redrawing the guide over the
// channel geometry. The guide itself lives in the static scene behind traces.
void TraceView::buildCornerMarkers(TraceSceneBuilder& builder) {
    if (!store_ || store_->focusedCorner() < 0) return;
    const int highlighted = focusedMarkerIndex();
    if (highlighted < 0) return;
    const QVector<CornerMarker>& markers = store_->cornerMarkers();
    if (highlighted >= markers.size()) return;

    const CornerMarker& marker = markers[highlighted];
    const QColor color = cornerMarkerColor(marker.key);
    const double top = 2.0;
    const double bottom = height() - kBottomPad;
    auto paintMarker = [&](double fraction, int fade) {
        const double x = xForFrac(fraction);
        if (x < labelWidth() || x > width()) return;
        dashedVLine(builder, x, top, bottom, 6.0, 4.0, 1.5, alpha(color, fade));
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
    outline(builder, pill, alpha(color, 190));
    builder.text(marker.label, markerFont_, color, pill, Qt::AlignCenter);
}
// Focused-corner events sit behind the channel geometry as dim dashed guides.
// They reach from the corner-label band to the distance axis, so the event is
// easy to track across every lane without obscuring a trace.
void TraceView::buildCornerMarkerGuides(TraceSceneBuilder& builder) {
    if (!store_ || store_->focusedCorner() < 0) return;
    const QVector<CornerMarker>& markers = store_->cornerMarkers();
    const double bottom = height() - kBottomPad;
    for (const CornerMarker& marker : markers) {
        const double x = xForFrac(marker.fraction);
        if (x < labelWidth() || x > width()) continue;
        const QColor color = cornerMarkerColor(marker.key);
        dashedVLine(builder, x, 2.0, bottom, 5.0, 4.0, 1.0, alpha(color, 105));

        if (marker.referenceFraction < 0.0) continue;
        const double referenceX = xForFrac(marker.referenceFraction);
        if (referenceX < labelWidth() || referenceX > width() ||
            std::fabs(referenceX - x) < 0.5)
            continue;
        dashedVLine(builder, referenceX, 2.0, bottom, 2.0, 5.0, 1.0,
                    alpha(color, 70));
    }
}

// ── hit testing ─────────────────────────────────────────────────────

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

int TraceView::channelIndexAt(const QPointF& position) const {
    for (const Lane& lane : layoutLanes())
        if (position.y() >= lane.y && position.y() < lane.y + lane.height)
            return lane.spec;
    return -1;
}

// Corner markers live in a shallow band along the bottom of the trace area.
int TraceView::markerIndexAt(const QPointF& position) const {
    if (!store_ || store_->focusedCorner() < 0) return -1;
    if (position.y() < cursorBottom_ - kMarkerBand ||
        position.y() > cursorBottom_)
        return -1;
    const QVector<CornerMarker>& markers = store_->cornerMarkers();
    int best = -1;
    double bestDistance = 9.0;
    for (int i = 0; i < markers.size(); ++i) {
        const double distance =
            std::fabs(position.x() - xForFrac(markers[i].fraction));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

int TraceView::focusedMarkerIndex() const {
    if (!store_ || store_->focusedCorner() < 0) return -1;
    if (hoveredMarker_ >= 0) return hoveredMarker_;
    const QString key = store_->highlightedCornerMarker();
    if (key.isEmpty()) return -1;
    const QVector<CornerMarker>& markers = store_->cornerMarkers();
    for (int i = 0; i < markers.size(); ++i)
        if (markers[i].key == key) return i;
    return -1;
}

int TraceView::focusedZoneHandleAt(const QPointF& position) const {
    if (!store_ || store_->focusedCorner() < 0) return 0;
    const int focused = store_->focusedCorner();
    if (focused >= store_->corners().size()) return 0;
    const CornerZone& corner = store_->corners()[focused];
    const double x = position.x();
    const double x1 = xForFrac(corner.start);
    const double x2 = xForFrac(corner.end);
    constexpr double kEdge = 8.0;
    if (std::fabs(x - x1) <= kEdge) return 1;
    if (std::fabs(x - x2) <= kEdge) return 2;
    if (position.y() <= kTopPad && x >= x1 && x <= x2) return 3;
    return 0;
}

void TraceView::updateZoneHoverCursor(const QPointF& position) {
    if (dragging_ || panning_ || selecting_) return;
    switch (focusedZoneHandleAt(position)) {
        case 1:
        case 2: setCursor(Qt::SizeHorCursor); return;
        case 3: setCursor(Qt::OpenHandCursor); return;
        default: break;
    }
    unsetCursor();
}

// Shared by the hover and the button-held move paths so a marker opens up
// whichever way the pointer arrives over it.
void TraceView::updateHoveredMarker(const QPointF& position) {
    const int marker = markerIndexAt(position);
    if (marker == hoveredMarker_) return;
    hoveredMarker_ = marker;
    emit overlayChanged();
}

void TraceView::updateHoveredCorner(const QPointF& position) {
    const int index = cornerIndexAt(position);
    if (index == hoveredCorner_) return;
    hoveredCorner_ = index;
    emit overlayChanged();
}

void TraceView::updateHoveredSpan(const QPointF& position) {
    spanHoverX_ = position.x();
    spanHoverY_ = position.y();
    int index = -1;
    for (int i = 0; i < spanHits_.size(); ++i) {
        if (spanHits_.at(i).rect.contains(position)) {
            index = i;
            break;
        }
    }
    if (index == hoveredSpan_) {
        if (spanHoverVisible_) emit spanHoverChanged();
        return;
    }
    hoveredSpan_ = index;
    if (index < 0 || index >= spanHits_.size()) {
        spanHoverVisible_ = false;
        spanHoverTitle_.clear();
        spanHoverSubtitle_.clear();
        spanHoverMeta_.clear();
    } else {
        const SpanHit& hit = spanHits_.at(index);
        spanHoverVisible_ = true;
        spanHoverTitle_ = hit.title;
        spanHoverSubtitle_ = hit.subtitle;
        spanHoverColor_ = hit.color;
        spanHoverMeta_ = hit.meta;
    }
    emit spanHoverChanged();
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
    const QString title =
        spec.kind == ChannelSpec::Kind::GroupHeader ? spec.title : spec.title;
    emit channelMenuRequested(spec.key, title, store_->channelWeight(spec.key),
                              position.x(), position.y());
}

int TraceView::groupHeaderAt(const QPointF& position) const {
    for (const Lane& lane : layoutLanes()) {
        if (position.y() < lane.y || position.y() >= lane.y + lane.height)
            continue;
        if (lane.spec < 0 || lane.spec >= channelSpecs_.size()) return -1;
        if (channelSpecs_[lane.spec].kind == ChannelSpec::Kind::GroupHeader)
            return lane.spec;
        return -1;
    }
    return -1;
}

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
    for (const ChannelSpec& channel : channelSpecs_)
        if (channel.kind == ChannelSpec::Kind::Sample &&
            !channel.key.startsWith(QStringLiteral("raw:")) &&
            !channel.key.startsWith(QStringLiteral("sidecar:")))
            store_->setChannelVisible(channel.key, true);
}

// ── interaction ─────────────────────────────────────────────────────

// Double-click is the escape hatch out of any zoom, including corner focus.
void TraceView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    panning_ = false;
    selecting_ = false;
    selectionStart_ = -1.0;
    selectionEnd_ = -1.0;
    store_->resetView();
    unsetCursor();
    emit overlayChanged();
    event->accept();
}

void TraceView::mousePressEvent(QMouseEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    const double x = event->position().x();
    const double fraction = fracForX(x);
    if (event->button() == Qt::RightButton) {
        if (store_->editingCorners() || event->position().y() < kTopPad)
            showCornerMenu(event->position());
        else
            showChannelMenu(event->position());
        event->accept();
        return;
    }

    // Middle-drag pans, so left-drag is free to select a range.
    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        lastPanFrac_ = fraction;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) return;

    const int header = groupHeaderAt(event->position());
    if (header >= 0 && header < channelSpecs_.size()) {
        const QString groupId = channelSpecs_[header].groupId;
        store_->setOverlayExpanded(groupId, !store_->overlayExpanded(groupId));
        event->accept();
        return;
    }

    // A focused corner window can be slid or resized without entering
    // the global corner-edit mode: edges stretch it, the label band moves it.
    const int focusHandle = focusedZoneHandleAt(event->position());
    if (focusHandle > 0) {
        dragCorner_ = store_->focusedCorner();
        dragCornerMove_ = focusHandle == 3;
        dragStartFrac_ = dragCornerMove_
                             ? fraction - store_->corners()[dragCorner_].start
                             : 0.0;
        dragging_ = true;
        setCursor(dragCornerMove_ ? Qt::ClosedHandCursor : Qt::SizeHorCursor);
        event->accept();
        return;
    }

    // Clicking a corner in the ruler zooms the workspace onto it.
    const int cornerIndex = cornerIndexAt(event->position());
    if (cornerIndex >= 0) {
        store_->focusCorner(cornerIndex);
        event->accept();
        return;
    }

    // Corner editing takes precedence over selection.
    if (store_->editingCorners() && !store_->corners().isEmpty()) {
        for (int i = 0; i < store_->corners().size(); ++i) {
            const CornerZone& corner = store_->corners()[i];
            const double x1 = xForFrac(corner.start);
            const double x2 = xForFrac(corner.end);
            if (std::fabs(x - x1) < 5 || std::fabs(x - x2) < 5) {
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

    pressX_ = x;
    selecting_ = true;
    selectionStart_ = fraction;
    selectionEnd_ = fraction;
    store_->setCursorFrac(fraction);
    emit cursorChangedFromCanvas();
    setCursor(Qt::CrossCursor);
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
    updateHoveredMarker(event->position());
}

void TraceView::mouseReleaseEvent(QMouseEvent* event) {
    if (dragging_ && dragCorner_ >= 0 && store_) store_->saveCorners();
    dragging_ = false;
    panning_ = false;
    dragCorner_ = -1;
    if (selecting_) {
        selecting_ = false;
        // A click, not a drag: no range worth keeping on screen.
        if (std::fabs(event->position().x() - pressX_) < 3.0) {
            selectionStart_ = -1.0;
            selectionEnd_ = -1.0;
        }
    }
    unsetCursor();
    emit overlayChanged();
}

void TraceView::wheelEvent(QWheelEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    double delta = event->angleDelta().y();
    if (delta == 0.0) delta = event->pixelDelta().y();
    if (delta == 0.0) return;
    // Every lane is on screen, so the wheel has one job: zoom.
    const double anchor = fracForX(event->position().x());
    store_->zoomAt(anchor, std::pow(0.8, delta / 120.0));
    event->accept();
}

void TraceView::keyPressEvent(QKeyEvent* event) {
    if (!store_) return;
    int steps = 0;
    switch (event->key()) {
        case Qt::Key_Left: steps = -kCursorSamplesPerStep; break;
        case Qt::Key_Right: steps = kCursorSamplesPerStep; break;
        case Qt::Key_Escape:
            event->accept();
            store_->clearCornerFocus();
            return;
        case Qt::Key_Home:
            event->accept();
            store_->jumpToFraction(0.0);
            return;
        case Qt::Key_End:
            event->accept();
            store_->jumpToFraction(1.0);
            return;
        default: QQuickItem::keyPressEvent(event); return;
    }
    store_->moveCursorSteps(steps);
    emit cursorChangedFromCanvas();
    event->accept();
}

void TraceView::hoverMoveEvent(QHoverEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    updateZoneHoverCursor(event->position());
    updateHoveredCorner(event->position());
    updateHoveredSpan(event->position());
    if (cursorTimer_.isValid() && cursorTimer_.elapsed() < kHoverFrameMs) {
        event->accept();
        return;
    }
    cursorTimer_.restart();
    updateHoveredMarker(event->position());
    if (selecting_) {
        selectionEnd_ = fracForX(event->position().x());
        emit overlayChanged();
    }
    event->accept();
}

void TraceView::hoverLeaveEvent(QHoverEvent* event) {
    if (hoveredMarker_ >= 0 || hoveredCorner_ >= 0 || hoveredSpan_ >= 0 ||
        spanHoverVisible_) {
        hoveredMarker_ = -1;
        hoveredCorner_ = -1;
        hoveredSpan_ = -1;
        spanHoverVisible_ = false;
        spanHoverTitle_.clear();
        spanHoverSubtitle_.clear();
        spanHoverMeta_.clear();
        emit overlayChanged();
        emit spanHoverChanged();
    }
    if (!dragging_ && !panning_ && !selecting_) unsetCursor();
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
