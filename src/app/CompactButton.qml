pragma ComponentBehavior: Bound

// Desktop-sized push button.
//
// Material buttons are sized for touch targets; in a dense inspector they
// dominate rows they only support. Same behaviour, desktop metrics.

import QtQuick
import QtQuick.Controls
import Racecraft

Button {
    bottomPadding: 0
    font.capitalization: Font.MixedCase
    font.pixelSize: Style.fontSize
    implicitHeight: Style.controlHeight
    leftPadding: Style.controlPadding
    rightPadding: Style.controlPadding
    topPadding: 0
}
