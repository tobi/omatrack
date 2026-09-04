pragma ComponentBehavior: Bound
import Omatrack
import QtQuick

Item {
    id: overlay

    readonly property real aspectRatio: 0.21
    readonly property real availableX: Math.max(0, overlay.parent.width - overlay.width)
    readonly property real availableY: Math.max(0, overlay.parent.height - overlay.bottomInset - overlay.height)
    property real bottomInset: 0
    property real dragOriginX: 0
    property real dragOriginY: 0
    property point dragPosition: Qt.point(0, 0)
    property real mediaTime: 0
    readonly property real scaleFactor: 0.65
    readonly property real unscaledWidth: Math.min(overlay.parent.width - 16, 1000, Math.max(520, overlay.parent.width * 0.72))
    readonly property bool userPositioned: Store.videoHudPosition.x >= 0 && Store.videoHudPosition.y >= 0

    function clampX(value: real): real {
        return Math.max(0, Math.min(overlay.availableX, value));
    }
    function clampY(value: real): real {
        return Math.max(0, Math.min(overlay.availableY, value));
    }
    function savePosition(x: real, y: real): void {
        Store.setVideoHudPosition(overlay.availableX > 0 ? overlay.clampX(x) / overlay.availableX : 0.5, overlay.availableY > 0 ? overlay.clampY(y) / overlay.availableY : 0.5);
    }

    height: overlay.width * overlay.aspectRatio
    objectName: "videoTelemetryOverlay"
    width: Math.max(0, overlay.unscaledWidth * overlay.scaleFactor)
    z: 8

    Binding {
        property: "x"
        restoreMode: Binding.RestoreNone
        target: overlay
        value: overlay.userPositioned ? Store.videoHudPosition.x * overlay.availableX : overlay.availableX * 0.5
        when: !hudDrag.active
    }
    Binding {
        property: "y"
        restoreMode: Binding.RestoreNone
        target: overlay
        value: overlay.userPositioned ? Store.videoHudPosition.y * overlay.availableY : overlay.clampY(overlay.parent.height * 0.9 - overlay.height * 0.5)
        when: !hudDrag.active
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
        id: hudDrag

        target: null

        onActiveChanged: {
            if (hudDrag.active) {
                overlay.dragOriginX = overlay.x;
                overlay.dragOriginY = overlay.y;
                overlay.dragPosition = Qt.point(overlay.x, overlay.y);
            } else {
                // Use the last drag point, not x/y: release re-enables the
                // saved-position bindings in this same event-loop turn.
                overlay.savePosition(overlay.dragPosition.x, overlay.dragPosition.y);
            }
        }
        onTranslationChanged: {
            if (!hudDrag.active)
                return;
            overlay.dragPosition = Qt.point(overlay.clampX(overlay.dragOriginX + hudDrag.translation.x), overlay.clampY(overlay.dragOriginY + hudDrag.translation.y));
            overlay.x = overlay.dragPosition.x;
            overlay.y = overlay.dragPosition.y;
        }
    }
    HoverHandler {
        cursorShape: Qt.SizeAllCursor
    }
}
