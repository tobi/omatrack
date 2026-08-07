// TraceView — custom-painted telemetry trace canvas.
//
// QPainter-based rendering on a QQuickPaintedItem so the Qt Quick Material UI
// can host it while keeping omatrack's canvas drawing semantics. Renders the
// unified 50 Hz lap channels (speed, throttle, brake, steering, gear, dampers,
// delta-time), a shared cursor, zoom/pan viewport, and corner zones with
// drag-to-edit.

#pragma once

#include <QtQml/qqmlregistration.h>
#include <QQuickPaintedItem>
#include <QElapsedTimer>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QPainterPath>
#include <QSet>
#include <QVector>

#include "TelemetryStore.h"

#include <vector>

class TelemetryStore;

namespace omatrack {
struct UnifiedLap;
}

class TraceView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(
        TelemetryStore* store READ store WRITE setStore NOTIFY storeChanged)

public:
    explicit TraceView(QQuickItem* parent = nullptr);

    TelemetryStore* store() const { return store_; }
    void setStore(TelemetryStore* store);

    void paint(QPainter* painter) override;

    // Context-menu actions, driven by the QML Material menus.
    Q_INVOKABLE int addCornerAt(double fraction);
    Q_INVOKABLE void toggleSticky(const QString& key);
    Q_INVOKABLE void unpinAllChannels();
    Q_INVOKABLE void hideChannel(const QString& key);
    Q_INVOKABLE void showAllStandardChannels();

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    bool event(QEvent* event) override;

signals:
    void storeChanged();
    void cursorChangedFromCanvas();
    void cornerActivated(int index);
    void cornerEdited();
    void cornerRenameRequested(int index);
    void cornerMenuRequested(int cornerIndex, const QString& cornerName,
                             double fraction, qreal x, qreal y);
    void channelMenuRequested(const QString& key, const QString& title,
                              bool pinned, qreal x, qreal y);
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
        bool showDots = false;
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

    struct ChannelGeometry {
        QPainterPath primaryLine;
        QPainterPath primaryFill;
        QPainterPath compareLine;
        double min = 0.0;
        QImage primaryRaster;
        QImage compareRaster;
        double max = 1.0;
        bool gear = false;
        bool filled = false;
    };

    void rebuildChannelSpecs();
    void paintStatic(QPainter* painter);
    void paintCursorOverlay(QPainter* painter);
    void paintSelectionOverlay(QPainter* painter);
    void invalidateStaticLayer();
    void invalidateGeometry();
    const ChannelGeometry& geometryFor(const ChannelSpec& spec,
                                       const omatrack::UnifiedLap* primary,
                                       const omatrack::UnifiedLap* compare);
    void paintChannel(QPainter& p, const ChannelSpec& spec, const QRectF& rect,
                      int index, const omatrack::UnifiedLap* primary,
                      const omatrack::UnifiedLap* compare,
                      const std::vector<double>* primaryField,
                      const std::vector<double>* compareField);
    void paintDelta(QPainter& p, const QRectF& rect);
    void paintCornerZones(QPainter& p, const QRectF& totalRect);
    double xForFrac(double frac) const;
    int cornerIndexAt(const QPointF& position) const;
    double fracForX(double x) const;
    int channelIndexAt(const QPointF& position) const;
    void showChannelMenu(const QPointF& position);
    void showCornerMenu(const QPointF& position);
    bool isSticky(const QString& key) const;
    double rowHeightFor(const ChannelSpec& spec) const;
    const std::vector<double>* fieldFor(const omatrack::UnifiedLap& lap,
                                        const QString& field) const;
    QColor colorForDriver() const;

    QImage deltaRaster_;
    double deltaMaxAbs_ = 0.001;
    QVector<CursorLane> cursorLanes_;
    QHash<QString, ChannelGeometry> geometryCache_;
    double cursorTop_ = 0.0;
    double cursorBottom_ = 0.0;
    double selectionStart_ = -1.0;
    double selectionEnd_ = -1.0;
    bool selecting_ = false;

    TelemetryStore* store_ = nullptr;
    QVector<ChannelSpec> channelSpecs_;
    bool dragging_ = false;
    bool panning_ = false;
    int dragCorner_ = -1;
    bool dragCornerMove_ = false;
    double dragStartFrac_ = 0.0;
    double lastPanFrac_ = 0.0;
    QSet<QString> stickyChannels_;
    double secondaryScroll_ = 0.0;
    QFont canvasFont_;
    QFont emptyStateFont_;
    QFont labelFont_;
    QFont unitFont_;
    QFont stickyFont_;
    friend class TraceCursorOverlay;
    QElapsedTimer cursorTimer_;
    mutable std::vector<double> scratch_[2];
    mutable int scratchIdx_ = 0;
};

class TraceCursorOverlay : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(TraceView* trace READ trace WRITE setTrace NOTIFY traceChanged)

public:
    explicit TraceCursorOverlay(QQuickItem* parent = nullptr);
    TraceView* trace() const { return trace_; }
    void setTrace(TraceView* trace);
    void paint(QPainter* painter) override;

signals:
    void traceChanged();

private:
    TraceView* trace_ = nullptr;
};
