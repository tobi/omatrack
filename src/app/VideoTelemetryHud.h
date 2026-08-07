#pragma once

#include <QColor>
#include <QFont>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

class TelemetryStore;

class VideoTelemetryHud : public QQuickPaintedItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(
        TelemetryStore* store READ store WRITE setStore NOTIFY storeChanged)
    Q_PROPERTY(QColor primaryColor MEMBER primaryColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor compareColor MEMBER compareColor_ NOTIFY paletteChanged)
    Q_PROPERTY(
        QColor foregroundColor MEMBER foregroundColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor mutedColor MEMBER mutedColor_ NOTIFY paletteChanged)
    Q_PROPERTY(QColor brakeColor MEMBER brakeColor_ NOTIFY paletteChanged)
    Q_PROPERTY(
        QColor backgroundColor MEMBER backgroundColor_ NOTIFY paletteChanged)
    Q_PROPERTY(
        QString monoFontFamily MEMBER monoFontFamily_ NOTIFY paletteChanged)

public:
    explicit VideoTelemetryHud(QQuickItem* parent = nullptr);

    TelemetryStore* store() const { return store_; }
    void setStore(TelemetryStore* store);
    void paint(QPainter* painter) override;

signals:
    void storeChanged();
    void paletteChanged();

private:
    TelemetryStore* store_ = nullptr;
    QColor primaryColor_ = QColor(QStringLiteral("#32d74b"));
    QColor compareColor_ = QColor(QStringLiteral("#e09d7f"));
    QColor foregroundColor_ = Qt::white;
    QColor mutedColor_ = QColor(QStringLiteral("#9da9a0"));
    QColor brakeColor_ = QColor(QStringLiteral("#ff2020"));
    QColor backgroundColor_ = QColor(0, 0, 0, 218);
    QString monoFontFamily_ = QStringLiteral("Geist Mono");
};
