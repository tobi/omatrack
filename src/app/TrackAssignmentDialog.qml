pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    property string detectedTrack: ""
    property var matches: []
    property string query: ""
    property string selectedSlug: ""
    property string sessionKey: ""

    function openForSession(key: string): void {
        dialog.sessionKey = key;
        dialog.detectedTrack = Store.detectedTrackForSession(key);
        dialog.selectedSlug = Store.assignedTrackForSession(key);
        dialog.query = "";
        trackSearch.text = "";
        dialog.refreshMatches();
        dialog.open();
        trackSearch.forceActiveFocus();
    }
    function refreshMatches(): void {
        const needle = dialog.query.trim().toLowerCase();
        let rows = [
            {
                name: "Automatic (%1)".arg(dialog.detectedTrack || "unknown"),
                slug: "",
                search: dialog.detectedTrack || "automatic unknown"
            }
        ];
        const atlasRows = Store.trackAtlasChoices();
        for (let i = 0; i < atlasRows.length; ++i) {
            const row = atlasRows[i];
            const haystack = String(row.search || (row.name + " " + row.slug)).toLowerCase();
            if (needle === "" || haystack.indexOf(needle) !== -1)
                rows.push(row);
        }
        dialog.matches = rows;
    }
    function selectMatch(index: int): void {
        if (index < 0 || index >= dialog.matches.length)
            return;
        dialog.selectedSlug = dialog.matches[index].slug;
    }

    closePolicy: Popup.CloseOnEscape
    focus: true
    modal: true
    standardButtons: Dialog.Save | Dialog.Cancel
    title: "Assign track"
    width: 420

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Detected as %1. The full day and consecutive event days in the same folder group will use this track.".arg(dialog.detectedTrack || "unknown")
            wrapMode: Text.WordWrap
        }
        CompactTextField {
            id: trackSearch

            Layout.fillWidth: true
            placeholderText: "Search tracks we know"

            onTextEdited: {
                dialog.query = text;
                dialog.refreshMatches();
            }
        }
        ListView {
            id: trackList

            Layout.fillWidth: true
            Layout.preferredHeight: 280
            clip: true
            model: dialog.matches.length

            ScrollBar.vertical: ThinScrollBar {
            }
            delegate: Rectangle {
                id: trackChoice

                readonly property var choice: dialog.matches[trackChoice.index]
                readonly property bool chosen: (trackChoice.choice.slug || "") === dialog.selectedSlug
                required property int index

                color: trackChoice.chosen ? Style.selectionColor : trackChoiceMouse.containsMouse ? Style.surfaceColor : "transparent"
                height: 28
                width: trackList.width

                Label {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    color: trackChoice.chosen ? Style.accentColor : Style.foregroundColor
                    elide: Text.ElideRight
                    font.bold: trackChoice.chosen
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.fontSize
                    text: trackChoice.choice.name + (trackChoice.choice.slug ? "  ·  " + trackChoice.choice.slug : "")
                    verticalAlignment: Text.AlignVCenter
                }
                MouseArea {
                    id: trackChoiceMouse

                    anchors.fill: parent
                    hoverEnabled: true

                    onClicked: dialog.selectMatch(trackChoice.index)
                }
            }
        }
    }

    onAccepted: Store.setSessionTrackAssignment(dialog.sessionKey, dialog.selectedSlug)
}
