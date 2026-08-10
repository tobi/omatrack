pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: browser

    property var _ancestorCache: ({})
    property var expandedNodes: ({})

    signal driverRenameRequested(string mappingKey, string driver)
    signal fileActivated(string path, string key, bool hasSession)
    signal fileIsolated(string key)
    signal folderMetadataRequested(string path)
    signal pointerTooltipDismissed(string owner)
    signal pointerTooltipMoved(string owner, real x, real y)
    signal pointerTooltipRequested(string owner, string text, real x, real y)
    signal setActiveRequested(string key)
    signal setReferenceRequested(string key)
    signal trackAssignmentRequested(string key)
    signal videoMetadataRequested(string path)

    function appendNode(node, indent: int, query: string): void {
        if (!browser.nodeMatches(node, query))
            return;
        const role = node.role || "file";
        const children = node.children || [];
        const expanded = query !== "" || browser.nodeExpanded(role, node.path || "");
        treeModel.append({
            available: node.available !== false,
            bestTime: node.bestTime || "",
            carClass: node.carClass || "",
            childCount: role === "source" ? (node.fileCount || 0) : children.length,
            driver: node.driver || "",
            driveTime: node.driveTime || "",
            expanded: expanded,
            hasSession: node.hasSession === true,
            indent: indent,
            isVideo: node.isVideo === true,
            key: node.key || "",
            lapCount: node.lapCount || 0,
            mappingKey: node.mappingKey || "",
            modified: node.modified || "",
            name: node.name || "",
            path: node.path || "",
            pinned: node.pinned === true,
            role: role,
            seriesName: node.seriesName || "",
            sessionDate: node.sessionDate || "",
            topQuartileTime: node.topQuartileTime || ""
        });
        if (expanded)
            browser.appendNodes(children, indent + 1, query);
    }
    function appendNodes(nodes, indent: int, query: string): void {
        for (let index = 0; index < nodes.length; ++index)
            browser.appendNode(nodes[index], indent, query);
    }
    function buildAncestorCache(nodes, ancestors: list<string>): void {
        for (let index = 0; index < nodes.length; ++index) {
            const node = nodes[index];
            const role = node.role || "file";
            let nextAncestors = ancestors;
            if (role === "source" || role === "folder")
                nextAncestors = ancestors.concat([role + ":" + (node.path || "")]);
            if (role === "file" && node.key && !browser._ancestorCache[node.key])
                browser._ancestorCache[node.key] = nextAncestors;
            const children = node.children || [];
            if (children.length > 0)
                browser.buildAncestorCache(children, nextAncestors);
        }
    }
    function nodeExpanded(role: string, path: string): bool {
        if (fileFilter.text.trim() !== "")
            return true;
        const stored = browser.expandedNodes[role + ":" + path];
        return stored === undefined ? role === "source" || role === "pins" : stored;
    }
    function nodeMatches(node, query: string): bool {
        if (query === "")
            return true;
        const searchable = [node.name || "", node.path || "", node.driver || "", node.bestTime || "", node.carClass || "", node.seriesName || "", node.sessionDate || ""].join(" ").toLowerCase();
        if (searchable.includes(query))
            return true;
        const children = node.children || [];
        for (let index = 0; index < children.length; ++index)
            if (browser.nodeMatches(children[index], query))
                return true;
        return false;
    }
    function rebuild(): void {
        treeModel.clear();
        browser.appendNodes(Store.fileSources(), 0, fileFilter.text.trim().toLowerCase());
    }
    function rebuildAncestorCache(): void {
        browser._ancestorCache = ({});
        browser.buildAncestorCache(Store.fileSources(), []);
    }
    function revealSession(key: string): bool {
        if (key === "" || fileFilter.text !== "")
            return false;
        const ancestors = browser._ancestorCache[key];
        if (!ancestors)
            return false;
        let changed = false;
        let expanded = browser.expandedNodes;
        for (let index = 0; index < ancestors.length; ++index) {
            if (!expanded[ancestors[index]]) {
                if (!changed) {
                    expanded = Object.assign({}, browser.expandedNodes);
                    changed = true;
                }
                expanded[ancestors[index]] = true;
            }
        }
        if (changed)
            browser.expandedNodes = expanded;
        return changed;
    }
    function toggleNode(role: string, path: string): void {
        const key = role + ":" + path;
        let expanded = Object.assign({}, browser.expandedNodes);
        expanded[key] = !browser.nodeExpanded(role, path);
        browser.expandedNodes = expanded;
        browser.rebuild();
    }
    function updateFileMetadata(path: string, details: var): void {
        for (let index = 0; index < treeModel.count; ++index) {
            const row = treeModel.get(index);
            if (row.role !== "file" || row.path !== path)
                continue;
            treeModel.setProperty(index, "bestTime", details.bestTime || "");
            treeModel.setProperty(index, "carClass", details.carClass || "");
            treeModel.setProperty(index, "driveTime", details.driveTime || "");
            treeModel.setProperty(index, "driver", details.driver || "");
            treeModel.setProperty(index, "hasSession", details.hasSession === true);
            treeModel.setProperty(index, "isVideo", details.isVideo === true);
            treeModel.setProperty(index, "key", details.key || "");
            treeModel.setProperty(index, "lapCount", details.lapCount || 0);
            treeModel.setProperty(index, "mappingKey", details.mappingKey || "");
            treeModel.setProperty(index, "seriesName", details.seriesName || "");
            treeModel.setProperty(index, "sessionDate", details.sessionDate || "");
            treeModel.setProperty(index, "topQuartileTime", details.topQuartileTime || "");
        }
    }

    padding: 0

    background: Rectangle {
        color: Style.darkBackgroundColor
    }

    Component.onCompleted: {
        browser.rebuildAncestorCache();
        browser.rebuild();
    }

    Connections {
        function onDriverMappingsChanged(): void {
            browser.rebuild();
        }
        function onFilePinsChanged(): void {
            browser.rebuild();
        }
        function onSelectionChanged(): void {
            if (browser.revealSession(Store.primarySessionKey))
                browser.rebuild();
        }
        function onSessionsChanged(): void {
            browser.rebuildAncestorCache();
            browser.rebuild();
        }
        function onSidebarMetadataChanged(path: string, details: var): void {
            browser.updateFileMetadata(path, details);
        }

        target: Store
    }
    Timer {
        id: filterTimer

        interval: 120
        repeat: false

        onTriggered: browser.rebuild()
    }
    ListModel {
        id: treeModel
    }
    Menu {
        id: fileContextMenu

        property string ctxDriver
        property bool ctxHasSession
        property string ctxKey
        property string ctxMappingKey
        property string ctxPath
        property bool ctxPinned
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

        property string ctxPath
        property bool ctxPinned
        property string ctxRole

        MenuItem {
            text: folderContextMenu.ctxPinned ? "Unpin from top" : "Pin to top"

            onTriggered: Store.setFilePinned(folderContextMenu.ctxRole, folderContextMenu.ctxPath, !folderContextMenu.ctxPinned)
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
                        browser.rebuild();
                    }
                    onTextEdited: filterTimer.restart()
                }
                ToolButton {
                    Layout.preferredHeight: 28
                    Layout.preferredWidth: 28
                    ToolTip.text: "Clear file filter"
                    ToolTip.visible: hovered
                    font.pixelSize: 11
                    text: "×"
                    visible: fileFilter.text !== ""

                    onClicked: {
                        fileFilter.clear();
                        browser.rebuild();
                    }
                }
                BusyIndicator {
                    Layout.preferredHeight: 22
                    Layout.preferredWidth: 28
                    running: Store.loading
                    visible: running
                }
                ToolButton {
                    Layout.preferredHeight: 28
                    Layout.preferredWidth: 28
                    ToolTip.text: "Rescan file sources"
                    ToolTip.visible: hovered
                    enabled: !Store.loading
                    font.pixelSize: 13
                    text: "↻"
                    visible: !Store.loading

                    onClicked: Store.scan()
                }
            }
        }
        Item {
            Layout.fillHeight: true
            Layout.fillWidth: true

            ListView {
                id: tree

                anchors.fill: parent
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                model: treeModel

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
                            fileContextMenu.popup();
                        } else {
                            folderContextMenu.ctxPath = path;
                            folderContextMenu.ctxRole = role;
                            folderContextMenu.ctxPinned = pinned;
                            folderContextMenu.popup();
                        }
                    }
                    onFileActivated: (path, key, hasSession) => browser.fileActivated(path, key, hasSession)
                    onPointerTooltipDismissed: owner => browser.pointerTooltipDismissed(owner)
                    onPointerTooltipMoved: (owner, x, y) => browser.pointerTooltipMoved(owner, x, y)
                    onPointerTooltipRequested: (owner, text, x, y) => browser.pointerTooltipRequested(owner, text, x, y)
                    onSetActiveRequested: key => browser.setActiveRequested(key)
                    onSetReferenceRequested: key => browser.setReferenceRequested(key)
                    onToggleNodeRequested: (role, path) => browser.toggleNode(role, path)
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
    }
}
