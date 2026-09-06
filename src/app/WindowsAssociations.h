// Per-user file associations for the Windows installer / first-run prompt.
//
// Writes HKCU only, so Velopack's per-user install never needs elevation.
// Telemetry formats default on; every generic video format defaults off.
// The catalog is platform-independent so policy tests never need the registry.
#pragma once

#include <QtGlobal>

#include <QString>
#include <QVector>

namespace omatrack {

struct FileAssociation {
    QString extension;
    QString description;
    bool video = false;
    bool defaultEnabled = true;
};

QVector<FileAssociation> fileAssociations();

#ifdef Q_OS_WIN

/// True when this process was started as a Velopack install/update hook
/// and has already exited the hook contract. Call before Qt starts.
bool consumeWindowsSetupHook(int argc, char** argv);

// Reports Omatrack's per-user registration, not Windows' protected UserChoice.
// Never changes UserChoice; Windows may still ask the user to choose an app.
bool associationEnabled(const QString& extension);
bool setAssociationEnabled(const QString& extension, bool enabled);
void registerDefaultAssociations();
void unregisterAllAssociations();

bool velopackFirstRun();
QString velopackUpdateExe(const QString& applicationDir);

#endif

}  // namespace omatrack
