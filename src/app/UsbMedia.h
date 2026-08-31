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

/// Parse Linux mount metadata without probing network filesystems. The
/// injectable sysfs tree keeps discovery tests independent of real devices.
QVector<UsbVolume> usbVolumesFromMountInfo(
    const QByteArray& mountInfo,
    const QString& sysClassBlockRoot = QStringLiteral("/sys/class/block"));

/// USB filesystems listed by the Linux kernel; call from an I/O worker.
/// Other platforms retain the existing unsupported-discovery behavior.
QVector<UsbVolume> mountedUsbVolumes();

}  // namespace omatrack
