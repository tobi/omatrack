pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Modeless preferences workspace. Each section owns its domain state and
// applies changes immediately; the shell only owns navigation.

ApplicationWindow {
    id: preferencesWindow

    property int currentSection: 0

    function refresh(): void {
        libraryPage.refresh();
        driversPage.refresh();
    }

    Material.accent: Style.accentColor
    Material.background: Style.backgroundColor
    Material.foreground: Style.foregroundColor
    Material.primary: Style.surfaceColor
    Material.theme: Material.Dark
    color: Style.backgroundColor
    font.family: Style.uiFontFamily
    font.pixelSize: Style.fontSize
    height: 640
    minimumHeight: 520
    minimumWidth: 760
    objectName: "settingsWindow"
    title: "Omatrack Preferences"
    visible: false
    width: 920

    Shortcut {
        sequences: [StandardKey.Close]

        onActivated: preferencesWindow.hide()
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 68
            border.color: Style.borderColor
            border.width: 1
            color: Style.surfaceColor

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 16
                spacing: 12

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1

                    Label {
                        font.bold: true
                        font.pixelSize: 18
                        text: "Preferences"
                    }
                    Label {
                        color: Style.mutedTextColor
                        text: "Data sources, driver identity, track metadata, and updates"
                    }
                }
                CompactButton {
                    text: "Done"

                    onClicked: preferencesWindow.hide()
                }
            }
        }
        RowLayout {
            Layout.fillHeight: true
            Layout.fillWidth: true
            spacing: 0

            Rectangle {
                Layout.fillHeight: true
                Layout.preferredWidth: 190
                border.color: Style.borderColor
                border.width: 1
                color: Style.darkBackgroundColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    Label {
                        Layout.bottomMargin: 4
                        Layout.leftMargin: 8
                        color: Style.dimTextColor
                        font.bold: true
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: "SETTINGS"
                    }
                    Repeater {
                        delegate: Button {
                            id: sectionButton

                            required property string detail
                            required property int index
                            required property string label

                            Layout.fillWidth: true
                            Layout.preferredHeight: 54
                            bottomPadding: 7
                            checkable: true
                            checked: preferencesWindow.currentSection === sectionButton.index
                            flat: true
                            leftPadding: 12
                            rightPadding: 8
                            topPadding: 7

                            background: Rectangle {
                                border.color: sectionButton.checked ? Style.accentColor : "transparent"
                                border.width: 1
                                color: sectionButton.checked ? Style.selectionColor : "transparent"
                                opacity: sectionButton.checked ? 0.22 : 1
                                radius: 5
                            }
                            contentItem: Column {
                                spacing: 2

                                Label {
                                    color: sectionButton.checked ? Style.accentColor : Style.foregroundColor
                                    font.bold: sectionButton.checked
                                    text: sectionButton.label
                                    width: parent.width
                                }
                                Label {
                                    color: Style.mutedTextColor
                                    font.pixelSize: Style.smallFontSize
                                    text: sectionButton.detail
                                    width: parent.width
                                }
                            }

                            onClicked: preferencesWindow.currentSection = sectionButton.index
                        }
                        model: ListModel {
                            ListElement {
                                detail: "Folders and connections"
                                label: "Telemetry library"
                            }
                            ListElement {
                                detail: "Names across sessions"
                                label: "Drivers"
                            }
                            ListElement {
                                detail: "Track Atlas cache"
                                label: "Track data"
                            }
                            ListElement {
                                detail: "Track and session"
                                label: "Event"
                            }
                            ListElement {
                                detail: "Reference HUD traces"
                                label: "Overlay"
                            }
                            ListElement {
                                detail: "GitHub AppImage"
                                label: "Updates"
                            }
                        }
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: 8
                        Layout.rightMargin: 8
                        color: Style.dimTextColor
                        elide: Text.ElideMiddle
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: Store.configFilePath()
                    }
                }
            }
            StackLayout {
                Layout.fillHeight: true
                Layout.fillWidth: true
                currentIndex: preferencesWindow.currentSection

                PreferencesLibraryPage {
                    id: libraryPage

                }
                PreferencesDriversPage {
                    id: driversPage

                }
                PreferencesTrackDataPage {
                }
                PreferencesEventPage {
                }
                PreferencesOverlayPage {
                }
                PreferencesUpdatesPage {
                }
            }
        }
    }
}
