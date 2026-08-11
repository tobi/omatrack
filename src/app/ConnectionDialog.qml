pragma ComponentBehavior: Bound
import Omatrack

// Add or edit one connection to an outside server.
//
// Everything on this form comes from Store.connectionTypes(): the kinds
// offered, what the credential fields are called, and any rows a particular
// protocol needs (an S3 region, an endpoint for something that is not AWS).
// A new connection kind therefore appears here with no change to this file.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    readonly property var currentType: dialog.types.length > 0 && typePicker.currentIndex >= 0 ? dialog.types[typePicker.currentIndex] : null
    property string editingId: ""
    property string errorText: ""
    // Protocol-specific values keyed by field name, filled from the row when
    // editing and read back out of the Repeater's fields on submit.
    property var extraValues: ({})
    property bool hasStoredPassword: false
    property var types: []

    function openForEdit(row): void {
        dialog.resetForm();
        dialog.editingId = row.id;
        dialog.hasStoredPassword = row.hasPassword;
        for (let i = 0; i < dialog.types.length; ++i)
            if (dialog.types[i].type === row.type)
                typePicker.currentIndex = i;
        nameField.text = row.name;
        targetField.text = row.target;
        userField.text = row.username;
        dialog.extraValues = row.options !== undefined ? row.options : {};
        dialog.open();
    }
    function openForNew(type): void {
        dialog.resetForm();
        for (let i = 0; i < dialog.types.length; ++i)
            if (dialog.types[i].type === type)
                typePicker.currentIndex = i;
        dialog.open();
    }
    function resetForm(): void {
        dialog.types = Store.connectionTypes();
        dialog.editingId = "";
        dialog.errorText = "";
        dialog.extraValues = {};
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
        let options = {};
        for (let i = 0; i < extraRepeater.count; ++i) {
            const row = extraRepeater.itemAt(i);
            if (row !== null)
                options[row.fieldKey] = row.fieldValue;
        }
        const error = Store.saveConnection({
            id: dialog.editingId,
            type: dialog.currentType.type,
            name: nameField.text,
            target: targetField.text,
            username: userField.text,
            password: passwordField.text,
            options: options
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
                text: dialog.currentType !== null && dialog.currentType.usernameLabel !== undefined ? dialog.currentType.usernameLabel : "Username"
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
                text: dialog.currentType !== null && dialog.currentType.passwordLabel !== undefined ? dialog.currentType.passwordLabel : "Password"
            }
            CompactTextField {
                id: passwordField

                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: dialog.hasStoredPassword ? "Unchanged" : "Optional"

                onAccepted: dialog.submit()
            }
        }
        Repeater {
            id: extraRepeater

            model: dialog.currentType !== null && dialog.currentType.extraFields !== undefined ? dialog.currentType.extraFields : []

            RowLayout {
                id: extraRow

                // Read back by submit(); every one of these is optional, so a
                // blank simply means "let the protocol work it out".
                readonly property string fieldKey: extraRow.modelData.key
                readonly property alias fieldValue: extraField.text
                required property var modelData

                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.preferredWidth: 74
                    text: extraRow.modelData.label
                }
                CompactTextField {
                    id: extraField

                    Layout.fillWidth: true
                    placeholderText: extraRow.modelData.placeholder
                    text: dialog.extraValues[extraRow.modelData.key] !== undefined ? dialog.extraValues[extraRow.modelData.key] : ""

                    onAccepted: dialog.submit()
                }
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
