pragma ComponentBehavior: Bound

// Compact scrollbar for dense lists.
//
// Material's ScrollBar reserves touch-sized padding around a 9 px handle,
// which eats horizontal room in every sidebar and inspector list. This keeps
// the same behaviour and hover affordance at a desktop scale, and fades out
// when the view is idle so the data keeps the space.

import QtQuick
import QtQuick.Controls
import Racecraft

ScrollBar {
    id: bar

    bottomPadding: 0
    leftPadding: 0

    // Only worth showing when the view can actually scroll.
    policy: size < 1 ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
    rightPadding: 0
    topPadding: 0

    background: Item {
    }
    contentItem: Rectangle {
        color: bar.pressed ? Style.accentColor : Style.borderColor
        implicitHeight: Style.scrollBarWidth
        implicitWidth: Style.scrollBarWidth
        opacity: bar.pressed || bar.hovered ? 0.9 : 0.45
        radius: width / 2

        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }
    }
}
