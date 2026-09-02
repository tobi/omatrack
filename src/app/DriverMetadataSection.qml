pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: driverSection

    property bool channelMappingVisible: true
    required property var channelRow
    required property bool folderScope
    required property var mappingRows

    signal channelEdited(string key, string value)
    signal mappingAdded
    signal mappingEdited(int index, string driverId, string name)
    signal mappingRemoved(int index)

    Layout.fillWidth: true
    spacing: 8

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 1

        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.bold: true
            text: "Driver identity"
        }
        Label {
            Layout.fillWidth: true
            color: Style.dimTextColor
            font.pixelSize: Style.smallFontSize
            text: (driverSection.folderScope ? "First choose the numeric Driver ID channel. Then define the code = name mappings inherited by recordings below this folder. " : "First choose the numeric Driver ID channel. Then map each logger code to the driver's real name. ") + "Use * = Name for any driver ID; an exact ID mapping wins."
            wrapMode: Text.WordWrap
        }
    }
    ChannelMappingField {
        automaticValue: driverSection.channelRow.automaticValue || ""
        channelKey: driverSection.channelRow.key || "driver_id"
        detail: driverSection.channelRow.detail || "Numeric logger code identifying the active driver"
        expectedUnit: driverSection.channelRow.expectedUnit || "numeric code"
        folderScope: driverSection.folderScope
        inheritedValue: driverSection.channelRow.inheritedValue || ""
        label: driverSection.channelRow.label || "Driver ID channel"
        suggestions: driverSection.channelRow.suggestions || []
        value: driverSection.channelRow.value || ""
        visible: driverSection.channelMappingVisible

        onMappingEdited: (key, value) => driverSection.channelEdited(key, value)
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        ColumnLayout {
            Layout.maximumWidth: 148
            Layout.minimumWidth: 148
            Layout.preferredWidth: 148
            spacing: 0

            Label {
                Layout.fillWidth: true
                color: Style.foregroundColor
                font.bold: true
                text: "Driver names"
            }
            Label {
                Layout.fillWidth: true
                color: Style.dimTextColor
                font.pixelSize: Style.smallFontSize
                text: "Driver ID = Name"
            }
        }
        Label {
            Layout.preferredWidth: 92
            color: Style.dimTextColor
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: "ID OR *"
        }
        Label {
            color: Style.dimTextColor
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: "="
        }
        Label {
            Layout.fillWidth: true
            color: Style.dimTextColor
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: "DRIVER NAME"
        }
        Item {
            Layout.preferredWidth: Style.controlHeight
        }
    }
    // Model is the row *count*, not the array: every keystroke replaces
    // `mappingRows` with a fresh array, and a Repeater bound to the array
    // itself destroys and recreates all delegates on each edit, so the
    // focused editor dies after one character. Binding to the length keeps
    // the delegates alive across in-row edits while add/remove still rebuild.
    Repeater {
        model: driverSection.mappingRows.length

        delegate: ColumnLayout {
            id: mappingDelegate

            required property int index
            readonly property var row: driverSection.mappingRows[mappingDelegate.index] || ({})

            Layout.fillWidth: true
            spacing: 3

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Item {
                    Layout.maximumWidth: 148
                    Layout.minimumWidth: 148
                    Layout.preferredWidth: 148
                }
                CompactTextField {
                    id: driverIdEditor

                    Layout.preferredWidth: 92
                    horizontalAlignment: Text.AlignRight
                    inputMethodHints: Qt.ImhNoPredictiveText
                    placeholderText: "ID or *"
                    text: mappingDelegate.row.id || ""

                    validator: RegularExpressionValidator {
                        regularExpression: /(?:|\*|(?:0*[1-9][0-9]*(?:\.[0-9]*)?|0*\.(?:[0-9]*[1-9][0-9]*)))/
                    }

                    onTextEdited: driverSection.mappingEdited(mappingDelegate.index, text, driverNameEditor.text)
                }
                Label {
                    color: Style.foregroundColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    text: "="
                }
                CompactTextField {
                    id: driverNameEditor

                    Layout.fillWidth: true
                    objectName: "driverNameEditor" + mappingDelegate.index
                    placeholderText: mappingDelegate.row.inheritedValue || "Real driver name"
                    placeholderTextColor: mappingDelegate.row.inheritedValue ? Style.blueColor : Style.dimTextColor
                    text: mappingDelegate.row.value || ""

                    onTextEdited: driverSection.mappingEdited(mappingDelegate.index, driverIdEditor.text, text)
                }
                Label {
                    color: Style.accentColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    text: mappingDelegate.row.wildcard === true ? "ANY" : "DETECTED"
                    visible: mappingDelegate.row.wildcard === true || mappingDelegate.row.detected === true
                }
                ToolButton {
                    Accessible.name: mappingDelegate.row.inheritedValue ? "Revert to inherited driver name" : "Remove mapping"
                    Layout.preferredHeight: Style.controlHeight
                    Layout.preferredWidth: Style.controlHeight
                    enabled: driverNameEditor.text !== "" || !mappingDelegate.row.inheritedValue
                    text: "×"

                    onClicked: driverSection.mappingRemoved(mappingDelegate.index)
                }
            }
            Flow {
                Layout.fillWidth: true
                Layout.leftMargin: 277
                spacing: 4
                visible: mappingDelegate.row.suggestions && mappingDelegate.row.suggestions.length > 0

                Repeater {
                    model: mappingDelegate.row.suggestions || []

                    delegate: CompactButton {
                        id: nameSuggestion

                        required property int index
                        required property var modelData

                        font.pixelSize: Style.smallFontSize
                        implicitHeight: Style.smallControlHeight
                        objectName: "driverNameSuggestion" + mappingDelegate.index + "_" + nameSuggestion.index
                        text: nameSuggestion.modelData.value

                        // No direct `driverNameEditor.text =` here: the
                        // delegate now outlives model updates, so breaking the
                        // text binding would leave the field stale forever.
                        onClicked: driverSection.mappingEdited(mappingDelegate.index, driverIdEditor.text, nameSuggestion.modelData.value)
                    }
                }
            }
        }
    }
    CompactButton {
        Layout.leftMargin: 160
        text: "+ Add Driver ID = Name"

        onClicked: driverSection.mappingAdded()
    }
}
