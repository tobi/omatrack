pragma ComponentBehavior: Bound
import Omatrack

// Draggable card chrome shared by the floating overlays.
//
// Provides the card visual (border, background, radius, clip) and a drag
// gesture that emits signals the caller clamps in its own coordinate space:
// CornerFocusOverlay uses a Translate transform; VideoDeltaBar sets x/y
// directly; SpanHoverCard disables drag and follows the pointer.
//
// The caller saves its own drag origin on dragBegun and applies translation
// on dragMoved — the clamping strategy differs per surface, so it stays in
// the caller.

import QtQuick

Rectangle {
    id: card

    property bool dragEnabled: true

    signal dragBegun
    signal dragMoved(real x, real y)

    border.color: Style.borderColor
    border.width: 1
    clip: true
    color: Style.surfaceColor
    radius: 6

    DragHandler {
        enabled: card.dragEnabled
        target: null

        onActiveChanged: {
            if (active)
                card.dragBegun();
        }
        onTranslationChanged: {
            card.dragMoved(translation.x, translation.y);
        }
    }
    HoverHandler {
        cursorShape: Qt.SizeAllCursor
        enabled: card.dragEnabled
    }
}
