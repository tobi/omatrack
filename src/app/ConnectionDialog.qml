pragma ComponentBehavior: Bound
import Omatrack

// Add or edit one connection to an outside server.
//
// The type list comes from Store.connectionTypes(), so a new connection kind
// appears here as soon as the backend offers it. WebDAV is the only kind
// today; the form is already generic over URL plus optional credentials.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    readonly property var currentType: dialog.types.length > 0 && typePicker.currentIndex >= 0 ? dialog.types[typePicker.currentIndex] : null
    property string editingId: ""
    property string errorText: ""
    property bool hasStoredPassword: false
    property var types: []

    function openForEdit(row): void {
        dialog.reset();
        dialog.editingId = row.id;
        dialog.hasStoredPassword = row.hasPassword;
        for (let i = 0; i < dialog.types.length; ++i)
            if (dialog.types[i].type === row.type)
                typePicker.currentIndex = i;
        nameField.text = row.name;
        targetField.text = row.target;
        userField.text = row.username;
        dialog.open();
    }
    function openForNew(type): void {
        dialog.reset();
        for (let i = 0; i < dialog.types.length; ++i)
            if (dialog.types[i].type === type)
                typePicker.currentIndex = i;
        dialog.open();
    }
    function reset(): void {
        dialog.types = Store.connectionTypes();
        dialog.editingId = "";
        dialog.errorText = "";
        dialog.hasStoredPassword = false;
        typePicker.currentIndex = 0;
        nameField.text = "";
        targetField.text = "";
        userField.text = "";
        passwordField.text = "";
    }
    function submit(): void {
        if (dialog.currentType === null)
            return;
        const error = Store.saveConnection({
            id: dialog.editingId,
            type: dialog.currentType.type,
            name: nameField.text,
            target: targetField.text,
            username: userField.text,
            password: passwordField.text
        });
        if (error === "") {
            dialog.close();
            return;
        }
        dialog.errorText = error;
    }

    closePolicy: Popup.CloseOnEscape
    focus: true
    modal: true
    title: dialog.editingId === "" ? "Connect to a server" : "Edit connection"
    width: 460

    contentItem: ColumnLayout {
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.preferredWidth: 74
                text: "Type"
            }
            ComboBox {
                id: typePicker

                Layout.fillWidth: true
                model: dialog.types
                textRole: "label"
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: Style.smallFontSize
            text: dialog.currentType !== null ? dialog.currentType.detail : ""
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.preferredWidth: 74
                text: "Address"
            }
            CompactTextField {
                id: targetField

                Layout.fillWidth: true
                placeholderText: dialog.currentType !== null ? dialog.currentType.placeholder : ""

                onAccepted: dialog.submit()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.preferredWidth: 74
                text: "Name"
            }
            CompactTextField {
                id: nameField

                Layout.fillWidth: true
                placeholderText: "Optional label"

                onAccepted: dialog.submit()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: dialog.currentType !== null && dialog.currentType.needsCredentials

            Label {
                Layout.preferredWidth: 74
                text: "Username"
            }
            CompactTextField {
                id: userField

                Layout.fillWidth: true
                placeholderText: "Optional"

                onAccepted: dialog.submit()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: dialog.currentType !== null && dialog.currentType.needsCredentials

            Label {
                Layout.preferredWidth: 74
                text: "Password"
            }
            CompactTextField {
                id: passwordField

                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: dialog.hasStoredPassword ? "Unchanged" : "Optional"

                onAccepted: dialog.submit()
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.redColor
            font.pixelSize: Style.smallFontSize
            text: dialog.errorText
            visible: dialog.errorText !== ""
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.dimTextColor
            font.pixelSize: Style.smallFontSize
            text: "Credentials are stored in omatrack.yml in plain text."
            visible: dialog.currentType !== null && dialog.currentType.needsCredentials
            wrapMode: Text.WordWrap
        }
    }
    footer: DialogButtonBox {
        CompactButton {
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            text: "Cancel"
        }
        CompactButton {
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            enabled: targetField.text.trim() !== ""
            text: dialog.editingId === "" ? "Connect" : "Save"
        }
    }

    onAccepted: dialog.submit()
}
