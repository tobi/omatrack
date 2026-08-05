pragma ComponentBehavior: Bound

// Desktop-sized text input.
//
// Material sizes TextField for a finger and reserves vertical room for a
// floating label the app never uses, which makes every inline edit twice as
// tall as the rows around it. Same control, desktop metrics.

import QtQuick
import QtQuick.Controls
import Racecraft

TextField {
    bottomPadding: 2
    font.pixelSize: Style.fontSize
    implicitHeight: Style.controlHeight
    leftPadding: 6
    rightPadding: 6
    selectByMouse: true
    topPadding: 2
}
