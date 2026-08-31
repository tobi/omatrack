#include "app/PluginHost.h"

#include <QDir>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <cmath>

using namespace omatrack;
// Lua sources live in PluginHostTestScripts.cpp: moc cannot parse raw string
// literals containing JSON.
QString luaScript(int index);

namespace {
bool writePlugin(const QString& dir, const QString& body) {
    QDir().mkpath(dir);
    QFile file(QDir(dir).filePath(QStringLiteral("plugin.lua")));
    return file.open(QIODevice::WriteOnly) && file.write(body.toUtf8()) > 0;
}
PluginSession session() {
    PluginSession s;
    s.path = QStringLiteral("/copied/session.pds");
    s.name = QStringLiteral("session");
    s.track = QStringLiteral("Fixture Ring");
    s.utcStartNs = 1700000000LL * 1000000000LL;
    s.startNs = 0;
    s.endNs = 600LL * 1000000000LL;
    s.hasLocation = true;
    s.latitude = 43.0;
    s.longitude = -81.0;
    s.lapId = 2;
    s.lapStartNs = 100LL * 1000000000LL;
    s.lapEndNs = 200LL * 1000000000LL;
    return s;
}
const QString kGoodPlugin = luaScript(0);
}  // namespace

class PluginHostTest : public QObject {
    Q_OBJECT
private slots:
    void describesAndValidates() {
        QTemporaryDir temp;
        QVERIFY(
            writePlugin(temp.filePath(QStringLiteral("demo")), kGoodPlugin));
        const PluginInfo good =
            describePlugin(temp.filePath(QStringLiteral("demo")));
        QVERIFY2(good.error.isEmpty(), qPrintable(good.error));
        QCOMPARE(good.id, QStringLiteral("demo"));
        QCOMPARE(good.name, QStringLiteral("Demo"));
        QCOMPARE(good.version, 3);

        QVERIFY(writePlugin(
            temp.filePath(QStringLiteral("bad")),
            QStringLiteral(
                "return { id = 'Bad Id', channels = function() end }")));
        const PluginInfo bad =
            describePlugin(temp.filePath(QStringLiteral("bad")));
        QVERIFY(bad.error.contains(QStringLiteral("id")));

        QVERIFY(writePlugin(temp.filePath(QStringLiteral("syntax")),
                            QStringLiteral("return {")));
        QVERIFY(!describePlugin(temp.filePath(QStringLiteral("syntax")))
                     .error.isEmpty());
    }

    void channelsAndSamplesRoundTripWithKvAndUtc() {
        QTemporaryDir temp;
        PluginPaths paths{temp.filePath(QStringLiteral("plugins")),
                          temp.filePath(QStringLiteral("cache"))};
        QVERIFY(
            writePlugin(QDir(paths.pluginRoot).filePath(QStringLiteral("demo")),
                        kGoodPlugin));
        const PluginInfo info = describePlugin(
            QDir(paths.pluginRoot).filePath(QStringLiteral("demo")));
        const PluginOffer offer = runPluginChannels(info, session(), paths, {});
        QVERIFY2(offer.error.isEmpty(), qPrintable(offer.error));
        QCOMPARE(offer.channels.size(), 2);
        QCOMPARE(offer.channels[0].name, QStringLiteral("Alpha"));
        QCOMPARE(offer.channels[0].unit, QStringLiteral("u"));
        QVERIFY(offer.channels[0].defaultVisible);
        QCOMPARE(offer.channels[1].name, QStringLiteral("b"));
        QVERIFY(!offer.channels[1].defaultVisible);
        QVERIFY(
            QFile::exists(temp.filePath(QStringLiteral("cache/demo/kv.json"))));

        QThread::msleep(1100);  // the 1 s ttl entry must be gone, "seen" stays
        const PluginSamplesResult result = runPluginSamples(
            info, session(), {QStringLiteral("a"), QStringLiteral("b")}, paths,
            {});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.series.size(), 2);
        QCOMPARE(result.series[0].key, QStringLiteral("a"));
        QCOMPARE(*result.series[0].times,
                 (std::vector<qint64>{0, 300000000000LL, 600000000000LL}));
        QCOMPARE(result.series[1].key, QStringLiteral("b"));
        QCOMPARE(*result.series[1].times,
                 (std::vector<qint64>{0, 600000000000LL}));
        QCOMPARE(result.series[1].values->at(0), 10.0);
        QVERIFY(std::isnan(result.series[1].values->at(1)));
    }

    void unsortedTimesAreRejected() {
        QTemporaryDir temp;
        PluginPaths paths{temp.filePath(QStringLiteral("p")),
                          temp.filePath(QStringLiteral("c"))};
        QVERIFY(
            writePlugin(QDir(paths.pluginRoot).filePath(QStringLiteral("x")),
                        luaScript(1)));
        const PluginInfo info = describePlugin(
            QDir(paths.pluginRoot).filePath(QStringLiteral("x")));
        const auto result =
            runPluginSamples(info, session(), {QStringLiteral("a")}, paths, {});
        QVERIFY(result.error.contains(QStringLiteral("sorted")));
    }

    void ioIsJailedAndUnsafeGlobalsAreGone() {
        QTemporaryDir temp;
        PluginPaths paths{temp.filePath(QStringLiteral("p")),
                          temp.filePath(QStringLiteral("c"))};
        const QString dir =
            QDir(paths.pluginRoot).filePath(QStringLiteral("jail"));
        QVERIFY(writePlugin(dir, luaScript(2)));
        const PluginInfo info = describePlugin(dir);
        const PluginOffer offer = runPluginChannels(info, session(), paths, {});
        QVERIFY2(offer.error.isEmpty(), qPrintable(offer.error));
        QFile original(QDir(dir).filePath(QStringLiteral("plugin.lua")));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(original.readAll()), luaScript(2));
        QVERIFY(QFile::exists(
            temp.filePath(QStringLiteral("c/jail/state/notes.txt"))));
        QVERIFY(
            QFile::exists(temp.filePath(QStringLiteral("c/jail/plugin.lua"))));
    }

    void runawayPluginTimesOut() {
        QTemporaryDir temp;
        PluginPaths paths{temp.filePath(QStringLiteral("p")),
                          temp.filePath(QStringLiteral("c"))};
        const QString dir =
            QDir(paths.pluginRoot).filePath(QStringLiteral("spin"));
        QVERIFY(writePlugin(
            dir,
            QStringLiteral("return { id='spin', channels=function() while true "
                           "do end end, samples=function() return {} end }")));
        const PluginOffer offer =
            runPluginChannels(describePlugin(dir), session(), paths, {}, 200);
        QVERIFY2(offer.error.contains(QStringLiteral("timed out")),
                 qPrintable(offer.error));
    }

    void httpGetReachesALocalServerAndJsonDecodes() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server]() {
            QTcpSocket* socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                if (!socket->readAll().contains("\r\n\r\n")) return;
                const QByteArray body =
                    R"({"hourly":{"time":[1700000000,1700003600],"t":[12.5,null]}})";
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: "
                    "application/json\r\nContent-Length: " +
                    QByteArray::number(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body);
                socket->flush();
                socket->disconnectFromHost();
            });
        });
        QTemporaryDir temp;
        PluginPaths paths{temp.filePath(QStringLiteral("p")),
                          temp.filePath(QStringLiteral("c"))};
        const QString dir =
            QDir(paths.pluginRoot).filePath(QStringLiteral("net"));
        QVERIFY(writePlugin(dir, luaScript(3).arg(server.serverPort())));
        // The blocking HTTP round trip must run off the GUI thread, as the
        // host does; here a worker stands in for the AsyncJob.
        PluginOffer offer;
        QThread* worker = QThread::create([&]() {
            offer =
                runPluginChannels(describePlugin(dir), session(), paths, {});
        });
        worker->start();
        QTRY_VERIFY_WITH_TIMEOUT(worker->isFinished(), 10000);
        delete worker;
        QVERIFY2(offer.error.isEmpty(), qPrintable(offer.error));
        QCOMPARE(offer.channels.size(), 1);
    }

    void hostDiscoversAndDeliversGroups() {
        QTemporaryDir temp;
        PluginPaths paths{temp.filePath(QStringLiteral("p")),
                          temp.filePath(QStringLiteral("c"))};
        QVERIFY(
            writePlugin(QDir(paths.pluginRoot).filePath(QStringLiteral("demo")),
                        kGoodPlugin));
        PluginHost host(nullptr, paths);
        int delivered = 0;
        PluginSamplesResult last;
        QObject::connect(&host, &PluginHost::samplesReady, &host,
                         [&](const PluginSamplesResult& result) {
                             ++delivered;
                             last = result;
                         });
        host.setEnabled({QStringLiteral("demo")});
        host.discover();
        QTRY_VERIFY_WITH_TIMEOUT(!host.offers().isEmpty(), 5000);
        QVERIFY(!host.hasSession());
        host.setSession(session());
        QTRY_COMPARE_WITH_TIMEOUT(delivered, 1, 10000);
        QCOMPARE(last.series.size(), 2);
        QCOMPARE(host.library().size(), 1);
        QCOMPARE(host.library()
                     .first()
                     .toMap()
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("2 channels"));
        // An unchanged session does not re-run anything.
        host.setSession(session());
        QTest::qWait(300);
        QCOMPARE(delivered, 1);
    }
};

QTEST_GUILESS_MAIN(PluginHostTest)
#include "PluginHostTest.moc"
