pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Pane {
    id: browser

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
    function nodeExpanded(role: string, path: string): bool {
        if (fileFilter.text.trim() !== "")
            return true;
        const stored = browser.expandedNodes[role + ":" + path];
        return stored === undefined ? role === "source" : stored;
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
    function revealInNodes(nodes, key: string, ancestors): bool {
        for (let index = 0; index < nodes.length; ++index) {
            const node = nodes[index];
            let nextAncestors = ancestors;
            if (node.role === "source" || node.role === "folder")
                nextAncestors = ancestors.concat([node.role + ":" + node.path]);
            if (node.role === "file" && node.key === key) {
                let expanded = Object.assign({}, browser.expandedNodes);
                for (let ancestor = 0; ancestor < ancestors.length; ++ancestor)
                    expanded[ancestors[ancestor]] = true;
                browser.expandedNodes = expanded;
                return true;
            }
            if (browser.revealInNodes(node.children || [], key, nextAncestors))
                return true;
        }
        return false;
    }
    function revealSession(key: string): void {
        if (key === "" || fileFilter.text !== "")
            return;
        browser.revealInNodes(Store.fileSources(), key, []);
    }
    function toggleNode(role: string, path: string): void {
        const key = role + ":" + path;
        let expanded = Object.assign({}, browser.expandedNodes);
        expanded[key] = !browser.nodeExpanded(role, path);
        browser.expandedNodes = expanded;
        browser.rebuild();
    }

    padding: 0

    background: Rectangle {
        color: Style.darkBackgroundColor
    }

    Component.onCompleted: browser.rebuild()

    Connections {
        function onDriverMappingsChanged(): void {
            browser.rebuild();
        }
        function onSelectionChanged(): void {
            browser.revealSession(Store.primarySessionKey);
            browser.rebuild();
        }
        function onSessionsChanged(): void {
            browser.rebuild();
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
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            border.color: Style.borderColor
            border.width: 1
            color: Style.surfaceColor

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

                    onDriverRenameRequested: (mappingKey, driver) => browser.driverRenameRequested(mappingKey, driver)
                    onFileActivated: (path, key, hasSession) => browser.fileActivated(path, key, hasSession)
                    onFileIsolated: key => browser.fileIsolated(key)
                    onFolderMetadataRequested: path => browser.folderMetadataRequested(path)
                    onPointerTooltipDismissed: owner => browser.pointerTooltipDismissed(owner)
                    onPointerTooltipMoved: (owner, x, y) => browser.pointerTooltipMoved(owner, x, y)
                    onPointerTooltipRequested: (owner, text, x, y) => browser.pointerTooltipRequested(owner, text, x, y)
                    onSetActiveRequested: key => browser.setActiveRequested(key)
                    onSetReferenceRequested: key => browser.setReferenceRequested(key)
                    onToggleNodeRequested: (role, path) => browser.toggleNode(role, path)
                    onTrackAssignmentRequested: key => browser.trackAssignmentRequested(key)
                    onVideoMetadataRequested: path => browser.videoMetadataRequested(path)
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
