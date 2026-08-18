pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls

Item {
    id: ruler

    required property real rulerHeight
    readonly property real viewSpan: Math.max(0.001, Store.viewEnd - Store.viewStart)

    clip: true
    implicitHeight: ruler.rulerHeight

    Repeater {
        model: Store.corners

        delegate: Rectangle {
            id: cornerBand

            required property real end
            required property string name
            readonly property real rawLeft: (cornerBand.start - Store.viewStart) / ruler.viewSpan * ruler.width
            readonly property real rawRight: (cornerBand.end - Store.viewStart) / ruler.viewSpan * ruler.width
            required property real start

            Accessible.ignored: true
            border.color: Qt.rgba(Style.magentaColor.r, Style.magentaColor.g, Style.magentaColor.b, 0.55)
            border.width: Store.editingCorners ? 1 : 0
            color: Qt.rgba(Style.magentaColor.r, Style.magentaColor.g, Style.magentaColor.b, Store.editingCorners ? 0.25 : 0.13)
            height: Math.max(1, ruler.height - 5)
            visible: cornerBand.rawRight > 0 && cornerBand.rawLeft < ruler.width && cornerBand.width > 0
            width: Math.max(0, Math.min(ruler.width, cornerBand.rawRight) - cornerBand.x)
            x: Math.max(0, cornerBand.rawLeft)
            y: 2

            Label {
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 3
                color: Qt.rgba(Style.foregroundColor.r, Style.foregroundColor.g, Style.foregroundColor.b, Store.editingCorners ? 0.86 : 0.63)
                elide: Text.ElideRight
                font.bold: Store.editingCorners
                font.family: Style.monoFontFamily
                font.pixelSize: Style.smallFontSize
                text: cornerBand.name
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
