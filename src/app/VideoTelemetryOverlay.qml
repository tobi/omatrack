pragma ComponentBehavior: Bound
import Omatrack

import QtQuick

Item {
    id: overlay

    readonly property real aspectRatio: 0.21
    property real dragStartX: 0
    property real dragStartY: 0
    property real pressX: 0
    property real pressY: 0
    readonly property real scaleFactor: 0.65
    readonly property real unscaledWidth: Math.min(overlay.parent.width - 16, 1000, Math.max(520, overlay.parent.width * 0.72))
    property bool userPositioned: false

    height: overlay.width * overlay.aspectRatio
    objectName: "videoTelemetryOverlay"
    width: Math.max(0, overlay.unscaledWidth * overlay.scaleFactor)
    z: 8

    Binding {
        property: "x"
        target: overlay
        value: (overlay.parent.width - overlay.width) * 0.5
        when: !overlay.userPositioned
    }
    Binding {
        property: "y"
        target: overlay
        value: Math.max(0, Math.min(overlay.parent.height - overlay.height, overlay.parent.height * 0.9 - overlay.height * 0.5))
        when: !overlay.userPositioned
    }
    VideoTelemetryHud {
        anchors.fill: parent
        backgroundColor: Qt.rgba(0, 0, 0, 0.84)
        brakeColor: Style.brakeTelemetryColor
        compareColor: Style.orangeColor
        foregroundColor: Style.foregroundColor
        monoFontFamily: Style.monoFontFamily
        mutedColor: Style.mutedTextColor
        steeringColor: Style.steeringTelemetryColor
        store: Store
        throttleColor: Style.throttleTelemetryColor
    }
    MouseArea {
        acceptedButtons: Qt.LeftButton
        anchors.fill: parent
        cursorShape: Qt.SizeAllCursor
        preventStealing: true

        onPositionChanged: mouse => {
            if (!pressed)
                return;
            const maxX = Math.max(0, overlay.parent.width - overlay.width);
            const maxY = Math.max(0, overlay.parent.height - overlay.height);
            overlay.x = Math.max(0, Math.min(maxX, overlay.dragStartX + mouse.x - overlay.pressX));
            overlay.y = Math.max(0, Math.min(maxY, overlay.dragStartY + mouse.y - overlay.pressY));
        }
        onPressed: mouse => {
            overlay.userPositioned = true;
            overlay.dragStartX = overlay.x;
            overlay.dragStartY = overlay.y;
            overlay.pressX = mouse.x;
            overlay.pressY = mouse.y;
        }
    }
}
