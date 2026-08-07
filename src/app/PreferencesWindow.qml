pragma ComponentBehavior: Bound
import Omatrack
import Qt.labs.platform as Platform

// Preferences window: driver mappings, telemetry directories, and Track Atlas
// controls.
//
// Owns its alias/mapping/directory row caches and refreshes them from the
// store. A rename that needs the root-owned dialog is requested via
// driverRenameRequested.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: preferencesWindow

    // Owned row caches.
    property var aliasRows: []
    property var directoryRows: []

    // Internal edit state for the inline driver-mapping rename.
    property string mappingEditKey: ""
    property string mappingEditName: ""
    property var mappingRows: []

    // Emitted to open the root-owned driver rename dialog.
    signal driverRenameRequested(string mappingKey, string displayName)

    function addDirectory(path): void {
        const local = preferencesWindow.toLocalPath(path);
        if (local === "")
            return;
        Store.addSessionDirectory(local);
    }
    function defaultTelemetryFolder(): url {
        const home = preferencesWindow.toLocalPath(Platform.StandardPaths.writableLocation(Platform.StandardPaths.HomeLocation));
        const preferred = home + "/Documents/Telemetry";
        return "file://" + (Store.directoryExists(preferred) ? preferred : home);
    }
    function refresh(): void {
        preferencesWindow.aliasRows = Store.driverAliases();
        preferencesWindow.mappingRows = Store.driverMappings();
        preferencesWindow.directoryRows = Store.sessionDirectories();
    }
    function toLocalPath(value): string {
        const text = value.toString();
        return text.startsWith("file://") ? decodeURIComponent(text.substring(7)) : text;
    }

    Material.accent: Style.accentColor
    Material.background: Style.backgroundColor
    Material.foreground: Style.foregroundColor
    Material.primary: Style.surfaceColor
    Material.theme: Material.Dark
    color: Style.backgroundColor
    font.family: Style.uiFontFamily
    font.pixelSize: 11
    height: 680
    minimumHeight: 560
    minimumWidth: 700
    objectName: "settingsWindow"
    title: "Omatrack Preferences"
    visible: false
    width: 820

    Component.onCompleted: preferencesWindow.refresh()

    Connections {
        function onDriverMappingsChanged(): void {
            preferencesWindow.aliasRows = Store.driverAliases();
            preferencesWindow.mappingRows = Store.driverMappings();
        }
        function onSessionsChanged(): void {
            preferencesWindow.directoryRows = Store.sessionDirectories();
        }

        target: Store
    }
    Platform.FolderDialog {
        id: settingsFolderDialog

        acceptLabel: "Add"
        folder: preferencesWindow.defaultTelemetryFolder()
        title: "Choose telemetry directory"

        onAccepted: {
            preferencesWindow.addDirectory(settingsFolderDialog.folder);
            preferencesWindow.refresh();
        }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 6

        Label {
            font.bold: true
            font.pixelSize: 16
            text: "Preferences"
        }
        RowLayout {
            Layout.fillWidth: true

            Label {
                color: Style.accentColor
                font.bold: true
                text: "Track atlas"
            }
            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                elide: Text.ElideRight
                font.family: Style.monoFontFamily
                font.pixelSize: 9
                text: Store.trackAtlasStatus
            }
            CompactButton {
                text: "Update now"

                onClicked: Store.refreshTrackAtlas()
            }
        }
        RowLayout {
            Layout.fillWidth: true

            Label {
                color: Style.accentColor
                font.bold: true
                text: "Telemetry directories"
            }
            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                elide: Text.ElideMiddle
                font.family: Style.monoFontFamily
                font.pixelSize: 9
                text: "stored in " + Store.configFilePath()
            }
            BusyIndicator {
                Layout.preferredHeight: 22
                Layout.preferredWidth: 22
                running: Store.loading
                visible: running
            }
            CompactButton {
                enabled: !Store.loading
                text: "Rescan"

                onClicked: Store.scan()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            CompactTextField {
                id: settingsDir

                Layout.fillWidth: true
                placeholderText: "/path/to/telemetry"

                onAccepted: settingsAddDir.clicked()
            }
            CompactButton {
                text: "Browse…"

                onClicked: settingsFolderDialog.open()
            }
            CompactButton {
                id: settingsAddDir

                enabled: settingsDir.text !== ""
                text: "Add"

                onClicked: {
                    preferencesWindow.addDirectory(settingsDir.text);
                    settingsDir.text = "";
                    preferencesWindow.refresh();
                }
            }
        }
        ListView {
            id: settingsDirectories

            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(160, Math.max(40, preferencesWindow.directoryRows.length * 38))
            clip: true
            model: preferencesWindow.directoryRows

            delegate: RowLayout {
                id: dirRow

                required property var modelData

                height: 38
                spacing: 6
                width: settingsDirectories.width

                Label {
                    Layout.fillWidth: true
                    color: Store.directoryExists(dirRow.modelData) ? Style.foregroundColor : Style.redColor
                    elide: Text.ElideMiddle
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: dirRow.modelData
                }
                Label {
                    color: Style.redColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: 9
                    text: "MISSING"
                    visible: !Store.directoryExists(dirRow.modelData)
                }
                CompactButton {
                    Layout.preferredHeight: 30
                    text: "Remove"

                    onClicked: {
                        Store.removeSessionDirectory(dirRow.modelData);
                        preferencesWindow.refresh();
                    }
                }
            }
        }
        Label {
            color: Style.accentColor
            font.bold: true
            text: "Driver mappings"
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: 9
            text: "Names apply to the same car number, class, and driver ID across sessions."
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            CompactTextField {
                id: mappingNameField

                Layout.fillWidth: true
                placeholderText: "Select a mapping or edit a driver name"
                text: preferencesWindow.mappingEditName

                onTextChanged: {
                    if (activeFocus)
                        preferencesWindow.mappingEditName = text;
                }
            }
            CompactButton {
                enabled: preferencesWindow.mappingEditKey !== "" && mappingNameField.text.trim() !== ""
                text: "Save"

                onClicked: {
                    Store.setDriverMapping(preferencesWindow.mappingEditKey, mappingNameField.text);
                    preferencesWindow.mappingEditKey = "";
                    preferencesWindow.mappingEditName = "";
                    preferencesWindow.refresh();
                }
            }
        }
        ListView {
            id: mappingListView

            Layout.fillWidth: true
            Layout.preferredHeight: 250
            clip: true
            model: preferencesWindow.mappingRows

            delegate: RowLayout {
                id: mappingRow

                required property var modelData

                height: 38
                spacing: 6
                width: mappingListView.width

                Label {
                    Layout.preferredWidth: 92
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 9
                    text: "Car " + mappingRow.modelData.carNumber
                }
                Label {
                    Layout.preferredWidth: 88
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 9
                    text: mappingRow.modelData.carClass || "Unknown class"
                }
                Label {
                    Layout.preferredWidth: 72
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 9
                    text: "ID " + (mappingRow.modelData.driverId || "—")
                }
                Label {
                    Layout.fillWidth: true
                    color: Style.accentColor
                    elide: Text.ElideRight
                    text: mappingRow.modelData.display
                    visible: preferencesWindow.mappingEditKey !== mappingRow.modelData.key
                }
                CompactTextField {
                    Layout.fillWidth: true
                    text: preferencesWindow.mappingEditName
                    visible: preferencesWindow.mappingEditKey === mappingRow.modelData.key

                    onTextChanged: {
                        if (activeFocus)
                            preferencesWindow.mappingEditName = text;
                    }
                }
                ToolButton {
                    text: preferencesWindow.mappingEditKey === mappingRow.modelData.key ? "✓" : "✎"

                    onClicked: {
                        if (preferencesWindow.mappingEditKey === mappingRow.modelData.key) {
                            Store.setDriverMapping(mappingRow.modelData.key, preferencesWindow.mappingEditName);
                            preferencesWindow.mappingEditKey = "";
                            preferencesWindow.mappingEditName = "";
                            preferencesWindow.refresh();
                        } else {
                            preferencesWindow.mappingEditKey = mappingRow.modelData.key;
                            preferencesWindow.mappingEditName = mappingRow.modelData.display;
                        }
                    }
                }
                ToolButton {
                    text: "×"

                    onClicked: {
                        Store.setDriverMapping(mappingRow.modelData.key, "");
                        preferencesWindow.refresh();
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true

            Item {
                Layout.fillWidth: true
            }
            CompactButton {
                text: "Close"

                onClicked: preferencesWindow.hide()
            }
        }
    }
}
