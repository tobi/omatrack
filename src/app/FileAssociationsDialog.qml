pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    closePolicy: Popup.CloseOnEscape
    focus: true
    modal: true
    standardButtons: Dialog.Ok
    title: "File associations"

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            Layout.fillWidth: true
            Layout.preferredWidth: 420
            color: Style.mutedTextColor
            text: "Omatrack can open these types from Explorer. Telemetry formats are on; MPEG-4 video stays off unless you want every .mp4 to launch Omatrack."
            wrapMode: Text.Wrap
        }
        Repeater {
            model: Updater.associationCount

            delegate: CheckBox {
                id: associationBox

                required property int index

                checked: Updater.associationEnabled(associationBox.index)
                text: Updater.associationLabel(associationBox.index) + (Updater.associationVideo(associationBox.index) ? " (optional)" : "")

                onToggled: Updater.setAssociationEnabled(associationBox.index, checked)
            }
        }
    }

    onAccepted: Updater.finishAssociationPrompt()
    onRejected: Updater.finishAssociationPrompt()
}
