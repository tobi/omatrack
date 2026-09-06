#include "app/FileOpenEvents.h"

#include <QFileOpenEvent>
#include <QtTest>

class FileOpenEventsTest : public QObject {
    Q_OBJECT
private slots:
    void queuesUntilStoreReady() {
        QObject application;
        omatrack::FileOpenEvents events(&application);
        QFileOpenEvent first(
            QUrl::fromLocalFile(QStringLiteral("/tmp/one video.MKV")));
        QVERIFY(QCoreApplication::sendEvent(&application, &first));
        QStringList paths;
        events.setHandler(
            [&paths](const QStringList& opened) { paths.append(opened); });
        QCOMPARE(paths, QStringList{QStringLiteral("/tmp/one video.MKV")});
        QFileOpenEvent second(
            QUrl::fromLocalFile(QStringLiteral("/tmp/two.mov")));
        QVERIFY(QCoreApplication::sendEvent(&application, &second));
        QCOMPARE(paths.size(), 2);
        QCOMPARE(paths.last(), QStringLiteral("/tmp/two.mov"));
    }
    void noRemoteUrlOrUnrelatedEvents() {
        QObject application;
        omatrack::FileOpenEvents events(&application);
        QStringList paths;
        events.setHandler(
            [&paths](const QStringList& opened) { paths.append(opened); });
        QFileOpenEvent remote(
            QUrl(QStringLiteral("https://example.com/recording.mp4")));
        QCoreApplication::sendEvent(&application, &remote);
        QEvent unrelated(QEvent::User);
        QCoreApplication::sendEvent(&application, &unrelated);
        QVERIFY(paths.isEmpty());
    }
};
QTEST_GUILESS_MAIN(FileOpenEventsTest)
#include "FileOpenEventsTest.moc"
