pragma ComponentBehavior: Bound
import Omatrack
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs

AbstractButton {
    id: button

    required property color traceColor

    signal colorSelected(string value)

    Accessible.name: button.text + " trace color"
    ToolTip.delay: 500
    ToolTip.text: button.text + " trace color"
    ToolTip.visible: button.hovered
    hoverEnabled: true
    implicitHeight: 26
    implicitWidth: 74

    background: Rectangle {
        border.color: button.activeFocus ? Style.accentColor : Style.borderColor
        color: button.hovered ? Style.surfaceColor : Style.backgroundColor
        radius: 3
    }
    contentItem: Row {
        leftPadding: 6
        spacing: 5
        topPadding: 5

        Rectangle {
            color: button.traceColor
            height: 14
            radius: 2
            width: 14
        }
        Label {
            color: Style.mutedTextColor
            font.pixelSize: 10
            text: button.text
        }
    }

    onClicked: picker.open()

    ColorDialog {
        id: picker

        selectedColor: button.traceColor
        title: button.text + " trace color"

        onAccepted: button.colorSelected(String(picker.selectedColor))
    }
}
