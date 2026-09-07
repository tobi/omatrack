pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

Item {
    id: root

    property real chromeTopInset: 0
    required property ImageTelemetryController controller
    readonly property list<string> directionIds: ["unknown", "bottom_to_top", "top_to_bottom", "left_to_right", "right_to_left"]
    readonly property list<string> directionLabels: ["Unknown direction", "Bottom → top", "Top → bottom", "Left → right", "Right → left"]
    readonly property list<string> fieldIds: ["unknown", "gear", "stint_lap", "brake", "throttle", "speed", "rpm", "steering"]
    readonly property list<string> fieldLabels: ["Unknown field", "Gear", "Displayed stint lap", "Brake fill", "Throttle fill", "Speed", "Engine RPM", "Steering"]
    readonly property real imageHeight: root.imageWidth / Math.max(0.01, root.sourceAspect)
    readonly property real imageWidth: Math.min(root.videoViewport.width, root.videoViewport.height * root.sourceAspect)
    readonly property real imageX: root.videoViewport.x + (root.videoViewport.width - root.imageWidth) / 2
    readonly property real imageY: root.videoViewport.y + (root.videoViewport.height - root.imageHeight) / 2
    readonly property list<string> representationIds: ["unknown", "digits", "bar", "wheel", "needle"]
    readonly property list<string> representationLabels: ["Unknown type", "Digits", "Fill bar", "Steering wheel", "Needle dial"]
    property bool reviewOpen: true
    property int revision: 0
    readonly property gaugeRegionRow selected: {
        root.revision;
        return root.controller.gauges.rowForKey(root.selectedKey);
    }
    property string selectedKey: ""
    required property real sourceAspect
    required property rect videoViewport

    function fieldLabel(value: string): string {
        const index = root.fieldIds.indexOf(value);
        return index >= 0 ? root.fieldLabels[index] : value;
    }
    function moveBox(key: string, semantic: string, selected: bool, box: rect, dx: real, dy: real): void {
        const x = Math.max(0, Math.min(1 - box.width, box.x + dx / root.imageWidth));
        const y = Math.max(0, Math.min(1 - box.height, box.y + dy / root.imageHeight));
        root.controller.editGauge(key, semantic, selected, Qt.rect(x, y, box.width, box.height));
    }
    function resizeBox(key: string, semantic: string, selected: bool, box: rect, dx: real, dy: real): void {
        const w = Math.max(0.001, Math.min(1 - box.x, box.width + dx / root.imageWidth));
        const h = Math.max(0.001, Math.min(1 - box.y, box.height + dy / root.imageHeight));
        root.controller.editGauge(key, semantic, selected, Qt.rect(box.x, box.y, w, h));
    }

    Connections {
        function onRefreshed(): void {
            root.revision++;
        }

        target: root.controller.gauges
    }
    Connections {
        function onPlayerChanged(): void {
            root.selectedKey = "";
        }

        target: root.controller
    }
    Repeater {
        model: root.controller.enabled && root.controller.geometryCompatible ? root.controller.gauges : null

        delegate: Rectangle {
            id: boxItem

            property rect dragBox
            required property int index
            property point pressPoint
            required property gaugeRegionRow region

            border.color: !boxItem.region.selected ? Style.mutedTextColor : boxItem.region.confirmed ? Style.accentColor : Style.yellowColor
            border.width: root.selectedKey === boxItem.region.key ? 2 : 1
            color: Qt.rgba(0, 0, 0, 0)
            height: boxItem.region.box.height * root.imageHeight
            width: boxItem.region.box.width * root.imageWidth
            x: root.imageX + boxItem.region.box.x * root.imageWidth
            y: root.imageY + boxItem.region.box.y * root.imageHeight

            Label {
                id: caption

                anchors.bottom: parent.top
                color: boxItem.border.color
                font.family: Style.monoFontFamily
                font.pixelSize: Style.smallFontSize
                padding: 2
                text: root.fieldLabel(boxItem.region.semantic) + " · " + boxItem.region.origin + (boxItem.region.confirmed ? " ✓" : " · seen " + boxItem.region.evidence)
                x: Math.max(-boxItem.x, Math.min(root.width - boxItem.x - caption.width, boxItem.region.representation === "bar" && boxItem.width < 80 ? (boxItem.index % 2 === 0 ? -caption.width : boxItem.width) : 0))

                background: Rectangle {
                    color: Style.videoControlBackgroundColor
                }
            }
            MouseArea {
                id: moveArea

                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor
                enabled: root.reviewOpen && root.controller.phase !== ImageTelemetryController.Extracting

                onPressed: mouse => {
                    root.selectedKey = boxItem.region.key;
                    boxItem.dragBox = boxItem.region.box;
                    boxItem.pressPoint = moveArea.mapToItem(root, mouse.x, mouse.y);
                }
                onReleased: mouse => {
                    const point = moveArea.mapToItem(root, mouse.x, mouse.y);
                    if (Math.abs(point.x - boxItem.pressPoint.x) + Math.abs(point.y - boxItem.pressPoint.y) > 2)
                        root.moveBox(boxItem.region.key, boxItem.region.semantic, boxItem.region.selected, boxItem.dragBox, point.x - boxItem.pressPoint.x, point.y - boxItem.pressPoint.y);
                }
            }
            Rectangle {
                id: resizeHandle

                anchors.bottom: parent.bottom
                anchors.right: parent.right
                color: boxItem.border.color
                height: 10
                visible: root.reviewOpen && root.selectedKey === boxItem.region.key && root.controller.phase !== ImageTelemetryController.Extracting
                width: 10

                MouseArea {
                    id: resizeArea

                    anchors.fill: parent
                    cursorShape: Qt.SizeFDiagCursor

                    onPressed: mouse => {
                        boxItem.dragBox = boxItem.region.box;
                        boxItem.pressPoint = resizeArea.mapToItem(root, mouse.x, mouse.y);
                    }
                    onReleased: mouse => {
                        const point = resizeArea.mapToItem(root, mouse.x, mouse.y);
                        root.resizeBox(boxItem.region.key, boxItem.region.semantic, boxItem.region.selected, boxItem.dragBox, point.x - boxItem.pressPoint.x, point.y - boxItem.pressPoint.y);
                    }
                }
            }
        }
    }
    Rectangle {
        color: Style.videoControlBackgroundColor
        height: discoveryToolbar.implicitHeight
        radius: 4
        width: discoveryToolbar.implicitWidth + 8
        x: discoveryToolbar.x - 4
        y: discoveryToolbar.y
    }
    RowLayout {
        id: discoveryToolbar

        anchors.left: parent.left
        anchors.leftMargin: 44
        anchors.top: parent.top
        anchors.topMargin: root.chromeTopInset
        spacing: 4

        CheckBox {
            Material.foreground: Style.foregroundColor
            checked: root.controller.enabled
            font.pixelSize: Style.smallFontSize
            text: "Discover gauges"

            onToggled: Store.imageTelemetryEnabled = checked
        }
        ToolButton {
            Material.foreground: Style.foregroundColor
            font.pixelSize: Style.smallFontSize
            text: root.reviewOpen ? "Hide setup" : "Review setup"
            visible: root.controller.enabled

            onClicked: root.reviewOpen = !root.reviewOpen
        }
    }
    Rectangle {
        id: panel

        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.top: parent.top
        anchors.topMargin: 38 + root.chromeTopInset
        border.color: Style.borderColor
        color: Style.videoControlBackgroundColor
        height: Math.max(0, Math.min(root.height - 136, contents.implicitHeight + 16))
        radius: 4
        visible: root.controller.enabled && root.reviewOpen
        width: Math.min(360, root.width - 16)

        MouseArea {
            anchors.fill: parent
        }
        ColumnLayout {
            id: contents

            anchors.fill: parent
            anchors.margins: 8
            spacing: 4

            Label {
                Layout.fillWidth: true
                color: Style.accentColor
                font.bold: true
                font.pixelSize: Style.smallFontSize
                text: root.controller.phase === ImageTelemetryController.Extracting ? "3  EXTRACT · predicted values" : root.controller.phase === ImageTelemetryController.Confirmed ? (root.controller.canExtract ? "2  CONFIRMED · ready to extract" : "2  CONFIRMED · reading unavailable") : "1  DISCOVER → 2  CONFIRM → 3  EXTRACT"
            }
            Label {
                Layout.fillWidth: true
                color: Style.foregroundColor
                font.pixelSize: Style.smallFontSize
                text: root.controller.status
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                color: Style.yellowColor
                font.pixelSize: Style.smallFontSize
                objectName: "gaugeExperimentalWarning"
                text: "EXPERIMENTAL inventory · AiM profile crops are checked separately from image structure. Other boxes remain unreadable."
                visible: root.controller.experimentalDetector
                wrapMode: Text.Wrap
            }
            ListView {
                id: regions

                Layout.fillHeight: true
                Layout.fillWidth: true
                Layout.minimumHeight: 32
                Layout.preferredHeight: Math.min(150, regions.contentHeight)
                clip: true
                model: root.controller.gauges

                ScrollBar.vertical: ScrollBar {
                }
                delegate: ItemDelegate {
                    id: regionItem

                    required property int index
                    required property gaugeRegionRow region

                    font.pixelSize: Style.smallFontSize
                    height: 32
                    highlighted: root.selectedKey === regionItem.region.key
                    text: (regionItem.region.selected ? "☑ " : "☐ ") + root.fieldLabel(regionItem.region.semantic) + " · " + regionItem.region.origin + (regionItem.region.confirmed ? " ✓" : " · seen " + regionItem.region.evidence) + (regionItem.region.readable ? "" : " · unreadable")
                    width: regions.width

                    onClicked: root.selectedKey = regionItem.region.key
                }

                ScrollAnchor {
                    role: "key"
                    view: regions
                }
            }
            RowLayout {
                Layout.fillWidth: true
                enabled: root.controller.phase !== ImageTelemetryController.Extracting
                visible: root.selected.key !== ""

                CheckBox {
                    checked: root.selected.selected
                    text: "Read"

                    onToggled: root.controller.editGauge(root.selected.key, root.selected.semantic, checked, root.selected.box)
                }
                ComboBox {
                    id: semanticPicker

                    Layout.fillWidth: true
                    currentIndex: root.fieldIds.indexOf(root.selected.semantic)
                    model: root.fieldLabels

                    onActivated: root.controller.editGauge(root.selected.key, root.fieldIds[semanticPicker.currentIndex], root.selected.selected, root.selected.box)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                enabled: root.controller.phase !== ImageTelemetryController.Extracting
                visible: root.selected.key !== ""

                ComboBox {
                    id: representationPicker

                    Layout.fillWidth: true
                    currentIndex: root.representationIds.indexOf(root.selected.representation)
                    model: root.representationLabels

                    onActivated: root.controller.editGauge(root.selected.key, root.selected.semantic, root.selected.selected, root.selected.box, root.representationIds[representationPicker.currentIndex], root.selected.direction)
                }
                ComboBox {
                    id: directionPicker

                    Layout.fillWidth: true
                    currentIndex: root.directionIds.indexOf(root.selected.direction)
                    enabled: root.selected.representation === "bar"
                    model: root.directionLabels

                    onActivated: root.controller.editGauge(root.selected.key, root.selected.semantic, root.selected.selected, root.selected.box, root.selected.representation, root.directionIds[directionPicker.currentIndex])
                }
            }
            Label {
                Layout.fillWidth: true
                color: root.selected.readable ? Style.mutedTextColor : Style.yellowColor
                font.pixelSize: Style.smallFontSize
                text: root.selected.support + "\nDrag box to move; corner square to resize."
                visible: root.selected.key !== ""
                wrapMode: Text.Wrap
            }
            Button {
                Layout.fillWidth: true
                enabled: root.controller.discoverySamples > 0
                font.pixelSize: Style.smallFontSize
                text: "I visually confirm this box and label"
                visible: root.selected.key !== "" && root.controller.phase === ImageTelemetryController.Discovery

                onClicked: root.controller.confirmGauge(root.selected.key)
            }
            CheckBox {
                id: saveDefault

                checked: true
                font.pixelSize: Style.smallFontSize
                objectName: "gaugeRememberExtension"
                text: "Remember for this extension · revalidate next file"
                visible: root.controller.phase === ImageTelemetryController.Discovery
            }
            RowLayout {
                Layout.fillWidth: true

                Button {
                    Layout.fillWidth: true
                    enabled: root.controller.canConfirm
                    text: "Confirm setup"
                    visible: root.controller.phase === ImageTelemetryController.Discovery

                    onClicked: root.controller.confirmSetup(saveDefault.checked)
                }
                Button {
                    Layout.fillWidth: true
                    enabled: root.controller.canExtract
                    text: "Start extraction"
                    visible: root.controller.phase === ImageTelemetryController.Confirmed

                    onClicked: root.controller.startExtraction()
                }
                Button {
                    Layout.fillWidth: true
                    text: root.controller.phase === ImageTelemetryController.Extracting ? "Stop / edit setup" : "Edit setup"
                    visible: root.controller.phase !== ImageTelemetryController.Discovery

                    onClicked: root.controller.reviewSetup()
                }
            }
        }
    }
}
