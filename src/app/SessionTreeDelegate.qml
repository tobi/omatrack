pragma ComponentBehavior: Bound

// Sidebar tree row: a track / date / session entry in the session list.
//
// Reads every model role it renders as a required property and emits signals
// for actions the root window owns (activation, isolation, reference pick,
// expansion toggle, driver rename). Expansion state arrives as the resolved
// `expanded` role, so this delegate never touches the root expansion maps.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Racecraft

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
    required property string sessionTime
    required property string stem

    signal driverRenameRequested(string mappingKey, string driver)
    signal sessionActivated(string key)
    signal sessionIsolated(string key)
    signal setActiveRequested(string key)
    signal setReferenceRequested(string key)
    signal toggleDateRequested(string key)

    // ── actions delegated to the root window ──────────────────────
    signal toggleTrackRequested(string name)

    height: row.role === "track" ? 28 : row.role === "date" ? 24 : 38
    width: ListView.view.width

    Rectangle {
        anchors.fill: parent
        color: row.activeSession ? Style.selectionColor : row.referenceSession ? Qt.rgba(224 / 255, 157 / 255, 127 / 255, 0.14) : row.role === "track" ? Style.surfaceColor : rowMouse.containsMouse ? Style.backgroundColor : "transparent"
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
                    visible: row.isVideo === true

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
                    text: (row.driver || "Unknown") + (row.driverId !== "" ? "  ID " + row.driverId : "")
                }
            }
            Label {
                Layout.fillWidth: true
                color: row.isDayBest ? Style.magentaColor : row.isDriverBest ? Style.greenColor : Style.mutedTextColor
                elide: Text.ElideMiddle
                font.family: Style.monoFontFamily
                font.pixelSize: 8
                text: (row.stem || row.name) + "  ·  " + (row.sessionTime || "--:--:--") + "  ·  best " + (row.bestTime || "—")
                visible: row.role === "session"
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
        MenuSeparator {
            visible: Store.comparing
        }
        MenuItem {
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
                if (row.role === "session") {
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
    }
}
