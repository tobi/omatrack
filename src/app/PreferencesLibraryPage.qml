pragma ComponentBehavior: Bound
import Omatrack
import Qt.labs.platform as Platform

// The telemetry library: one ordered list of locations.
//
// A location is either a local folder or a connection to an outside server.
// Both kinds share the same row, the same enable switch, and the same status
// column, because from the driver's point of view they are the same thing —
// somewhere recordings come from.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: libraryPage

    property real cacheBytes: 0
    property string cacheLimitText: ""
    property string cacheText: ""
    property real cacheVideoBytes: 0
    property string cacheVideoText: ""
    property var locationRows: []

    function addDirectory(path): bool {
        const local = libraryPage.toLocalPath(path).trim();
        if (local === "" || !Store.directoryExists(local))
            return false;
        Store.addSessionDirectory(local);
        libraryPage.refresh();
        return true;
    }
    function refresh(): void {
        libraryPage.locationRows = Store.libraryLocations();
        // Measured by walking the cache, so this is only read when the page
        // is on screen or something changed — never on a timer.
        const usage = Store.cacheUsage();
        libraryPage.cacheBytes = usage.bytes;
        libraryPage.cacheText = usage.text;
        libraryPage.cacheLimitText = usage.limitText;
        libraryPage.cacheVideoBytes = usage.videoBytes;
        libraryPage.cacheVideoText = usage.videoText;
    }
    function toLocalPath(value): string {
        return Store.localPathFromUrl(value);
    }

    objectName: "preferencesLibraryPage"

    Component.onCompleted: libraryPage.refresh()
    onVisibleChanged: {
        if (visible)
            libraryPage.refresh();
    }

    Connections {
        function onLocationsChanged(): void {
            libraryPage.refresh();
        }
        function onSessionsChanged(): void {
            libraryPage.refresh();
        }

        target: Store
    }
    Platform.FolderDialog {
        id: telemetryFolderDialog

        acceptLabel: "Add folder"
        folder: Store.defaultTelemetryDirectoryUrl()
        title: "Choose telemetry folder"

        onAccepted: libraryPage.addDirectory(telemetryFolderDialog.folder)
    }
    ConnectionDialog {
        id: connectionDialog

        anchors.centerIn: Overlay.overlay
    }
    Menu {
        id: connectMenu

        Repeater {
            model: Store.connectionTypes()

            delegate: MenuItem {
                required property var modelData

                text: modelData.label

                onTriggered: connectionDialog.openForNew(modelData.type)
            }
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
                    text: "Locations are scanned recursively for supported telemetry and onboard video."
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
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            CompactButton {
                text: "Add folder…"

                onClicked: telemetryFolderDialog.open()
            }
            CompactButton {
                text: "Connect…"

                onClicked: connectMenu.popup(this, 0, height)
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                color: Style.mutedTextColor
                font.family: Style.monoFontFamily
                font.pixelSize: Style.smallFontSize
                text: libraryPage.locationRows.length + (libraryPage.locationRows.length === 1 ? " location" : " locations")
            }
        }
        Rectangle {
            Layout.fillHeight: true
            Layout.fillWidth: true
            border.color: Style.borderColor
            border.width: 1
            color: Style.traceBackgroundColor
            radius: 6

            Label {
                anchors.centerIn: parent
                color: Style.mutedTextColor
                horizontalAlignment: Text.AlignHCenter
                text: "No locations configured\nAdd a folder or connect to a server to build the session library."
                visible: libraryPage.locationRows.length === 0
            }
            ListView {
                id: locationList

                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: libraryPage.locationRows.length
                visible: libraryPage.locationRows.length > 0

                ScrollBar.vertical: ThinScrollBar {
                }
                delegate: Rectangle {
                    id: locationRow

                    required property int index
                    readonly property var modelData: libraryPage.locationRows[locationRow.index]

                    color: locationRow.index % 2 === 0 ? "transparent" : Qt.rgba(1, 1, 1, 0.025)
                    height: locationBody.implicitHeight + 16
                    width: ListView.view.width

                    MouseArea {
                        acceptedButtons: Qt.RightButton
                        anchors.fill: parent
                        z: 1

                        onClicked: rowMenu.popup()
                    }
                    RowLayout {
                        id: locationBody

                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.top: parent.top
                        anchors.topMargin: 8
                        spacing: 8

                        Switch {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredWidth: 42
                            checked: locationRow.modelData.enabled
                            scale: 0.7

                            onToggled: Store.setLocationEnabled(locationRow.modelData.id, checked)
                        }
                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredHeight: 16
                            Layout.preferredWidth: 46
                            border.color: Style.borderColor
                            border.width: 1
                            color: "transparent"
                            radius: 3

                            Label {
                                anchors.centerIn: parent
                                color: locationRow.modelData.isConnection ? Style.blueColor : Style.mutedTextColor
                                font.family: Style.monoFontFamily
                                font.pixelSize: 8
                                text: locationRow.modelData.isConnection ? locationRow.modelData.type.toUpperCase() : "FOLDER"
                            }
                        }
                        ColumnLayout {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.fillWidth: true
                            Layout.minimumWidth: 120
                            spacing: 0

                            Label {
                                Layout.fillWidth: true
                                color: locationRow.modelData.enabled ? Style.foregroundColor : Style.dimTextColor
                                elide: Text.ElideRight
                                text: locationRow.modelData.name
                            }
                            Label {
                                Layout.fillWidth: true
                                color: Style.mutedTextColor
                                elide: Text.ElideMiddle
                                font.family: Style.monoFontFamily
                                font.pixelSize: 10
                                text: locationRow.modelData.target
                            }
                        }
                        ColumnLayout {
                            Layout.alignment: Qt.AlignTop
                            Layout.fillWidth: false
                            Layout.maximumWidth: 248
                            Layout.minimumWidth: 248
                            Layout.preferredWidth: 248
                            spacing: 1

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 5

                                Rectangle {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.preferredHeight: 7
                                    Layout.preferredWidth: 7
                                    Layout.topMargin: 4
                                    color: !locationRow.modelData.enabled ? Style.dimTextColor : locationRow.modelData.available ? Style.greenColor : Style.redColor
                                    radius: 4
                                }
                                Label {
                                    Layout.fillWidth: true
                                    color: locationRow.modelData.enabled && !locationRow.modelData.available ? Style.redColor : Style.mutedTextColor
                                    font.pixelSize: Style.smallFontSize
                                    text: locationRow.modelData.status
                                    wrapMode: Text.Wrap
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.leftMargin: 12
                                color: Style.dimTextColor
                                font.family: Style.monoFontFamily
                                font.pixelSize: Style.smallFontSize
                                text: locationRow.modelData.fileCount >= 0 ? locationRow.modelData.fileCount + (locationRow.modelData.fileCount === 1 ? " file" : " files") : ""
                                visible: text !== ""
                            }
                        }
                        CompactButton {
                            Layout.alignment: Qt.AlignVCenter
                            text: "⋯"

                            onClicked: rowMenu.popup(this, 0, height)

                            Menu {
                                id: rowMenu

                                MenuItem {
                                    text: "Edit connection…"
                                    visible: locationRow.modelData.isConnection

                                    onTriggered: connectionDialog.openForEdit(locationRow.modelData)
                                }
                                MenuItem {
                                    enabled: !Store.loading && locationRow.modelData.enabled
                                    text: "Rescan server"
                                    visible: locationRow.modelData.isConnection

                                    onTriggered: Store.scan()
                                }
                                MenuItem {
                                    text: "Rename…"
                                    visible: !locationRow.modelData.isConnection

                                    onTriggered: renameDialog.openForLocation(locationRow.modelData)
                                }
                                MenuItem {
                                    text: locationRow.modelData.isConnection ? "Open cache folder" : "Open folder"

                                    onTriggered: Store.openContainingFolder(locationRow.modelData.isConnection ? locationRow.modelData.cachePath : locationRow.modelData.target)
                                }
                                MenuItem {
                                    text: "Copy address"

                                    onTriggered: Store.copyFilePath(locationRow.modelData.target)
                                }
                                MenuSeparator {
                                }
                                MenuItem {
                                    enabled: locationRow.index > 0
                                    text: "Move up"

                                    onTriggered: Store.moveLocation(locationRow.modelData.id, -1)
                                }
                                MenuItem {
                                    enabled: locationRow.index < libraryPage.locationRows.length - 1
                                    text: "Move down"

                                    onTriggered: Store.moveLocation(locationRow.modelData.id, 1)
                                }
                                MenuSeparator {
                                }
                                MenuItem {
                                    text: "Remove"

                                    onTriggered: removeDialog.openForLocation(locationRow.modelData)
                                }
                            }
                        }
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            // Only connections download anything, so with none configured
            // this row would report zero for something that cannot happen.
            visible: libraryPage.cacheBytes > 0 || libraryPage.cacheVideoBytes > 0

            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                font.pixelSize: Style.smallFontSize
                // Naming the limit is what makes the number mean something:
                // past it, the least recently opened files are dropped. It is
                // set with `cache: {limit: 20 GB}` in the configuration file.
                text: "Downloaded from servers: " + libraryPage.cacheText + " of " + libraryPage.cacheLimitText
            }
            CompactButton {
                text: "Clear cache"

                onClicked: clearCacheDialog.open()
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: Style.smallFontSize
            // Outside the limit on purpose. A recording is here because
            // somebody chose it for a flight, so nothing evicts it and it is
            // removed the way it arrived — from the file's context menu.
            text: "Recordings kept for offline use: " + libraryPage.cacheVideoText
            visible: libraryPage.cacheVideoBytes > 0
            wrapMode: Text.WordWrap
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
    Dialog {
        id: clearCacheDialog

        anchors.centerIn: Overlay.overlay
        closePolicy: Popup.CloseOnEscape
        modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        title: "Clear downloaded files"
        width: 420

        onAccepted: {
            Store.clearCache();
            libraryPage.refresh();
        }

        Label {
            anchors.fill: parent
            // Worth saying plainly: this costs time and bandwidth, and on a
            // metered connection it costs money.
            text: "Delete " + libraryPage.cacheText + " downloaded from connected servers, and any recordings kept for offline use. Every enabled connection will download its files again on the next scan."
            wrapMode: Text.WordWrap
        }
    }
    Dialog {
        id: removeDialog

        property string locationId: ""
        property string locationName: ""

        function openForLocation(row): void {
            removeDialog.locationId = row.id;
            removeDialog.locationName = row.name;
            removeDialog.open();
        }

        anchors.centerIn: Overlay.overlay
        closePolicy: Popup.CloseOnEscape
        modal: true
        standardButtons: Dialog.Cancel | Dialog.Ok
        title: "Remove location"
        width: 360

        onAccepted: Store.removeLocation(removeDialog.locationId)

        Label {
            anchors.fill: parent
            text: "Remove “" + removeDialog.locationName + "” from the library? The files themselves are not deleted."
            wrapMode: Text.WordWrap
        }
    }
    Dialog {
        id: renameDialog

        property string locationId: ""

        function openForLocation(row): void {
            renameDialog.locationId = row.id;
            renameField.text = row.name;
            renameDialog.open();
        }

        anchors.centerIn: Overlay.overlay
        closePolicy: Popup.CloseOnEscape
        modal: true
        standardButtons: Dialog.Save | Dialog.Cancel
        title: "Rename location"
        width: 360

        contentItem: CompactTextField {
            id: renameField

            placeholderText: "Folder name"

            onAccepted: renameDialog.accept()
        }

        onAccepted: Store.setLocationName(renameDialog.locationId, renameField.text)
    }
}
