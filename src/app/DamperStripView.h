// Front-damper trace strip for the manual reference-alignment tool.
//
// Replaces the QML Canvas that used to draw this in JavaScript: the samples
// are walked straight out of TelemetryStore's cached DamperAlignment, so a
// drag of the alignment handle repaints without boxing a thousand doubles into
// QVariants and re-running a JS loop on the GUI thread.

#pragma once

#include <QColor>
#include <QPointF>
#include <QQuickItem>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include "TelemetryStore.h"
#include "TraceSceneBuilder.h"

class DamperStripView : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(
        TelemetryStore* store READ store WRITE setStore NOTIFY storeChanged)
    Q_PROPERTY(Series series READ series WRITE setSeries NOTIFY seriesChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal shift READ shift WRITE setShift NOTIFY shiftChanged)
    Q_PROPERTY(qreal strokeOpacity READ strokeOpacity WRITE setStrokeOpacity
                   NOTIFY strokeOpacityChanged)

public:
    enum Series { Primary, Compare };
    Q_ENUM(Series)

    explicit DamperStripView(QQuickItem* parent = nullptr);

    TelemetryStore* store() const { return store_; }
    void setStore(TelemetryStore* store);

    Series series() const { return series_; }
    void setSeries(Series series);

    QColor color() const { return color_; }
    void setColor(const QColor& color);

    /// Reference offset as a lap fraction.
    qreal shift() const { return shift_; }
    void setShift(qreal shift);

    qreal strokeOpacity() const { return strokeOpacity_; }
    void setStrokeOpacity(qreal opacity);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             UpdatePaintNodeData* data) override;
    void releaseResources() override;

signals:
    void storeChanged();
    void seriesChanged();
    void colorChanged();
    void shiftChanged();
    void strokeOpacityChanged();

private:
    TelemetryStore* store_ = nullptr;
    Series series_ = Primary;
    QColor color_ = Qt::white;
    qreal shift_ = 0.0;
    qreal strokeOpacity_ = 1.0;

    TraceSceneBuilder builder_;
    QVector<QPointF> points_;
};
