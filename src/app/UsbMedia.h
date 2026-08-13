#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace omatrack {

struct UsbVolume {
    QString rootPath;
    QString name;
    QByteArray device;

    bool operator==(const UsbVolume& other) const {
        return rootPath == other.rootPath && name == other.name &&
               device == other.device;
    }
};

/// True when `device` resolves to a USB-backed or kernel-removable block
/// device. `sysClassBlockRoot` is injectable for deterministic tests.
bool isUsbBlockDevice(
    const QByteArray& device,
    const QString& sysClassBlockRoot = QStringLiteral("/sys/class/block"));

/// Ready USB filesystems currently mounted by the operating system.
QVector<UsbVolume> mountedUsbVolumes();

}  // namespace omatrack
