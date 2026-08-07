pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dialog

    property var choices: []
    property string detectedTrack: ""
    property string sessionKey: ""

    function choiceIndex(slug: string): int {
        for (let i = 0; i < dialog.choices.length; ++i)
            if (dialog.choices[i].slug === slug)
                return i;
        return 0;
    }
    function openForSession(key: string): void {
        dialog.sessionKey = key;
        dialog.detectedTrack = Store.detectedTrackForSession(key);
        const rows = [
            {
                name: "Automatic (%1)".arg(dialog.detectedTrack || "unknown"),
                slug: ""
            }
        ];
        const atlasRows = Store.trackAtlasChoices();
        for (let i = 0; i < atlasRows.length; ++i)
            rows.push(atlasRows[i]);
        dialog.choices = rows;
        trackPicker.currentIndex = dialog.choiceIndex(Store.assignedTrackForSession(key));
        dialog.open();
    }

    closePolicy: Popup.CloseOnEscape
    focus: true
    modal: true
    standardButtons: Dialog.Save | Dialog.Cancel
    title: "Assign track"

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Detected as %1. The full day and consecutive event days in the same folder group will use this track.".arg(dialog.detectedTrack || "unknown")
            wrapMode: Text.WordWrap
        }
        ComboBox {
            id: trackPicker

            Layout.fillWidth: true
            model: dialog.choices
            textRole: "name"
        }
    }

    onAccepted: {
        if (trackPicker.currentIndex < 0 || trackPicker.currentIndex >= dialog.choices.length)
            return;
        Store.setSessionTrackAssignment(dialog.sessionKey, dialog.choices[trackPicker.currentIndex].slug);
    }
}
