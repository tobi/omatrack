pragma ComponentBehavior: Bound
import Omatrack

import QtQuick

Item {
    id: overlay

    readonly property real aspectRatio: 0.21
    property real dragOriginX: 0
    property real dragOriginY: 0
    property real mediaTime: 0
    readonly property real scaleFactor: 0.65
    readonly property real unscaledWidth: Math.min(overlay.parent.width - 16, 1000, Math.max(520, overlay.parent.width * 0.72))
    property bool userPositioned: false

    function clampX(value) {
        if (!overlay.parent)
            return 0;
        return Math.max(0, Math.min(overlay.parent.width - overlay.width, value));
    }
    function clampY(value) {
        if (!overlay.parent)
            return 0;
        return Math.max(0, Math.min(overlay.parent.height - overlay.height, value));
    }

    height: overlay.width * overlay.aspectRatio
    objectName: "videoTelemetryOverlay"
    width: Math.max(0, overlay.unscaledWidth * overlay.scaleFactor)
    z: 8

    Binding {
        property: "x"
        restoreMode: Binding.RestoreNone
        target: overlay
        value: (overlay.parent.width - overlay.width) * 0.5
        when: !overlay.userPositioned
    }
    Binding {
        property: "y"
        restoreMode: Binding.RestoreNone
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
        mediaTime: overlay.mediaTime
        monoFontFamily: Style.monoFontFamily
        mutedColor: Style.mutedTextColor
        steeringColor: Style.steeringTelemetryColor
        store: Store
        throttleColor: Style.throttleTelemetryColor
    }
    DragHandler {
        target: null

        onActiveChanged: {
            if (active) {
                overlay.dragOriginX = overlay.x;
                overlay.dragOriginY = overlay.y;
                overlay.userPositioned = true;
            }
        }
        onTranslationChanged: {
            overlay.x = overlay.clampX(overlay.dragOriginX + translation.x);
            overlay.y = overlay.clampY(overlay.dragOriginY + translation.y);
        }
    }
    HoverHandler {
        cursorShape: Qt.SizeAllCursor
    }
}
