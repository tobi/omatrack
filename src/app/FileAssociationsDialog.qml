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
            text: "Register file types for opening from Explorer. Telemetry formats are enabled by default. Video formats are optional and off by default; choose each type separately. Windows may still ask you to choose a default app."
            wrapMode: Text.Wrap
        }
        ScrollView {
            id: associationScroll

            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(associationList.implicitHeight, 360)
            clip: true
            contentWidth: associationScroll.availableWidth

            Column {
                id: associationList

                spacing: 4
                width: associationScroll.availableWidth

                Repeater {
                    model: Updater.associationCount

                    delegate: CheckBox {
                        id: associationBox

                        required property int index

                        checked: Updater.associationEnabled(associationBox.index)
                        text: Updater.associationLabel(associationBox.index) + (Updater.associationVideo(associationBox.index) ? " (optional)" : "")
                        width: associationList.width

                        onToggled: {
                            Updater.setAssociationEnabled(associationBox.index, associationBox.checked);
                            associationBox.checked = Updater.associationEnabled(associationBox.index);
                        }
                    }
                }
            }
        }
    }

    onAccepted: Updater.finishAssociationPrompt()
    onRejected: Updater.finishAssociationPrompt()
}
