pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: mappingField

    property var autocompleteRows: []
    required property string automaticValue
    required property string channelKey
    required property string detail
    required property string expectedUnit
    property bool folderScope: false
    required property string inheritedValue
    required property string label
    property var quickRows: []
    property string selectedValue: mappingField.value
    required property var suggestions
    required property string value

    signal mappingEdited(string key, string value)

    function browseChannels(): void {
        mappingField.refreshRows("");
        channelBrowser.openBelow(mappingEditor, mappingGrid.width - 160);
        mappingEditor.forceActiveFocus();
    }
    function chooseSuggestion(value: string): void {
        mappingEditor.text = value;
        mappingField.selectedValue = value;
        mappingField.mappingEdited(mappingField.channelKey, value);
        mappingField.refreshRows(value);
        channelBrowser.close();
        mappingEditor.forceActiveFocus();
    }
    function refreshRows(query: string): void {
        const cleanQuery = query.trim().toLowerCase();
        let filtered = [];
        let quick = [];
        for (let index = 0; index < mappingField.suggestions.length; ++index) {
            const row = mappingField.suggestions[index];
            const examples = row.examples || [];
            const searchable = ((row.value || "") + " " + (row.unit || "") + " " + examples.join(" ")).toLowerCase();
            if (cleanQuery === "" || searchable.indexOf(cleanQuery) !== -1)
                filtered.push(row);
            if (quick.length < 4 && row.value !== mappingField.selectedValue && (row.automatic || row.historicalCount > 0))
                quick.push(row);
        }
        mappingField.autocompleteRows = filtered;
        mappingField.quickRows = quick;
    }
    function showAutocomplete(): void {
        mappingField.refreshRows(mappingEditor.text);
        if (mappingField.autocompleteRows.length === 0)
            return;
        channelBrowser.openBelow(mappingEditor, mappingGrid.width - 160);
    }
    function suggestionCaption(row: var): string {
        const prefix = row.automatic ? "AUTO  " : "";
        const unit = row.unit ? "  ·  " + row.unit : "";
        const count = row.historicalCount > 0 ? "  ×" + row.historicalCount : "";
        return prefix + row.value + unit + count;
    }

    Layout.fillWidth: true
    implicitHeight: mappingGrid.implicitHeight
    objectName: mappingField.channelKey === "driver_id" ? "driverChannelMapping" : ""

    Component.onCompleted: mappingField.refreshRows(mappingField.value)
    onSuggestionsChanged: mappingField.refreshRows(mappingField.selectedValue)
    onValueChanged: {
        if (!mappingEditor.activeFocus) {
            mappingField.selectedValue = mappingField.value;
            mappingEditor.text = mappingField.value;
            mappingField.refreshRows(mappingField.value);
        }
    }

    GridLayout {
        id: mappingGrid

        anchors.fill: parent
        columnSpacing: 12
        columns: 2
        rowSpacing: 4

        ColumnLayout {
            Layout.alignment: Qt.AlignTop
            Layout.maximumWidth: 148
            Layout.minimumWidth: 148
            Layout.preferredWidth: 148
            spacing: 0

            Label {
                Layout.fillWidth: true
                color: Style.foregroundColor
                font.bold: true
                font.pixelSize: Style.fontSize
                text: mappingField.label
            }
            Label {
                Layout.fillWidth: true
                color: Style.dimTextColor
                elide: Text.ElideRight
                font.family: Style.monoFontFamily
                font.pixelSize: Style.smallFontSize
                text: mappingField.channelKey + "  ·  " + mappingField.expectedUnit
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            CompactTextField {
                id: mappingEditor

                Layout.fillWidth: true
                placeholderText: mappingField.inheritedValue !== "" ? mappingField.inheritedValue : mappingField.automaticValue !== "" ? "Automatic: " + mappingField.automaticValue : "Search source channels"
                placeholderTextColor: mappingField.inheritedValue !== "" ? Style.blueColor : Style.dimTextColor
                text: mappingField.value

                onActiveFocusChanged: {
                    if (activeFocus)
                        mappingField.showAutocomplete();
                }
                onTextEdited: {
                    mappingField.selectedValue = text;
                    mappingField.mappingEdited(mappingField.channelKey, text);
                    mappingField.refreshRows(text);
                    if (mappingField.autocompleteRows.length > 0)
                        mappingField.showAutocomplete();
                    else
                        channelBrowser.close();
                }
            }
            CompactButton {
                id: browseButton

                text: "Browse"

                onClicked: mappingField.browseChannels()
            }
            ToolButton {
                Accessible.name: mappingField.inheritedValue !== "" ? "Use inherited mapping" : "Use automatic mapping"
                Layout.preferredHeight: Style.controlHeight
                Layout.preferredWidth: Style.controlHeight
                enabled: mappingEditor.text !== ""
                text: "×"

                onClicked: mappingField.chooseSuggestion("")
            }
        }
        Item {
            Layout.maximumWidth: 148
            Layout.minimumWidth: 148
            Layout.preferredHeight: suggestionFlow.implicitHeight
            Layout.preferredWidth: 148
        }
        Flow {
            id: suggestionFlow

            Layout.fillWidth: true
            Layout.minimumHeight: visible ? Style.smallControlHeight : 0
            spacing: 4
            visible: mappingField.quickRows.length > 0

            Repeater {
                model: mappingField.quickRows

                delegate: CompactButton {
                    id: suggestionButton

                    required property var modelData

                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    implicitHeight: Style.smallControlHeight
                    text: mappingField.suggestionCaption(suggestionButton.modelData)

                    onClicked: mappingField.chooseSuggestion(suggestionButton.modelData.value)
                }
            }
        }
        Label {
            Layout.columnSpan: 2
            Layout.fillWidth: true
            Layout.leftMargin: 160
            color: Style.dimTextColor
            font.pixelSize: Style.smallFontSize
            text: mappingField.detail
            visible: text !== ""
        }
    }
    ChannelBrowserPopup {
        id: channelBrowser

        destinationLabel: mappingField.label
        expectedUnit: mappingField.expectedUnit
        folderScope: mappingField.folderScope
        rows: mappingField.autocompleteRows

        onChannelChosen: value => mappingField.chooseSuggestion(value)
    }
}
