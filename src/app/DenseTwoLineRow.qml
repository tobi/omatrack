pragma ComponentBehavior: Bound
import Omatrack

// Two-line row used by the file/session tree delegates and the lap filmstrip.
//
// Line 1: optional video icon + title (fill, elide right) + optional right
//         value (right-aligned).
// Line 2: optional leading label + detail (fill, elide) + trailing items
//         injected by the caller through the default property (role dots,
//         close buttons, etc.).
//
// Every colour, font, and elide mode is a property so each caller reproduces
// its current visual exactly — no hex literals, no font-family strings.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: row

    property string detail
    property color detailColor: Style.mutedTextColor
    property int detailElide: Text.ElideRight
    property string detailFamily: Style.monoFontFamily
    property string detailLeading: ""
    property color detailLeadingColor: Style.mutedTextColor
    property string detailLeadingFamily: Style.monoFontFamily
    property int detailLeadingSize: 8
    property int detailSize: 8
    property int detailSpacing: 4
    property bool detailVisible: true
    property color rightColor: Style.foregroundColor
    property string rightFamily: Style.monoFontFamily
    property int rightSize: 10
    property string rightValue: ""
    property bool rightVisible: row.rightValue !== ""
    property bool showVideoIcon: false
    property string title
    property bool titleBold: false
    property color titleColor: Style.foregroundColor
    property string titleFamily: Style.uiFontFamily
    property int titleSize: 10
    property int titleSpacing: 6
    default property alias trailing: detailLine.children
    property color videoIconColor: row.titleColor

    spacing: 0

    RowLayout {
        Layout.fillWidth: true
        spacing: row.titleSpacing

        Rectangle {
            Layout.preferredHeight: 11
            Layout.preferredWidth: 15
            border.color: row.videoIconColor
            border.width: 1
            color: "transparent"
            radius: 2
            visible: row.showVideoIcon

            Label {
                anchors.centerIn: parent
                color: row.videoIconColor
                font.family: Style.monoFontFamily
                font.pixelSize: 6
                text: "▶"
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            color: row.titleColor
            elide: Text.ElideRight
            font.bold: row.titleBold
            font.family: row.titleFamily
            font.pixelSize: row.titleSize
            text: row.title
        }
        Label {
            Layout.alignment: Qt.AlignRight
            color: row.rightColor
            font.family: row.rightFamily
            font.pixelSize: row.rightSize
            text: row.rightValue
            visible: row.rightVisible
        }
    }
    RowLayout {
        id: detailLine

        Layout.fillWidth: true
        spacing: row.detailSpacing
        visible: row.detailVisible

        Label {
            Layout.minimumWidth: implicitWidth
            color: row.detailLeadingColor
            font.family: row.detailLeadingFamily
            font.pixelSize: row.detailLeadingSize
            text: row.detailLeading
            visible: row.detailLeading !== ""
        }
        Label {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            color: row.detailColor
            elide: row.detailElide
            font.family: row.detailFamily
            font.pixelSize: row.detailSize
            text: row.detail
        }
    }
}
