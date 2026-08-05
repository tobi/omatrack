// Omarchy desktop palette bridge.
//
// Reads the active Omarchy theme's colors.toml once at construction and
// exposes it to QML as the `Theme` singleton. Outside Omarchy the map is
// empty and QML falls back to SystemPalette, so this never blocks startup
// beyond the short `omarchy theme current` probe.

#pragma once

#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class OmarchyTheme : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Theme)
    QML_SINGLETON
    Q_PROPERTY(QVariantMap colors READ colors CONSTANT)

public:
    explicit OmarchyTheme(QObject* parent = nullptr);

    const QVariantMap& colors() const { return colors_; }

private:
    QVariantMap colors_;
};
