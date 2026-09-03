pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: browser

    signal driverRenameRequested(string mappingKey, string driver)
    signal fileActivated(string path, string key, bool hasSession)
    signal fileIsolated(string key)
    signal folderMetadataRequested(string path)
    signal setActiveRequested(string key)
    signal setReferenceRequested(string key)
    signal trackAssignmentRequested(string key)
    signal videoMetadataRequested(string path)

    function clearFilters(): void {
        fileFilter.clear();
        filterModel.clearAllFilters();
        // The event filter is a facet preset: clearing the sidebar leaves it.
        // Event track/date/session stay configured for USB naming.
        if (Store.eventMode)
            Store.eventMode = false;
    }
    function toggleDriver(name: string): void {
        const current = filterModel.selectedDrivers;
        const index = current.indexOf(name);
        if (index >= 0)
            filterModel.selectedDrivers = current.filter(item => item !== name);
        else
            filterModel.selectedDrivers = current.concat([name]);
    }
    function toggleYear(name: string): void {
        const current = filterModel.selectedYears;
        const index = current.indexOf(name);
        if (index >= 0)
            filterModel.selectedYears = current.filter(item => item !== name);
        else
            filterModel.selectedYears = current.concat([name]);
    }

    padding: 0

    background: Rectangle {
        color: Style.darkBackgroundColor
    }

    Component.onCompleted: filterModel.setEventFilter(Store.eventMode, Store.eventTrack, Store.eventDate)

    LibraryFilterModel {
        id: filterModel

        sourceModel: Store.library
    }
    Connections {
        function onEventChanged(): void {
            filterModel.setEventFilter(Store.eventMode, Store.eventTrack, Store.eventDate);
        }
        function onSelectionChanged(): void {
            filterModel.revealSession(Store.primarySessionKey);
        }

        target: Store
    }
    Timer {
        id: filterTimer

        interval: 120
        repeat: false

        onTriggered: filterModel.filterText = fileFilter.text.trim().toLowerCase()
    }
    Menu {
        id: fileContextMenu

        property bool ctxDownloading
        property string ctxDriver
        property bool ctxHasSession
        property string ctxKey
        property string ctxMappingKey
        property bool ctxOffline
        property string ctxPath
        property bool ctxPinned
        /// Whether this recording streams from a server, whether it is being
        /// kept on this machine, and whether that is happening right now.
        property bool ctxRemoteVideo
        property string ctxRole
        property bool ctxVideoFile

        MenuItem {
            text: "Open"

            onTriggered: browser.fileActivated(fileContextMenu.ctxPath, fileContextMenu.ctxKey, fileContextMenu.ctxHasSession)
        }
        MenuItem {
            height: visible ? implicitHeight : 0
            text: "Edit video metadata…"
            visible: fileContextMenu.ctxVideoFile

            onTriggered: browser.videoMetadataRequested(fileContextMenu.ctxPath)
        }
        MenuItem {
            height: visible ? implicitHeight : 0
            text: fileContextMenu.ctxPinned ? "Unpin from top" : "Pin to top"
            visible: fileContextMenu.ctxVideoFile

            onTriggered: Store.setFilePinned(fileContextMenu.ctxRole, fileContextMenu.ctxPath, !fileContextMenu.ctxPinned)
        }
        MenuItem {
            enabled: !fileContextMenu.ctxDownloading
            height: visible ? implicitHeight : 0
            text: fileContextMenu.ctxDownloading ? "Downloading for offline…" : fileContextMenu.ctxOffline ? "Remove offline download" : "Download for offline use"
            visible: fileContextMenu.ctxRemoteVideo

            onTriggered: Store.setVideoOffline(fileContextMenu.ctxPath, !fileContextMenu.ctxOffline)
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Copy file path to clipboard"

            onTriggered: Store.copyFilePath(fileContextMenu.ctxPath)
        }
        MenuItem {
            text: "Open folder containing"

            onTriggered: Store.openContainingFolder(fileContextMenu.ctxPath)
        }
        MenuSeparator {
            height: visible ? implicitHeight : 0
            visible: fileContextMenu.ctxHasSession
        }
        MenuItem {
            enabled: fileContextMenu.ctxMappingKey !== ""
            height: visible ? implicitHeight : 0
            objectName: "renameDriverMenuItem"
            text: "Rename driver"
            visible: fileContextMenu.ctxHasSession

            onTriggered: browser.driverRenameRequested(fileContextMenu.ctxMappingKey, fileContextMenu.ctxDriver)
        }
        MenuItem {
            height: visible ? implicitHeight : 0
            text: "Set active session (best lap)"
            visible: fileContextMenu.ctxHasSession

            onTriggered: browser.setActiveRequested(fileContextMenu.ctxKey)
        }
        MenuItem {
            enabled: Store.primarySessionKey !== "" && fileContextMenu.ctxKey !== Store.primarySessionKey
            height: visible ? implicitHeight : 0
            text: "Set as reference (best lap)"
            visible: fileContextMenu.ctxHasSession

            onTriggered: browser.setReferenceRequested(fileContextMenu.ctxKey)
        }
        MenuItem {
            height: visible ? implicitHeight : 0
            text: "Use this session only"
            visible: fileContextMenu.ctxHasSession

            onTriggered: browser.fileIsolated(fileContextMenu.ctxKey)
        }
        MenuItem {
            enabled: Store.trackAtlasReady
            height: visible ? implicitHeight : 0
            text: "Assign track…"
            visible: fileContextMenu.ctxHasSession

            onTriggered: browser.trackAssignmentRequested(fileContextMenu.ctxKey)
        }
        MenuSeparator {
            height: visible ? implicitHeight : 0
            visible: fileContextMenu.ctxHasSession && Store.comparing
        }
        MenuItem {
            height: visible ? implicitHeight : 0
            text: "Clear reference"
            visible: fileContextMenu.ctxHasSession && Store.comparing

            onTriggered: Store.clearCompare()
        }
    }
    Menu {
        id: folderContextMenu

        property string ctxLocationId
        property string ctxPath
        property bool ctxPinned
        property string ctxRole

        MenuItem {
            text: folderContextMenu.ctxPinned ? "Unpin from top" : "Pin to top"

            onTriggered: Store.setFilePinned(folderContextMenu.ctxRole, folderContextMenu.ctxPath, !folderContextMenu.ctxPinned)
        }
        MenuItem {
            enabled: !Store.loading
            height: visible ? implicitHeight : 0
            text: "Rescan server"
            visible: folderContextMenu.ctxLocationId !== ""

            onTriggered: Store.scan()
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Edit folder TRACK.yml…"

            onTriggered: browser.folderMetadataRequested(folderContextMenu.ctxPath)
        }
        MenuItem {
            enabled: false
            text: "Inherited by videos and subfolders"
        }
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Style.surfaceColor

            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                color: Style.borderColor
                height: 1
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 4
                spacing: 6

                Label {
                    color: Style.accentColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.letterSpacing: 0.8
                    font.pixelSize: 9
                    text: "FILES"
                }
                CompactTextField {
                    id: fileFilter

                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredHeight: 26
                    color: Style.foregroundColor
                    placeholderText: "Filter…"
                    placeholderTextColor: Style.dimTextColor

                    Keys.onEscapePressed: {
                        fileFilter.clear();
                        filterModel.filterText = "";
                    }
                    onTextEdited: filterTimer.restart()
                }
                ToolButton {
                    Accessible.name: "Clear file filter"
                    Layout.preferredHeight: 28
                    Layout.preferredWidth: 28
                    font.pixelSize: 11
                    text: "×"
                    visible: fileFilter.text !== ""

                    onClicked: {
                        fileFilter.clear();
                        filterModel.filterText = "";
                    }
                }
                BusyIndicator {
                    Layout.preferredHeight: 22
                    Layout.preferredWidth: 28
                    running: Store.loading
                    visible: running
                }
                ToolButton {
                    Accessible.name: "Rescan file sources"
                    Layout.preferredHeight: 28
                    Layout.preferredWidth: 28
                    enabled: !Store.loading
                    font.pixelSize: 13
                    text: "↻"
                    visible: !Store.loading

                    onClicked: Store.scan()
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: eventSection.implicitHeight + 12
            color: Style.surfaceColor

            ColumnLayout {
                id: eventSection

                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.top: parent.top
                anchors.topMargin: 6
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        color: Style.accentColor
                        font.bold: true
                        font.family: Style.monoFontFamily
                        font.letterSpacing: 0.8
                        font.pixelSize: 9
                        text: "EVENT"
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Switch {
                        checked: Store.eventMode

                        onToggled: Store.eventMode = checked
                    }
                }
                ComboBox {
                    id: eventTrackPick

                    Layout.fillWidth: true
                    Layout.preferredHeight: 26
                    currentIndex: Math.max(0, eventTrackPick.model.indexOf(Store.eventTrack))
                    model: [""].concat(Store.library.trackPills)

                    onActivated: index => Store.eventTrack = index <= 0 ? "" : eventTrackPick.model[index]
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    CompactTextField {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        placeholderText: "Session (CT4)"
                        text: Store.eventSession

                        onEditingFinished: Store.eventSession = text.trim()
                    }
                    CompactTextField {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        placeholderText: "YYYY-MM-DD"
                        text: Store.eventDate

                        onEditingFinished: Store.eventDate = text.trim()
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            color: Style.surfaceColor
            visible: Store.usbPresent || Store.manualUsbSource !== ""

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 4
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    color: Style.accentColor
                    elide: Text.ElideRight
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: Store.usbLabel
                }
                CompactButton {
                    text: "Sync…"

                    onClicked: Store.showUsbSync()
                }
                CompactButton {
                    text: "Copy…"

                    onClicked: Store.showUsbCopy()
                }
            }
        }
        Item {
            Layout.fillHeight: true
            Layout.fillWidth: true

            // The row under the top edge stays put across rescans, metadata
            // arrivals, filter changes and folder toggles.
            ScrollAnchor {
                role: "rowIdentity"
                view: tree
            }
            ListView {
                id: tree

                anchors.fill: parent
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                highlightFollowsCurrentItem: false
                model: filterModel
                objectName: "libraryTree"
                reuseItems: true

                ScrollBar.vertical: ThinScrollBar {
                }
                delegate: FileTreeDelegate {
                    activeSessionKey: Store.primarySessionKey
                    referenceSessionKey: Store.compareSessionKey

                    onContextMenuRequested: (role, path, key, hasSession, isVideo, mappingKey, driver, pinned) => {
                        if (role === "file") {
                            fileContextMenu.ctxPath = path;
                            fileContextMenu.ctxKey = key;
                            fileContextMenu.ctxHasSession = hasSession;
                            fileContextMenu.ctxVideoFile = isVideo;
                            fileContextMenu.ctxMappingKey = mappingKey;
                            fileContextMenu.ctxDriver = driver;
                            fileContextMenu.ctxPinned = pinned;
                            fileContextMenu.ctxRole = role;
                            const offline = Store.videoOffline(path);
                            fileContextMenu.ctxRemoteVideo = offline.remote;
                            fileContextMenu.ctxOffline = offline.offline;
                            fileContextMenu.ctxDownloading = offline.busy;
                            fileContextMenu.popup();
                        } else {
                            folderContextMenu.ctxPath = path;
                            folderContextMenu.ctxRole = role;
                            folderContextMenu.ctxPinned = pinned;
                            folderContextMenu.ctxLocationId = Store.locationIdForPath(path);
                            folderContextMenu.popup();
                        }
                    }
                    onFileActivated: (path, key, hasSession) => browser.fileActivated(path, key, hasSession)
                    onSetActiveRequested: key => browser.setActiveRequested(key)
                    onSetReferenceRequested: key => browser.setReferenceRequested(key)
                    onToggleNodeRequested: (role, path) => filterModel.toggleNode(role, path)
                }
            }
            Column {
                anchors.centerIn: parent
                spacing: 4
                visible: Store.loading
                z: 2

                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    objectName: "sessionLoadingIndicator"
                    running: Store.loading
                }
                Label {
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: "SCANNING FILES"
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            color: Style.surfaceColor
            implicitHeight: filterColumn.implicitHeight + 10
            visible: Store.library.driverPills.length > 0 || Store.library.yearPills.length > 0 || Store.library.trackPills.length > 0

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                color: Style.borderColor
                height: 1
            }
            ColumnLayout {
                id: filterColumn

                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.top: parent.top
                anchors.topMargin: 6
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        color: Style.accentColor
                        font.bold: true
                        font.family: Style.monoFontFamily
                        font.letterSpacing: 0.8
                        font.pixelSize: 9
                        text: "FILTERS"
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    ToolButton {
                        Accessible.name: filterModel.eventFilterActive ? "Clear filters and leave event mode" : "Clear filters"
                        Layout.preferredHeight: 20
                        Layout.preferredWidth: 20
                        font.pixelSize: 11
                        text: "×"
                        visible: filterModel.anyFilterActive

                        onClicked: browser.clearFilters()
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    visible: Store.library.driverPills.length > 0

                    Repeater {
                        model: Store.library.driverPills

                        FilterPill {
                            required property string modelData

                            label: modelData
                            selected: filterModel.selectedDrivers.indexOf(modelData) >= 0

                            onActivated: browser.toggleDriver(modelData)
                        }
                    }
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    visible: Store.library.yearPills.length > 0

                    Repeater {
                        model: Store.library.yearPills

                        FilterPill {
                            required property string modelData

                            label: modelData
                            selected: filterModel.selectedYears.indexOf(modelData) >= 0

                            onActivated: browser.toggleYear(modelData)
                        }
                    }
                }
                ComboBox {
                    id: trackFilter

                    Layout.fillWidth: true
                    Layout.preferredHeight: Style.controlHeight
                    currentIndex: filterModel.selectedTrack === "" ? 0 : Store.library.trackPills.indexOf(filterModel.selectedTrack) + 1
                    font.family: Style.uiFontFamily
                    font.pixelSize: Style.smallFontSize
                    implicitHeight: Style.controlHeight
                    model: ["All tracks"].concat(Store.library.trackPills)
                    visible: Store.library.trackPills.length > 0

                    onActivated: index => {
                        filterModel.selectedTrack = index <= 0 ? "" : Store.library.trackPills[index - 1];
                    }
                }
            }
        }
        // A recording is tens of gigabytes, so the one being fetched for
        // offline use says so for as long as it takes, and can be called off.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            color: Style.surfaceColor
            visible: Store.videoDownloadStatus !== ""

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                color: Style.borderColor
                height: 1
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 4
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    elide: Text.ElideMiddle
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: Store.videoDownloadStatus
                }
                CompactButton {
                    text: "Cancel"

                    onClicked: Store.cancelVideoDownloads()
                }
            }
        }
    }
    Rectangle {
        id: usbSheet

        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        color: Style.traceBackgroundColor
        height: Store.usbCopyVisible ? Math.min(480, Math.max(280, browser.height * 0.55)) : 0
        visible: Store.usbCopyVisible || usbSheet.height > 0
        z: 10

        Behavior on height {
            NumberAnimation {
                duration: 220
                easing.type: Easing.OutCubic
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            color: Style.borderColor
            height: 1
        }
        UsbCopyOverlay {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 1
        }
    }
}
