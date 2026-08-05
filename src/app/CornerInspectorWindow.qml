pragma ComponentBehavior: Bound

// Corner analysis inspector (separate Material window).
//
// Owns its caches: cornerRows from Store.cornerComparison() and cornerZoneRows
// from Store.cornerList(), refreshed in Component.onCompleted and from a
// Connections { target: Store } block on cornersChanged, selectionChanged
// (corner comparison depends on the selected laps), and
// referenceAlignmentChanged. Main.qml sets selectedCornerIndex before showing
// the window; the rename and dismiss actions belong to the root window and are
// emitted as signals. The graphs and the damper strip are C++ items
// (CornerGraphView, DamperStripView) fed from typed store data.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Racecraft

ApplicationWindow {
    id: cornerWindow

    property real cornerDamperShift: 0

    // Owned caches.
    property var cornerRows: []
    property var cornerZoneRows: []
    readonly property real damperCornerStartMeters: Number(cornerWindow.selectedCorner.damperCornerStartMeters || 0)
    readonly property real damperWindowMeters: Number(cornerWindow.selectedCorner.damperWindowMeters || 1)
    readonly property var selectedCorner: cornerWindow.cornerRows.length > 0 ? cornerWindow.cornerRows[Math.min(cornerWindow.selectedCornerIndex, cornerWindow.cornerRows.length - 1)] : ({})

    // Set by Main.qml before the window is shown.
    property int selectedCornerIndex: 0

    signal cornerDismissRequested

    // Root-window actions emitted up to Main.qml.
    signal cornerRenameRequested(int index)

    function commitZoneRange(index, startText, endText): void {
        let start = Number(startText) / 100;
        let end = Number(endText) / 100;
        if (!isFinite(start) || !isFinite(end))
            return;
        start = Math.max(0, Math.min(1, start));
        end = Math.max(start + 0.001, Math.min(1, end));
        Store.updateCorner(index, start, end);
        Store.saveCorners();
    }

    // Damper alignment painter helpers, moved verbatim from Main.qml. Shared
    // only between the two corner-damper Canvas items below.
    function refresh(): void {
        cornerWindow.cornerRows = Store.cornerComparison();
        cornerWindow.cornerZoneRows = Store.cornerList();
    }

    Material.accent: Style.accentColor
    Material.background: Style.backgroundColor
    Material.foreground: Style.foregroundColor
    Material.primary: Style.surfaceColor
    Material.theme: Material.Dark
    color: Style.backgroundColor
    font.family: Style.uiFontFamily
    font.pixelSize: 11
    height: 700
    minimumHeight: 520
    minimumWidth: 700
    objectName: "cornerWindow"
    title: Store.comparing ? "Corner Analysis — primary vs reference" : "Corner Analysis"
    visible: false
    width: 900

    Component.onCompleted: cornerWindow.refresh()
    onSelectedCornerChanged: cornerWindow.cornerDamperShift = Number(cornerWindow.selectedCorner.damperAlignment || 0)

    Connections {
        function onCornersChanged(): void {
            cornerWindow.refresh();
        }
        function onReferenceAlignmentChanged(): void {
            cornerWindow.refresh();
        }
        function onSelectionChanged(): void {
            cornerWindow.refresh();
        }

        target: Store
    }
    Shortcut {
        // StandardKey.Cancel maps to more than one sequence; `sequences` binds
        // all of them instead of silently taking the first.
        context: Qt.ApplicationShortcut
        sequences: [StandardKey.Cancel]

        onActivated: cornerWindow.cornerDismissRequested()
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true

            Label {
                color: Style.foregroundColor
                font.bold: true
                font.pixelSize: 17
                text: cornerWindow.selectedCorner.name || "Corner Analysis"
            }
            Label {
                color: Style.mutedTextColor
                font.pixelSize: 11
                text: Store.comparing ? "distance-aligned primary vs reference" : Store.primaryDetail
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                color: Style.accentColor
                font.family: Style.monoFontFamily
                font.pixelSize: 9
                text: "TRACK ATLAS"
                visible: Store.trackAtlasReady
            }
        }
        ListView {
            id: cornerPicker

            Layout.fillWidth: true
            Layout.preferredHeight: 34
            clip: true
            model: cornerWindow.cornerRows
            orientation: ListView.Horizontal
            spacing: 4

            ScrollBar.horizontal: ThinScrollBar {
            }
            delegate: Rectangle {
                id: cornerPickerDelegate

                required property int index
                required property var modelData

                border.color: Style.accentColor
                border.width: cornerPickerDelegate.index === cornerWindow.selectedCornerIndex ? 1 : 0
                color: cornerPickerDelegate.index === cornerWindow.selectedCornerIndex ? Style.selectionColor : Style.darkBackgroundColor
                height: 30
                radius: 4
                width: Math.max(86, cornerName.implicitWidth + 20)

                Label {
                    id: cornerName

                    anchors.centerIn: parent
                    color: cornerPickerDelegate.index === cornerWindow.selectedCornerIndex ? Style.accentColor : Style.mutedTextColor
                    font.bold: cornerPickerDelegate.index === cornerWindow.selectedCornerIndex
                    font.pixelSize: 10
                    text: cornerPickerDelegate.modelData.name
                }
                MouseArea {
                    anchors.fill: parent

                    onClicked: cornerWindow.selectedCornerIndex = cornerPickerDelegate.index
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            CompactButton {
                checkable: true
                checked: Store.editingCorners
                text: Store.editingCorners ? "Editing zones" : "Edit zones"

                onClicked: Store.setEditingCorners(checked)
            }
            CompactButton {
                enabled: Store.editingCorners
                text: "Add zone"

                onClicked: {
                    Store.setEditingCorners(true);
                    const width = 0.04;
                    const start = Math.max(0, Math.min(1 - width, Store.cursorFrac - width / 2));
                    const index = Store.addCorner(start, start + width);
                    if (index >= 0) {
                        cornerWindow.selectedCornerIndex = index;
                        cornerWindow.cornerRenameRequested(index);
                    }
                }
            }
            CompactButton {
                enabled: Store.editingCorners
                text: "Auto-generate"

                onClicked: {
                    Store.autoGenerateCorners();
                    Store.saveCorners();
                }
            }
            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                elide: Text.ElideRight
                font.pixelSize: 10
                text: Store.editingCorners ? "Drag zone edges on the trace; edits save automatically." : "Enable editing to add, rename, or delete zones."
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 158 : 0
            border.color: Style.borderColor
            color: Style.darkBackgroundColor
            radius: 4
            visible: Store.editingCorners

            ListView {
                id: zoneEditor

                anchors.fill: parent
                anchors.margins: 5
                clip: true
                model: cornerWindow.cornerZoneRows
                spacing: 3

                ScrollBar.vertical: ThinScrollBar {
                }
                delegate: RowLayout {
                    id: zoneRow

                    required property int index
                    required property var modelData

                    height: 34
                    spacing: 5
                    width: ListView.view.width - 12

                    CompactTextField {
                        id: zoneNameField

                        Layout.fillWidth: true
                        font.pixelSize: Style.smallFontSize
                        text: zoneRow.modelData.name

                        onEditingFinished: {
                            Store.setCornerName(zoneRow.index, text);
                            const zones = Store.cornerList();
                            if (zoneRow.index < zones.length)
                                text = zones[zoneRow.index].name;
                        }
                    }
                    CompactTextField {
                        id: zoneStartField

                        Layout.preferredWidth: 66
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        horizontalAlignment: Text.AlignRight
                        text: (zoneRow.modelData.start * 100).toFixed(2)

                        validator: DoubleValidator {
                            bottom: 0
                            top: 100
                        }

                        onEditingFinished: cornerWindow.commitZoneRange(zoneRow.index, zoneStartField.text, zoneEndField.text)
                    }
                    Label {
                        color: Style.mutedTextColor
                        font.pixelSize: 10
                        text: "→"
                    }
                    CompactTextField {
                        id: zoneEndField

                        Layout.preferredWidth: 66
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        horizontalAlignment: Text.AlignRight
                        text: (zoneRow.modelData.end * 100).toFixed(2)

                        validator: DoubleValidator {
                            bottom: 0
                            top: 100
                        }

                        onEditingFinished: cornerWindow.commitZoneRange(zoneRow.index, zoneStartField.text, zoneEndField.text)
                    }
                    Label {
                        color: Style.mutedTextColor
                        font.family: Style.monoFontFamily
                        font.pixelSize: 9
                        text: "% lap"
                    }
                    ToolButton {
                        ToolTip.text: "Delete zone"
                        ToolTip.visible: hovered
                        implicitWidth: 28
                        text: "×"

                        onClicked: Store.deleteCorner(zoneRow.index)
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            Repeater {
                model: [
                    {
                        label: "TIME",
                        value: cornerWindow.selectedCorner.time !== undefined ? cornerWindow.selectedCorner.time.toFixed(3) + "s" : "—",
                        delta: cornerWindow.selectedCorner.hasCompare ? "Δ " + (cornerWindow.selectedCorner.delta >= 0 ? "+" : "") + cornerWindow.selectedCorner.delta.toFixed(3) + "s" : ""
                    },
                    {
                        label: "SPEED",
                        value: cornerWindow.selectedCorner.apexSpeed !== undefined ? Math.round(cornerWindow.selectedCorner.entrySpeed) + " → " + Math.round(cornerWindow.selectedCorner.apexSpeed) + " → " + Math.round(cornerWindow.selectedCorner.exitSpeed) : "—",
                        delta: cornerWindow.selectedCorner.hasCompare ? "exit " + (cornerWindow.selectedCorner.exitDelta >= 0 ? "+" : "") + cornerWindow.selectedCorner.exitDelta.toFixed(1) + " km/h" : ""
                    },
                    {
                        label: "CONTROL",
                        value: cornerWindow.selectedCorner.minGear !== undefined ? "G" + cornerWindow.selectedCorner.minGear + "  steer " + Math.round(cornerWindow.selectedCorner.maxSteering) + "°" : "—",
                        delta: cornerWindow.selectedCorner.turnInPoint !== undefined ? "turn-in " + Math.round(cornerWindow.selectedCorner.turnInPoint) + "m  apex " + Math.round(cornerWindow.selectedCorner.apexPoint) + "m" : ""
                    },
                    {
                        label: "SCORE",
                        value: cornerWindow.selectedCorner.hasCompare ? Math.round(cornerWindow.selectedCorner.score) + " / 100" : "single lap",
                        delta: cornerWindow.selectedCorner.hasCompare ? (cornerWindow.selectedCorner.delta < 0 ? "primary ahead" : "reference ahead") : ""
                    }
                ]

                delegate: Rectangle {
                    id: scoreCard

                    required property var modelData

                    Layout.fillWidth: true
                    Layout.preferredHeight: 58
                    color: Style.surfaceColor
                    radius: 4

                    Column {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 2

                        Label {
                            color: Style.dimTextColor
                            font.bold: true
                            font.family: Style.monoFontFamily
                            font.pixelSize: 8
                            text: scoreCard.modelData.label
                        }
                        Label {
                            color: Style.foregroundColor
                            font.bold: true
                            font.family: Style.monoFontFamily
                            font.pixelSize: 12
                            text: scoreCard.modelData.value
                        }
                        Label {
                            color: Style.mutedTextColor
                            font.family: Style.monoFontFamily
                            font.pixelSize: 9
                            text: scoreCard.modelData.delta
                        }
                    }
                }
            }
        }
        CornerGraphView {
            id: cornerGraphs

            Layout.fillHeight: true
            Layout.fillWidth: true
            apexColor: Style.magentaColor
            backgroundColor: Style.traceBackgroundColor
            brakeColor: Style.redColor
            compareBrakeColor: Style.orangeColor
            compareColor: Style.mutedTextColor
            cornerIndex: cornerWindow.selectedCornerIndex
            dimColor: Qt.rgba(0, 0, 0, 0.58)
            gridColor: Style.borderColor
            labelColor: Style.mutedTextColor
            monoFontFamily: Style.monoFontFamily
            pickupColor: Style.greenColor
            speedColor: Style.greenColor
            steeringColor: Style.yellowColor
            store: Store
            throttleColor: Style.greenColor
            turnInColor: Style.accentColor
        }
        Rectangle {
            id: cornerDamperPanel

            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 76 : 0
            border.color: Style.borderColor
            color: Style.surfaceColor
            radius: 4
            visible: !Store.hasGpsData && !!cornerWindow.selectedCorner.hasCompare && cornerWindow.selectedCorner.damperPrimarySeries !== undefined && cornerWindow.selectedCorner.damperCompareSeries !== undefined && cornerWindow.selectedCorner.damperCompareSeries.length > 1

            Label {
                anchors.left: parent.left
                anchors.leftMargin: 7
                anchors.verticalCenter: parent.verticalCenter
                color: Style.mutedTextColor
                font.bold: true
                font.family: Style.monoFontFamily
                font.pixelSize: 8
                lineHeight: 0.9
                text: "DAMPER\nALIGN"
                width: 82
            }
            Item {
                id: cornerDamperCanvas

                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.leftMargin: 90
                anchors.right: cornerDamperDragLabel.left
                anchors.rightMargin: 6
                anchors.top: parent.top

                Rectangle {
                    anchors.fill: parent
                    color: Style.traceBackgroundColor

                    // Three quarter-height guides, matching the trace lanes.
                    Repeater {
                        model: 3

                        delegate: Rectangle {
                            id: damperGuide

                            required property int index

                            color: Style.borderColor
                            height: 1
                            width: parent.width
                            y: Math.round(parent.height * (damperGuide.index + 1) / 4)
                        }
                    }
                }
                DamperStripView {
                    anchors.fill: parent
                    color: Style.orangeColor
                    cornerIndex: cornerWindow.selectedCornerIndex
                    series: DamperStripView.Compare
                    shift: cornerWindow.cornerDamperShift
                    source: DamperStripView.CornerWindow
                    store: Store
                    strokeOpacity: 0.58
                }
                DamperStripView {
                    anchors.fill: parent
                    color: Style.accentColor
                    cornerIndex: cornerWindow.selectedCornerIndex
                    series: DamperStripView.Primary
                    source: DamperStripView.CornerWindow
                    store: Store
                    strokeOpacity: 0.82
                }
                Rectangle {
                    id: cornerStartMarker

                    readonly property real position: Math.max(0, Math.min(1, cornerWindow.damperCornerStartMeters / Math.max(1, cornerWindow.damperWindowMeters)))

                    color: Style.accentColor
                    height: parent.height
                    opacity: 0.75
                    width: 1
                    x: Math.round(cornerStartMarker.position * parent.width)
                }
                Label {
                    anchors.left: cornerStartMarker.right
                    anchors.leftMargin: 4
                    anchors.top: parent.top
                    color: Style.accentColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: 8
                    text: "CORNER START"
                }
            }
            Label {
                id: cornerDamperDragLabel

                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                color: cornerWindow.selectedCorner.damperAlignmentValid ? Style.accentColor : Style.orangeColor
                font.bold: true
                font.family: Style.monoFontFamily
                font.pixelSize: 8
                horizontalAlignment: Text.AlignRight
                text: (cornerWindow.selectedCorner.damperAlignmentValid ? "AUTO " : "MANUAL ") + (cornerWindow.cornerDamperShift >= 0 ? "+" : "") + cornerWindow.cornerDamperShift.toFixed(1) + "m  ↔"
                width: 104
            }
            MouseArea {
                id: cornerDamperAlignmentMouse

                property real pressX: 0
                property real startShift: 0

                ToolTip.text: "Auto-aligned from damper peaks in the prior 300m; drag to adjust"
                ToolTip.visible: containsMouse
                anchors.bottom: parent.bottom
                anchors.left: cornerDamperCanvas.left
                anchors.right: parent.right
                anchors.top: parent.top
                cursorShape: Qt.SizeHorCursor

                onDoubleClicked: cornerWindow.cornerDamperShift = Number(cornerWindow.selectedCorner.damperAlignment || 0)
                onPositionChanged: mouse => {
                    if (!pressed || width <= 0)
                        return;
                    const span = Number(cornerWindow.selectedCorner.damperWindowMeters || 1);
                    cornerWindow.cornerDamperShift = Math.max(-50, Math.min(50, startShift + (mouse.x - pressX) / width * span));
                }
                onPressed: mouse => {
                    pressX = mouse.x;
                    startShift = cornerWindow.cornerDamperShift;
                }
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: 11
            text: cornerWindow.selectedCorner.note || ""
            visible: cornerWindow.selectedCorner.note !== undefined
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6

            CompactButton {
                checkable: true
                checked: Store.editingCorners
                text: "Edit zones"

                onClicked: Store.setEditingCorners(checked)
            }
            Item {
                Layout.fillWidth: true
            }
            CompactButton {
                text: "Close"

                onClicked: cornerWindow.hide()
            }
        }
    }
}
