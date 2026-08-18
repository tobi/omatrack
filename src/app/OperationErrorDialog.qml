pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls

// Modal error dialog with an Ok button. Replaces the inline
// operationErrorDialog in Main.qml.

Dialog {
    id: dialog

    property string errorMessage: ""
    property string errorTitle: "Unable to complete operation"

    closePolicy: Popup.CloseOnEscape
    focus: true
    modal: true
    standardButtons: Dialog.Ok
    title: dialog.errorTitle
    width: Math.min(520, parent.width - 32)
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)

    contentItem: Label {
        color: Style.foregroundColor
        font.pixelSize: 11
        text: dialog.errorMessage
        wrapMode: Text.Wrap
    }
}
