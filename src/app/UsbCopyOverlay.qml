pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: overlay

    color: Style.traceBackgroundColor
    objectName: "usbCopyOverlay"
    visible: Store.usbCopyVisible
    z: 20

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 6

        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                font.bold: true
                text: Store.usbLabel === "" ? "Copy from USB" : "Copy from " + Store.usbLabel
            }
            Label {
                color: Store.usbCopyInvalidCount > 0 ? Style.orangeColor : Style.mutedTextColor
                font.pixelSize: Style.smallFontSize
                text: Store.usbPreviewLoading ? "Preparing preview…" : Store.usbCopySummary
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: Style.smallFontSize
            text: "Sources stay unchanged. Existing targets are skipped, not verified. Review every destination before copying."
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: Store.usbCopyTarget
        }
        ListView {
            id: preview

            Layout.fillHeight: true
            Layout.fillWidth: true
            Layout.minimumHeight: 60
            clip: true
            model: Store.usbCopyModel
            objectName: "usbCopyPreview"
            opacity: Store.usbPreviewLoading ? 0.5 : 1
            spacing: 3

            ScrollBar.vertical: ScrollBar {
            }
            delegate: Rectangle {
                id: row

                required property bool ready
                required property string sizeText
                required property string sourcePath
                required property string statusText
                required property string targetPath

                color: Style.backgroundColor
                height: 58
                width: preview.width - 10

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 1

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                            font.family: Style.monoFontFamily
                            font.pixelSize: Style.smallFontSize
                            text: row.sourcePath
                        }
                        Label {
                            color: Style.mutedTextColor
                            font.family: Style.monoFontFamily
                            font.pixelSize: Style.smallFontSize
                            text: row.sizeText
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        color: row.ready ? Style.accentColor : Style.mutedTextColor
                        elide: Text.ElideMiddle
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: row.targetPath === "" ? "No valid destination" : "→ " + row.targetPath
                    }
                    Label {
                        Layout.fillWidth: true
                        color: row.ready ? Style.mutedTextColor : Style.orangeColor
                        elide: Text.ElideRight
                        font.pixelSize: Style.smallFontSize
                        text: row.statusText
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.orangeColor
            font.pixelSize: Style.smallFontSize
            text: Store.usbCopyStatus
            visible: Store.usbCopyStatus !== ""
            wrapMode: Text.Wrap
        }
        ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 1
            value: Store.usbCopyProgress
            visible: Store.usbCopyBusy || Store.usbCopyStatus !== ""
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            CompactButton {
                enabled: !Store.usbCopyBusy && !Store.usbPreviewLoading && Store.usbCopyReadyCount > 0 && Store.usbCopyInvalidCount === 0
                objectName: "usbCopyConfirm"
                text: "Copy " + Store.usbCopyReadyCount + " new files"

                onClicked: Store.copyUsbFiles()
            }
            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                font.pixelSize: Style.smallFontSize
                text: "Naming rules: Preferences → Library"
            }
            CompactButton {
                objectName: "usbCopyCancel"
                text: Store.usbCopyBusy ? "Cancel copy" : "Close"

                onClicked: {
                    if (Store.usbCopyBusy)
                        Store.cancelUsbCopy();
                    else
                        Store.hideUsbCopy();
                }
            }
        }
    }
}
