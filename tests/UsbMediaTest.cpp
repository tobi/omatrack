#include "app/UsbMedia.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

void writeFlag(const QString& path, const QByteArray& value) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(value), value.size());
}

}  // namespace

class UsbMediaTest : public QObject {
    Q_OBJECT

private slots:
    void detectsUsbTransportPath() {
#ifdef Q_OS_WIN
        QSKIP("Windows cannot follow the POSIX sysfs symlink fixtures");
#endif
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sys = directory.filePath(QStringLiteral("class/block"));
        const QString block = directory.filePath(
            QStringLiteral("devices/pci0000/usb1/1-2/block/sdb/sdb1"));
        QVERIFY(QDir().mkpath(sys));
        QVERIFY(QDir().mkpath(block));
        QVERIFY(QFile::link(block, QDir(sys).filePath(QStringLiteral("sdb1"))));

        QVERIFY(
            omatrack::isUsbBlockDevice(QByteArrayLiteral("/dev/sdb1"), sys));
    }

    void detectsKernelRemovableFlagOnParentDisk() {
#ifdef Q_OS_WIN
        QSKIP("Windows cannot follow the POSIX sysfs symlink fixtures");
#endif
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sys = directory.filePath(QStringLiteral("class/block"));
        const QString disk =
            directory.filePath(QStringLiteral("devices/block/mmcblk0"));
        const QString partition =
            QDir(disk).filePath(QStringLiteral("mmcblk0p1"));
        QVERIFY(QDir().mkpath(sys));
        QVERIFY(QDir().mkpath(partition));
        writeFlag(QDir(disk).filePath(QStringLiteral("removable")), "1\n");
        QVERIFY(QFile::link(partition,
                            QDir(sys).filePath(QStringLiteral("mmcblk0p1"))));

        QVERIFY(omatrack::isUsbBlockDevice(QByteArrayLiteral("/dev/mmcblk0p1"),
                                           sys));
    }

    void rejectsFixedAndUnknownDevices() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sys = directory.filePath(QStringLiteral("class/block"));
        const QString disk =
            directory.filePath(QStringLiteral("devices/pci0000/nvme0n1"));
        QVERIFY(QDir().mkpath(sys));
        QVERIFY(QDir().mkpath(disk));
        writeFlag(QDir(disk).filePath(QStringLiteral("removable")), "0\n");
        QVERIFY(
            QFile::link(disk, QDir(sys).filePath(QStringLiteral("nvme0n1"))));

        QVERIFY(!omatrack::isUsbBlockDevice(QByteArrayLiteral("/dev/nvme0n1"),
                                            sys));
        QVERIFY(!omatrack::isUsbBlockDevice(
            QByteArrayLiteral("/dev/does-not-exist"), sys));
    }
};

QTEST_GUILESS_MAIN(UsbMediaTest)
#include "UsbMediaTest.moc"
