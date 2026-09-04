pragma ComponentBehavior: Bound
import Omatrack
import QtQuick
import QtQuick.Controls

Rectangle {
    id: context

    property bool alignRight: false
    required property bool reference
    readonly property color roleColor: context.reference ? Style.orangeColor : Style.accentColor

    color: Style.videoControlBackgroundColor
    implicitHeight: 42
    objectName: context.reference ? "referenceVideoContext" : "activeVideoContext"
    radius: 3

    Column {
        anchors.fill: parent
        anchors.margins: 5
        spacing: 2

        Label {
            color: context.roleColor
            elide: Text.ElideRight
            font.bold: true
            font.pixelSize: 13
            horizontalAlignment: context.alignRight ? Text.AlignRight : Text.AlignLeft
            text: (context.reference ? "REF · " : "ACTIVE · ") + ((context.reference ? Store.compareDriverName : Store.primaryDriverName) || "—")
            width: parent.width
        }
        Label {
            color: Style.mutedTextColor
            elide: Text.ElideRight
            font.family: Style.monoFontFamily
            font.pixelSize: 10
            horizontalAlignment: context.alignRight ? Text.AlignRight : Text.AlignLeft
            text: "LAP " + (context.reference ? Store.compareLapOrdinal : Store.primaryLapOrdinal) + "/" + (context.reference ? Store.compareLapTotal : Store.primaryLapTotal) + " · FUEL " + ((context.reference ? Store.compareFuelLoad : Store.primaryFuelLoad) || "—")
            width: parent.width
        }
    }
}
