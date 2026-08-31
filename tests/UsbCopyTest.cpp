#include "app/UsbCopy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

namespace {
// Synthetic byte payloads, deliberately not claims of valid video codecs.
bool put(const QString& path, const QByteArray& bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QByteArray bytes(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}
omatrack::UsbCopyOptions options(const QString& root) {
    return {root,
            QStringLiteral("{original}"),
            {},
            QStringLiteral("Test track"),
            QStringLiteral("2026-08-30"),
            QStringLiteral("c1")};
}
QStringList filesAt(const QString& dir) {
    return QDir(dir).entryList(QDir::Files | QDir::Hidden |
                               QDir::NoDotAndDotDot);
}
}  // namespace

class UsbCopyTest : public QObject {
    Q_OBJECT
private slots:
    void previewIsReadOnlyAndUsesTheActualLuaNames() {
        QTemporaryDir temp;
        const QString source = temp.filePath(QStringLiteral("usb/camera.mp4"));
        const QString destination = temp.filePath(QStringLiteral("archive"));
        QVERIFY(put(source, "synthetic recording"));
        auto opts = options(destination);
        opts.script = QStringLiteral(
            "function rename(ctx) return ctx.session .. '/' .. ctx.index .. "
            "'_' .. ctx.original end");
        const auto plan = omatrack::planUsbCopy({source, source}, opts);
        QCOMPARE(plan.ready, 1);
        QCOMPARE(plan.entries[0].relative, QStringLiteral("c1/1_camera.mp4"));
        QVERIFY(!QFileInfo::exists(destination));
        const auto result = omatrack::executeUsbCopy(plan);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.copied, 1);
        QCOMPARE(bytes(plan.entries[0].destination), bytes(source));
        QCOMPARE(QFileInfo(plan.entries[0].destination)
                     .lastModified()
                     .toSecsSinceEpoch(),
                 QFileInfo(source).lastModified().toSecsSinceEpoch());
    }
    void duplicateDestinationsBlockTheWholePlan() {
        QTemporaryDir temp;
        const QString a = temp.filePath(QStringLiteral("usb/a/camera.mp4"));
        const QString b = temp.filePath(QStringLiteral("usb/b/camera.mp4"));
        const QString destination = temp.filePath(QStringLiteral("archive"));
        QVERIFY(put(a, "first"));
        QVERIFY(put(b, "second"));
        const auto plan = omatrack::planUsbCopy({a, b}, options(destination));
        QCOMPARE(plan.invalid, 2);
        QCOMPARE(plan.ready, 0);
        QVERIFY(!omatrack::executeUsbCopy(plan).error.isEmpty());
        QVERIFY(!QFileInfo::exists(destination));
    }
    void existingDifferentBytesAreSkippedNotCountedAsCopied() {
        QTemporaryDir temp;
        const QString source = temp.filePath(QStringLiteral("usb/camera.mp4"));
        const QString destination = temp.filePath(QStringLiteral("archive"));
        const QString target =
            QDir(destination).filePath(QStringLiteral("camera.mp4"));
        QVERIFY(put(source, "new recording"));
        QVERIFY(put(target, "different existing recording"));
        const auto plan = omatrack::planUsbCopy({source}, options(destination));
        QCOMPARE(plan.existing, 1);
        const auto result = omatrack::executeUsbCopy(plan);
        QCOMPARE(result.copied, 0);
        QCOMPARE(result.skipped, 1);
        QCOMPARE(bytes(source), QByteArray("new recording"));
        QCOMPARE(bytes(target), QByteArray("different existing recording"));
    }
    void cancelMidFileRemovesTemporaryAndNeverPublishesPartial() {
        QTemporaryDir temp;
        const QString source = temp.filePath(QStringLiteral("usb/camera.mp4"));
        const QString destination = temp.filePath(QStringLiteral("archive"));
        const QByteArray payload(3 * 1024 * 1024, 'x');
        QVERIFY(put(source, payload));
        const auto plan = omatrack::planUsbCopy({source}, options(destination));
        bool cancel = false;
        qint64 observed = 0;
        const auto result = omatrack::executeUsbCopy(
            plan, [&] { return cancel; },
            [&](qint64 n) {
                observed = n;
                cancel = true;
            });
        QVERIFY(result.cancelled);
        QCOMPARE(result.copied, 0);
        QVERIFY(observed > 0 && observed < payload.size());
        QVERIFY(filesAt(destination).isEmpty());
        QCOMPARE(bytes(source), payload);
    }
    void targetAppearingDuringCopyIsNeverOverwritten() {
        QTemporaryDir temp;
        const QString source = temp.filePath(QStringLiteral("usb/camera.mp4"));
        const QString destination = temp.filePath(QStringLiteral("archive"));
        const QString target =
            QDir(destination).filePath(QStringLiteral("camera.mp4"));
        QVERIFY(put(source, QByteArray(2 * 1024 * 1024, 'x')));
        const auto plan = omatrack::planUsbCopy({source}, options(destination));
        bool inserted = false;
        const auto result = omatrack::executeUsbCopy(plan, {}, [&](qint64) {
            if (!inserted) {
                inserted = put(target, "concurrent target");
            }
        });
        QVERIFY(inserted);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.copied, 0);
        QCOMPARE(result.skipped, 1);
        QCOMPARE(bytes(target), QByteArray("concurrent target"));
        QCOMPARE(filesAt(destination),
                 QStringList{QStringLiteral("camera.mp4")});
    }
    void sourceChangeBeforeCopyRequiresNewPreview() {
        QTemporaryDir temp;
        const QString source = temp.filePath(QStringLiteral("usb/camera.mp4"));
        const QString destination = temp.filePath(QStringLiteral("archive"));
        QVERIFY(put(source, "before"));
        const auto plan = omatrack::planUsbCopy({source}, options(destination));
        QVERIFY(put(source, "changed after preview"));
        const auto result = omatrack::executeUsbCopy(plan);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(result.copied, 0);
        QVERIFY(!QFileInfo::exists(destination));
    }
    void truncatedSourceMidCopyDiscardsThePartial() {
        QTemporaryDir temp;
        const QString source = temp.filePath(QStringLiteral("usb/camera.mp4"));
        const QString destination = temp.filePath(QStringLiteral("archive"));
        QVERIFY(put(source, QByteArray(3 * 1024 * 1024, 'x')));
        const auto plan = omatrack::planUsbCopy({source}, options(destination));
        bool changed = false;
        const auto result = omatrack::executeUsbCopy(plan, {}, [&](qint64) {
            // Deliberate external mutation of a synthetic fixture, not the
            // importer's behaviour and never an original user recording.
            if (!changed) changed = put(source, "x");
        });
        QVERIFY(changed);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(result.copied, 0);
        QVERIFY(filesAt(destination).isEmpty());
        QCOMPARE(bytes(source), QByteArray("x"));
    }
    void symlinkInsideDestinationCannotRedirectTheCopy() {
#ifdef Q_OS_WIN
        QSKIP("POSIX symbolic-link fixture");
#else
        QTemporaryDir temp;
        const QString source = temp.filePath(QStringLiteral("usb/camera.mp4"));
        const QString destination = temp.filePath(QStringLiteral("archive"));
        const QString outside = temp.filePath(QStringLiteral("outside"));
        QVERIFY(put(source, "fixture"));
        QVERIFY(QDir().mkpath(destination));
        QVERIFY(QDir().mkpath(outside));
        QVERIFY(QFile::link(
            outside, QDir(destination).filePath(QStringLiteral("Daytona"))));
        auto opts = options(destination);
        opts.format = QStringLiteral("Daytona/{date}/{original}");
        const auto plan = omatrack::planUsbCopy({source}, opts);
        QCOMPARE(plan.invalid, 1);
        QVERIFY(!omatrack::executeUsbCopy(plan).error.isEmpty());
        QVERIFY(filesAt(outside).isEmpty());
        QVERIFY(!QFileInfo::exists(
            QDir(outside).filePath(QStringLiteral("2026-08-30"))));
#endif
    }
    void namingErrorsBlockCopyWithoutCreatingFolders() {
        QTemporaryDir temp;
        const QString source = temp.filePath(QStringLiteral("camera.mp4"));
        const QString destination = temp.filePath(QStringLiteral("archive"));
        QVERIFY(put(source, "fixture"));
        auto opts = options(destination);
        opts.format = QStringLiteral("{typo}/{original}");
        auto plan = omatrack::planUsbCopy({source}, opts);
        QCOMPARE(plan.invalid, 1);
        QVERIFY(plan.entries[0].message.contains(QStringLiteral("typo")));
        opts.script =
            QStringLiteral("function rename(ctx) return '../escape.mp4' end");
        plan = omatrack::planUsbCopy({source}, opts);
        QCOMPARE(plan.invalid, 1);
        QVERIFY(!omatrack::executeUsbCopy(plan).error.isEmpty());
        QVERIFY(!QFileInfo::exists(destination));
    }
};
QTEST_GUILESS_MAIN(UsbCopyTest)
#include "UsbCopyTest.moc"
