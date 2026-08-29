pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Dialog {
    id: metadataDialog

    property var autoCompletedFields: []
    property bool channelOverridesExpanded: true
    property var channelRows: []
    property bool channelSampleLoading: false
    property var channelValues: ({})
    property var driverChannel: ({})
    property var driverRows: []
    property bool folderScope: false
    property var metadata: ({})
    property string targetPath: ""
    property var trackSlugMatches: []

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
        metadataDialog.channelOverridesExpanded = loaded.folderScope !== true || (loaded.directRecordingCount || 0) > 0;
        metadataDialog.channelSampleLoading = loaded.channelSampleLoading === true;
        let values = {};
        for (let index = 0; index < metadataDialog.channelRows.length; ++index)
            values[metadataDialog.channelRows[index].key] = metadataDialog.channelRows[index].value || "";
        if (metadataDialog.driverChannel.key)
            values[metadataDialog.driverChannel.key] = metadataDialog.driverChannel.value || "";
        metadataDialog.channelValues = values;
        metadataDialog.autoCompletedFields = metadataDialog.autoCompletionLabels(loaded);
        folderNameField.text = loaded.folderName || "";
        carNumberField.text = metadataDialog.completedValue(loaded, "carNumber", "inheritedCarNumber", "suggestedCarNumber");
        carClassField.text = metadataDialog.completedValue(loaded, "carClass", "inheritedCarClass", "suggestedCarClass");
        eventField.text = metadataDialog.completedValue(loaded, "event", "inheritedEvent", "suggestedEvent");
        seriesField.text = metadataDialog.completedValue(loaded, "series", "inheritedSeries", "suggestedSeries");
        trackNameField.text = metadataDialog.completedValue(loaded, "trackName", "inheritedTrackName", "suggestedTrackName");
        trackSlugField.text = metadataDialog.completedValue(loaded, "trackSlug", "inheritedTrackSlug", "suggestedTrackSlug");
        metadataDialog.open();
        if (loaded.folderScope === true && loaded.channelSampleComplete !== true) {
            metadataDialog.channelSampleLoading = true;
            Store.sampleFolderChannels(loaded.path);
        }
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
    function chooseTrackSlug(slug: string): void {
        trackSlugField.text = slug;
        metadataDialog.trackSlugMatches = [];
        trackSlugPopup.close();
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
    function refreshTrackSlugMatches(query: string): void {
        const needle = query.trim().toLowerCase();
        let rows = [];
        const atlasRows = Store.trackAtlasChoices();
        for (let i = 0; i < atlasRows.length; ++i) {
            const row = atlasRows[i];
            const haystack = String(row.search || (row.name + " " + row.slug)).toLowerCase();
            if (needle === "" || haystack.indexOf(needle) !== -1)
                rows.push(row);
        }
        metadataDialog.trackSlugMatches = rows;
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
            folderName: folderNameField.text,
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
    function updateChannelSample(loaded: var): void {
        metadataDialog.metadata = loaded;
        metadataDialog.channelRows = loaded.channels || [];
        metadataDialog.driverChannel = loaded.driverChannel || {};
        let values = Object.assign({}, metadataDialog.channelValues);
        for (let index = 0; index < metadataDialog.channelRows.length; ++index) {
            const row = metadataDialog.channelRows[index];
            if (values[row.key] === undefined)
                values[row.key] = row.value || "";
        }
        if (metadataDialog.driverChannel.key && values[metadataDialog.driverChannel.key] === undefined)
            values[metadataDialog.driverChannel.key] = metadataDialog.driverChannel.value || "";
        metadataDialog.channelValues = values;
        metadataDialog.channelSampleLoading = false;
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
                        text: metadataDialog.folderScope && folderNameField.text.trim() !== "" ? folderNameField.text.trim() : metadataDialog.metadata.fileName || ""
                    }
                    Label {
                        color: Style.mutedTextColor
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: (metadataDialog.metadata.sourceChannelCount || 0) + " source channels"
                        visible: !metadataDialog.folderScope && (metadataDialog.metadata.sourceChannelCount || 0) > 0
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

                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.preferredHeight: folderIdentityLayout.implicitHeight + 20
                    Layout.rightMargin: 4
                    Layout.topMargin: 10
                    border.color: Style.borderColor
                    border.width: 1
                    color: Style.surfaceColor
                    radius: 4
                    visible: metadataDialog.folderScope

                    RowLayout {
                        id: folderIdentityLayout

                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 10

                        ColumnLayout {
                            Layout.preferredWidth: 150
                            spacing: 1

                            Label {
                                Layout.fillWidth: true
                                color: Style.foregroundColor
                                font.bold: true
                                text: "Folder display name"
                            }
                            Label {
                                Layout.fillWidth: true
                                color: Style.dimTextColor
                                font.pixelSize: Style.smallFontSize
                                text: "Does not rename the folder on disk"
                            }
                        }
                        CompactTextField {
                            id: folderNameField

                            Layout.fillWidth: true
                            objectName: "folderNameField"
                            placeholderText: metadataDialog.metadata.fileName || "Folder name"
                            placeholderTextColor: Style.dimTextColor
                        }
                    }
                }
                DriverMetadataSection {
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    Layout.topMargin: metadataDialog.folderScope ? 0 : 10
                    channelMappingVisible: !metadataDialog.folderScope || metadataDialog.channelOverridesExpanded
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
                    Item {
                        Layout.fillWidth: true
                        implicitHeight: trackSlugField.implicitHeight

                        CompactTextField {
                            id: trackSlugField

                            anchors.fill: parent
                            placeholderText: metadataDialog.inheritedPlaceholder(metadataDialog.metadata.inheritedTrackSlug || "", "search tracks we know")
                            placeholderTextColor: metadataDialog.placeholderColor(metadataDialog.metadata.inheritedTrackSlug || "")

                            onActiveFocusChanged: {
                                if (activeFocus) {
                                    metadataDialog.refreshTrackSlugMatches(text);
                                    if (metadataDialog.trackSlugMatches.length > 0)
                                        trackSlugPopup.open();
                                }
                            }
                            onTextEdited: {
                                metadataDialog.refreshTrackSlugMatches(text);
                                if (metadataDialog.trackSlugMatches.length > 0)
                                    trackSlugPopup.open();
                                else
                                    trackSlugPopup.close();
                            }
                        }
                    }
                }
                ToolButton {
                    id: channelOverridesButton

                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.preferredHeight: 38
                    Layout.rightMargin: 4
                    checkable: true
                    checked: metadataDialog.channelOverridesExpanded
                    visible: metadataDialog.folderScope

                    background: Rectangle {
                        border.color: channelOverridesButton.hovered ? Style.accentColor : Style.borderColor
                        border.width: 1
                        color: channelOverridesButton.hovered ? Style.surfaceColor : Style.darkBackgroundColor
                        radius: 4
                    }
                    contentItem: RowLayout {
                        spacing: 8

                        Label {
                            color: Style.mutedTextColor
                            font.family: Style.monoFontFamily
                            text: channelOverridesButton.checked ? "▾" : "▸"
                        }
                        Label {
                            Layout.fillWidth: true
                            color: Style.foregroundColor
                            font.bold: true
                            text: "Channel overrides"
                        }
                        BusyIndicator {
                            Layout.preferredHeight: 18
                            Layout.preferredWidth: 18
                            running: metadataDialog.channelSampleLoading
                            visible: running
                        }
                        Label {
                            color: Style.mutedTextColor
                            font.family: Style.monoFontFamily
                            font.pixelSize: Style.smallFontSize
                            text: metadataDialog.channelSampleLoading ? "Sampling recordings…" : (metadataDialog.metadata.sourceChannelCount || 0) + " channels · sampled " + (metadataDialog.metadata.sourceRecordingCount || 0) + " of " + Math.min(8, metadataDialog.metadata.channelSampleCandidateCount || 0) + " files"
                        }
                    }

                    onToggled: metadataDialog.channelOverridesExpanded = checked
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.preferredHeight: 1
                    Layout.rightMargin: 4
                    color: Style.borderColor
                    visible: !metadataDialog.folderScope || metadataDialog.channelOverridesExpanded
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    spacing: 8
                    visible: !metadataDialog.folderScope || metadataDialog.channelOverridesExpanded

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
                    visible: !metadataDialog.folderScope || metadataDialog.channelOverridesExpanded
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                    spacing: 10
                    visible: !metadataDialog.folderScope || metadataDialog.channelOverridesExpanded

                    Repeater {
                        model: metadataDialog.channelRows.length

                        delegate: ChannelMappingField {
                            id: channelField

                            required property int index
                            readonly property var row: metadataDialog.channelRows[channelField.index] || ({})

                            automaticValue: channelField.row.automaticValue || ""
                            channelKey: channelField.row.key
                            detail: channelField.row.detail
                            expectedUnit: channelField.row.expectedUnit
                            folderScope: metadataDialog.folderScope
                            inheritedValue: channelField.row.inheritedValue || ""
                            label: channelField.row.label
                            suggestions: channelField.row.suggestions || []
                            value: metadataDialog.channelValues[channelField.row.key] || ""

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
                    visible: !metadataDialog.folderScope || metadataDialog.channelOverridesExpanded
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

    Popup {
        id: trackSlugPopup

        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        height: Math.min(220, 8 + metadataDialog.trackSlugMatches.length * 26)
        padding: 1
        parent: Overlay.overlay
        width: trackSlugField.width
        x: trackSlugField.mapToItem(Overlay.overlay, 0, 0).x
        y: trackSlugField.mapToItem(Overlay.overlay, 0, trackSlugField.height + 2).y

        background: Rectangle {
            border.color: Style.borderColor
            border.width: 1
            color: Style.darkBackgroundColor
            radius: 4
        }
        contentItem: ListView {
            clip: true
            model: metadataDialog.trackSlugMatches.length

            delegate: Rectangle {
                id: slugChoice

                readonly property var choice: metadataDialog.trackSlugMatches[slugChoice.index]
                required property int index

                color: slugChoiceMouse.containsMouse ? Style.selectionColor : "transparent"
                height: 26
                width: ListView.view.width

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    color: Style.foregroundColor
                    elide: Text.ElideRight
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    text: slugChoice.choice.name + "  ·  " + slugChoice.choice.slug
                    verticalAlignment: Text.AlignVCenter
                }
                MouseArea {
                    id: slugChoiceMouse

                    anchors.fill: parent
                    hoverEnabled: true

                    onClicked: metadataDialog.chooseTrackSlug(slugChoice.choice.slug)
                }
            }
        }
    }
    Connections {
        function onFolderChannelSampleReady(loaded: var): void {
            if (metadataDialog.folderScope && loaded.path === metadataDialog.targetPath)
                metadataDialog.updateChannelSample(loaded);
        }

        target: Store
    }
}
