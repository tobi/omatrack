pragma ComponentBehavior: Bound
import Omatrack

// Compact selectable chip for the sidebar library filters.

import QtQuick
import QtQuick.Controls

Item {
    id: pill

    required property string label
    required property bool selected

    signal activated

    implicitHeight: 18
    implicitWidth: Math.ceil(pillText.implicitWidth + 12)

    Rectangle {
        anchors.fill: parent
        border.color: pill.selected ? Style.accentColor : Style.borderColor
        border.width: 1
        color: pill.selected ? Style.selectionColor : Style.surfaceColor
        radius: height / 2
    }
    Label {
        id: pillText

        anchors.centerIn: parent
        color: pill.selected ? Style.accentColor : Style.mutedTextColor
        font.family: Style.monoFontFamily
        font.pixelSize: Style.smallFontSize
        text: pill.label
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor

        onClicked: pill.activated()
    }
}
