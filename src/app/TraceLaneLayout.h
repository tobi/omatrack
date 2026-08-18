// Lane layout for the trace workspace: channel specs, lane geometry, and
// per-channel vertical range caching.
//
// Extracted from TraceView so the QQuickItem shell owns rendering while the
// layout computation lives in a testable plain class. TraceView sets the
// store/snapshot pointers, item size, and fonts, then calls rebuildSpecs /
// layoutLanes / laneRows. Range and gear caches are invalidated together
// through invalidateRanges().

#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <functional>
#include <vector>

class TelemetryStore;

struct TraceSnapshot;
struct OverlayGroup;
struct OverlaySpan;

namespace omatrack {
struct UnifiedLap;
}

class TraceLaneLayout {
public:
    struct Clamp {
        double min = 0.0;
        double max = 1.0;
        bool autoRange = false;
        bool symmetric = false;
    };

    struct ChannelSpec {
        enum class Kind { Sample, GroupHeader, SpanTrack };
        QString key;
        QString title;
        QString unit;
        QColor color;
        Clamp clamp;
        bool filled = false;
        // which UnifiedLap field to read; empty = derived
        QString field;
        Kind kind = Kind::Sample;
        QString groupId;
        QString spanName;
    };

    // One visible lane, sized as its weight share of the item height.
    struct Lane {
        int spec = 0;
        double y = 0.0;
        double height = 0.0;
    };

    // Cached vertical range of one channel across the whole lap.
    struct ChannelRange {
        double min = 0.0;
        double max = 1.0;
        bool gear = false;
        bool empty = true;
    };

    void setStore(TelemetryStore* store) { store_ = store; }
    void setSnapshot(TraceSnapshot* snapshot) { snapshot_ = snapshot; }
    void setLabelFont(const QFont& font) { labelFont_ = font; }
    void setUnitFont(const QFont& font) { unitFont_ = font; }
    void setItemSize(double w, double h) {
        itemWidth_ = w;
        itemHeight_ = h;
    }

    double labelWidth() const { return labelWidth_; }
    bool fitChannels() const { return fitChannels_; }
    void setFitChannels(bool fit) { fitChannels_ = fit; }
    qreal verticalScroll() const { return verticalScroll_; }
    void setVerticalScroll(qreal scroll) { verticalScroll_ = scroll; }
    qreal contentHeight() const { return contentHeight_; }
    double deltaMaxAbs() const { return deltaMaxAbs_; }
    double& deltaMaxAbsRef() { return deltaMaxAbs_; }
    double itemWidth() const { return itemWidth_; }
    double itemHeight() const { return itemHeight_; }

    const QVector<ChannelSpec>& channelSpecs() const { return channelSpecs_; }

    void rebuildChannelSpecs();
    QVector<Lane> layoutLanes() const;
    QVariantList laneRows() const;
    void updateLabelWidth();
    void invalidateRanges();
    const ChannelRange& rangeFor(const ChannelSpec& spec,
                                 const omatrack::UnifiedLap* primary,
                                 const omatrack::UnifiedLap* compare);
    const std::vector<double>* fieldFor(const omatrack::UnifiedLap& lap,
                                        const QString& field) const;
    const OverlayGroup* overlayGroup(const QString& id) const;
    bool overlaySpanVisibleOnLap(const OverlaySpan& span) const;
    bool primaryLapWindowNs(qint64* startNs, qint64* endNs) const;

    // Callbacks — TraceView wires these to its QML-facing signals.
    std::function<void()> onLaneLayoutChanged;
    std::function<void()> onLabelWidthChanged;
    std::function<void()> onInvalidateScene;

private:
    double laneWeightFor(const ChannelSpec& spec) const;

    TelemetryStore* store_ = nullptr;
    TraceSnapshot* snapshot_ = nullptr;
    QFont labelFont_;
    QFont unitFont_;
    double itemWidth_ = 0.0;
    double itemHeight_ = 0.0;
    double labelWidth_ = 62.0;
    bool fitChannels_ = true;
    mutable qreal verticalScroll_ = 0.0;
    mutable qreal contentHeight_ = 0.0;
    QVector<ChannelSpec> channelSpecs_;
    QHash<QString, ChannelRange> rangeCache_;
    mutable QHash<const omatrack::UnifiedLap*, std::vector<double>> gearCache_;
    double deltaMaxAbs_ = 0.001;
};
