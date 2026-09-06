// origin: PUBLIC — native image-derived, absolute-presentation-time traces.
#pragma once

#include "inference/ImageTelemetrySeries.h"

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

#include <atomic>

class ImageTelemetryController;

class ImageTelemetryTraces : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_MOC_INCLUDE("ImageTelemetryController.h")
    Q_PROPERTY(ImageTelemetryController* controller READ controller WRITE
                   setController NOTIFY controllerChanged FINAL)
    Q_PROPERTY(double duration READ duration WRITE setDuration NOTIFY
                   durationChanged FINAL)
    Q_PROPERTY(double position READ position WRITE setPosition NOTIFY
                   positionChanged FINAL)
    Q_PROPERTY(double viewStart READ viewStart WRITE setViewStart NOTIFY
                   viewChanged FINAL)
    Q_PROPERTY(
        double viewEnd READ viewEnd WRITE setViewEnd NOTIFY viewChanged FINAL)
    Q_PROPERTY(QColor backgroundColor MEMBER backgroundColor_ NOTIFY
                   paletteChanged FINAL)
    Q_PROPERTY(QColor gridColor MEMBER gridColor_ NOTIFY paletteChanged FINAL)
    Q_PROPERTY(QColor foregroundColor MEMBER foregroundColor_ NOTIFY
                   paletteChanged FINAL)
    Q_PROPERTY(QColor mutedColor MEMBER mutedColor_ NOTIFY paletteChanged FINAL)
    Q_PROPERTY(
        QColor cursorColor MEMBER cursorColor_ NOTIFY paletteChanged FINAL)
    Q_PROPERTY(QColor gearColor MEMBER gearColor_ NOTIFY paletteChanged FINAL)
    Q_PROPERTY(QColor lapColor MEMBER lapColor_ NOTIFY paletteChanged FINAL)
    Q_PROPERTY(QColor brakeColor MEMBER brakeColor_ NOTIFY paletteChanged FINAL)
    Q_PROPERTY(
        QColor throttleColor MEMBER throttleColor_ NOTIFY paletteChanged FINAL)
    Q_PROPERTY(QFont font MEMBER font_ NOTIFY paletteChanged FINAL)

public:
    explicit ImageTelemetryTraces(QQuickItem* parent = nullptr);
    ImageTelemetryController* controller() const;
    void setController(ImageTelemetryController* controller);
    double duration() const { return duration_; }
    void setDuration(double seconds);
    double position() const { return position_; }
    void setPosition(double seconds);
    double viewStart() const { return viewStart_; }
    double viewEnd() const;
    void setViewStart(double seconds);
    void setViewEnd(double seconds);
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void setView(double startSeconds, double endSeconds);

    // C++ snapshot seam for renderer tests/hosts without QML. Immutable only;
    // no parsing, model work, or recording I/O lives in this component.
    void setSeries(omatrack::inference::ImageTelemetrySnapshot series);
    quint64 staticBuildCount() const { return staticBuilds_.load(); }
    quint64 cursorBuildCount() const { return cursorBuilds_.load(); }
    double lastStaticBuildMs() const { return double(staticNs_.load()) / 1e6; }
    double lastCursorBuildMs() const { return double(cursorNs_.load()) / 1e6; }

signals:
    void controllerChanged();
    void durationChanged();
    void positionChanged();
    void viewChanged();
    void paletteChanged();
    void seekRequested(double seconds);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*) override;
    void geometryChange(const QRectF& newGeometry,
                        const QRectF& oldGeometry) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void refreshSeries();
    void invalidateStatic();
    void updateLayout();
    double timeAt(qreal x) const;
    void pan(double seconds);

    QPointer<ImageTelemetryController> controller_;
    omatrack::inference::ImageTelemetrySnapshot series_;
    double duration_ = 0;
    double position_ = -1;
    double viewStart_ = 0;
    double viewEnd_ =
        -1;  // negative means follow the complete recording duration
    quint64 staticRevision_ = 1;
    QRectF plot_;
    QPointF pressPosition_;
    Qt::MouseButton pressedButton_ = Qt::NoButton;
    double pressStart_ = 0;
    double pressEnd_ = 0;
    // Supplied by Style bindings. No independent palette/font-family literals.
    QColor backgroundColor_, gridColor_, foregroundColor_, mutedColor_,
        cursorColor_;
    QColor gearColor_, lapColor_, brakeColor_, throttleColor_;
    QFont font_;
    std::atomic<quint64> staticBuilds_{0}, cursorBuilds_{0};
    std::atomic<qint64> staticNs_{0}, cursorNs_{0};
};
