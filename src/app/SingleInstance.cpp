#include "SingleInstance.h"

#include <QFileInfo>
#include <QLocalSocket>

namespace omatrack {
namespace {

constexpr int kConnectTimeoutMs = 500;
constexpr int kWriteTimeoutMs = 2000;
constexpr char kSeparator = '\n';

}  // namespace

SingleInstance::SingleInstance(const QString& key, QObject* parent)
    : QObject(parent), key_(key) {
    connect(&server_, &QLocalServer::newConnection, this,
            &SingleInstance::readClient);
}

bool SingleInstance::forwardToPrimary(const QStringList& paths) {
    QLocalSocket socket;
    socket.connectToServer(key_);
    if (!socket.waitForConnected(kConnectTimeoutMs)) return false;
    QByteArray payload;
    for (const QString& path : paths) {
        // Relative arguments are relative to *this* process' cwd, which the
        // primary does not share.
        payload += QFileInfo(path).absoluteFilePath().toUtf8();
        payload += kSeparator;
    }
    // A launch with no argument still means "show me the app".
    if (payload.isEmpty()) payload += kSeparator;
    socket.write(payload);
    const bool written = socket.waitForBytesWritten(kWriteTimeoutMs);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.waitForDisconnected(kWriteTimeoutMs);
    return written;
}

bool SingleInstance::listen() {
    // A crash leaves the socket file behind on Unix; on Windows the pipe
    // dies with the process and this is a no-op.
    QLocalServer::removeServer(key_);
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    return server_.listen(key_);
}

void SingleInstance::readClient() {
    while (QLocalSocket* client = server_.nextPendingConnection()) {
        // The sender writes one payload and disconnects; reading on
        // disconnect sees the complete message without framing.
        connect(client, &QLocalSocket::disconnected, this, [this, client] {
            const QByteArray payload = client->readAll();
            client->deleteLater();
            QStringList paths;
            for (const QByteArray& line : payload.split(kSeparator)) {
                const QString path = QString::fromUtf8(line).trimmed();
                if (!path.isEmpty()) paths.append(path);
            }
            emit pathsReceived(paths);
        });
    }
}

}  // namespace omatrack
