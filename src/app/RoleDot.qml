pragma ComponentBehavior: Bound
import Omatrack

// Current / reference selector dot used by the session tree rows.
//
// Two of these replace the old "ACTIVE" / "⇄ REF" text labels: the role is
// directly clickable, and the size/opacity animation reads as a state rather
// than as decoration.

import QtQuick

Item {
    id: dot

    property color activeColor: Style.accentColor
    property bool selected: false
    property int size: 14
    property string tip: ""

    signal activated

    Accessible.name: dot.tip
    Accessible.role: Accessible.Button
    activeFocusOnTab: true
    implicitHeight: dot.size
    implicitWidth: dot.size

    Accessible.onPressAction: dot.activated()
    Keys.onEnterPressed: dot.activated()
    Keys.onReturnPressed: dot.activated()
    Keys.onSpacePressed: dot.activated()

    Rectangle {
        anchors.centerIn: parent
        color: dot.selected ? dot.activeColor : dotMouse.containsMouse ? Style.foregroundColor : Style.dimTextColor
        height: width
        opacity: dot.selected ? 1.0 : dotMouse.containsMouse ? 0.85 : 0.45
        radius: width / 2
        width: dot.selected ? Math.max(6, dot.size - 2) : dotMouse.containsMouse ? Math.max(5, dot.size - 6) : Math.max(3, dot.size - 9)

        Behavior on opacity {
            NumberAnimation {
                duration: 110
            }
        }
        Behavior on width {
            NumberAnimation {
                duration: 110
                easing.type: Easing.OutCubic
            }
        }
    }
    MouseArea {
        id: dotMouse

        anchors.fill: parent
        hoverEnabled: true

        onClicked: dot.activated()
    }
}
