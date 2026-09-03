#include "TraceLaneLayout.h"

#include "TelemetryStore.h"
#include "TraceSnapshot.h"
#include "core/TelemetryEngine.h"

#include <QFontMetricsF>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kTopPad = 22.0;
constexpr double kBottomPad = 18.0;
constexpr double kMinLabelW = 62.0;
constexpr double kConsistencyStripHeight = 14.0;
constexpr double kGroupHeaderHeight = 20.0;
constexpr double kSpanTrackHeight = 14.0;
constexpr double kStandardSampleHeight = 72.0;

const QColor kAccent("#7fbbb3");

}  // namespace

void TraceLaneLayout::invalidateRanges() {
    rangeCache_.clear();
    gearCache_.clear();
    deltaMaxAbs_ = 0.0;
    if (onInvalidateScene) onInvalidateScene();
}

void TraceLaneLayout::rebuildChannelSpecs() {
    channelSpecs_.clear();
    auto add = [&](const QString& key, const QString& title,
                   const QString& unit, QColor color, Clamp clamp,
                   const QString& field) {
        ChannelSpec s;
        s.key = key;
        s.title = title;
        s.unit = unit;
        s.color = color;
        s.clamp = clamp;
        s.field = field;
        channelSpecs_.append(s);
    };
    add("delta", "Δ Time", "s", QColor("#83c092"), Clamp{0, 0, true, true}, "");
    add("speed", "Speed", "km/h", QColor("#a7c080"), Clamp{0, 0, true, false},
        "speed");
    add("throttle", "Throttle", "%", QColor("#a7c080"),
        Clamp{0, 1, false, false}, "throttle");
    add("brake", "Brake", "bar", QColor("#e67e80"), Clamp{0, 0, true, false},
        "brake");
    add("steering", "Steering", "deg", QColor("#dbbc7f"),
        Clamp{0, 0, true, true}, "steering");
    add("gear", "Gear", "", QColor("#d699b6"), Clamp{0, 7, false, false},
        "gear");
    add("dampers", "Dampers", "mm", QColor("#7fbbb3"), Clamp{0, 0, true, true},
        "damperFL");
    add("g_long", "G Long", "g", QColor("#e09d7f"), Clamp{0, 0, true, true},
        "gForceLong");
    add("clutch", "Clutch", "%", QColor("#d3c6aa"), Clamp{0, 1, false, false},
        "clutch");
    add("driver_throttle", "Driver throttle", "%", QColor("#9da9a0"),
        Clamp{0, 1, false, false}, "driverThrottle");
    add("gps_lat", "GPS latitude", "°", QColor("#83c092"),
        Clamp{0, 0, true, false}, "gpsLat");
    add("gps_lon", "GPS longitude", "°", QColor("#e09d7f"),
        Clamp{0, 0, true, false}, "gpsLon");
    if (store_) {
        if (const SessionHandle* session = store_->primarySession()) {
            for (const SourceChannelSummary& channel :
                 session->sourceChannels()) {
                const QString key = QStringLiteral("raw:") + channel.name;
                QColor color(store_->channelColor(key));
                if (!color.isValid()) color = QColor("#9da9a0");
                add(key, channel.name, channel.unit, color,
                    Clamp{0, 0, true, false}, key);
            }
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
    if (onLaneLayoutChanged) onLaneLayoutChanged();
}

void TraceLaneLayout::updateLabelWidth() {
    QFontMetricsF titleMetrics(labelFont_);
    QFontMetricsF unitMetrics(unitFont_);
    double widest = kMinLabelW - 8.0;
    // Only lanes that are on screen set the width. Every raw source channel
    // is a spec too (AiM names run to `+-_Test_Best_Time`), and sizing the
    // column for a hidden one steals trace width for nothing.
    for (const ChannelSpec& spec : channelSpecs_) {
        if (spec.kind == ChannelSpec::Kind::GroupHeader) continue;
        if (store_ && !store_->channelVisible(spec.key)) continue;
        if (!spec.title.isEmpty())
            widest =
                std::max(widest, titleMetrics.horizontalAdvance(spec.title));
        if (!spec.unit.isEmpty())
            widest = std::max(widest, unitMetrics.horizontalAdvance(spec.unit));
    }
    double next = std::ceil(widest + 10.0);
    if (itemWidth_ > 200.0)
        next = std::min(next, std::max(kMinLabelW, itemWidth_ * 0.5));
    if (std::fabs(next - labelWidth_) < 0.5) return;
    labelWidth_ = next;
    if (onLabelWidthChanged) onLabelWidthChanged();
}

double TraceLaneLayout::laneWeightFor(const ChannelSpec& spec) const {
    if (spec.kind != ChannelSpec::Kind::Sample) return 0.0;
    const double weight = std::max(0.25, store_->channelWeight(spec.key));
    const double speedBoost = spec.key == QStringLiteral("speed") ? 1.35 : 1.0;
    return weight * speedBoost;
}

QVector<TraceLaneLayout::Lane> TraceLaneLayout::layoutLanes() const {
    QVector<Lane> lanes;
    if (!store_) return lanes;
    const omatrack::UnifiedLap* compare = store_->compareUnified();
    double totalWeight = 0.0;
    QSet<QString> collapsed;
    for (const OverlayGroup& group : store_->overlayGroups()) {
        if (!group.expanded) collapsed.insert(group.id);
    }
    for (int i = 0; i < channelSpecs_.size(); ++i) {
        const ChannelSpec& spec = channelSpecs_[i];
        if (spec.key == QStringLiteral("delta") && !compare) continue;
        if (spec.kind == ChannelSpec::Kind::GroupHeader) {
            // Headers stay visible so a collapsed folder can be reopened.
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
    if (lanes.isEmpty()) {
        contentHeight_ = 0.0;
        return lanes;
    }

    const double consistencyHeight =
        store_->traceConfidenceMode() ? kConsistencyStripHeight : 0.0;
    const double available =
        std::max(0.0, itemHeight_ - kTopPad - kBottomPad - consistencyHeight);
    if (!fitChannels_) {
        double y = kTopPad + consistencyHeight - verticalScroll_;
        for (Lane& lane : lanes) {
            const ChannelSpec& spec = channelSpecs_[lane.spec];
            if (spec.kind == ChannelSpec::Kind::Sample)
                lane.height = kStandardSampleHeight *
                              std::max(0.5, store_->channelWeight(spec.key));
            lane.y = y;
            y += lane.height;
        }
        contentHeight_ = y + kBottomPad + verticalScroll_;
        const qreal maxScroll =
            std::max<qreal>(0.0, contentHeight_ - itemHeight_);
        if (verticalScroll_ > maxScroll) {
            verticalScroll_ = maxScroll;
            y = kTopPad + consistencyHeight - verticalScroll_;
            for (Lane& lane : lanes) {
                lane.y = y;
                y += lane.height;
            }
        }
        return lanes;
    }

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
    contentHeight_ = itemHeight_;
    return lanes;
}

QVariantList TraceLaneLayout::laneRows() const {
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

const std::vector<double>* TraceLaneLayout::fieldFor(
    const omatrack::UnifiedLap& lap, const QString& field) const {
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
        auto it = gearCache_.find(&lap);
        if (it == gearCache_.end()) {
            std::vector<double> buf;
            buf.resize(lap.gear.size());
            for (size_t i = 0; i < lap.gear.size(); ++i)
                buf[i] = double(lap.gear[i]);
            it = gearCache_.insert(&lap, std::move(buf));
        }
        return &it.value();
    }
    return nullptr;
}

const TraceLaneLayout::ChannelRange& TraceLaneLayout::rangeFor(
    const ChannelSpec& spec, const omatrack::UnifiedLap* primary,
    const omatrack::UnifiedLap* compare) {
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

const OverlayGroup* TraceLaneLayout::overlayGroup(const QString& id) const {
    if (!store_) return nullptr;
    for (const OverlayGroup& group : store_->overlayGroups()) {
        if (group.id == id) return &group;
    }
    return nullptr;
}

bool TraceLaneLayout::overlaySpanVisibleOnLap(const OverlaySpan& span) const {
    if (!span.visible) return false;
    qint64 lapStartNs = 0;
    qint64 lapEndNs = 0;
    if (!primaryLapWindowNs(&lapStartNs, &lapEndNs)) return false;
    qint64 clipStart = lapStartNs;
    qint64 clipEnd = lapEndNs;
    if (snapshot_->videoClipValid) {
        clipStart = std::max(clipStart, snapshot_->videoClipStartNs);
        clipEnd = std::min(clipEnd, snapshot_->videoClipEndNs);
    }
    return span.startHostNs < clipEnd && span.endHostNs > clipStart;
}

bool TraceLaneLayout::primaryLapWindowNs(qint64* startNs, qint64* endNs) const {
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
