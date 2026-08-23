#pragma once

#include <QLocalServer>
#include <QObject>
#include <QStringList>

namespace omatrack {

/// One running Omatrack per user. Opening a file from Explorer, Finder or a
/// file manager launches a second process; that process hands its paths to
/// the instance already running and exits, so a double-clicked `.ld` lands in
/// the open workspace instead of a second window.
///
/// The hand-off is a `QLocalServer` (a named pipe on Windows, a socket under
/// `$XDG_RUNTIME_DIR` elsewhere) keyed on the user name. A stale socket from
/// a crashed instance is removed before listening. When nothing is listening
/// this process becomes the primary and `pathsReceived` fires for every later
/// launch, already converted to absolute paths by the sender.
class SingleInstance : public QObject {
    Q_OBJECT
public:
    explicit SingleInstance(const QString& key, QObject* parent = nullptr);

    /// Try to hand `paths` to a running primary. Returns true when they were
    /// delivered and this process should exit; false when this process must
    /// become the primary (call `listen()` next).
    bool forwardToPrimary(const QStringList& paths);
    bool listen();
    QString serverError() const { return server_.errorString(); }

signals:
    /// Paths another launch asked this instance to open (absolute, may be
    /// empty when the launch carried no argument — still raise the window).
    void pathsReceived(const QStringList& paths);

private:
    void readClient();

    QString key_;
    QLocalServer server_;
};

}  // namespace omatrack
