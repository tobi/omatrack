#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <functional>
#include <utility>
#include <QObject>
#include <QAbstractNativeEventFilter>

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

#ifdef Q_OS_WIN
/// Removable logical drives (GetDriveTypeW == DRIVE_REMOVABLE) that are
/// ready: empty card readers fail GetVolumeInformationW and are skipped.
QVector<UsbVolume> windowsRemovableVolumes();
#endif

/// Calls onChange when Windows reports a volume arrival or removal
/// (WM_DEVICECHANGE, DBT_DEVTYP_VOLUME). Install once on the application;
/// never consumes the message. The QObject ownership contract is compiled
/// on every platform; non-Windows events are ignored.
class UsbDeviceChangeFilter : public QObject,
                              public QAbstractNativeEventFilter {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(UsbDeviceChangeFilter)
public:
    explicit UsbDeviceChangeFilter(std::function<void()> onChange,
                                   QObject* parent = nullptr)
        : QObject(parent), onChange_(std::move(onChange)) {}
    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr* result) override;

private:
    std::function<void()> onChange_;
};

}  // namespace omatrack
