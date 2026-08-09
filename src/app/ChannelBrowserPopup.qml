pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Popup {
    id: browser

    required property string destinationLabel
    required property string expectedUnit
    required property var rows

    signal channelChosen(string value)

    function detailText(row: var): string {
        let parts = [];
        parts.push(row.unit ? "Unit " + row.unit : "Unit unknown");
        if (row.frequencyHz > 0)
            parts.push(Number(row.frequencyHz).toLocaleString(Qt.locale(), 'f', row.frequencyHz >= 10 ? 0 : 1) + " Hz");
        if (row.recordingCount > 1)
            parts.push("Seen in " + row.recordingCount + " videos");
        if (row.examples && row.examples.length > 0)
            parts.push("Examples  " + row.examples.join("   "));
        else if (!row.available)
            parts.push("TRACK.yml suggestion; not present in this video");
        return parts.join("  ·  ");
    }
    function openBelow(anchorItem: Item, preferredWidth: real): void {
        const point = anchorItem.mapToItem(Overlay.overlay, 0, anchorItem.height + 2);
        browser.x = point.x;
        browser.y = point.y;
        browser.width = Math.max(480, Math.min(preferredWidth, Overlay.overlay.width - point.x - 12));
        browser.open();
    }

    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    height: Math.min(372, 52 + browser.rows.length * 58)
    padding: 1
    parent: Overlay.overlay

    background: Rectangle {
        border.color: Style.borderColor
        border.width: 1
        color: Style.darkBackgroundColor
        radius: 5
    }
    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            color: Style.surfaceColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 10
                spacing: 10

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        color: Style.foregroundColor
                        font.bold: true
                        text: "Channel browser — " + browser.destinationLabel
                    }
                    Label {
                        Layout.fillWidth: true
                        color: Style.dimTextColor
                        font.pixelSize: Style.smallFontSize
                        text: "Type above to filter by name, unit, or example value; matching units rank first"
                    }
                }
                Label {
                    color: Style.accentColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    text: "TARGET  " + browser.expectedUnit
                }
                Label {
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    text: browser.rows.length + (browser.rows.length === 1 ? " channel" : " channels")
                }
            }
        }
        ListView {
            id: channelList

            Layout.fillHeight: true
            Layout.fillWidth: true
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            model: browser.rows

            ScrollBar.vertical: ThinScrollBar {
            }
            delegate: ItemDelegate {
                id: channelRow

                required property int index
                required property var modelData

                font.family: Style.monoFontFamily
                height: 58
                highlighted: hovered
                width: ListView.view.width

                contentItem: RowLayout {
                    spacing: 10

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                color: Style.foregroundColor
                                elide: Text.ElideRight
                                font.bold: true
                                font.family: Style.monoFontFamily
                                text: channelRow.modelData.value
                            }
                            Label {
                                color: Style.greenColor
                                font.family: Style.monoFontFamily
                                font.pixelSize: Style.smallFontSize
                                text: "AUTO"
                                visible: channelRow.modelData.automatic === true
                            }
                            Label {
                                color: Style.blueColor
                                font.family: Style.monoFontFamily
                                font.pixelSize: Style.smallFontSize
                                text: "TRACK ×" + channelRow.modelData.historicalCount
                                visible: channelRow.modelData.historicalCount > 0
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            color: channelRow.modelData.available ? Style.mutedTextColor : Style.dimTextColor
                            elide: Text.ElideRight
                            font.family: Style.monoFontFamily
                            font.pixelSize: Style.smallFontSize
                            text: browser.detailText(channelRow.modelData)
                        }
                    }
                    Label {
                        color: channelRow.modelData.unitCompatible ? Style.greenColor : channelRow.modelData.unit ? Style.accentColor : Style.dimTextColor
                        font.bold: true
                        font.family: Style.monoFontFamily
                        text: channelRow.modelData.unit || "—"
                    }
                }

                onClicked: browser.channelChosen(channelRow.modelData.value)
            }
        }
    }
}
