#include "app/RemoteCache.h"

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>

#include <memory>

using namespace omatrack;

class RemoteCacheTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        cacheDir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(cacheDir_->isValid());
        qputenv("XDG_CACHE_HOME", cacheDir_->path().toUtf8());
        QVERIFY(server_.listen(QHostAddress::LocalHost));
        connect(&server_, &QTcpServer::newConnection, this,
                &RemoteCacheTest::acceptConnection);
    }

    void cleanupTestCase() {
        server_.close();
        qunsetenv("XDG_CACHE_HOME");
    }

    void downloadsAndReusesCachedFiles() {
        requests_ = 0;
        propfinds_ = 0;
        gets_ = 0;
        RemoteConnection connection;
        connection.name = QStringLiteral("Test server");
        connection.target = QStringLiteral("http://127.0.0.1:%1/dav/")
                             .arg(server_.serverPort());
        connection.id = locationId(connection.target, {});

        const RemoteSyncResult first = syncConnection(connection);
        QVERIFY2(first.success, qPrintable(first.error));
        QCOMPARE(first.files, QStringList{QStringLiteral("session.vbo")});
        QCOMPARE(propfinds_, 1);
        QCOMPARE(gets_, 1);
        QFile cached(QDir(first.cachePath).filePath("session.vbo"));
        QVERIFY(cached.open(QIODevice::ReadOnly));
        QCOMPARE(cached.readAll(), QByteArrayLiteral("telemetry"));

        const RemoteSyncResult second = syncConnection(connection);
        QVERIFY2(second.success, qPrintable(second.error));
        QVERIFY(!second.fromCache);
        QCOMPARE(propfinds_, 2);
        QCOMPARE(gets_, 1);
    }

    void servesCacheWhenServerIsOffline() {
        RemoteConnection connection;
        connection.target = QStringLiteral("http://127.0.0.1:%1/dav/")
                             .arg(server_.serverPort());
        connection.id = locationId(connection.target, {});
        const RemoteSyncResult online = syncConnection(connection);
        QVERIFY2(online.success, qPrintable(online.error));

        server_.close();
        const RemoteSyncResult offline = syncConnection(connection);
        QVERIFY(offline.success);
        QVERIFY(offline.fromCache);
        QCOMPARE(offline.files, QStringList{QStringLiteral("session.vbo")});

        QVERIFY(server_.listen(QHostAddress::LocalHost));
    }

private:
    void acceptConnection() {
        while (server_.hasPendingConnections()) {
            QTcpSocket* socket = server_.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket]() { serve(socket); });
            connect(socket, &QTcpSocket::disconnected, socket,
                    &QObject::deleteLater);
        }
    }

    void serve(QTcpSocket* socket) {
        const QByteArray request = socket->readAll();
        if (!request.contains("\r\n\r\n")) return;
        ++requests_;
        QByteArray payload;
        QByteArray status;
        if (request.startsWith("PROPFIND")) {
            ++propfinds_;
            const QString root = QStringLiteral("http://127.0.0.1:%1/dav/")
                                     .arg(server_.serverPort());
            payload = QStringLiteral(
                          "<?xml version=\"1.0\"?><multistatus "
                          "xmlns=\"DAV:\"><response><href>%1</href>"
                          "<propstat><prop><resourcetype><collection/>"
                          "</resourcetype></prop></propstat></response>"
                          "<response><href>%1session.vbo</href>"
                          "<propstat><prop><getetag>\"one\"</getetag>"
                          "<getlastmodified>now</getlastmodified>"
                          "<getcontentlength>9</getcontentlength>"
                          "</prop></propstat></response></multistatus>")
                          .arg(root)
                          .toUtf8();
            status = "207 Multi-Status";
        } else if (request.startsWith("GET /dav/session.vbo")) {
            ++gets_;
            payload = QByteArrayLiteral("telemetry");
            status = "200 OK";
        } else {
            status = "404 Not Found";
        }
        QByteArray response = "HTTP/1.1 ";
        response += status;
        response += "\r\nConnection: close\r\n";
        response += "Content-Type: application/xml\r\nContent-Length: ";
        response += QByteArray::number(payload.size());
        response += "\r\n\r\n";
        response += payload;
        socket->write(response);
        socket->disconnectFromHost();
    }

    std::unique_ptr<QTemporaryDir> cacheDir_;
    QTcpServer server_;
    int requests_ = 0;
    int propfinds_ = 0;
    int gets_ = 0;
};

QTEST_MAIN(RemoteCacheTest)
#include "RemoteCacheTest.moc"
