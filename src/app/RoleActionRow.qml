pragma ComponentBehavior: Bound
import Omatrack

// Primary/reference role dots shared by the file and session tree delegates.
//
// Both delegates render the same pair: an accent dot for "make current" and
// an orange dot for "make reference / clear reference." The dots sit above
// the full-row MouseArea so they capture their own clicks instead of
// activating the row.

import QtQuick

Row {
    id: dots

    property int dotSize: 14
    property bool primarySelected: false
    property bool primaryVisible: true
    property bool referenceSelected: false
    property bool referenceVisible: true

    signal primaryActivated
    signal referenceActivated

    spacing: 8

    RoleDot {
        activeColor: Style.accentColor
        selected: dots.primarySelected
        size: dots.dotSize
        tip: "Make current lap"
        visible: dots.primaryVisible

        onActivated: dots.primaryActivated()
    }
    RoleDot {
        activeColor: Style.orangeColor
        selected: dots.referenceSelected
        size: dots.dotSize
        tip: dots.referenceSelected ? "Clear reference" : "Make reference lap"
        visible: dots.referenceVisible

        onActivated: dots.referenceActivated()
    }
}
