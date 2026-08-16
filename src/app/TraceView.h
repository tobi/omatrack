// TraceView — scene-graph telemetry trace surface.
//
// A QQuickItem that builds its frame as QSGGeometryNode content through
// TraceSceneBuilder: every lane, grid line, corner zone and trace becomes
// vertex-coloured triangles in a single batch, and text is composited from
// cached textures. Nothing is rasterised with QPainter on the frame path, so
// the cost of a frame is geometry generation plus one GPU draw call rather
// than QPainter path filling.
//
// Renders the unified 50 Hz lap channels (speed, throttle, brake, steering,
// gear, dampers, delta-time), a shared cursor, zoom/pan viewport, and corner
// zones with drag-to-edit.
//
// Every visible lane always fits the item height: lane height is the channel's
// weight share of the available space, so the workspace never scrolls
// vertically. A focused corner zooms the viewport, dims the traces outside the
// zone, and annotates the zoomed view with brake / turn-in / apex / throttle
// markers.

#pragma once

#include <QtQml/qqmlregistration.h>
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "TelemetryStore.h"
#include "TraceSceneBuilder.h"

#include <vector>

class TelemetryStore;

namespace omatrack {
struct UnifiedLap;
}

class TraceView : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(
        TelemetryStore* store READ store WRITE setStore NOTIFY storeChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE
                   setBackgroundColor NOTIFY backgroundColorChanged)
    Q_PROPERTY(qreal labelWidth READ labelWidth NOTIFY labelWidthChanged)
    Q_PROPERTY(QVariantList laneRows READ laneRows NOTIFY laneLayoutChanged)
    Q_PROPERTY(qreal rulerHeight READ rulerHeight CONSTANT)
    Q_PROPERTY(
        bool spanHoverVisible READ spanHoverVisible NOTIFY spanHoverChanged)
    Q_PROPERTY(
        QString spanHoverTitle READ spanHoverTitle NOTIFY spanHoverChanged)
    Q_PROPERTY(QString spanHoverSubtitle READ spanHoverSubtitle NOTIFY
                   spanHoverChanged)
    Q_PROPERTY(
        QColor spanHoverColor READ spanHoverColor NOTIFY spanHoverChanged)
    Q_PROPERTY(
        QVariantList spanHoverMeta READ spanHoverMeta NOTIFY spanHoverChanged)
    Q_PROPERTY(qreal spanHoverX READ spanHoverX NOTIFY spanHoverChanged)
    Q_PROPERTY(qreal spanHoverY READ spanHoverY NOTIFY spanHoverChanged)

public:
    explicit TraceView(QQuickItem* parent = nullptr);

    TelemetryStore* store() const { return store_; }
    void setStore(TelemetryStore* store);
    QColor backgroundColor() const { return backgroundColor_; }
    void setBackgroundColor(const QColor& color);
    qreal labelWidth() const { return labelWidth_; }
    qreal rulerHeight() const;
    QVariantList laneRows() const;

    bool spanHoverVisible() const { return spanHoverVisible_; }
    QString spanHoverTitle() const { return spanHoverTitle_; }
    QString spanHoverSubtitle() const { return spanHoverSubtitle_; }
    QColor spanHoverColor() const { return spanHoverColor_; }
    QVariantList spanHoverMeta() const { return spanHoverMeta_; }
    qreal spanHoverX() const { return spanHoverX_; }
    qreal spanHoverY() const { return spanHoverY_; }

    // Context-menu actions, driven by the QML Material menus.
    Q_INVOKABLE int addCornerAt(double fraction);
    Q_INVOKABLE void hideChannel(const QString& key);
    Q_INVOKABLE void showAllStandardChannels();

    /// Rebuilds the frame's geometry `frames` times without committing it and
    /// reports {averageMs, quads, lanes}. This is the CPU half of the
    /// renderer — the only part of a frame this process controls — and is what
    /// the acceptance harness benchmarks.
    Q_INVOKABLE QVariantMap benchmarkGeometry(int frames);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             UpdatePaintNodeData* data) override;
    void releaseResources() override;
    void geometryChange(const QRectF& newGeometry,
                        const QRectF& oldGeometry) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void hoverLeaveEvent(QHoverEvent* event) override;

signals:
    void storeChanged();
    void backgroundColorChanged();
    void labelWidthChanged();
    void laneLayoutChanged();
    void cursorChangedFromCanvas();
    void cornerEdited();
    void cornerRenameRequested(int index);
    void cornerMenuRequested(int cornerIndex, const QString& cornerName,
                             double fraction, qreal x, qreal y);
    void channelMenuRequested(const QString& key, const QString& title,
                              double weight, qreal x, qreal y);
    void overlayChanged();
    void spanHoverChanged();
    void channelsRequested();

private:
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

    struct SpanHit {
        QRectF rect;
        QString title;
        QString subtitle;
        QColor color;
        QVariantList meta;
    };

    struct CursorLane {
        QString field;
        QRectF rect;
        double min = 0.0;
        double max = 1.0;
        QColor color;
        bool gear = false;
    };

    // One visible lane, sized as its weight share of the item height.
    struct Lane {
        int spec = 0;
        double y = 0.0;
        double height = 0.0;
    };

    // Cached vertical range of one channel across the whole lap. Only the
    // range is cached: the vertices themselves are cheap to regenerate and
    // depend on the viewport.
    struct ChannelRange {
        double min = 0.0;
        double max = 1.0;
        bool gear = false;
        bool empty = true;
    };

    void rebuildChannelSpecs();
    QVector<Lane> layoutLanes() const;
    void buildScene(TraceSceneBuilder& builder);
    void buildCursorScene(TraceSceneBuilder& builder);
    void buildSelection(TraceSceneBuilder& builder);
    void buildCornerMarkerGuides(TraceSceneBuilder& builder);
    void buildCornerMarkers(TraceSceneBuilder& builder);
    void invalidateScene();
    void invalidateRanges();
    const ChannelRange& rangeFor(const ChannelSpec& spec,
                                 const omatrack::UnifiedLap* primary,
                                 const omatrack::UnifiedLap* compare);
    void buildChannel(TraceSceneBuilder& builder, const ChannelSpec& spec,
                      const QRectF& rect, const omatrack::UnifiedLap* primary,
                      const omatrack::UnifiedLap* compare);
    void buildGroupHeader(TraceSceneBuilder& builder, const ChannelSpec& spec,
                          const QRectF& rect);
    void buildSpanTrack(TraceSceneBuilder& builder, const ChannelSpec& spec,
                        const QRectF& rect);
    const OverlayGroup* overlayGroup(const QString& id) const;
    bool overlaySpanVisibleOnLap(const OverlaySpan& span) const;
    double sidecarValueAt(const QString& key, double fraction) const;
    void buildConfidenceBand(TraceSceneBuilder& builder,
                             const TraceConfidenceBand* band,
                             const QRectF& rect, const ChannelRange& range);
    void buildConsistencyStrip(TraceSceneBuilder& builder, const QRectF& rect);
    /// Emits one lap's trace for `values` into `rect`. Zoomed-out columns
    /// carry a min/max envelope at device-pixel width. Once there is less
    /// than one sample per device pixel the same series is a polyline
    /// through the samples, so a slope stays a line instead of a staircase.
    void buildSeries(TraceSceneBuilder& builder,
                     const std::vector<double>* values, const QRectF& rect,
                     const ChannelRange& range, const QColor& color, bool fill,
                     bool alignCompare, double shift, qreal width,
                     double clipLow = 0.0, double clipHigh = 1.0);
    void emitSeriesStroke(TraceSceneBuilder& builder,
                          const QVector<QPointF>& points, const QRectF& rect,
                          const QColor& color, bool fill, qreal width);
    qreal devicePixelRatio() const;
    int deviceColumns(const QRectF& rect) const;
    /// Masks the space outside the lap and names the lap on the other side.
    void buildOutOfLap(TraceSceneBuilder& builder, const QRectF& totalRect);
    void buildDelta(TraceSceneBuilder& builder, const QRectF& rect);
    void buildCornerZones(TraceSceneBuilder& builder, const QRectF& totalRect);
    void buildCornerFocus(TraceSceneBuilder& builder, const QRectF& totalRect);
    void buildHoveredCornerDelta(TraceSceneBuilder& builder);
    double xForFrac(double frac) const;
    int cornerIndexAt(const QPointF& position) const;
    int markerIndexAt(const QPointF& position) const;
    int focusedMarkerIndex() const;
    // 0 = none, 1 = start edge, 2 = end edge, 3 = label-band body.
    int focusedZoneHandleAt(const QPointF& position) const;
    void updateHoveredMarker(const QPointF& position);
    void updateHoveredCorner(const QPointF& position);
    void updateHoveredSpan(const QPointF& position);
    void updateZoneHoverCursor(const QPointF& position);
    int groupHeaderAt(const QPointF& position) const;
    bool primaryLapWindowNs(qint64* startNs, qint64* endNs) const;
    double fracForX(double x) const;
    int channelIndexAt(const QPointF& position) const;
    void showChannelMenu(const QPointF& position);
    void showCornerMenu(const QPointF& position);
    double laneWeightFor(const ChannelSpec& spec) const;
    void updateLabelWidth();
    const std::vector<double>* fieldFor(const omatrack::UnifiedLap& lap,
                                        const QString& field) const;

    double deltaMaxAbs_ = 0.001;
    QVector<CursorLane> cursorLanes_;
    QHash<QString, ChannelRange> rangeCache_;
    double cursorTop_ = 0.0;
    double cursorBottom_ = 0.0;
    double selectionStart_ = -1.0;
    double selectionEnd_ = -1.0;
    bool selecting_ = false;

    double labelWidth_ = 62.0;
    TelemetryStore* store_ = nullptr;
    QColor backgroundColor_{QStringLiteral("#181d20")};
    QVector<ChannelSpec> channelSpecs_;
    bool dragging_ = false;
    bool panning_ = false;
    int dragCorner_ = -1;
    bool dragCornerMove_ = false;
    double dragStartFrac_ = 0.0;
    double lastPanFrac_ = 0.0;
    int hoveredMarker_ = -1;
    int hoveredCorner_ = -1;
    int hoveredSpan_ = -1;
    bool spanHoverVisible_ = false;
    QString spanHoverTitle_;
    QString spanHoverSubtitle_;
    QColor spanHoverColor_;
    QVariantList spanHoverMeta_;
    qreal spanHoverX_ = 0;
    qreal spanHoverY_ = 0;
    QVector<SpanHit> spanHits_;
    double pressX_ = 0.0;
    QFont canvasFont_;
    QFont emptyStateFont_;
    QFont labelFont_;
    QFont unitFont_;
    QFont valueFont_;
    QFont markerFont_;
    QFont pillFont_;
    friend class TraceCursorOverlay;
    QElapsedTimer cursorTimer_;
    TraceSceneBuilder builder_;
    mutable std::vector<double> scratch_[2];
    mutable int scratchIdx_ = 0;
};

class TraceCursorOverlay : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(TraceView* trace READ trace WRITE setTrace NOTIFY traceChanged)

public:
    explicit TraceCursorOverlay(QQuickItem* parent = nullptr);
    TraceView* trace() const { return trace_; }
    void setTrace(TraceView* trace);

    /// Rebuilds the overlay geometry `frames` times and reports
    /// {averageMs, quads}; the hot path while the pointer moves.
    Q_INVOKABLE QVariantMap benchmarkGeometry(int frames);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             UpdatePaintNodeData* data) override;
    void releaseResources() override;

signals:
    void traceChanged();

private:
    TraceView* trace_ = nullptr;
    TraceSceneBuilder builder_;
};
