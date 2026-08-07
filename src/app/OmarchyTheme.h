// Omarchy desktop palette bridge.
//
// Reads the active Omarchy theme's colors.toml and exposes it to QML as the
// `Theme` singleton. On Omarchy it watches the desktop's current-theme state
// and reloads after an atomic theme swap. Outside Omarchy the map stays empty
// and QML falls back to SystemPalette.

#pragma once

#include <QtGlobal>

#ifdef Q_OS_LINUX
#include <QFileSystemWatcher>
#include <QTimer>
#endif
#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

class OmarchyTheme : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Theme)
    QML_SINGLETON
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY colorsChanged)

public:
    explicit OmarchyTheme(QObject* parent = nullptr);

    const QVariantMap& colors() const { return colors_; }

signals:
    void colorsChanged();

private:
#ifdef Q_OS_LINUX
    void reload();
    void refreshWatchPaths();
#endif

    QVariantMap colors_;
#ifdef Q_OS_LINUX
    QString currentStatePath_;
    QFileSystemWatcher watcher_;
    QTimer reloadTimer_;
#endif
};
