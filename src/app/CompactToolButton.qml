pragma ComponentBehavior: Bound

// Compact toolbar button. `tip` is the accessible name; it is not shown as a
// hover tooltip.

import QtQuick
import QtQuick.Controls

ToolButton {
    id: control

    property string tip: ""

    Accessible.name: control.tip !== "" ? control.tip : control.text
}
