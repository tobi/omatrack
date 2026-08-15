#include "app/RecordingSidecar.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

namespace {

void touch(const QString& path) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("fixture") > 0);
}

}  // namespace

class RecordingSidecarTest : public QObject {
    Q_OBJECT

private slots:
    void namesHiddenCompanionFromTheVideo() {
        const QString video = QStringLiteral("/cache/event/run.mp4");
        QCOMPARE(omatrack::nativeCompanionPath(video),
                 QFileInfo(video).dir().filePath(
                     QStringLiteral(".run.mp4.telemetry")));
        const QString pds = QStringLiteral("/cache/event/run.pds");
        QCOMPARE(omatrack::nativeCompanionPath(pds),
                 QFileInfo(pds).dir().filePath(
                     QStringLiteral(".run.pds.telemetry")));
        const QString telemetry = QStringLiteral("/cache/event/run.telemetry");
        QCOMPARE(omatrack::nativeCompanionPath(telemetry),
                 QFileInfo(telemetry).absoluteFilePath());
        QCOMPARE(omatrack::nativeCompanionRelativePath(
                     QStringLiteral("event/run.mp4")),
                 QStringLiteral("event/.run.mp4.telemetry"));
    }

    void readsHiddenTelemetryCompanion() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString video = directory.filePath(QStringLiteral("run.mp4"));
        const QString telemetry =
            directory.filePath(QStringLiteral(".run.mp4.telemetry"));
        touch(video);
        touch(telemetry);

        const auto sidecar = omatrack::readRecordingSidecar(video);
        QVERIFY(sidecar.has_value());
        QCOMPARE(sidecar->telemetryPath,
                 QFileInfo(telemetry).absoluteFilePath());
        QVERIFY(sidecar->supported);
    }

    void missesWhenCompanionIsAbsent() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString video = directory.filePath(QStringLiteral("run.mp4"));
        touch(video);
        QVERIFY(!omatrack::readRecordingSidecar(video).has_value());
    }
};

QTEST_GUILESS_MAIN(RecordingSidecarTest)
#include "RecordingSidecarTest.moc"
