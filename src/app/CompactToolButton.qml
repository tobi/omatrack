pragma ComponentBehavior: Bound

// ToolButton with a wired ToolTip. The raw ToolButton repeats
// `ToolTip.text` / `ToolTip.visible: hovered` on every call site in the
// toolbar and header; this component folds that into a single `tip` property
// so the callers read as intent, not boilerplate.

import QtQuick
import QtQuick.Controls

ToolButton {
    id: control

    property string tip: ""

    ToolTip.text: control.tip
    ToolTip.visible: hovered && control.tip !== ""
}
