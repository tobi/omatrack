pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Span metadata card. Lives on Overlay.overlay so it paints above scene-graph
// traces, lane labels, and the cursor overlay. Follows the pointer; never
// accepts hover, or it would steal the TraceView hit-test that drives it.
Rectangle {
    id: card

    property color accent: "transparent"
    property Item anchorItem: null
    property real localX: 0
    property real localY: 0
    property var metaRows: []
    property string subtitleText: ""
    property string titleText: ""

    function follow(item: Item, x: real, y: real): void {
        card.anchorItem = item;
        card.localX = x;
        card.localY = y;
        card.reposition();
    }
    function reposition(): void {
        const layer = Overlay.overlay;
        if (!layer || !card.anchorItem || !card.visible)
            return;
        const point = card.anchorItem.mapToItem(layer, card.localX, card.localY);
        let nextX = point.x + 16;
        let nextY = point.y + 16;
        if (nextX + card.width > layer.width - 8)
            nextX = point.x - card.width - 12;
        if (nextY + card.height > layer.height - 8)
            nextY = point.y - card.height - 12;
        card.x = Math.max(8, nextX);
        card.y = Math.max(8, nextY);
    }

    border.color: Style.borderColor
    border.width: 1
    color: Style.darkBackgroundColor
    enabled: false
    height: body.implicitHeight + 12
    parent: Overlay.overlay
    radius: 4
    width: 260
    z: 100

    onHeightChanged: card.reposition()
    onVisibleChanged: {
        if (card.visible)
            card.reposition();
    }
    onWidthChanged: card.reposition()

    Rectangle {
        color: card.accent
        height: parent.height
        radius: 4
        width: 3
    }
    ColumnLayout {
        id: body

        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.top: parent.top
        anchors.topMargin: 6
        spacing: 2

        Label {
            Layout.fillWidth: true
            color: Style.foregroundColor
            font.bold: true
            font.pixelSize: Style.fontSize
            text: card.titleText
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.pixelSize: Style.smallFontSize
            text: card.subtitleText
            visible: card.subtitleText !== ""
            wrapMode: Text.Wrap
        }
        Repeater {
            model: card.metaRows

            delegate: RowLayout {
                id: metaRow

                required property string name
                required property string value

                Layout.fillWidth: true
                spacing: 10

                Label {
                    Layout.alignment: Qt.AlignTop
                    Layout.preferredWidth: 80
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: metaRow.name
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    color: Style.foregroundColor
                    font.pixelSize: Style.smallFontSize
                    text: metaRow.value
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
