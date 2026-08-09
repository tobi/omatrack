pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Dialog {
    id: metadataDialog

    property var autoCompletedFields: []
    property var channelRows: []
    property var channelValues: ({})
    property var driverChannel: ({})
    property var driverRows: []
    property bool folderScope: false
    property var metadata: ({})
    property string targetPath: ""

    function addDriverMapping(): void {
        const rows = metadataDialog.driverRows.slice();
        rows.push({
            id: "",
            value: "",
            inheritedValue: "",
            detected: false,
            suggestions: []
        });
        metadataDialog.driverRows = rows;
    }
    function applyMetadata(loaded: var): void {
        if (!loaded.path)
            return;
        metadataDialog.metadata = loaded;
        metadataDialog.folderScope = loaded.folderScope === true;
        metadataDialog.targetPath = loaded.path;
        metadataDialog.channelRows = loaded.channels || [];
        metadataDialog.driverChannel = loaded.driverChannel || {};
        metadataDialog.driverRows = loaded.driverMappings || [];
        let values = {};
        for (let index = 0; index < metadataDialog.channelRows.length; ++index)
            values[metadataDialog.channelRows[index].key] = metadataDialog.channelRows[index].value || "";
        if (metadataDialog.driverChannel.key)
            values[metadataDialog.driverChannel.key] = metadataDialog.driverChannel.value || "";
        metadataDialog.channelValues = values;
        metadataDialog.autoCompletedFields = metadataDialog.autoCompletionLabels(loaded);
        carNumberField.text = metadataDialog.completedValue(loaded, "carNumber", "inheritedCarNumber", "suggestedCarNumber");
        carClassField.text = metadataDialog.completedValue(loaded, "carClass", "inheritedCarClass", "suggestedCarClass");
        eventField.text = metadataDialog.completedValue(loaded, "event", "inheritedEvent", "suggestedEvent");
        seriesField.text = metadataDialog.completedValue(loaded, "series", "inheritedSeries", "suggestedSeries");
        trackNameField.text = metadataDialog.completedValue(loaded, "trackName", "inheritedTrackName", "suggestedTrackName");
        trackSlugField.text = metadataDialog.completedValue(loaded, "trackSlug", "inheritedTrackSlug", "suggestedTrackSlug");
        metadataDialog.open();
    }
    function autoCompletionLabels(loaded: var): var {
        if (loaded.folderScope !== true)
            return [];
        const fields = [
            {
                label: "car number",
                value: "carNumber",
                inherited: "inheritedCarNumber",
                suggested: "suggestedCarNumber"
            },
            {
                label: "class",
                value: "carClass",
                inherited: "inheritedCarClass",
                suggested: "suggestedCarClass"
            },
            {
                label: "event",
                value: "event",
                inherited: "inheritedEvent",
                suggested: "suggestedEvent"
            },
            {
                label: "series",
                value: "series",
                inherited: "inheritedSeries",
                suggested: "suggestedSeries"
            },
            {
                label: "track",
                value: "trackName",
                inherited: "inheritedTrackName",
                suggested: "suggestedTrackName"
            },
            {
                label: "Atlas slug",
                value: "trackSlug",
                inherited: "inheritedTrackSlug",
                suggested: "suggestedTrackSlug"
            }
        ];
        let result = [];
        for (let index = 0; index < fields.length; ++index) {
            const field = fields[index];
            if (!(loaded[field.value] || "") && !(loaded[field.inherited] || "") && (loaded[field.suggested] || ""))
                result.push(field.label);
        }
        return result;
    }
    function completedValue(loaded: var, valueKey: string, inheritedKey: string, suggestedKey: string): string {
        const value = loaded[valueKey] || "";
        if (value !== "")
            return value;
        if (loaded.folderScope === true && !(loaded[inheritedKey] || ""))
            return loaded[suggestedKey] || "";
        return "";
    }
    function driverMappingPayload(): var {
        let mappings = {};
        for (let index = 0; index < metadataDialog.driverRows.length; ++index) {
            const row = metadataDialog.driverRows[index];
            const id = (row.id || "").trim();
            const name = (row.value || "").trim();
            if (id !== "" && name !== "")
                mappings[id] = name;
        }
        return mappings;
    }
    function inheritedPlaceholder(inheritedValue: string, fallback: string): string {
        return inheritedValue !== "" ? inheritedValue : fallback;
    }
    function openForFolder(path: string): void {
        metadataDialog.applyMetadata(Store.folderMetadata(path));
    }
    function openForVideo(path: string): void {
        metadataDialog.applyMetadata(Store.videoMetadata(path));
    }
    function placeholderColor(inheritedValue: string): color {
        return inheritedValue !== "" ? Style.blueColor : Style.dimTextColor;
    }
    function removeDriverMapping(index: int): void {
        const rows = metadataDialog.driverRows.slice();
        if (rows[index].inheritedValue) {
            const row = Object.assign({}, rows[index]);
            row.value = "";
            rows[index] = row;
        } else {
            rows.splice(index, 1);
        }
        metadataDialog.driverRows = rows;
    }
    function savePayload(): var {
        return {
            driverMappings: metadataDialog.driverMappingPayload(),
            carNumber: carNumberField.text,
            carClass: carClassField.text,
            event: eventField.text,
            series: seriesField.text,
            trackName: trackNameField.text,
            trackSlug: trackSlugField.text,
            channels: metadataDialog.channelValues
        };
    }
    function setChannelMapping(key: string, value: string): void {
        const values = Object.assign({}, metadataDialog.channelValues);
        values[key] = value;
        metadataDialog.channelValues = values;
    }
    function setDriverMapping(index: int, driverId: string, name: string): void {
        const rows = metadataDialog.driverRows.slice();
        const row = Object.assign({}, rows[index]);
        row.id = driverId;
        row.value = name;
        rows[index] = row;
        metadataDialog.driverRows = rows;
    }

    closePolicy: Popup.CloseOnEscape
    focus: true
    modal: true
    objectName: "videoMetadataDialog"
    standardButtons: Dialog.Save | Dialog.Cancel
    title: metadataDialog.folderScope ? "Folder metadata" : "Video metadata"

    background: Rectangle {
        border.color: Style.borderColor
        border.width: 1
        color: Style.backgroundColor
        radius: 6
    }
    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: metadataHeader.implicitHeight + 20
            border.color: Style.borderColor
            border.width: 1
            color: Style.darkBackgroundColor

            ColumnLayout {
                id: metadataHeader

                anchors.fill: parent
                anchors.margins: 10
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        color: Style.accentColor
                        font.bold: true
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: metadataDialog.folderScope ? "FOLDER" : "VIDEO"
                    }
                    Label {
                        Layout.fillWidth: true
                        color: Style.foregroundColor
                        elide: Text.ElideMiddle
                        font.bold: true
                        text: metadataDialog.metadata.fileName || ""
                    }
                    Label {
                        color: Style.mutedTextColor
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: metadataDialog.folderScope ? (metadataDialog.metadata.sourceChannelCount || 0) + " channels across " + (metadataDialog.metadata.sourceRecordingCount || 0) + " videos" : (metadataDialog.metadata.sourceChannelCount || 0) + " source channels"
                        visible: !metadataDialog.folderScope || (metadataDialog.metadata.sourceChannelCount || 0) > 0
                    }
                }
                Label {
                    Layout.fillWidth: true
                    color: Style.dimTextColor
                    elide: Text.ElideMiddle
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    text: metadataDialog.folderScope ? (metadataDialog.metadata.sidecarPath || "") : metadataDialog.targetPath
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: inheritanceMessage.implicitHeight + 16
            border.color: Style.blueColor
            border.width: 1
            color: Qt.rgba(Style.blueColor.r, Style.blueColor.g, Style.blueColor.b, 0.09)

            Label {
                id: inheritanceMessage

                anchors.fill: parent
                anchors.margins: 8
                color: Style.blueColor
                font.pixelSize: Style.smallFontSize
                text: metadataDialog.folderScope ? "Values entered here are saved to this folder's TRACK.yml and inherited by every video and subfolder below it. A closer TRACK.yml or individual video override wins." : "Blue placeholder values are inherited from the TRACK.yml files above this video. Enter a value only when this video should override them."
                wrapMode: Text.WordWrap
            }
        }
        ScrollView {
            Layout.fillHeight: true
            Layout.fillWidth: true
            clip: true
            contentWidth: availableWidth

            ColumnLayout {
                spacing: 10
                width: parent.width

                DriverMetadataSection {
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    Layout.topMargin: 10
                    channelRow: metadataDialog.driverChannel
                    folderScope: metadataDialog.folderScope
                    mappingRows: metadataDialog.driverRows

                    onChannelEdited: (key, value) => metadataDialog.setChannelMapping(key, value)
                    onMappingAdded: metadataDialog.addDriverMapping()
                    onMappingEdited: (index, driverId, name) => metadataDialog.setDriverMapping(index, driverId, name)
                    onMappingRemoved: index => metadataDialog.removeDriverMapping(index)
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.preferredHeight: 1
                    Layout.rightMargin: 4
                    color: Style.borderColor
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    spacing: 1

                    Label {
                        Layout.fillWidth: true
                        color: Style.mutedTextColor
                        font.bold: true
                        text: "Recording identity"
                    }
                    Label {
                        Layout.fillWidth: true
                        color: Style.greenColor
                        font.pixelSize: Style.smallFontSize
                        text: "Completed " + metadataDialog.autoCompletedFields.join(", ") + " after inspecting " + (metadataDialog.metadata.metadataSourceCount || 0) + ((metadataDialog.metadata.metadataSourceCount || 0) === 1 ? " descendant recording. Review before saving." : " descendant recordings. Review before saving.")
                        visible: metadataDialog.autoCompletedFields.length > 0
                        wrapMode: Text.WordWrap
                    }
                }
                GridLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    columnSpacing: 8
                    columns: 4
                    rowSpacing: 6

                    Label {
                        Layout.preferredWidth: 70
                        color: Style.mutedTextColor
                        horizontalAlignment: Text.AlignRight
                        text: "Car number"
                    }
                    CompactTextField {
                        id: carNumberField

                        Layout.fillWidth: true
                        placeholderText: metadataDialog.inheritedPlaceholder(metadataDialog.metadata.inheritedCarNumber || "", "Number")
                        placeholderTextColor: metadataDialog.placeholderColor(metadataDialog.metadata.inheritedCarNumber || "")
                    }
                    Label {
                        Layout.preferredWidth: 70
                        color: Style.mutedTextColor
                        horizontalAlignment: Text.AlignRight
                        text: "Class"
                    }
                    CompactTextField {
                        id: carClassField

                        Layout.fillWidth: true
                        placeholderText: metadataDialog.inheritedPlaceholder(metadataDialog.metadata.inheritedCarClass || "", "Car class")
                        placeholderTextColor: metadataDialog.placeholderColor(metadataDialog.metadata.inheritedCarClass || "")
                    }
                    Label {
                        Layout.preferredWidth: 70
                        color: Style.mutedTextColor
                        horizontalAlignment: Text.AlignRight
                        text: "Event"
                    }
                    CompactTextField {
                        id: eventField

                        Layout.fillWidth: true
                        placeholderText: metadataDialog.inheritedPlaceholder(metadataDialog.metadata.inheritedEvent || "", "Event name")
                        placeholderTextColor: metadataDialog.placeholderColor(metadataDialog.metadata.inheritedEvent || "")
                    }
                    Label {
                        Layout.preferredWidth: 70
                        color: Style.mutedTextColor
                        horizontalAlignment: Text.AlignRight
                        text: "Series"
                    }
                    CompactTextField {
                        id: seriesField

                        Layout.fillWidth: true
                        placeholderText: metadataDialog.inheritedPlaceholder(metadataDialog.metadata.inheritedSeries || "", "Series")
                        placeholderTextColor: metadataDialog.placeholderColor(metadataDialog.metadata.inheritedSeries || "")
                    }
                    Label {
                        Layout.preferredWidth: 70
                        color: Style.mutedTextColor
                        horizontalAlignment: Text.AlignRight
                        text: "Track"
                    }
                    CompactTextField {
                        id: trackNameField

                        Layout.fillWidth: true
                        placeholderText: metadataDialog.inheritedPlaceholder(metadataDialog.metadata.inheritedTrackName || "", "Track name")
                        placeholderTextColor: metadataDialog.placeholderColor(metadataDialog.metadata.inheritedTrackName || "")
                    }
                    Label {
                        Layout.preferredWidth: 70
                        color: Style.mutedTextColor
                        horizontalAlignment: Text.AlignRight
                        text: "Atlas slug"
                    }
                    CompactTextField {
                        id: trackSlugField

                        Layout.fillWidth: true
                        placeholderText: metadataDialog.inheritedPlaceholder(metadataDialog.metadata.inheritedTrackSlug || "", "track-atlas-slug")
                        placeholderTextColor: metadataDialog.placeholderColor(metadataDialog.metadata.inheritedTrackSlug || "")
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.preferredHeight: 1
                    Layout.rightMargin: 4
                    color: Style.borderColor
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    spacing: 8

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1

                        Label {
                            Layout.fillWidth: true
                            color: Style.mutedTextColor
                            font.bold: true
                            text: "Telemetry channel mappings"
                        }
                        Label {
                            Layout.fillWidth: true
                            color: Style.dimTextColor
                            font.pixelSize: Style.smallFontSize
                            text: metadataDialog.folderScope ? "Mappings entered here become defaults for every video below this folder. Empty fields continue inheriting from parent TRACK.yml files." : "Type to search this video's channels, or click a suggestion learned from TRACK.yml files. Empty fields inherit first, then use automatic matching."
                            wrapMode: Text.WordWrap
                        }
                    }
                    Label {
                        color: Style.mutedTextColor
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: (metadataDialog.metadata.trackFileCount || 0) + (metadataDialog.metadata.trackFileCount === 1 ? " TRACK.yml" : " TRACK.yml files")
                    }
                }
                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    color: Style.dimTextColor
                    elide: Text.ElideMiddle
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    text: metadataDialog.folderScope ? (metadataDialog.metadata.inheritedSidecarPath ? "Nearest inherited file: " + metadataDialog.metadata.inheritedSidecarPath : "No parent TRACK.yml; this folder starts a new metadata scope") : (metadataDialog.metadata.sidecarPath ? "Nearest inherited file: " + metadataDialog.metadata.sidecarPath : "No inherited TRACK.yml in this folder tree")
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    spacing: 10

                    Repeater {
                        model: metadataDialog.channelRows

                        delegate: ChannelMappingField {
                            id: channelField

                            required property int index
                            required property var modelData

                            automaticValue: channelField.modelData.automaticValue || ""
                            channelKey: channelField.modelData.key
                            detail: channelField.modelData.detail
                            expectedUnit: channelField.modelData.expectedUnit
                            inheritedValue: channelField.modelData.inheritedValue || ""
                            label: channelField.modelData.label
                            suggestions: channelField.modelData.suggestions || []
                            value: metadataDialog.channelValues[channelField.modelData.key] || ""

                            onMappingEdited: (key, value) => metadataDialog.setChannelMapping(key, value)
                        }
                    }
                }
                Label {
                    Layout.bottomMargin: 10
                    Layout.fillWidth: true
                    Layout.leftMargin: 164
                    Layout.rightMargin: 4
                    color: Style.dimTextColor
                    font.pixelSize: Style.smallFontSize
                    text: metadataDialog.folderScope ? "This folder's values are written atomically to TRACK.yml. Existing files and unrelated keys in that document are preserved." : "Video overrides are stored in omatrack.yml. Source videos and inherited TRACK.yml files remain unchanged."
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    onAccepted: {
        if (metadataDialog.folderScope)
            Store.saveFolderMetadata(metadataDialog.targetPath, metadataDialog.savePayload());
        else
            Store.saveVideoMetadata(metadataDialog.targetPath, metadataDialog.savePayload());
    }
}
