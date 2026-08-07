pragma ComponentBehavior: Bound
import Omatrack
import Qt.labs.platform as Platform

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: libraryPage

    property var directoryRows: []

    function addDirectory(path): bool {
        const local = libraryPage.toLocalPath(path).trim();
        if (local === "" || !Store.directoryExists(local))
            return false;
        Store.addSessionDirectory(local);
        libraryPage.refresh();
        return true;
    }
    function defaultTelemetryFolder(): url {
        const home = libraryPage.toLocalPath(Platform.StandardPaths.writableLocation(Platform.StandardPaths.HomeLocation));
        const preferred = home + "/Documents/Telemetry";
        return "file://" + (Store.directoryExists(preferred) ? preferred : home);
    }
    function refresh(): void {
        libraryPage.directoryRows = Store.sessionDirectories();
    }
    function toLocalPath(value): string {
        const text = value.toString();
        return text.startsWith("file://") ? decodeURIComponent(text.substring(7)) : text;
    }

    Component.onCompleted: libraryPage.refresh()
    onVisibleChanged: {
        if (visible)
            libraryPage.refresh();
    }

    Connections {
        function onSessionsChanged(): void {
            libraryPage.refresh();
        }

        target: Store
    }
    Platform.FolderDialog {
        id: telemetryFolderDialog

        acceptLabel: "Add folder"
        folder: libraryPage.defaultTelemetryFolder()
        title: "Choose telemetry folder"

        onAccepted: {
            if (libraryPage.addDirectory(telemetryFolderDialog.folder))
                directoryField.text = "";
        }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    font.bold: true
                    font.pixelSize: 16
                    text: "Telemetry library"
                }
                Label {
                    color: Style.mutedTextColor
                    text: "Folders are scanned recursively for supported telemetry and onboard video."
                }
            }
            BusyIndicator {
                Layout.preferredHeight: 22
                Layout.preferredWidth: 22
                running: Store.loading
                visible: running
            }
            CompactButton {
                enabled: !Store.loading
                text: Store.loading ? "Scanning…" : "Rescan now"

                onClicked: Store.scan()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 104
            border.color: Style.borderColor
            border.width: 1
            color: Style.surfaceColor
            radius: 6

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                Label {
                    font.bold: true
                    text: "Add a telemetry folder"
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    CompactTextField {
                        id: directoryField

                        Layout.fillWidth: true
                        placeholderText: "/path/to/telemetry"

                        onAccepted: {
                            if (libraryPage.addDirectory(text))
                                text = "";
                        }
                    }
                    CompactButton {
                        text: "Browse…"

                        onClicked: telemetryFolderDialog.open()
                    }
                    CompactButton {
                        enabled: directoryField.text.trim() !== "" && Store.directoryExists(libraryPage.toLocalPath(directoryField.text))
                        text: "Add folder"

                        onClicked: {
                            if (libraryPage.addDirectory(directoryField.text))
                                directoryField.text = "";
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    color: Style.redColor
                    font.pixelSize: Style.smallFontSize
                    text: "Choose an existing folder."
                    visible: directoryField.text.trim() !== "" && !Store.directoryExists(libraryPage.toLocalPath(directoryField.text))
                }
            }
        }
        Rectangle {
            Layout.fillHeight: true
            Layout.fillWidth: true
            border.color: Style.borderColor
            border.width: 1
            color: Style.traceBackgroundColor
            radius: 6

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    color: Style.surfaceColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 10

                        Label {
                            Layout.fillWidth: true
                            font.bold: true
                            text: "Folders"
                        }
                        Label {
                            color: Style.mutedTextColor
                            font.family: Style.monoFontFamily
                            font.pixelSize: Style.smallFontSize
                            text: libraryPage.directoryRows.length + (libraryPage.directoryRows.length === 1 ? " source" : " sources")
                        }
                    }
                }
                Item {
                    Layout.fillHeight: true
                    Layout.fillWidth: true

                    Label {
                        anchors.centerIn: parent
                        color: Style.mutedTextColor
                        horizontalAlignment: Text.AlignHCenter
                        text: "No telemetry folders configured\nAdd a folder above to build the session library."
                        visible: libraryPage.directoryRows.length === 0
                    }
                    ListView {
                        id: directoryList

                        anchors.fill: parent
                        clip: true
                        model: libraryPage.directoryRows
                        visible: libraryPage.directoryRows.length > 0

                        ScrollBar.vertical: ThinScrollBar {
                        }
                        delegate: Rectangle {
                            id: directoryRow

                            required property int index
                            required property var modelData

                            color: directoryRow.index % 2 === 0 ? "transparent" : Qt.rgba(1, 1, 1, 0.025)
                            height: 50
                            width: ListView.view.width

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 8
                                spacing: 8

                                Rectangle {
                                    Layout.preferredHeight: 8
                                    Layout.preferredWidth: 8
                                    color: Store.directoryExists(directoryRow.modelData) ? Style.greenColor : Style.redColor
                                    radius: 4
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    Label {
                                        Layout.fillWidth: true
                                        color: Store.directoryExists(directoryRow.modelData) ? Style.foregroundColor : Style.redColor
                                        elide: Text.ElideMiddle
                                        font.family: Style.monoFontFamily
                                        font.pixelSize: 10
                                        text: directoryRow.modelData
                                    }
                                    Label {
                                        color: Style.mutedTextColor
                                        font.pixelSize: Style.smallFontSize
                                        text: Store.directoryExists(directoryRow.modelData) ? "Available" : "Folder not found"
                                    }
                                }
                                CompactButton {
                                    text: "Remove"

                                    onClicked: Store.removeSessionDirectory(directoryRow.modelData)
                                }
                            }
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.dimTextColor
            elide: Text.ElideMiddle
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: "Configuration: " + Store.configFilePath()
        }
    }
}
