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
// vertically. A focused corner zooms the viewport, dims the traces outside
// the zone, and annotates the zoomed view with brake / turn-in / apex /
// throttle markers.
//
// Lane layout (channel specs, geometry, range caching) is delegated to
// TraceLaneLayout; mouse gestures and hit-testing are delegated to
// TraceInteraction. TraceView keeps the QQuickItem shell, the QML-facing
// Q_PROPERTY/Q_INVOKABLE surface, and the scene-graph rendering.

#pragma once

#include <QtQml/qqmlregistration.h>
#include <QColor>
#include <QFont>
#include <QQuickItem>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "TelemetryStore.h"
#include "TraceSceneBuilder.h"
#include "TraceSnapshot.h"
#include "TraceLaneLayout.h"
#include "TraceInteraction.h"

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
    Q_PROPERTY(bool fitChannels READ fitChannels WRITE setFitChannels NOTIFY
                   fitChannelsChanged)

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
    qreal labelWidth() const { return layout_.labelWidth(); }
    qreal rulerHeight() const;
    bool fitChannels() const { return layout_.fitChannels(); }
    void setFitChannels(bool fit);

    QVariantList laneRows() const { return layout_.laneRows(); }

    bool spanHoverVisible() const { return interaction_.spanHoverVisible(); }
    QString spanHoverTitle() const { return interaction_.spanHoverTitle(); }
    QString spanHoverSubtitle() const {
        return interaction_.spanHoverSubtitle();
    }
    QColor spanHoverColor() const { return interaction_.spanHoverColor(); }
    QVariantList spanHoverMeta() const { return interaction_.spanHoverMeta(); }
    qreal spanHoverX() const { return interaction_.spanHoverX(); }
    qreal spanHoverY() const { return interaction_.spanHoverY(); }

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
    void fitChannelsChanged();

    void cursorChangedFromCanvas();
    void cornerEdited();
    void channelMenuRequested(const QString& key, const QString& title,
                              double weight, qreal x, qreal y);
    void overlayChanged();
    void spanHoverChanged();
    void channelsRequested();

private:
    struct CursorLane {
        QString field;
        QRectF rect;
        double min = 0.0;
        double max = 1.0;
        QColor color;
        bool gear = false;
    };

    // ── rendering ──────────────────────────────────────────────────
    void buildScene(TraceSceneBuilder& builder);
    void buildCursorScene(TraceSceneBuilder& builder);
    void buildSelection(TraceSceneBuilder& builder);
    void buildCornerMarkerGuides(TraceSceneBuilder& builder);
    void buildCornerMarkers(TraceSceneBuilder& builder);
    void invalidateScene();
    void buildChannel(TraceSceneBuilder& builder,
                      const TraceLaneLayout::ChannelSpec& spec,
                      const QRectF& rect, const omatrack::UnifiedLap* primary,
                      const omatrack::UnifiedLap* compare);
    void buildGroupHeader(TraceSceneBuilder& builder,
                          const TraceLaneLayout::ChannelSpec& spec,
                          const QRectF& rect);
    void buildSpanTrack(TraceSceneBuilder& builder,
                        const TraceLaneLayout::ChannelSpec& spec,
                        const QRectF& rect);
    double sidecarValueAt(const QString& key, double fraction) const;
    void buildConfidenceBand(TraceSceneBuilder& builder,
                             const TraceConfidenceBand* band,
                             const QRectF& rect,
                             const TraceLaneLayout::ChannelRange& range);
    void buildConsistencyStrip(TraceSceneBuilder& builder, const QRectF& rect);
    void buildSeries(TraceSceneBuilder& builder,
                     const std::vector<double>* values, const QRectF& rect,
                     const TraceLaneLayout::ChannelRange& range,
                     const QColor& color, bool fill, bool alignCompare,
                     double shift, qreal width, double clipLow = 0.0,
                     double clipHigh = 1.0);
    qreal devicePixelRatio() const;
    int deviceColumns(const QRectF& rect) const;
    void buildOutOfLap(TraceSceneBuilder& builder, const QRectF& totalRect);
    void buildDelta(TraceSceneBuilder& builder, const QRectF& rect);
    void buildCornerZones(TraceSceneBuilder& builder, const QRectF& totalRect);
    void buildCornerFocus(TraceSceneBuilder& builder, const QRectF& totalRect);
    void buildHoveredCornerDelta(TraceSceneBuilder& builder);

    // ── layout delegation ──────────────────────────────────────────
    void rebuildChannelSpecs() { layout_.rebuildChannelSpecs(); }
    void invalidateRanges() { layout_.invalidateRanges(); }
    void updateLabelWidth() { layout_.updateLabelWidth(); }
    QVector<TraceLaneLayout::Lane> layoutLanes() const {
        return layout_.layoutLanes();
    }
    const QVector<TraceLaneLayout::ChannelSpec>& channelSpecs() const {
        return layout_.channelSpecs();
    }
    const TraceLaneLayout::ChannelRange& rangeFor(
        const TraceLaneLayout::ChannelSpec& spec,
        const omatrack::UnifiedLap* primary,
        const omatrack::UnifiedLap* compare) {
        return layout_.rangeFor(spec, primary, compare);
    }
    const std::vector<double>* fieldFor(const omatrack::UnifiedLap& lap,
                                        const QString& field) const {
        return layout_.fieldFor(lap, field);
    }
    const OverlayGroup* overlayGroup(const QString& id) const {
        return layout_.overlayGroup(id);
    }
    bool overlaySpanVisibleOnLap(const OverlaySpan& span) const {
        return layout_.overlaySpanVisibleOnLap(span);
    }
    bool primaryLapWindowNs(qint64* startNs, qint64* endNs) const {
        return layout_.primaryLapWindowNs(startNs, endNs);
    }

    // ── interaction delegation ─────────────────────────────────────
    double xForFrac(double frac) const { return interaction_.xForFrac(frac); }
    double fracForX(double x) const { return interaction_.fracForX(x); }
    int hoveredMarker() const { return interaction_.hoveredMarker(); }
    int hoveredCorner() const { return interaction_.hoveredCorner(); }
    int focusedMarkerIndex() const {
        return interaction_.focusedMarkerIndexValue();
    }
    double selectionStart() const { return interaction_.selectionStart(); }
    double selectionEnd() const { return interaction_.selectionEnd(); }
    double cursorTop() const { return interaction_.cursorTop(); }
    double cursorBottom() const { return interaction_.cursorBottom(); }
    void setCursorTop(double v) { interaction_.setCursorTop(v); }
    void setCursorBottom(double v) { interaction_.setCursorBottom(v); }

    // ── data ───────────────────────────────────────────────────────
    TelemetryStore* store_ = nullptr;
    QColor backgroundColor_{QStringLiteral("#181d20")};
    QFont canvasFont_;
    QFont emptyStateFont_;
    QFont labelFont_;
    QFont unitFont_;
    QFont valueFont_;
    QFont markerFont_;
    QFont pillFont_;
    QVector<CursorLane> cursorLanes_;
    TraceSceneBuilder builder_;
    TraceSnapshot snapshot_;
    TraceLaneLayout layout_;
    TraceInteraction interaction_;
    friend class TraceCursorOverlay;
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
