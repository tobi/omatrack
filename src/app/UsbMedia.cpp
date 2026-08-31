#include "UsbMedia.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace omatrack {
namespace {

bool removableFlag(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.readAll().trimmed() == "1";
}

QByteArray mountField(const QByteArray& field) {
    QByteArray decoded;
    for (qsizetype i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] >= '0' &&
            field[i + 1] <= '7' && field[i + 2] >= '0' && field[i + 2] <= '7' &&
            field[i + 3] >= '0' && field[i + 3] <= '7') {
            decoded += char((field[i + 1] - '0') * 64 +
                            (field[i + 2] - '0') * 8 + field[i + 3] - '0');
            i += 3;
        } else {
            decoded += field[i];
        }
    }
    return decoded;
}

}  // namespace

bool isUsbBlockDevice(const QByteArray& device,
                      const QString& sysClassBlockRoot) {
#ifndef Q_OS_LINUX
    // Live mounts are Linux-only. Tests inject a fake sysfs tree and still
    // exercise the classifier on every platform.
    if (sysClassBlockRoot == QStringLiteral("/sys/class/block")) return false;
#endif
    const QString deviceName =
        QFileInfo(QString::fromLocal8Bit(device)).fileName();
    if (deviceName.isEmpty()) return false;

    const QFileInfo classEntry(QDir(sysClassBlockRoot).filePath(deviceName));
    const QString canonical = classEntry.canonicalFilePath();
    if (canonical.isEmpty()) return false;

    const QString slashCanonical = QDir::fromNativeSeparators(canonical);
    for (const QString& component :
         slashCanonical.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (!component.startsWith(QStringLiteral("usb"), Qt::CaseInsensitive))
            continue;
        bool busNumber = false;
        component.sliced(3).toUInt(&busNumber);
        if (busNumber) return true;
    }

    if (removableFlag(classEntry.filePath() + QStringLiteral("/removable")) ||
        removableFlag(QDir(canonical).filePath(QStringLiteral("removable"))))
        return true;

    const QDir parent = QFileInfo(canonical).dir();
    return removableFlag(parent.filePath(QStringLiteral("removable")));
}

QVector<UsbVolume> usbVolumesFromMountInfo(const QByteArray& mountInfo,
                                           const QString& sysClassBlockRoot) {
    QVector<UsbVolume> result;
    for (const QByteArray& line : mountInfo.split('\n')) {
        const auto fields = line.simplified().split(' ');
        const auto separator = fields.indexOf("-");
        if (fields.size() < 7 || separator < 6 ||
            separator + 2 >= fields.size())
            continue;
        const QByteArray device = mountField(fields[separator + 2]);
        // Filter by local sysfs before touching any mount. QStorageInfo's
        // mountedVolumes() stats every filesystem, including an offline NAS.
        if (!device.startsWith("/dev/") ||
            !isUsbBlockDevice(device, sysClassBlockRoot))
            continue;
        const QString root =
            QDir::cleanPath(QString::fromLocal8Bit(mountField(fields[4])));
        if (root.isEmpty() || root == QStringLiteral("/")) continue;
        const QString name = QFileInfo(root).fileName();
        result.append(UsbVolume{
            root, name.isEmpty() ? QStringLiteral("USB storage") : name,
            device});
    }
    std::sort(result.begin(), result.end(),
              [](const UsbVolume& left, const UsbVolume& right) {
                  return left.rootPath < right.rootPath;
              });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

QVector<UsbVolume> mountedUsbVolumes() {
#ifdef OMATRACK_ENABLE_AUTOTEST_HARNESS
    // Acceptance builds only: a scratch folder stands in for a stick so the
    // preview/copy overlay can be exercised without real removable media.
    const QString fake = qEnvironmentVariable("OMATRACK_AUTOTEST_USB_ROOT");
    if (!fake.isEmpty()) {
        if (!QFileInfo(fake).isDir()) return {};
        return {UsbVolume{QDir::cleanPath(fake), QStringLiteral("AUTOTEST-USB"),
                          QByteArrayLiteral("/dev/autotest")}};
    }
#endif
#ifdef Q_OS_LINUX
    QFile mounts(QStringLiteral("/proc/self/mountinfo"));
    if (mounts.open(QIODevice::ReadOnly))
        return usbVolumesFromMountInfo(mounts.readAll());
#endif
    return {};
}

}  // namespace omatrack
