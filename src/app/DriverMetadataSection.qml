pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: driverSection

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
            text: (driverSection.folderScope ? "First choose the numeric Driver ID channel. Then define the code = name mappings inherited by videos below this folder. " : "First choose the numeric Driver ID channel. Then map each logger code to the driver's real name. ") + "Use * = Name for any driver ID; an exact ID mapping wins."
            wrapMode: Text.WordWrap
        }
    }
    ChannelMappingField {
        automaticValue: driverSection.channelRow.automaticValue || ""
        channelKey: driverSection.channelRow.key || "driver_id"
        detail: driverSection.channelRow.detail || "Numeric logger code identifying the active driver"
        expectedUnit: driverSection.channelRow.expectedUnit || "numeric code"
        inheritedValue: driverSection.channelRow.inheritedValue || ""
        label: driverSection.channelRow.label || "Driver ID channel"
        suggestions: driverSection.channelRow.suggestions || []
        value: driverSection.channelRow.value || ""

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
    Repeater {
        model: driverSection.mappingRows

        delegate: ColumnLayout {
            id: mappingDelegate

            required property int index
            required property var modelData

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
                    text: mappingDelegate.modelData.id || ""

                    validator: RegularExpressionValidator {
                        regularExpression: /(?:\*|(?:0*[1-9][0-9]*(?:\.[0-9]*)?|0*\.(?:[0-9]*[1-9][0-9]*)))/
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
                    placeholderText: mappingDelegate.modelData.inheritedValue || "Real driver name"
                    placeholderTextColor: mappingDelegate.modelData.inheritedValue ? Style.blueColor : Style.dimTextColor
                    text: mappingDelegate.modelData.value || ""

                    onTextEdited: driverSection.mappingEdited(mappingDelegate.index, driverIdEditor.text, text)
                }
                Label {
                    color: Style.accentColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    text: mappingDelegate.modelData.wildcard === true ? "ANY" : "DETECTED"
                    visible: mappingDelegate.modelData.wildcard === true || mappingDelegate.modelData.detected === true
                }
                ToolButton {
                    Layout.preferredHeight: Style.controlHeight
                    Layout.preferredWidth: Style.controlHeight
                    ToolTip.text: mappingDelegate.modelData.inheritedValue ? "Revert to inherited driver name" : "Remove mapping"
                    ToolTip.visible: hovered
                    enabled: driverNameEditor.text !== "" || !mappingDelegate.modelData.inheritedValue
                    text: "×"

                    onClicked: driverSection.mappingRemoved(mappingDelegate.index)
                }
            }
            Flow {
                Layout.fillWidth: true
                Layout.leftMargin: 277
                spacing: 4
                visible: mappingDelegate.modelData.suggestions && mappingDelegate.modelData.suggestions.length > 0

                Repeater {
                    model: mappingDelegate.modelData.suggestions || []

                    delegate: CompactButton {
                        id: nameSuggestion

                        required property var modelData

                        ToolTip.text: nameSuggestion.modelData.historicalCount > 0 ? "Used by " + nameSuggestion.modelData.historicalCount + (nameSuggestion.modelData.historicalCount === 1 ? " TRACK.yml file" : " TRACK.yml files") : "Suggested driver name"
                        ToolTip.visible: hovered
                        font.pixelSize: Style.smallFontSize
                        implicitHeight: Style.smallControlHeight
                        text: nameSuggestion.modelData.value

                        onClicked: {
                            driverNameEditor.text = nameSuggestion.modelData.value;
                            driverSection.mappingEdited(mappingDelegate.index, driverIdEditor.text, nameSuggestion.modelData.value);
                        }
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
