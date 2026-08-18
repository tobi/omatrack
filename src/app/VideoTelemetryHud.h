#pragma once

#include <QColor>
#include <QQuickItem>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <limits>

#include "TraceSceneBuilder.h"
#include "TraceSnapshot.h"
class TelemetryStore;

class VideoTelemetryHud : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(
        TelemetryStore* store READ store WRITE setStore NOTIFY storeChanged)
    Q_PROPERTY(QColor throttleColor MEMBER throttleColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor compareColor MEMBER compareColor_ NOTIFY paletteChanged)
    Q_PROPERTY(
        QColor foregroundColor MEMBER foregroundColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor mutedColor MEMBER mutedColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor brakeColor MEMBER brakeColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor steeringColor MEMBER steeringColor_ NOTIFY paletteChanged)
    Q_PROPERTY(
        QColor backgroundColor MEMBER backgroundColor_ NOTIFY paletteChanged)
    Q_PROPERTY(
        QString monoFontFamily MEMBER monoFontFamily_ NOTIFY paletteChanged)
    Q_PROPERTY(double mediaTime READ mediaTime WRITE setMediaTime NOTIFY
                   mediaTimeChanged)

public:
    explicit VideoTelemetryHud(QQuickItem* parent = nullptr);

    TelemetryStore* store() const { return store_; }
    void setStore(TelemetryStore* store);
    double mediaTime() const { return mediaTime_; }
    void setMediaTime(double mediaTime);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             UpdatePaintNodeData* data) override;
    void releaseResources() override;

signals:
    void storeChanged();
    void paletteChanged();
    void mediaTimeChanged();

private:
    TelemetryStore* store_ = nullptr;
    double mediaTime_ = std::numeric_limits<double>::quiet_NaN();
    QColor throttleColor_ = QColor(QStringLiteral("#a7c080"));
    QColor compareColor_ = QColor(QStringLiteral("#e09d7f"));
    QColor foregroundColor_ = Qt::white;
    QColor mutedColor_ = QColor(QStringLiteral("#9da9a0"));
    QColor brakeColor_ = QColor(QStringLiteral("#e67e80"));
    QColor steeringColor_ = QColor(QStringLiteral("#dbbc7f"));
    QColor backgroundColor_ = QColor(0, 0, 0, 218);
    QString monoFontFamily_ = QStringLiteral("Geist Mono");

    TraceSceneBuilder builder_;

    // Cached per selection (not per frame): the compare-fraction map and the
    // brake-scale peak are stable while the cursor moves, so a cursor frame
    bool snapshotDirty_ = true;
    bool brakeMaxDirty_ = true;
    TraceSnapshot snapshot_;
    double cachedBrakeMax_ = 1.0;
    double cachedThrottleScale_ = 1.0;
    QVector<QPointF> convexScratch_;
};
