pragma ComponentBehavior: Bound
import Omatrack

// Application header bar: brand, live session context (track / driver / lap /
// comparison), the imperative cursor readout, and the overflow actions menu.
//
// Owns its cursor-readout refresh: a Connections block on Store plus
// Component.onCompleted drive readout.refresh() so Main.qml never has to poke
// it. Actions that open sibling windows or toggle root state are emitted as
// signals; the only state read back is sidebarVisible (for the toggle tooltip).

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ToolBar {
    id: appBar

    // Root-window state the tooltip text reads.
    required property bool sidebarVisible

    signal channelsRequested
    signal cornersRequested
    signal driverRenameRequested(string key, string name)
    signal openTelemetryRequested
    signal openVideoRequested
    signal preferencesRequested

    // Overflow / toggle actions handed back to the root window.
    signal sidebarToggleRequested

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

    Component.onCompleted: readout.refresh()

    Connections {
        function onCursorFracChanged(): void {
            readout.refresh();
        }
        function onSelectionChanged(): void {
            readout.refresh();
        }

        target: Store
    }
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 8

        ToolButton {
            ToolTip.text: appBar.sidebarVisible ? "Hide sessions" : "Show sessions"
            ToolTip.visible: hovered
            font.pixelSize: 16
            text: "☰"

            onClicked: appBar.sidebarToggleRequested()
        }
        Label {
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
                    color: Store.comparing ? Style.orangeColor : Style.mutedTextColor
                    elide: Text.ElideRight
                    font.pixelSize: 10
                    text: Store.primaryDriverName
                    visible: headerDriverName.text !== ""
                }
                ToolButton {
                    id: headerDriverEdit

                    Layout.preferredHeight: 20
                    Layout.preferredWidth: 20
                    ToolTip.text: "Rename driver"
                    ToolTip.visible: hovered
                    font.pixelSize: 10
                    objectName: "headerDriverEdit"
                    text: "✎"
                    visible: Store.primaryDriverMappingKey !== ""

                    onClicked: appBar.driverRenameRequested(Store.primaryDriverMappingKey, Store.primaryDriverName)
                }
                Label {
                    id: headerDetail

                    Layout.fillWidth: true
                    color: Store.comparing ? Style.orangeColor : Style.mutedTextColor
                    elide: Text.ElideRight
                    font.pixelSize: 10
                    text: {
                        const driver = Store.primaryDriverName;
                        const detail = Store.primaryDetail;
                        const suffix = driver !== "" && detail.indexOf(driver) === 0 ? detail.substring(driver.length) : detail;
                        return suffix + (Store.comparing ? "  ·  vs " + Store.compareLabel : "");
                    }
                    visible: headerDetail.text !== ""
                }
            }
        }
        Label {
            id: readout

            function refresh(): void {
                if (!Store.ready) {
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
                if (Store.comparing && r.delta !== undefined)
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
        ToolButton {
            ToolTip.text: "Actions"
            ToolTip.visible: hovered
            font.pixelSize: 14
            text: "•••"

            onClicked: actionsMenu.open()
        }
    }
    Menu {
        id: actionsMenu

        x: Math.max(0, appBar.width - actionsMenu.width - 8)
        y: appBar.height

        MenuItem {
            text: "Open telemetry…"

            onTriggered: appBar.openTelemetryRequested()
        }
        MenuItem {
            text: "Open video…"

            onTriggered: appBar.openVideoRequested()
        }
        MenuSeparator {
        }
        MenuItem {
            text: "Corner inspector"

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
        MenuItem {
            checkable: true
            checked: Store.channelHeight > 110
            text: "Larger trace lanes"

            onTriggered: Store.channelHeight = checked ? 150 : 110
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
