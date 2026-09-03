// Native USB preview/copy acceptance. Compiled only with the harness.
//
// OMATRACK_AUTOTEST_USB_ROOT points mountedUsbVolumes() at a scratch folder
// holding copied fixtures. The check verifies that discovery opens the
// overlay with per-file destinations, that nothing is written before the
// button, and that the button copies exactly the planned new files.
#include "AutotestHarness.h"
#include "TelemetryStore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>

namespace {
QQuickItem* visualItem(QQuickItem* root, const QString& name) {
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (QQuickItem* child : root->childItems())
        if (auto* found = visualItem(child, name)) return found;
    return nullptr;
}
QStringList filesUnder(const QString& root) {
    QStringList result;
    QDirIterator it(root, QDir::Files | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) result.append(QDir(root).relativeFilePath(it.next()));
    result.sort();
    return result;
}
}  // namespace

bool omatrack::autotest::installUsbCopy(QQmlApplicationEngine& engine,
                                        TelemetryStore& store) {
    const QString usbRoot = qEnvironmentVariable("OMATRACK_AUTOTEST_USB_ROOT");
    const QString dest = qEnvironmentVariable("OMATRACK_AUTOTEST_USB_DEST");
    if (usbRoot.isEmpty() || dest.isEmpty()) return false;
    const QString shot = qEnvironmentVariable("OMATRACK_AUTOTEST");
    const bool syncPhase =
        !qEnvironmentVariable("OMATRACK_AUTOTEST_USB_EXPORT").isEmpty();
    store.setUsbDest(dest);
    auto* timer = new QTimer(&engine);
    timer->setInterval(200);
    auto phase = std::make_shared<int>(0);
    auto ticks = std::make_shared<int>(0);
    auto before = std::make_shared<QStringList>();
    auto planned = std::make_shared<QStringList>();
    auto syncPlanned = std::make_shared<QStringList>();
    auto syncBefore = std::make_shared<QStringList>();
    QObject::connect(
        timer, &QTimer::timeout, &engine,
        [&, timer, phase, ticks, before, planned, syncPlanned, syncBefore, shot,
         dest, usbRoot, syncPhase]() {
            const auto fail = [&](const QString& why) {
                timer->stop();
                qWarning() << "AUTOTEST usb FAILED:" << why;
                QCoreApplication::exit(1);
            };
            if (++*ticks > 300) return fail(QStringLiteral("timed out"));
            QObject* rootObject = engine.rootObjects().isEmpty()
                                      ? nullptr
                                      : engine.rootObjects().first();
            auto* window = qobject_cast<QQuickWindow*>(rootObject);
            if (!window) return;
            if (*phase == 0) {
                // Discovery must open the overlay by itself with a preview.
                if (!store.usbPresent() || !store.usbCopyVisible() ||
                    store.usbPreviewLoading() ||
                    store.usbCopyModel()->count() == 0)
                    return;
                auto* overlay = visualItem(window->contentItem(),
                                           QStringLiteral("usbCopyOverlay"));
                auto* preview = visualItem(window->contentItem(),
                                           QStringLiteral("usbCopyPreview"));
                if (!overlay || !overlay->isVisible() || !preview)
                    return fail(QStringLiteral("overlay/preview not visible"));
                const int rows = store.usbCopyModel()->count();
                for (int row = 0; row < rows; ++row) {
                    const auto index = store.usbCopyModel()->index(row, 0);
                    const QString target =
                        index.data(UsbCopyListModel::TargetPathRole).toString();
                    if (index.data(UsbCopyListModel::ReadyRole).toBool()) {
                        if (!target.startsWith(QDir::cleanPath(dest)))
                            return fail(
                                QStringLiteral("target outside destination"));
                        planned->append(QDir(dest).relativeFilePath(target));
                    }
                }
                planned->sort();
                if (planned->isEmpty() || store.usbCopyInvalidCount() != 0)
                    return fail(
                        QStringLiteral(
                            "preview has no ready rows or has invalid rows: ") +
                        store.usbCopySummary());
                *before = filesUnder(dest);
                if (!before->isEmpty())
                    return fail(
                        QStringLiteral("preview wrote into the destination"));
                window->grabWindow().save(shot +
                                          QStringLiteral(".preview.png"));
                qWarning() << "AUTOTEST usb: discovery opened preview with"
                           << rows << "rows," << planned->size() << "new;"
                           << store.usbCopySummary();
                *phase = 1;
                *ticks = 0;
                return;
            }
            if (*phase == 1) {
                // Wait a second with the overlay open: still nothing copied.
                if (*ticks < 5) return;
                if (!filesUnder(dest).isEmpty())
                    return fail(
                        QStringLiteral("files copied without the button"));
                auto* button = visualItem(window->contentItem(),
                                          QStringLiteral("usbCopyConfirm"));
                if (!button || !button->property("enabled").toBool())
                    return fail(
                        QStringLiteral("copy button missing or disabled"));
                QMetaObject::invokeMethod(button, "clicked");
                *phase = 2;
                *ticks = 0;
                return;
            }
            if (*phase == 2) {
                if (store.usbCopyBusy() || store.usbPreviewLoading()) return;
                if (*ticks < 3) return;
                const QStringList after = filesUnder(dest);
                if (after != *planned)
                    return fail(QStringLiteral("copied set %1 != planned %2")
                                    .arg(after.join(','), planned->join(',')));
                if (store.usbCopyReadyCount() != 0)
                    return fail(QStringLiteral(
                        "refreshed preview still lists new files"));
                if (!store.usbCopyStatus().startsWith(
                        QStringLiteral("Copied %1").arg(planned->size())))
                    return fail(QStringLiteral("status: ") +
                                store.usbCopyStatus());
                window->grabWindow().save(shot + QStringLiteral(".copied.png"));
                qWarning() << "AUTOTEST usb: button copied exactly the planned "
                              "new files;"
                           << store.usbCopyStatus();
                if (!syncPhase) {
                    timer->stop();
                    QCoreApplication::exit(0);
                    return;
                }
                *phase = 3;
                *ticks = 0;
                return;
            }
            if (*phase == 3) {
                // Export: library files missing on the stick, named by the
                // event entry. The event values double as the naming input,
                // exactly as a client handoff would set them.
                if (!store.usbSyncVisible()) {
                    store.setEventTrack(QStringLiteral("Road America"));
                    store.setEventSession(QStringLiteral("FP1"));
                    store.showUsbSync();
                    return;
                }
                if (!store.usbSyncVisible() || store.usbSyncPreviewLoading() ||
                    store.usbSyncModel()->count() == 0)
                    return;
                const int rows = store.usbSyncModel()->count();
                for (int row = 0; row < rows; ++row) {
                    const auto index = store.usbSyncModel()->index(row, 0);
                    const QString target =
                        index.data(UsbCopyListModel::TargetPathRole).toString();
                    if (index.data(UsbCopyListModel::ReadyRole).toBool()) {
                        if (!target.startsWith(QDir::cleanPath(usbRoot)))
                            return fail(QStringLiteral(
                                "sync target outside the device"));
                        syncPlanned->append(
                            QDir(usbRoot).relativeFilePath(target));
                    }
                }
                syncPlanned->sort();
                if (syncPlanned->isEmpty() || store.usbSyncInvalidCount() != 0)
                    return fail(QStringLiteral("sync preview has no ready "
                                               "rows or has invalid rows: ") +
                                store.usbSyncSummary());
                *syncBefore = filesUnder(usbRoot);
                window->grabWindow().save(shot +
                                          QStringLiteral(".sync-preview.png"));
                qWarning() << "AUTOTEST usb sync: preview lists" << rows
                           << "rows," << syncPlanned->size()
                           << "missing on device;" << store.usbSyncSummary();
                *phase = 4;
                *ticks = 0;
                return;
            }
            if (*phase == 4) {
                if (*ticks < 5) return;
                if (filesUnder(usbRoot) != *syncBefore)
                    return fail(
                        QStringLiteral("files synced without the button"));
                // The sync UI is its own window: a Window declared inside
                // Main.qml is not an engine root object, so find it in the
                // object tree instead.
                QObject* button = nullptr;
                for (QObject* root : engine.rootObjects()) {
                    auto* syncWin = root->findChild<QQuickWindow*>(
                        QStringLiteral("usbSyncWindow"));
                    if (!syncWin) continue;
                    button = visualItem(syncWin->contentItem(),
                                        QStringLiteral("usbSyncConfirm"));
                    if (button) break;
                }
                if (!button || !button->property("enabled").toBool())
                    return fail(
                        QStringLiteral("sync button missing or disabled"));
                QMetaObject::invokeMethod(button, "clicked");
                *phase = 5;
                *ticks = 0;
                return;
            }
            if (*phase == 5) {
                if (store.usbSyncBusy() || store.usbSyncPreviewLoading())
                    return;
                if (*ticks < 3) return;
                const QStringList after = filesUnder(usbRoot);
                QStringList expected = *syncBefore + *syncPlanned;
                expected.sort();
                if (after != expected)
                    return fail(QStringLiteral("synced set %1 != planned %2")
                                    .arg(after.join(','), expected.join(',')));
                if (!store.usbSyncStatus().startsWith(
                        QStringLiteral("Synced %1").arg(syncPlanned->size())))
                    return fail(QStringLiteral("sync status: ") +
                                store.usbSyncStatus());
                QQuickWindow* syncWindow = nullptr;
                for (QObject* root : engine.rootObjects()) {
                    syncWindow = root->findChild<QQuickWindow*>(
                        QStringLiteral("usbSyncWindow"));
                    if (syncWindow) break;
                }
                if (!syncWindow)
                    return fail(QStringLiteral("sync window not found"));
                syncWindow->grabWindow().save(shot +
                                              QStringLiteral(".synced.png"));
                timer->stop();
                qWarning() << "AUTOTEST usb sync: button synced exactly the "
                              "planned missing files;"
                           << store.usbSyncStatus();
                QCoreApplication::exit(0);
            }
        });
    timer->start();
    return true;
}
