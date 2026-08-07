pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: trackDataPage

    function statusColor(): color {
        const status = Store.trackAtlasStatus.toLowerCase();
        if (status.includes("updating"))
            return Style.yellowColor;
        if (status.includes("invalid") || status.includes("failed") || status.includes("unavailable") || status.startsWith("no "))
            return Style.redColor;
        return Style.greenColor;
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    font.bold: true
                    font.pixelSize: 16
                    text: "Track data"
                }
                Label {
                    color: Style.mutedTextColor
                    text: "Authoritative track identity and corner metadata from Track Atlas."
                }
            }
            CompactButton {
                enabled: !Store.trackAtlasStatus.toLowerCase().includes("updating")
                text: Store.trackAtlasStatus.toLowerCase().includes("updating") ? "Updating…" : "Check for updates"

                onClicked: Store.refreshTrackAtlas()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 106
            border.color: Style.borderColor
            border.width: 1
            color: Style.surfaceColor
            radius: 6

            RowLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Rectangle {
                    Layout.preferredHeight: 14
                    Layout.preferredWidth: 14
                    color: trackDataPage.statusColor()
                    radius: 7
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3

                    Label {
                        font.bold: true
                        font.pixelSize: 14
                        text: "Track Atlas cache"
                    }
                    Label {
                        color: trackDataPage.statusColor()
                        font.family: Style.monoFontFamily
                        text: Store.trackAtlasStatus
                    }
                    Label {
                        color: Style.mutedTextColor
                        text: "Cached data remains available when the network is offline."
                    }
                }
                BusyIndicator {
                    Layout.preferredHeight: 26
                    Layout.preferredWidth: 26
                    running: Store.trackAtlasStatus.toLowerCase().includes("updating")
                    visible: running
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 194
            border.color: Style.borderColor
            border.width: 1
            color: Style.traceBackgroundColor
            radius: 6

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Label {
                    font.bold: true
                    text: "What the cache provides"
                }
                Repeater {
                    model: [
                        {
                            "code": "01",
                            "detail": "Canonical track and layout names across logger formats",
                            "label": "Track identity"
                        },
                        {
                            "code": "02",
                            "detail": "Driver-facing labels and analysis ranges for each turn",
                            "label": "Corner definitions"
                        },
                        {
                            "code": "03",
                            "detail": "External IDs, series hints, aliases, and lap-length matching",
                            "label": "Session matching"
                        }
                    ]

                    delegate: RowLayout {
                        id: atlasFeature

                        required property var modelData

                        Layout.fillWidth: true
                        spacing: 10

                        Rectangle {
                            Layout.preferredHeight: 24
                            Layout.preferredWidth: 30
                            border.color: Style.borderColor
                            border.width: 1
                            color: Style.surfaceColor
                            radius: 4

                            Label {
                                anchors.centerIn: parent
                                color: Style.accentColor
                                font.bold: true
                                font.family: Style.monoFontFamily
                                font.pixelSize: Style.smallFontSize
                                text: atlasFeature.modelData.code
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Label {
                                font.bold: true
                                text: atlasFeature.modelData.label
                            }
                            Label {
                                Layout.fillWidth: true
                                color: Style.mutedTextColor
                                text: atlasFeature.modelData.detail
                            }
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 92
            border.color: Style.borderColor
            border.width: 1
            color: Style.surfaceColor
            radius: 6

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 4

                Label {
                    font.bold: true
                    text: "Update policy"
                }
                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    text: "Omatrack checks the cache periodically and starts without waiting for the network. Manual corner edits remain local overrides and never rewrite Track Atlas data."
                    wrapMode: Text.Wrap
                }
            }
        }
        Item {
            Layout.fillHeight: true
        }
    }
}
