pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: overlayPage

    objectName: "preferencesOverlayPage"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        Label {
            font.bold: true
            font.pixelSize: 16
            text: "Fullscreen overlay"
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Reference traces on the video HUD. White preset uses similar thickness to the primary traces."
            wrapMode: Text.Wrap
        }
        Switch {
            checked: Store.overlayRefWhite
            text: "White reference lines"

            onToggled: Store.overlayRefWhite = checked
        }
        Label {
            text: "Reference style"
        }
        ComboBox {
            id: refStyle

            Layout.fillWidth: true
            Layout.preferredHeight: Style.controlHeight
            currentIndex: Math.max(0, refStyle.model.indexOf(Store.overlayRefStyle))
            model: ["dashed", "dotted", "solid"]

            onActivated: index => Store.overlayRefStyle = refStyle.model[index]
        }
        Label {
            text: "Reference color"
        }
        CompactTextField {
            Layout.fillWidth: true
            enabled: !Store.overlayRefWhite
            text: Store.overlayRefColor

            onEditingFinished: Store.overlayRefColor = text.trim()
        }
        Item {
            Layout.fillHeight: true
        }
    }
}
