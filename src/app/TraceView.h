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

public:
    explicit TraceView(QQuickItem* parent = nullptr);

    TelemetryStore* store() const { return store_; }
    void setStore(TelemetryStore* store);
    QColor backgroundColor() const { return backgroundColor_; }
    void setBackgroundColor(const QColor& color);

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

signals:
    void storeChanged();
    void backgroundColorChanged();
    void cursorChangedFromCanvas();
    void cornerEdited();
    void cornerRenameRequested(int index);
    void cornerMenuRequested(int cornerIndex, const QString& cornerName,
                             double fraction, qreal x, qreal y);
    void channelMenuRequested(const QString& key, const QString& title,
                              double weight, qreal x, qreal y);
    void overlayChanged();
    void channelsRequested();

private:
    struct Clamp {
        double min = 0.0;
        double max = 1.0;
        bool autoRange = false;
        bool symmetric = false;
    };

    struct ChannelSpec {
        QString key;
        QString title;
        QString unit;
        QColor color;
        Clamp clamp;
        bool filled = false;
        // which UnifiedLap field to read; empty = derived
        QString field;
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
    void buildConfidenceBand(TraceSceneBuilder& builder,
                             const TraceConfidenceBand* band,
                             const QRectF& rect, const ChannelRange& range);
    /// Emits one lap's trace for `values` into `rect`. Each pixel column
    /// contributes the min/max of the samples it covers, joined to the
    /// previous column, so the same code draws a whole lap and a 20 m zoom.
    void buildSeries(TraceSceneBuilder& builder,
                     const std::vector<double>* values, const QRectF& rect,
                     const ChannelRange& range, const QColor& color, bool fill,
                     bool alignCompare, double shift, qreal width,
                     double clipLow = 0.0, double clipHigh = 1.0);
    /// Masks the space outside the lap and names the lap on the other side.
    void buildOutOfLap(TraceSceneBuilder& builder, const QRectF& totalRect);
    void buildDelta(TraceSceneBuilder& builder, const QRectF& rect);
    void buildCornerZones(TraceSceneBuilder& builder, const QRectF& totalRect);
    void buildCornerFocus(TraceSceneBuilder& builder, const QRectF& totalRect);
    double xForFrac(double frac) const;
    int cornerIndexAt(const QPointF& position) const;
    int markerIndexAt(const QPointF& position) const;
    void updateHoveredMarker(const QPointF& position);
    double fracForX(double x) const;
    int channelIndexAt(const QPointF& position) const;
    void showChannelMenu(const QPointF& position);
    void showCornerMenu(const QPointF& position);
    double laneWeightFor(const ChannelSpec& spec) const;
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
    double pressX_ = 0.0;
    QFont canvasFont_;
    QFont emptyStateFont_;
    QFont labelFont_;
    QFont unitFont_;
    QFont valueFont_;
    QFont markerFont_;
    QFont zoneFont_;
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
