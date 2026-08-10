pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: row

    readonly property bool activeFile: row.hasSession && row.key === row.activeSessionKey
    required property string activeSessionKey
    required property bool available
    required property string bestTime
    required property string carClass
    required property int childCount
    required property string driveTime
    required property string driver
    required property bool expanded
    required property bool hasSession
    required property int indent
    required property bool isVideo
    required property string key
    required property int lapCount
    required property string mappingKey
    readonly property bool metadataInViewport: row.role === "file" && row.y + row.height >= ListView.view.contentY && row.y <= ListView.view.contentY + ListView.view.height
    required property string modified
    required property string name
    required property string path
    required property bool pinned
    readonly property bool referenceFile: row.hasSession && row.key === row.referenceSessionKey
    required property string referenceSessionKey
    required property string role
    required property string seriesName
    required property string sessionDate
    readonly property string tooltipOwner: "file:" + row.path
    required property string topQuartileTime
    readonly property bool videoFile: row.role === "file" && row.isVideo

    signal contextMenuRequested(string role, string path, string key, bool hasSession, bool isVideo, string mappingKey, string driver, bool pinned)
    signal fileActivated(string path, string key, bool hasSession)
    signal pointerTooltipDismissed(string owner)
    signal pointerTooltipMoved(string owner, real x, real y)
    signal pointerTooltipRequested(string owner, string text, real x, real y)
    signal setActiveRequested(string key)
    signal setReferenceRequested(string key)
    signal toggleNodeRequested(string role, string path)

    function hoverText(): string {
        if (row.role !== "file")
            return row.path;
        const lines = [];
        if (row.hasSession) {
            lines.push("Driver: " + (row.driver !== "" ? row.driver : "—"));
            lines.push("Best lap: " + (row.bestTime !== "" ? row.bestTime : "—"));
            lines.push("Avg fastest 25%: " + (row.topQuartileTime !== "" ? row.topQuartileTime : "—"));
            lines.push("Drive time: " + (row.driveTime !== "" ? row.driveTime : "—"));
            lines.push("Laps: " + row.lapCount);
            lines.push("Car class: " + (row.carClass !== "" ? row.carClass : "—"));
            lines.push("Series: " + (row.seriesName !== "" ? row.seriesName : "—"));
            lines.push("Date: " + (row.sessionDate !== "" ? row.sessionDate : "—"));
        } else {
            if (row.driver !== "")
                lines.push("Driver: " + row.driver);
            if (row.carClass !== "")
                lines.push("Car class: " + row.carClass);
            if (row.seriesName !== "")
                lines.push("Series: " + row.seriesName);
            if (row.sessionDate !== "")
                lines.push("Date: " + row.sessionDate);
        }
        return (lines.length > 0 ? lines.join("\n") : "No telemetry metadata available") + "\n\n" + row.name;
    }
    function requestVisibleMetadata(): void {
        if (row.role === "file")
            Store.requestSidebarMetadata(row.path, row.metadataInViewport);
    }

    height: row.role === "source" || row.role === "pins" ? 40 : row.role === "file" ? 38 : 28
    width: ListView.view.width

    Component.onCompleted: row.requestVisibleMetadata()
    onMetadataInViewportChanged: row.requestVisibleMetadata()
    onPathChanged: row.requestVisibleMetadata()

    Rectangle {
        anchors.fill: parent
        color: row.activeFile ? Style.selectionColor : row.referenceFile ? Style.referenceSelectionColor : row.role === "source" || row.role === "pins" ? Style.surfaceColor : rowMouse.containsMouse ? Style.backgroundColor : "transparent"
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 5 + row.indent * 10
        anchors.rightMargin: row.hasSession ? 48 : 6
        spacing: 5
        z: 1

        Label {
            Layout.preferredWidth: 11
            color: Style.dimTextColor
            font.family: Style.monoFontFamily
            font.pixelSize: 8
            text: row.role === "source" || row.role === "folder" || row.role === "pins" ? (row.expanded ? "▾" : "▸") : ""
        }
        Rectangle {
            Layout.preferredHeight: 11
            Layout.preferredWidth: 15
            border.color: fileName.color
            border.width: 1
            color: "transparent"
            radius: 2
            visible: row.videoFile

            Label {
                anchors.centerIn: parent
                color: fileName.color
                font.family: Style.monoFontFamily
                font.pixelSize: 6
                text: "▶"
            }
        }
        ColumnLayout {
            Layout.fillHeight: true
            Layout.fillWidth: true
            spacing: 0

            Label {
                id: fileName

                Layout.fillWidth: true
                color: !row.available ? Style.redColor : row.activeFile ? Style.accentColor : row.referenceFile ? Style.orangeColor : Style.foregroundColor
                elide: Text.ElideRight
                font.bold: row.role === "source" || row.role === "pins" || row.activeFile
                font.family: row.role === "file" ? Style.monoFontFamily : Style.uiFontFamily
                font.pixelSize: row.role === "source" || row.role === "pins" ? 10 : 9
                text: row.name
            }
            Label {
                Layout.fillWidth: true
                color: !row.available ? Style.redColor : Style.mutedTextColor
                elide: Text.ElideMiddle
                font.family: Style.monoFontFamily
                font.pixelSize: 8
                text: row.role === "pins" ? row.childCount + (row.childCount === 1 ? " pinned item" : " pinned items") : row.role === "source" ? (!row.available ? "Folder not found" : row.childCount + (row.childCount === 1 ? " file" : " files")) : row.driver !== "" ? row.driver + (row.bestTime !== "" ? " · best " + row.bestTime : "") + (row.modified !== "" ? " · " + row.modified : "") : row.modified
                visible: row.role !== "folder"
            }
        }
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
        visible: row.hasSession
        z: 2

        RoleDot {
            activeColor: Style.accentColor
            selected: row.activeFile
            tip: "Make current lap"

            onActivated: row.setActiveRequested(row.key)
        }
        RoleDot {
            activeColor: Style.orangeColor
            selected: row.referenceFile
            tip: row.referenceFile ? "Clear reference" : "Make reference lap"

            onActivated: row.setReferenceRequested(row.key)
        }
    }
    MouseArea {
        id: rowMouse

        acceptedButtons: Qt.LeftButton | Qt.RightButton
        anchors.fill: parent
        hoverEnabled: true
        z: 0

        onClicked: mouse => {
            if (mouse.button === Qt.RightButton) {
                row.pointerTooltipDismissed(row.tooltipOwner);
                if (row.role === "file" || row.role === "source" || row.role === "folder")
                    row.contextMenuRequested(row.role, row.path, row.key, row.hasSession, row.videoFile, row.mappingKey, row.driver, row.pinned);
                return;
            }
            if (row.role === "source" || row.role === "folder" || row.role === "pins")
                row.toggleNodeRequested(row.role, row.path);
            else
                row.fileActivated(row.path, row.key, row.hasSession);
        }
        onEntered: {
            const point = rowMouse.mapToItem(Overlay.overlay, rowMouse.mouseX, rowMouse.mouseY);
            row.pointerTooltipRequested(row.tooltipOwner, row.hoverText(), point.x, point.y);
        }
        onExited: row.pointerTooltipDismissed(row.tooltipOwner)
        onPositionChanged: mouse => {
            const point = rowMouse.mapToItem(Overlay.overlay, mouse.x, mouse.y);
            row.pointerTooltipMoved(row.tooltipOwner, point.x, point.y);
        }
    }
}
