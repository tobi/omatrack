// Per-user file associations for the Windows installer / first-run prompt.
//
// Writes HKCU only, so Velopack's per-user install never needs elevation.
// Telemetry formats default on; generic video (.mp4) defaults off.
#pragma once

#include <QtGlobal>

#ifdef Q_OS_WIN

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

/// True when this process was started as a Velopack install/update hook
/// and has already exited the hook contract. Call before Qt starts.
bool consumeWindowsSetupHook(int argc, char** argv);

bool associationEnabled(const QString& extension);
bool setAssociationEnabled(const QString& extension, bool enabled);
void registerDefaultAssociations();
void unregisterAllAssociations();

bool velopackFirstRun();
QString velopackUpdateExe(const QString& applicationDir);

}  // namespace omatrack

#endif
