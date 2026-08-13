#include "UsbMedia.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <algorithm>

namespace omatrack {
namespace {

bool removableFlag(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.readAll().trimmed() == "1";
}

QString displayName(const QStorageInfo& storage) {
    QString name = storage.name().trimmed();
    if (name.isEmpty()) name = storage.displayName().trimmed();
    if (name.isEmpty() || name == storage.rootPath())
        name = QFileInfo(storage.rootPath()).fileName();
    return name.isEmpty() ? QStringLiteral("USB storage") : name;
}

}  // namespace

bool isUsbBlockDevice(const QByteArray& device,
                      const QString& sysClassBlockRoot) {
#ifdef Q_OS_LINUX
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
#else
    Q_UNUSED(device)
    Q_UNUSED(sysClassBlockRoot)
    return false;
#endif
}

QVector<UsbVolume> mountedUsbVolumes() {
    QVector<UsbVolume> result;
    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    result.reserve(volumes.size());
    for (const QStorageInfo& storage : volumes) {
        if (!storage.isValid() || !storage.isReady() || storage.isRoot() ||
            storage.rootPath().isEmpty() || !isUsbBlockDevice(storage.device()))
            continue;
        result.append(UsbVolume{QDir::cleanPath(storage.rootPath()),
                                displayName(storage), storage.device()});
    }
    std::sort(result.begin(), result.end(),
              [](const UsbVolume& left, const UsbVolume& right) {
                  return left.rootPath < right.rootPath;
              });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

}  // namespace omatrack
