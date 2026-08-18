pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls

// Modal dialog with a single text field and Save/Cancel. Replaces the inline
// driver-rename and corner-rename dialogs in Main.qml. The caller sets
// fieldValue before opening and reads it back in onAccepted.

Dialog {
    id: dialog

    property string fieldObjectName: ""
    property alias fieldValue: field.text
    property string placeholderText: ""

    closePolicy: Popup.CloseOnEscape
    focus: true
    modal: true
    standardButtons: Dialog.Save | Dialog.Cancel
    width: Math.min(360, parent.width - 32)
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)

    contentItem: CompactTextField {
        id: field

        objectName: dialog.fieldObjectName
        placeholderText: dialog.placeholderText

        onAccepted: dialog.accept()
    }

    onOpened: {
        field.forceActiveFocus();
        field.selectAll();
    }
}
