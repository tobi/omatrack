// Mouse gestures, hit-testing, and hover state for the trace workspace.
//
// Extracted from TraceView so the QQuickItem shell owns rendering while
// interaction logic lives in a testable plain class. TraceView forwards its
// event handlers here and connects the callbacks to its QML-facing signals.
// Lane positions and channel specs come from TraceLaneLayout; the viewport
// and corner state come from TelemetryStore.

#pragma once

#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <vector>
#include <QVariantList>

#include <functional>

class QMouseEvent;
class QHoverEvent;
class QWheelEvent;
class QKeyEvent;

class TelemetryStore;
class TraceLaneLayout;
struct TraceSnapshot;

class TraceInteraction {
public:
    struct SpanHit {
        QRectF rect;
        QString title;
        QString subtitle;
        QColor color;
        QVariantList meta;
    };

    void setStore(TelemetryStore* store) { store_ = store; }
    void setLayout(TraceLaneLayout* layout) { layout_ = layout; }
    void setSnapshot(TraceSnapshot* snapshot) { snapshot_ = snapshot; }
    void setItemSize(double w, double h) {
        itemWidth_ = w;
        itemHeight_ = h;
    }

    // Geometry helpers.
    double xForFrac(double frac) const;
    double fracForX(double x) const;

    // Hit-testing.
    int cornerIndexAt(const QPointF& position) const;
    int channelIndexAt(const QPointF& position) const;
    int markerIndexAt(const QPointF& position) const;
    int focusedMarkerIndex() const;
    int focusedZoneHandleAt(const QPointF& position) const;
    int groupHeaderAt(const QPointF& position) const;
    int resizeBoundaryAt(const QPointF& position) const;
    int highlightedResizeBoundary() const {
        return resizingBoundary_ >= 0 ? resizingBoundary_
                                      : hoveredResizeBoundary_;
    }
    void cancelLaneResize();
    void validateLaneResize();

    // Hover updates.
    void updateHoveredMarker(const QPointF& position);
    void updateHoveredCorner(const QPointF& position);
    void updateHoveredSpan(const QPointF& position);
    void updateZoneHoverCursor(const QPointF& position);

    // Menus (emit callbacks).
    void showChannelMenu(const QPointF& position);

    // Event handlers. Return false when the base-class handler should run.
    void mouseDoubleClickEvent(QMouseEvent* event);
    void mousePressEvent(QMouseEvent* event);
    void mouseMoveEvent(QMouseEvent* event);
    void mouseReleaseEvent(QMouseEvent* event);
    void wheelEvent(QWheelEvent* event);
    bool keyPressEvent(QKeyEvent* event);
    void hoverMoveEvent(QHoverEvent* event);
    void hoverLeaveEvent(QHoverEvent* event);

    // State read by TraceView's rendering.
    int hoveredMarker() const { return hoveredMarker_; }
    int hoveredCorner() const { return hoveredCorner_; }
    int focusedMarkerIndexValue() const;  // renamed to avoid clash
    double selectionStart() const { return selectionStart_; }
    double selectionEnd() const { return selectionEnd_; }
    bool spanHoverVisible() const { return spanHoverVisible_; }
    QString spanHoverTitle() const { return spanHoverTitle_; }
    QString spanHoverSubtitle() const { return spanHoverSubtitle_; }
    QColor spanHoverColor() const { return spanHoverColor_; }
    QVariantList spanHoverMeta() const { return spanHoverMeta_; }
    qreal spanHoverX() const { return spanHoverX_; }
    qreal spanHoverY() const { return spanHoverY_; }
    double cursorTop() const { return cursorTop_; }
    double cursorBottom() const { return cursorBottom_; }

    // State written by TraceView's rendering.
    void setCursorTop(double v) { cursorTop_ = v; }
    void setCursorBottom(double v) { cursorBottom_ = v; }
    void clearSpanHits() {
        spanHits_.clear();
        hoveredSpan_ = -1;
    }
    void addSpanHit(const SpanHit& hit) { spanHits_.append(hit); }

    // Callbacks — TraceView wires these to its QML-facing signals.
    std::function<void()> onCursorChangedFromCanvas;
    std::function<void()> onCornerEdited;
    std::function<void(const QString&, const QString&, double, qreal, qreal)>
        onChannelMenuRequested;
    std::function<void()> onOverlayChanged;
    std::function<void()> onSpanHoverChanged;
    std::function<void(Qt::CursorShape)> onSetCursor;
    std::function<void()> onUnsetCursor;
    void resetSelection() {
        selectionStart_ = -1.0;
        selectionEnd_ = -1.0;
        selecting_ = false;
    }
    void resetHover() { hoveredMarker_ = -1; }

private:
    TelemetryStore* store_ = nullptr;
    TraceLaneLayout* layout_ = nullptr;
    TraceSnapshot* snapshot_ = nullptr;
    double itemWidth_ = 0.0;
    double itemHeight_ = 0.0;

    int resizingBoundary_ = -1;
    int hoveredResizeBoundary_ = -1;
    double resizeOriginY_ = 0.0;
    QStringList resizeKeys_;
    std::vector<double> resizeHeights_;
    bool dragging_ = false;
    bool panning_ = false;
    int dragCorner_ = -1;
    bool dragCornerMove_ = false;
    double dragStartFrac_ = 0.0;
    double lastPanFrac_ = 0.0;
    int hoveredMarker_ = -1;
    int hoveredCorner_ = -1;
    int hoveredSpan_ = -1;
    double pressX_ = 0.0;
    double selectionStart_ = -1.0;
    double selectionEnd_ = -1.0;
    bool selecting_ = false;

    bool spanHoverVisible_ = false;
    QString spanHoverTitle_;
    QString spanHoverSubtitle_;
    QColor spanHoverColor_;
    QVariantList spanHoverMeta_;
    qreal spanHoverX_ = 0;
    qreal spanHoverY_ = 0;
    QVector<SpanHit> spanHits_;

    double cursorTop_ = 0.0;
    double cursorBottom_ = 0.0;
    QElapsedTimer cursorTimer_;
};
