pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: overlay

    color: Style.traceBackgroundColor
    visible: Store.usbCopyVisible
    z: 20

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        Label {
            font.bold: true
            font.pixelSize: 15
            text: Store.usbLabel === "" ? "Copy from USB" : "Copy from " + Store.usbLabel
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Source recordings stay on the stick. Copy adds them to the library destination using the format in Preferences."
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.accentColor
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: Store.eventSession === "" ? "session unset" : "session " + Store.eventSession
            visible: Store.eventMode
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            elide: Text.ElideMiddle
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: Store.usbDest === "" ? Store.defaultTelemetryDirectory() : Store.usbDest
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            elide: Text.ElideRight
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: Store.usbFormat
        }
        Label {
            Layout.fillWidth: true
            color: Style.orangeColor
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: Store.usbCopyStatus
            visible: Store.usbCopyStatus !== ""
        }
        ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 1
            value: Store.usbCopyProgress
            visible: Store.usbCopyStatus !== ""
        }
        Item {
            Layout.fillHeight: true
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            CompactButton {
                enabled: Store.usbCopyStatus.indexOf("Copying") !== 0
                text: "Copy into library"

                onClicked: Store.copyUsbFiles()
            }
            CompactButton {
                text: "Cancel"

                onClicked: Store.hideUsbCopy()
            }
        }
    }
}
