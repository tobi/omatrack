#include "app/RecordingSidecar.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

namespace {

void touch(const QString& path) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("fixture") > 0);
}

void writeJson(const QString& path, const QJsonObject& object) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray body =
        QJsonDocument(object).toJson(QJsonDocument::Compact);
    QCOMPARE(file.write(body), body.size());
}

QJsonObject sessionWithLaps() {
    return QJsonObject{
        {QStringLiteral("version"), 10},
        {QStringLiteral("track"), QStringLiteral("Road America")},
        {QStringLiteral("laps"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), 4},
                                {QStringLiteral("start"), 120.0},
                                {QStringLiteral("end"), 240.0},
                                {QStringLiteral("timeMs"), 120000.0},
                                {QStringLiteral("videoStart"), 32.5},
                                {QStringLiteral("videoEnd"), 152.6},
                                {QStringLiteral("videoFrame"), 1950},
                                {QStringLiteral("videoByte"), 8388608}},
                    QJsonObject{{QStringLiteral("id"), 5},
                                {QStringLiteral("start"), 240.0},
                                {QStringLiteral("end"), 359.0},
                                {QStringLiteral("timeMs"), 119000.0},
                                {QStringLiteral("videoStart"), 152.6}}}}};
}

}  // namespace

class RecordingSidecarTest : public QObject {
    Q_OBJECT

private slots:
    void resolvesExplicitCompanionsAndAllLaps() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString video = directory.filePath(QStringLiteral("run.mp4"));
        const QString telemetry = directory.filePath(QStringLiteral("run.ld"));
        touch(video);
        touch(telemetry);
        writeJson(
            directory.filePath(QStringLiteral(".run.mp4.json")),
            QJsonObject{
                {QStringLiteral("schema"),
                 QStringLiteral("omatrack.recording/1")},
                {QStringLiteral("source"),
                 QJsonObject{{QStringLiteral("etag"), QStringLiteral("e1")}}},
                {QStringLiteral("video"),
                 QJsonObject{
                     {QStringLiteral("path"), QStringLiteral("run.mp4")}}},
                {QStringLiteral("telemetry"),
                 QJsonObject{
                     {QStringLiteral("path"), QStringLiteral("run.ld")}}},
                {QStringLiteral("supported"), true},
                {QStringLiteral("session"), sessionWithLaps()}});

        const auto sidecar = omatrack::readRecordingSidecar(video);
        QVERIFY(sidecar.has_value());
        QCOMPARE(sidecar->videoPath, video);
        QCOMPARE(sidecar->telemetryPath, telemetry);
        QCOMPARE(sidecar->sourceEtag, QStringLiteral("e1"));
        QVERIFY(sidecar->supported);
        QCOMPARE(
            sidecar->session.value(QStringLiteral("laps")).toArray().size(), 2);
        const QJsonObject first = sidecar->session.value(QStringLiteral("laps"))
                                      .toArray()
                                      .first()
                                      .toObject();
        QCOMPARE(first.value(QStringLiteral("videoStart")).toDouble(), 32.5);
        QCOMPARE(first.value(QStringLiteral("videoFrame")).toInt(), 1950);
        QCOMPARE(first.value(QStringLiteral("videoByte")).toInt(), 8388608);
    }

    void namesHiddenCompanionsFromTheVideo() {
        QCOMPARE(omatrack::recordingTelemetryPath(
                     QStringLiteral("/cache/event/run.mp4")),
                 QStringLiteral("/cache/event/.run.mp4.ld"));
        QCOMPARE(omatrack::recordingTelemetryRelativePath(
                     QStringLiteral("event/run.mp4")),
                 QStringLiteral("event/.run.mp4.ld"));
    }

    void infersHiddenMotecWithoutJson() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString video = directory.filePath(QStringLiteral("run.mp4"));
        const QString telemetry =
            directory.filePath(QStringLiteral(".run.mp4.ld"));
        touch(video);
        touch(telemetry);

        const auto sidecar = omatrack::readRecordingSidecar(video);
        QVERIFY(sidecar.has_value());
        QCOMPARE(sidecar->telemetryPath, telemetry);
        QVERIFY(sidecar->supported);
        QVERIFY(sidecar->session.isEmpty());
    }

    void infersHiddenMotecCompanion() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString video = directory.filePath(QStringLiteral("run.mp4"));
        const QString telemetry =
            directory.filePath(QStringLiteral(".run.mp4.ld"));
        touch(video);
        touch(telemetry);
        writeJson(directory.filePath(QStringLiteral(".run.mp4.json")),
                  QJsonObject{{QStringLiteral("schema"),
                               QStringLiteral("omatrack.recording/1")},
                              {QStringLiteral("supported"), true},
                              {QStringLiteral("session"), sessionWithLaps()}});

        const auto sidecar = omatrack::readRecordingSidecar(video);
        QVERIFY(sidecar.has_value());
        QCOMPARE(sidecar->telemetryPath, telemetry);
    }

    void confinesRemoteLinksToConnectionCache() {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        QVERIFY(QDir().mkpath(parent.filePath(QStringLiteral("cache"))));
        const QString cache = parent.filePath(QStringLiteral("cache"));
        const QString video = QDir(cache).filePath(QStringLiteral("run.mp4"));
        const QString outside = parent.filePath(QStringLiteral("outside.ld"));
        touch(video);
        touch(outside);
        writeJson(QDir(cache).filePath(QStringLiteral(".run.mp4.json")),
                  QJsonObject{{QStringLiteral("schema"),
                               QStringLiteral("omatrack.recording/1")},
                              {QStringLiteral("telemetry"),
                               QJsonObject{{QStringLiteral("path"),
                                            QStringLiteral("../outside.ld")}}},
                              {QStringLiteral("supported"), true},
                              {QStringLiteral("session"), sessionWithLaps()}});

        const auto sidecar = omatrack::readRecordingSidecar(video, cache);
        QVERIFY(sidecar.has_value());
        QVERIFY(sidecar->telemetryPath.isEmpty());
    }

    void marksUnsupportedVideo() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString video = directory.filePath(QStringLiteral("run.mp4"));
        touch(video);
        writeJson(directory.filePath(QStringLiteral(".run.mp4.json")),
                  QJsonObject{{QStringLiteral("schema"),
                               QStringLiteral("omatrack.recording/1")},
                              {QStringLiteral("supported"), false}});

        const auto sidecar = omatrack::readRecordingSidecar(video);
        QVERIFY(sidecar.has_value());
        QVERIFY(!sidecar->supported);
        QVERIFY(sidecar->telemetryPath.isEmpty());
    }

    void derivesHiddenServerName() {
        QCOMPARE(omatrack::recordingSidecarRelativePath(
                     QStringLiteral("event/run.mp4")),
                 QStringLiteral("event/.run.mp4.json"));
        QCOMPARE(omatrack::recordingSidecarPath(
                     QStringLiteral("/cache/event/run.mp4")),
                 QStringLiteral("/cache/event/.run.mp4.json"));
    }
};

QTEST_GUILESS_MAIN(RecordingSidecarTest)
#include "RecordingSidecarTest.moc"
