pragma ComponentBehavior: Bound
import Omatrack
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: overlay

    border.color: Style.accentColor
    border.width: 1
    color: Style.surfaceColor
    implicitHeight: body.implicitHeight + 16
    objectName: "traceResizeOverlay"
    radius: 6
    visible: Store.resizingTraces

    ColumnLayout {
        id: body

        anchors.left: parent.left
        anchors.margins: 8
        anchors.right: parent.right
        anchors.top: parent.top
        spacing: 7

        Label {
            Layout.fillWidth: true
            color: Style.accentColor
            font.bold: true
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: "RESIZE TRACES"
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: Style.smallFontSize
            text: "Drag horizontal dividers. Keep dragging to make room across neighbouring lanes."
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.dimTextColor
            font.pixelSize: Style.smallFontSize
            text: "Save keeps the proportions. Escape cancels."
            wrapMode: Text.Wrap
        }
        CompactButton {
            Layout.fillWidth: true
            objectName: "traceResizeReset"
            text: "Reset heights"

            onClicked: Store.resetTraceHeights()
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            CompactButton {
                Layout.fillWidth: true
                objectName: "traceResizeCancel"
                text: "Cancel"

                onClicked: Store.cancelTraceResize()
            }
            CompactButton {
                Layout.fillWidth: true
                font.bold: true
                objectName: "traceResizeSave"
                text: "Save"

                onClicked: Store.commitTraceResize()
            }
        }
    }
    Shortcut {
        enabled: Store.resizingTraces
        objectName: "traceResizeSaveShortcut"
        sequence: "Ctrl+S"

        onActivated: Store.commitTraceResize()
    }
}
