// Front-damper trace strip for the manual reference-alignment tool.
//
// Replaces the QML Canvas that used to draw this in JavaScript: the samples
// are walked straight out of TelemetryStore's cached DamperAlignment, so a
// drag of the alignment handle repaints without boxing a thousand doubles into
// QVariants and re-running a JS loop on the GUI thread.

#pragma once

#include <QColor>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include "TelemetryStore.h"

class DamperStripView : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(
        TelemetryStore* store READ store WRITE setStore NOTIFY storeChanged)
    Q_PROPERTY(Source source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(Series series READ series WRITE setSeries NOTIFY seriesChanged)
    Q_PROPERTY(int cornerIndex READ cornerIndex WRITE setCornerIndex NOTIFY
                   cornerIndexChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(qreal shift READ shift WRITE setShift NOTIFY shiftChanged)
    Q_PROPERTY(qreal strokeOpacity READ strokeOpacity WRITE setStrokeOpacity
                   NOTIFY strokeOpacityChanged)

public:
    // LapAlignment walks the whole-lap front-damper traces the alignment tool
    // uses; CornerWindow walks the approach window of one corner, where the
    // shift is expressed in metres rather than lap fraction.
    enum Source { LapAlignment, CornerWindow };
    Q_ENUM(Source)

    enum Series { Primary, Compare };
    Q_ENUM(Series)

    explicit DamperStripView(QQuickItem* parent = nullptr);

    TelemetryStore* store() const { return store_; }
    void setStore(TelemetryStore* store);

    Source source() const { return source_; }
    void setSource(Source source);

    Series series() const { return series_; }
    void setSeries(Series series);

    int cornerIndex() const { return cornerIndex_; }
    void setCornerIndex(int index);

    QColor color() const { return color_; }
    void setColor(const QColor& color);

    // LapAlignment: offset as a lap fraction. CornerWindow: offset in metres,
    // converted against the corner's own window length.
    qreal shift() const { return shift_; }
    void setShift(qreal shift);

    qreal strokeOpacity() const { return strokeOpacity_; }
    void setStrokeOpacity(qreal opacity);

    void paint(QPainter* painter) override;

signals:
    void storeChanged();
    void sourceChanged();
    void seriesChanged();
    void cornerIndexChanged();
    void colorChanged();
    void shiftChanged();
    void strokeOpacityChanged();

private:
    TelemetryStore* store_ = nullptr;
    Source source_ = LapAlignment;
    Series series_ = Primary;
    int cornerIndex_ = 0;
    QColor color_ = Qt::white;
    qreal shift_ = 0.0;
    qreal strokeOpacity_ = 1.0;
};
