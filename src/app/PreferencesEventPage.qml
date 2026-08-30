pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: eventPage

    objectName: "preferencesEventPage"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        Label {
            font.bold: true
            font.pixelSize: 16
            text: "Current event"
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "When enabled, the sidebar shows one track and one day, and USB copy / Lua rename receive this session name."
            wrapMode: Text.Wrap
        }
        Switch {
            checked: Store.eventMode
            text: "Event mode"

            onToggled: Store.eventMode = checked
        }
        Label {
            text: "Track"
        }
        ComboBox {
            id: eventTrack

            Layout.fillWidth: true
            Layout.preferredHeight: Style.controlHeight
            currentIndex: Math.max(0, eventTrack.model.indexOf(Store.eventTrack))
            model: [""].concat(Store.library.trackPills)

            onActivated: index => Store.eventTrack = index <= 0 ? "" : eventTrack.model[index]
        }
        Label {
            text: "Session name"
        }
        CompactTextField {
            Layout.fillWidth: true
            placeholderText: "c1"
            text: Store.eventSession

            onEditingFinished: Store.eventSession = text.trim()
        }
        Label {
            text: "Event date"
        }
        CompactTextField {
            Layout.fillWidth: true
            placeholderText: "YYYY-MM-DD"
            text: Store.eventDate

            onEditingFinished: Store.eventDate = text.trim()
        }
        Item {
            Layout.fillHeight: true
        }
    }
}
