pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Sync-to-device: library videos/telemetry the stick doesn't have yet,
// named by the event entry and the shared renaming rules. The event fields
// are the same event-mode properties, so an active event mode pre-fills
// them and a manual entry here feeds event mode back.
ApplicationWindow {
    id: syncWindow

    Material.accent: Style.accentColor
    Material.background: Style.backgroundColor
    Material.foreground: Style.foregroundColor
    Material.primary: Style.surfaceColor
    Material.theme: Material.Dark
    color: Style.backgroundColor
    font.family: Style.uiFontFamily
    font.pixelSize: 11
    height: 640
    minimumHeight: 480
    minimumWidth: 720
    objectName: "usbSyncWindow"
    title: "Sync to USB"
    visible: Store.usbSyncVisible
    width: 860

    onClosing: Store.hideUsbSync()

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Label {
            Layout.fillWidth: true
            color: Style.foregroundColor
            elide: Text.ElideMiddle
            font.bold: true
            font.pixelSize: 13
            text: Store.usbSyncTarget !== "" ? "Sync to " + Store.usbSyncTarget : "Sync to USB"
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: Style.smallFontSize
            text: Store.usbSyncSummary
            visible: Store.usbSyncSummary !== ""
        }
        GridLayout {
            Layout.fillWidth: true
            columns: 4

            Label {
                text: "Track"
            }
            ComboBox {
                id: syncTrack

                Layout.fillWidth: true
                currentIndex: Math.max(0, syncTrack.model.indexOf(Store.eventTrack))
                model: [""].concat(Store.library.trackPills)

                onActivated: index => Store.eventTrack = index <= 0 ? "" : syncTrack.model[index]
            }
            Label {
                text: "Date"
            }
            CompactTextField {
                Layout.fillWidth: true
                placeholderText: "YYYY-MM-DD"
                text: Store.eventDate

                onEditingFinished: Store.eventDate = text.trim()
            }
            Label {
                text: "Session"
            }
            CompactTextField {
                Layout.columnSpan: 3
                Layout.fillWidth: true
                placeholderText: "CT4"
                text: Store.eventSession

                onEditingFinished: Store.eventSession = text.trim()
            }
            Label {
                text: "Naming"
            }
            CompactTextField {
                Layout.columnSpan: 3
                Layout.fillWidth: true
                font.family: Style.monoFontFamily
                text: Store.usbFormat

                onEditingFinished: Store.usbFormat = text.trim()
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: Style.smallFontSize
            text: "Only files missing on the device are listed. Existing targets are skipped, never overwritten. Lua rename scripts stay in Preferences → Library."
            wrapMode: Text.Wrap
        }
        ListView {
            id: syncPreview

            Layout.fillHeight: true
            Layout.fillWidth: true
            Layout.minimumHeight: 120
            clip: true
            model: Store.usbSyncModel
            objectName: "usbSyncPreview"

            delegate: ColumnLayout {
                id: syncRow

                required property bool ready
                required property string sizeText
                required property string sourcePath
                required property string statusText
                required property string targetPath

                spacing: 0
                width: ListView.view.width

                Label {
                    Layout.fillWidth: true
                    color: syncRow.ready ? Style.foregroundColor : Style.orangeColor
                    elide: Text.ElideMiddle
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: syncRow.sourcePath
                }
                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    elide: Text.ElideMiddle
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: "→ " + syncRow.targetPath
                }
                Label {
                    Layout.fillWidth: true
                    color: syncRow.ready ? Style.mutedTextColor : Style.orangeColor
                    font.pixelSize: 10
                    text: (syncRow.ready ? "New on device" : syncRow.statusText) + " · " + syncRow.sizeText
                }
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: Style.smallFontSize
            text: Store.usbSyncStatus
            visible: Store.usbSyncStatus !== ""
            wrapMode: Text.Wrap
        }
        ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 1
            value: Store.usbSyncProgress
            visible: Store.usbSyncBusy
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                font.pixelSize: Style.smallFontSize
                text: "Naming rules: Preferences → Library"
            }
            CompactButton {
                enabled: !Store.usbSyncBusy && !Store.usbSyncPreviewLoading && Store.usbSyncReadyCount > 0 && Store.usbSyncInvalidCount === 0
                objectName: "usbSyncConfirm"
                text: "Sync all"

                onClicked: Store.syncUsbFiles()
            }
            CompactButton {
                text: Store.usbSyncBusy ? "Cancel" : "Close"

                onClicked: Store.hideUsbSync()
            }
        }
    }
}
