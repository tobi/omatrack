pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: row

    readonly property bool activeFile: row.isPrimary
    required property string activeSessionKey
    required property bool available
    required property string bestLapText
    readonly property string bestTime: row.bestLapText
    required property string carClass
    required property int childCount
    readonly property string detailText: (row.startTimeText || "—") + " · " + row.lapCount + (row.lapCount === 1 ? " lap" : " laps") + " · " + (row.driveTimeText || "—")
    readonly property string driveTime: row.driveTimeText
    required property string driveTimeText
    required property string driver
    readonly property bool expandableRow: row.sectionRow || row.kind === "folder"
    required property bool expanded
    required property bool hasSession
    required property int indent
    required property bool isPrimary
    required property bool isReference
    required property bool isVideo
    required property string key
    required property string kind
    required property int lapCount
    required property string mappingKey
    readonly property bool metadataInViewport: row.kind === "file" && row.y + row.height >= ListView.view.contentY && row.y <= ListView.view.contentY + ListView.view.height
    required property string modified
    readonly property string name: row.title
    required property string path
    required property bool pinned
    readonly property bool referenceFile: row.isReference
    required property string referenceSessionKey
    readonly property string role: row.kind
    readonly property bool sectionRow: row.kind === "source" || row.kind === "pins" || row.kind === "recent"
    required property string seriesName
    required property string sessionDate
    required property string sessionName
    readonly property string sessionStart: row.startTimeText
    required property string startTimeText
    required property string title
    readonly property color titleColor: !row.available ? Style.redColor : row.activeFile ? Style.accentColor : row.referenceFile ? Style.orangeColor : Style.foregroundColor
    readonly property string titleText: row.sessionName !== "" && row.driver !== "" ? row.sessionName + " " + row.driver : row.sessionName || row.driver || "Untitled"
    required property string topQuartileTime
    readonly property bool videoFile: row.kind === "file" && row.isVideo

    signal contextMenuRequested(string role, string path, string key, bool hasSession, bool isVideo, string mappingKey, string driver, bool pinned)
    signal fileActivated(string path, string key, bool hasSession)
    signal setActiveRequested(string key)
    signal setReferenceRequested(string key)
    signal toggleNodeRequested(string role, string path)

    function requestVisibleMetadata(): void {
        if (row.role === "file")
            Store.requestSidebarMetadata(row.path, row.metadataInViewport);
    }

    height: row.sectionRow ? 40 : row.role === "file" ? 38 : row.role === "day" ? 22 : 28
    width: ListView.view.width

    Component.onCompleted: row.requestVisibleMetadata()
    onMetadataInViewportChanged: row.requestVisibleMetadata()
    onPathChanged: row.requestVisibleMetadata()

    Rectangle {
        anchors.fill: parent
        color: row.activeFile ? Style.selectionColor : row.referenceFile ? Style.referenceSelectionColor : row.sectionRow ? Style.surfaceColor : rowMouse.containsMouse ? Style.backgroundColor : "transparent"
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 5 + row.indent * 10
        anchors.rightMargin: 6
        spacing: 5
        z: 1

        Label {
            Layout.preferredWidth: 11
            color: Style.dimTextColor
            font.family: Style.monoFontFamily
            font.pixelSize: 8
            text: row.expandableRow ? (row.expanded ? "▾" : "▸") : ""
        }
        Rectangle {
            Layout.preferredHeight: 11
            Layout.preferredWidth: 15
            border.color: row.titleColor
            border.width: 1
            color: "transparent"
            radius: 2
            visible: row.videoFile

            Label {
                anchors.centerIn: parent
                color: row.titleColor
                font.family: Style.monoFontFamily
                font.pixelSize: 6
                text: "▶"
            }
        }
        ColumnLayout {
            Layout.fillHeight: true
            Layout.fillWidth: true
            spacing: 0

            DenseTwoLineRow {
                Layout.fillWidth: true
                detail: row.detailText
                detailColor: !row.available ? Style.redColor : Style.mutedTextColor
                rightColor: row.activeFile ? Style.accentColor : row.referenceFile ? Style.orangeColor : Style.foregroundColor
                rightValue: row.bestTime || "—"
                title: row.titleText
                titleBold: row.activeFile
                titleColor: row.titleColor
                visible: row.role === "file"

                RoleActionRow {
                    dotSize: 9
                    primarySelected: row.activeFile
                    primaryVisible: row.hasSession
                    referenceSelected: row.referenceFile
                    referenceVisible: row.hasSession

                    onPrimaryActivated: row.setActiveRequested(row.key)
                    onReferenceActivated: row.setReferenceRequested(row.key)
                }
            }
            Label {
                Layout.fillWidth: true
                color: !row.available ? Style.redColor : row.activeFile ? Style.accentColor : row.referenceFile ? Style.orangeColor : row.role === "day" ? Style.dimTextColor : Style.foregroundColor
                elide: Text.ElideRight
                font.bold: row.sectionRow || row.role === "day" || row.activeFile
                font.family: row.role === "day" ? Style.monoFontFamily : Style.uiFontFamily
                font.letterSpacing: row.role === "day" ? 0.6 : 0
                font.pixelSize: row.sectionRow ? 10 : row.role === "day" ? 8 : 9
                text: row.name
                visible: row.role !== "file"
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                visible: row.role === "pins" || row.role === "recent" || row.role === "source"

                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    color: !row.available ? Style.redColor : Style.mutedTextColor
                    elide: Text.ElideRight
                    font.family: Style.monoFontFamily
                    font.pixelSize: 8
                    text: row.role === "pins" ? row.childCount + (row.childCount === 1 ? " pinned item" : " pinned items") : row.role === "recent" ? row.childCount + (row.childCount === 1 ? " recent item" : " recent items") : row.role === "source" ? (!row.available ? "Folder not found" : row.childCount + (row.childCount === 1 ? " file" : " files")) : ""
                }
                RoleActionRow {
                    dotSize: 9
                    primarySelected: row.activeFile
                    primaryVisible: row.hasSession
                    referenceSelected: row.referenceFile
                    referenceVisible: row.hasSession

                    onPrimaryActivated: row.setActiveRequested(row.key)
                    onReferenceActivated: row.setReferenceRequested(row.key)
                }
            }
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
                if (row.role === "file" || row.role === "source" || row.role === "folder")
                    row.contextMenuRequested(row.role, row.path, row.key, row.hasSession, row.videoFile, row.mappingKey, row.driver, row.pinned);
                return;
            }
            if (row.expandableRow)
                row.toggleNodeRequested(row.role, row.path);
            else if (row.role === "file")
                row.fileActivated(row.path, row.key, row.hasSession);
        }
    }
}
