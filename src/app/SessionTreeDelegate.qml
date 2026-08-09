pragma ComponentBehavior: Bound
import Omatrack

// Sidebar tree row: a track / date / session entry in the session list.
//
// Reads every model role it renders as a required property and emits signals
// for actions the root window owns (activation, isolation, reference pick,
// expansion toggle, driver rename). Expansion state arrives as the resolved
// `expanded` role, so this delegate never touches the root expansion maps.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: row

    property bool activeSession: row.role === "session" && row.key === row.activeSessionKey

    // ── root-window state (read-only) ─────────────────────────────
    required property string activeSessionKey
    required property string bestTime
    required property string driver
    required property string driverId
    required property bool expanded
    required property int indent
    required property bool isDayBest
    required property bool isDriverBest
    required property bool isVideo
    required property string key
    required property string mappingKey
    required property string name
    property bool referenceSession: row.role === "session" && row.key === row.referenceSessionKey
    required property string referenceSessionKey

    // ── model roles ───────────────────────────────────────────────
    required property string role
    required property string stem
    readonly property string tooltipOwner: "session:" + row.key
    readonly property bool videoSession: row.role === "session" && row.isVideo

    signal driverRenameRequested(string mappingKey, string driver)
    signal pointerTooltipDismissed(string owner)
    signal pointerTooltipMoved(string owner, real x, real y)
    signal pointerTooltipRequested(string owner, string text, real x, real y)
    signal sessionActivated(string key)
    signal sessionIsolated(string key)
    signal setActiveRequested(string key)
    signal setReferenceRequested(string key)
    signal toggleDateRequested(string key)

    // ── actions delegated to the root window ──────────────────────
    signal toggleTrackRequested(string name)
    signal trackAssignmentRequested(string key)
    signal videoMetadataRequested(string key)

    height: row.role === "track" ? 28 : row.role === "date" ? 24 : 38
    width: ListView.view.width

    Rectangle {
        anchors.fill: parent
        color: row.activeSession ? Style.selectionColor : row.referenceSession ? Style.referenceSelectionColor : row.role === "track" ? Style.surfaceColor : rowMouse.containsMouse ? Style.backgroundColor : "transparent"
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 4 + row.indent * 8
        anchors.rightMargin: row.role === "session" ? 46 : 4
        spacing: 4
        z: 1

        Label {
            Layout.preferredWidth: 10
            color: Style.dimTextColor
            font.pixelSize: 8
            text: row.role === "track" || row.role === "date" ? (row.expanded ? "▾" : "▸") : ""
        }
        ColumnLayout {
            Layout.fillHeight: true
            Layout.fillWidth: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                visible: row.role === "session"

                Rectangle {
                    Layout.preferredHeight: 11
                    Layout.preferredWidth: 15
                    border.color: sessionRowLabel.color
                    border.width: 1
                    color: "transparent"
                    radius: 2
                    visible: row.videoSession

                    Label {
                        anchors.centerIn: parent
                        color: sessionRowLabel.color
                        font.family: Style.monoFontFamily
                        font.pixelSize: 6
                        text: "▶"
                    }
                }
                Label {
                    id: sessionRowLabel

                    Layout.fillWidth: true
                    color: row.activeSession ? Style.accentColor : row.referenceSession ? Style.orangeColor : Style.foregroundColor
                    elide: Text.ElideRight
                    font.bold: row.activeSession
                    font.family: Style.uiFontFamily
                    font.pixelSize: 10
                    text: row.driver || "Unknown"
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 5
                visible: row.role === "session"

                Label {
                    Layout.minimumWidth: implicitWidth
                    color: row.isDayBest ? Style.magentaColor : row.isDriverBest ? Style.greenColor : Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 8
                    text: "best " + (row.bestTime || "—")
                }
                Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    color: Style.mutedTextColor
                    elide: Text.ElideMiddle
                    font.family: Style.monoFontFamily
                    font.pixelSize: 8
                    text: row.stem || row.name
                }
            }
            Label {
                Layout.fillWidth: true
                color: row.role === "track" ? Style.foregroundColor : Style.mutedTextColor
                elide: Text.ElideRight
                font.bold: row.role === "track"
                font.family: Style.uiFontFamily
                font.pixelSize: row.role === "track" ? 10 : 9
                text: row.name
                visible: row.role !== "session"
            }
        }
        ToolButton {
            Layout.preferredHeight: 22
            Layout.preferredWidth: 22
            ToolTip.text: "Close track"
            ToolTip.visible: hovered
            text: "×"
            visible: row.role === "track"

            onClicked: Store.closeTrack(row.name)
        }
    }

    // Above the full-row MouseArea so the dots and the track close button take
    // their own clicks instead of activating the row.
    Row {
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
        visible: row.role === "session"
        z: 2

        RoleDot {
            activeColor: Style.accentColor
            selected: row.activeSession
            tip: "Make current lap"

            onActivated: row.setActiveRequested(row.key)
        }
        RoleDot {
            activeColor: Style.orangeColor
            selected: row.referenceSession
            tip: row.referenceSession ? "Clear reference" : "Make reference lap"

            onActivated: row.setReferenceRequested(row.key)
        }
    }
    Menu {
        id: sessionMenu

        property bool videoMetadataAttached: true

        function setVideoMetadataAvailable(available: bool): void {
            if (available === sessionMenu.videoMetadataAttached)
                return;
            if (available) {
                sessionMenu.insertItem(0, videoMetadataItem);
                sessionMenu.insertItem(1, videoMetadataSeparator);
            } else {
                sessionMenu.removeItem(videoMetadataItem);
                sessionMenu.removeItem(videoMetadataSeparator);
            }
            sessionMenu.videoMetadataAttached = available;
        }

        MenuItem {
            id: videoMetadataItem

            text: "Edit video metadata…"

            onTriggered: row.videoMetadataRequested(row.key)
        }
        MenuSeparator {
            id: videoMetadataSeparator
        }
        MenuItem {
            enabled: (row.mappingKey || "") !== ""
            objectName: "renameDriverMenuItem"
            text: "Rename driver"

            onTriggered: row.driverRenameRequested(row.mappingKey || "", row.driver || "")
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Set active session (best lap)"

            onTriggered: row.setActiveRequested(row.key)
        }
        MenuItem {
            enabled: row.activeSessionKey !== "" && row.key !== row.activeSessionKey
            text: "Set as reference (best lap)"

            onTriggered: row.setReferenceRequested(row.key)
        }
        MenuItem {
            text: "Use this session only"

            onTriggered: row.sessionIsolated(row.key)
        }
        MenuItem {
            enabled: Store.trackAtlasReady
            text: "Assign track…"

            onTriggered: row.trackAssignmentRequested(row.key)
        }
        MenuSeparator {
            height: visible ? implicitHeight : 0
            visible: Store.comparing
        }
        MenuItem {
            height: visible ? implicitHeight : 0
            text: "Clear reference"
            visible: Store.comparing

            onTriggered: Store.clearCompare()
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
                if (row.role === "session") {
                    sessionMenu.setVideoMetadataAvailable(row.videoSession);
                    sessionMenu.x = mouse.x;
                    sessionMenu.y = mouse.y;
                    sessionMenu.open();
                }
                return;
            }
            if (row.role === "track") {
                row.toggleTrackRequested(row.name);
            } else if (row.role === "date") {
                row.toggleDateRequested(row.key);
            } else if (row.role === "session") {
                row.sessionActivated(row.key);
            }
        }
        onEntered: {
            if (row.role !== "session")
                return;
            const point = rowMouse.mapToItem(Overlay.overlay, rowMouse.mouseX, rowMouse.mouseY);
            row.pointerTooltipRequested(row.tooltipOwner, row.stem, point.x, point.y);
        }
        onExited: row.pointerTooltipDismissed(row.tooltipOwner)
        onPositionChanged: mouse => {
            if (row.role !== "session")
                return;
            const point = rowMouse.mapToItem(Overlay.overlay, mouse.x, mouse.y);
            row.pointerTooltipMoved(row.tooltipOwner, point.x, point.y);
        }
    }
}
