// Front-damper strip for the manual reference-alignment tool.
//
// Both laps' front-damper traces in a window centred on the cursor, zoomable
// from a second to the whole lap, autoscaled to what is in the window. The
// reference is drawn through the shared primary→reference map (manual offset
// included), exactly like the traces and synchronized video, so lining up
// the bumps here is lining up everything.

#pragma once

#include <QColor>
#include <QQuickItem>
#include <QtQml/qqmlregistration.h>

#include "TelemetryStore.h"
#include "TraceSceneBuilder.h"

class DamperStripView : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(
        TelemetryStore* store READ store WRITE setStore NOTIFY storeChanged)
    Q_PROPERTY(QColor primaryColor MEMBER primaryColor_ NOTIFY styleChanged)
    Q_PROPERTY(QColor referenceColor MEMBER referenceColor_ NOTIFY styleChanged)
    Q_PROPERTY(QColor cursorColor MEMBER cursorColor_ NOTIFY styleChanged)
    /// Visible span in primary-lap seconds, centred on the cursor.
    Q_PROPERTY(double windowSeconds READ windowSeconds WRITE setWindowSeconds
                   NOTIFY windowSecondsChanged)
    /// The same span as a primary-lap fraction (what a drag converts with).
    Q_PROPERTY(
        double windowFraction READ windowFraction NOTIFY windowSecondsChanged)

public:
    static constexpr double kMinimumWindowSeconds = 1.0;
    static constexpr double kDefaultWindowSeconds = 6.0;

    explicit DamperStripView(QQuickItem* parent = nullptr);

    TelemetryStore* store() const { return store_; }
    void setStore(TelemetryStore* store);
    double windowSeconds() const { return windowSeconds_; }
    void setWindowSeconds(double seconds);
    double windowFraction() const;

    /// Multiply the window by `factor` (wheel zoom), clamped to [1 s, lap].
    Q_INVOKABLE void zoom(double factor);

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode,
                             UpdatePaintNodeData* data) override;
    void releaseResources() override;

signals:
    void storeChanged();
    void styleChanged();
    void windowSecondsChanged();

private:
    double lapSeconds() const;

    TelemetryStore* store_ = nullptr;
    QColor primaryColor_ = Qt::white;
    QColor referenceColor_ = QColor(224, 157, 127);
    QColor cursorColor_ = QColor(255, 255, 255, 120);
    double windowSeconds_ = kDefaultWindowSeconds;

    TraceSceneBuilder builder_;
};
