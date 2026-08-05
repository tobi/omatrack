// Corner speed / pedal / steering graphs.
//
// Replaces the QML Canvas that painted these three panels in JavaScript. The
// samples come from TelemetryStore's typed CornerGraph, so switching corners no
// longer boxes 1600 doubles into QVariants and re-runs a 150-line JS painter on
// the GUI thread.
//
// Context is drawn at full contrast and then recessed outside the selected
// zone, per the corner-trace rule in AGENTS.md: at least a 500 m window, dimmed
// approach and exit, full contrast between the zone boundaries.

#pragma once

#include <QColor>
#include <QFont>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include "TelemetryStore.h"

class CornerGraphView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(TelemetryStore* store READ store WRITE setStore NOTIFY storeChanged)
    Q_PROPERTY(int cornerIndex READ cornerIndex WRITE setCornerIndex NOTIFY
                   cornerIndexChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER backgroundColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor gridColor MEMBER gridColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor labelColor MEMBER labelColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor dimColor MEMBER dimColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor speedColor MEMBER speedColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor throttleColor MEMBER throttleColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor brakeColor MEMBER brakeColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor compareBrakeColor MEMBER compareBrakeColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor steeringColor MEMBER steeringColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor compareColor MEMBER compareColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor turnInColor MEMBER turnInColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor apexColor MEMBER apexColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor pickupColor MEMBER pickupColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QString monoFontFamily MEMBER monoFontFamily_ NOTIFY paletteChanged)

public:
    explicit CornerGraphView(QQuickItem* parent = nullptr);

    TelemetryStore* store() const { return store_; }
    void setStore(TelemetryStore* store);

    int cornerIndex() const { return cornerIndex_; }
    void setCornerIndex(int index);

    void paint(QPainter* painter) override;

signals:
    void storeChanged();
    void cornerIndexChanged();
    void paletteChanged();

private:
    struct Panel {
        double top = 0.0;
        double height = 0.0;
    };

    void paintPanel(QPainter* painter, const Panel& panel,
                    const QString& label) const;
    void paintSeries(QPainter* painter, const Panel& panel,
                     const std::vector<double>& values, double low, double high,
                     const QColor& color, double lineWidth) const;
    void paintAnnotation(QPainter* painter, double position,
                         const QString& label, const QColor& color,
                         double labelY, bool dashed) const;

    TelemetryStore* store_ = nullptr;
    int cornerIndex_ = 0;

    QColor backgroundColor_ = QColor(24, 26, 27);
    QColor gridColor_ = QColor(70, 74, 76);
    QColor labelColor_ = QColor(150, 156, 158);
    QColor dimColor_ = QColor(0, 0, 0, 148);
    QColor speedColor_ = QColor(167, 192, 128);
    QColor throttleColor_ = QColor(167, 192, 128);
    QColor brakeColor_ = QColor(230, 126, 128);
    QColor compareBrakeColor_ = QColor(224, 157, 127);
    QColor steeringColor_ = QColor(219, 188, 127);
    QColor compareColor_ = QColor(150, 156, 158);
    QColor turnInColor_ = QColor(127, 187, 179);
    QColor apexColor_ = QColor(214, 153, 182);
    QColor pickupColor_ = QColor(167, 192, 128);
    QString monoFontFamily_ = QStringLiteral("Geist Mono");
};
