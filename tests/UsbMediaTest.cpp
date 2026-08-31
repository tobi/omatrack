#include "app/UsbMedia.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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

    void mountListingIgnoresNetworkVolumesAndDecodesPaths() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sys = directory.filePath(QStringLiteral("sys"));
        QVERIFY(QDir().mkpath(QDir(sys).filePath(QStringLiteral("sdb1"))));
        writeFlag(QDir(sys).filePath(QStringLiteral("sdb1/removable")), "1\n");
        const QByteArray listing =
            "1 0 8:1 / / rw - ext4 /dev/sdb1 rw\n"
            "2 0 0:42 / /mnt/offline-nas rw - cifs //nas/archive rw\n"
            "3 0 8:17 / /media/TEST\\040STICK rw - vfat /dev/sdb1 rw\n"
            "4 0 8:17 / /media/TEST\\040STICK rw - vfat /dev/sdb1 rw\n"
            "malformed\n";
        const auto volumes = omatrack::usbVolumesFromMountInfo(listing, sys);
        QCOMPARE(volumes.size(), 1);
        QCOMPARE(volumes[0].rootPath, QStringLiteral("/media/TEST STICK"));
        QCOMPARE(volumes[0].name, QStringLiteral("TEST STICK"));
        QCOMPARE(volumes[0].device, QByteArray("/dev/sdb1"));
        // None of these mount paths exists: discovery read only the supplied
        // mount table and sysfs flags, not the actual device or NAS.
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("copy"))));
    }
};

QTEST_GUILESS_MAIN(UsbMediaTest)
#include "UsbMediaTest.moc"
