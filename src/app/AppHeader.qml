pragma ComponentBehavior: Bound
import Omatrack

// Application header bar: brand, live session context (track / driver / lap /
// comparison), the imperative cursor readout, and the overflow actions menu.
//
// Owns its cursor-readout refresh: a Connections block on Store plus
// Component.onCompleted drive readout.refresh() so Main.qml never has to poke
// it. Actions that open sibling windows or toggle root state are emitted as
// signals; the only root-window state read back is sidebarVisible (for the
// toggle tooltip). Portable AppImage updates come from the Updater singleton.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ToolBar {
    id: appBar

    property list<string> recentFileRows: []

    // Root-window state the tooltip text reads.
    required property bool sidebarVisible
    property var sidecarRows: []

    signal addTelemetryDirectoryRequested
    signal channelsRequested
    signal cornersRequested
    signal driverRenameRequested(string key, string name)
    signal metadataRequested(string path, bool folderScope)
    signal openFileRequested
    signal preferencesRequested

    // Overflow / toggle actions handed back to the root window.
    signal sidebarToggleRequested

    function recentFileLabel(filePath: string): string {
        const normalized = filePath.replace(/\\/g, "/");
        return normalized.substring(normalized.lastIndexOf("/") + 1);
    }
    function refreshRecentFiles(): void {
        appBar.recentFileRows = Store.recentFiles;
    }
    function refreshSidecars(): void {
        appBar.sidecarRows = Store.sidecarLibrary();
    }

    Material.elevation: 2
    height: 48

    background: Rectangle {
        color: Style.surfaceColor

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            color: Style.borderColor
            height: 1
        }
    }

    Component.onCompleted: {
        readout.refresh();
        appBar.refreshRecentFiles();
        appBar.refreshSidecars();
    }

    Connections {
        function onCursorReadoutChanged(): void {
            readout.refresh();
        }
        function onRecentFilesChanged(): void {
            appBar.refreshRecentFiles();
        }
        function onSelectionChanged(): void {
            readout.refresh();
        }
        function onSidecarLibraryChanged(): void {
            appBar.refreshSidecars();
        }

        target: Store
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 8

        CompactToolButton {
            font.pixelSize: 16
            text: "☰"
            tip: appBar.sidebarVisible ? "Hide sidebar" : "Show sidebar"

            onClicked: appBar.sidebarToggleRequested()
        }
        Label {
            Layout.leftMargin: 4
            Layout.rightMargin: 10
            color: Style.accentColor
            font.bold: true
            font.family: Style.monoFontFamily
            font.letterSpacing: 1.2
            font.pixelSize: 13
            text: "OMATRACK"
        }
        Rectangle {
            Layout.preferredHeight: 28
            Layout.preferredWidth: 1
            color: Style.borderColor
        }
        BusyIndicator {
            Layout.preferredHeight: 24
            Layout.preferredWidth: 24
            Material.accent: Style.accentColor
            running: Store.lapLoading
            visible: running
        }
        CompactToolButton {
            Layout.preferredWidth: 26
            enabled: Store.fileOpenLoading
            font.bold: true
            font.pixelSize: 16
            text: "×"
            tip: "Cancel opening " + Store.fileOpenPath
            visible: Store.fileOpenLoading

            onClicked: Store.cancelFileOpen()
        }
        Label {
            Layout.maximumWidth: 260
            color: Style.mutedTextColor
            elide: Text.ElideMiddle
            font.pixelSize: 10
            text: "Opening " + Store.fileOpenPath
            visible: Store.fileOpenLoading
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            spacing: 0

            Label {
                Layout.fillWidth: true
                color: Style.foregroundColor
                elide: Text.ElideRight
                font.bold: true
                font.pixelSize: 12
                text: Store.primaryLabel
                visible: text !== ""
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    id: headerDriverName

                    Layout.maximumWidth: Math.min(220, headerDriverName.implicitWidth)
                    color: Style.mutedTextColor
                    elide: Text.ElideRight
                    font.pixelSize: 10
                    text: Store.primaryDriverName
                    visible: headerDriverName.text !== ""
                }
                Label {
                    id: headerDetail

                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    elide: Text.ElideRight
                    font.pixelSize: 10
                    text: {
                        const driver = Store.primaryDriverName;
                        const detail = Store.primaryDetail;
                        return driver !== "" && detail.indexOf(driver) === 0 ? detail.substring(driver.length) : detail;
                    }
                    visible: headerDetail.text !== ""
                }
                Label {
                    id: headerCompare

                    color: Style.orangeColor
                    elide: Text.ElideRight
                    font.pixelSize: 10
                    text: Store.comparing ? "  ·  vs " + Store.compareLabel : ""
                    visible: headerCompare.text !== ""
                }
            }
        }
        CompactToolButton {
            Layout.preferredHeight: 28
            Layout.preferredWidth: appBar.width >= 900 ? implicitWidth : 30
            display: appBar.width >= 900 ? AbstractButton.TextBesideIcon : AbstractButton.IconOnly
            icon.name: "document-properties-symbolic"
            objectName: "headerMetadataEdit"
            text: "Metadata"
            tip: Store.primaryMetadataFolderScope ? "Edit metadata inherited by this session" : "Edit recording metadata"
            visible: Store.primaryMetadataPath !== ""

            onClicked: appBar.metadataRequested(Store.primaryMetadataPath, Store.primaryMetadataFolderScope)
        }
        RowLayout {
            id: updateCluster

            spacing: 4
            visible: Updater.supported && Updater.enabled && (Updater.available || Updater.busy)

            CompactToolButton {
                id: updateButton

                readonly property string tooltipText: Updater.busy ? Updater.status : "Omatrack " + Updater.latestVersion + " is available"

                Layout.preferredHeight: 28
                Layout.preferredWidth: 28
                display: AbstractButton.IconOnly
                font.pixelSize: 14
                icon.color: Style.accentColor
                icon.name: "software-update-available-symbolic"
                objectName: "headerUpdate"
                text: "↑"
                tip: updateButton.tooltipText

                onClicked: updateMenu.open()
            }
            Label {
                color: Style.accentColor
                font.family: Style.monoFontFamily
                font.pixelSize: 10
                text: Updater.latestVersion
                visible: Updater.bannerVisible && !Updater.busy && appBar.width >= 780
            }
            CompactButton {
                text: "Update"
                visible: Updater.bannerVisible && !Updater.busy && appBar.width >= 780

                onClicked: Updater.install()
            }
            CompactButton {
                text: "Later"
                visible: Updater.bannerVisible && !Updater.busy && appBar.width >= 780

                onClicked: Updater.snooze()
            }
            ProgressBar {
                Layout.preferredWidth: 88
                from: 0
                to: 1
                value: Updater.progress
                visible: Updater.busy
            }
            Label {
                color: Style.mutedTextColor
                elide: Text.ElideRight
                font.family: Style.monoFontFamily
                font.pixelSize: 10
                text: Updater.downloadLabel !== "" ? Updater.downloadLabel : Updater.status
                visible: Updater.busy && appBar.width >= 780
            }
        }
        Label {
            id: readout

            function refresh(): void {
                if (!Store.ready || Store.primarySessionKey === "") {
                    readout.text = "";
                    return;
                }
                const r = Store.cursorReadout();
                let parts = [];
                if (r.time !== undefined)
                    parts.push(r.time.toFixed(1) + "s");
                if (r.dist !== undefined)
                    parts.push(Math.round(r.dist) + "m");
                if (r.speed !== undefined)
                    parts.push(Math.round(r.speed) + " km/h");
                if (r.gear !== undefined)
                    parts.push("G" + r.gear);
                if (r.corner)
                    parts.push(r.corner);
                if (Store.comparing && r.hasDelta)
                    parts.push("Δ " + r.delta.toFixed(3) + "s");
                readout.text = parts.join("  ·  ");
            }

            Layout.maximumWidth: Math.max(120, appBar.width * 0.34)
            color: Style.mutedTextColor
            elide: Text.ElideLeft
            font.family: Style.monoFontFamily
            font.pixelSize: 10
            horizontalAlignment: Text.AlignRight
            visible: appBar.width >= 650
        }
        CompactToolButton {
            font.pixelSize: 14
            text: "•••"
            tip: "Actions"

            onClicked: actionsMenu.open()
        }
    }
    Menu {
        id: updateMenu

        x: Math.max(0, appBar.width - updateMenu.width - 48)
        y: appBar.height

        MenuItem {
            enabled: !Updater.busy
            text: "Update to " + Updater.latestVersion

            onTriggered: Updater.install()
        }
        MenuItem {
            enabled: !Updater.busy
            height: visible ? implicitHeight : 0
            text: "Remind in a week"
            visible: Updater.bannerVisible

            onTriggered: Updater.snooze()
        }
        MenuItem {
            height: visible ? implicitHeight : 0
            text: "Cancel update"
            visible: Updater.busy

            onTriggered: Updater.cancel()
        }
    }
    Menu {
        id: actionsMenu

        x: Math.max(0, appBar.width - actionsMenu.width - 8)
        y: appBar.height

        MenuItem {
            text: "Open file…"

            onTriggered: appBar.openFileRequested()
        }
        Menu {
            id: recentFilesMenu

            enabled: appBar.recentFileRows.length > 0
            title: "Open recent"

            Instantiator {
                model: appBar.recentFileRows

                delegate: MenuItem {
                    id: recentFileItem

                    required property string modelData

                    ToolTip.text: recentFileItem.modelData
                    ToolTip.visible: recentFileItem.hovered
                    text: appBar.recentFileLabel(recentFileItem.modelData)

                    onTriggered: Store.openFile(recentFileItem.modelData)
                }

                onObjectAdded: (index, object) => recentFilesMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => recentFilesMenu.removeItem(object)
            }
        }
        Menu {
            id: sidecarsMenu

            enabled: appBar.sidecarRows.length > 0
            title: "Sidecars"

            Instantiator {
                model: appBar.sidecarRows

                delegate: MenuItem {
                    id: sidecarItem

                    required property var modelData

                    ToolTip.text: sidecarItem.modelData.path
                    ToolTip.visible: sidecarItem.hovered
                    text: sidecarItem.modelData.name || sidecarItem.modelData.path

                    onTriggered: Store.attachSidecar(sidecarItem.modelData.path)
                }

                onObjectAdded: (index, object) => sidecarsMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => sidecarsMenu.removeItem(object)
            }
        }
        MenuItem {
            text: "Telemetry library…"

            onTriggered: appBar.preferencesRequested()
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Focus corner at cursor"

            onTriggered: appBar.cornersRequested()
        }
        MenuItem {
            checkable: true
            checked: Store.editingCorners
            text: "Edit corner zones"

            onTriggered: Store.setEditingCorners(checked)
        }
        MenuItem {
            text: "Auto-generate corners"

            onTriggered: Store.autoGenerateCorners()
        }
        MenuItem {
            text: "Save corners"

            onTriggered: Store.saveCorners()
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Trace channels"

            onTriggered: appBar.channelsRequested()
        }
        MenuItem {
            text: "Preferences"

            onTriggered: appBar.preferencesRequested()
        }
        MenuSeparator {
            height: visible ? implicitHeight : 0
            visible: Store.comparing
        }
        MenuItem {
            height: visible ? implicitHeight : 0
            text: "Clear comparison"
            visible: Store.comparing

            onTriggered: Store.clearCompare()
        }
    }
}
