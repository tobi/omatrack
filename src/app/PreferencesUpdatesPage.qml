pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: updatesPage

    readonly property string availabilityText: {
        if (!Updater.supported)
            return "This build is not a portable AppImage, Windows zip, or macOS app, so GitHub updates stay off.";
        if (!Updater.enabled)
            return "Automatic checks are off. The header icon stays hidden.";
        if (Updater.available)
            return "Omatrack " + Updater.latestVersion + " is available.";
        return "You are on the latest GitHub release.";
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
                    text: "Updates"
                }
                Label {
                    color: Style.mutedTextColor
                    text: "Portable Linux AppImages, Windows zips, and macOS apps can check GitHub Releases and replace themselves."
                }
            }
            CompactButton {
                enabled: Updater.supported && Updater.enabled && !Updater.busy
                text: Updater.busy ? "Working…" : "Check now"

                onClicked: Updater.checkNow()
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 118
            border.color: Style.borderColor
            border.width: 1
            color: Style.surfaceColor
            radius: 6

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Switch {
                        Layout.preferredWidth: 42
                        checked: Updater.enabled
                        enabled: Updater.supported
                        scale: 0.7

                        onToggled: Updater.enabled = checked
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            font.bold: true
                            text: "Check GitHub for updates"
                        }
                        Label {
                            Layout.fillWidth: true
                            color: Style.mutedTextColor
                            text: "Once a day, and never before you have used the app. Later hides the prompt for a week; the header icon stays."
                            wrapMode: Text.Wrap
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    color: Updater.error !== "" ? Style.redColor : Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    text: Updater.error !== "" ? Updater.error : updatesPage.availabilityText
                    wrapMode: Text.Wrap
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 72
            border.color: Style.borderColor
            border.width: 1
            color: Style.surfaceColor
            radius: 6

            RowLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 24

                ColumnLayout {
                    spacing: 2

                    Label {
                        color: Style.mutedTextColor
                        font.pixelSize: Style.smallFontSize
                        text: "RUNNING"
                    }
                    Label {
                        font.family: Style.monoFontFamily
                        text: Updater.currentVersion
                    }
                }
                ColumnLayout {
                    spacing: 2
                    visible: Updater.available

                    Label {
                        color: Style.mutedTextColor
                        font.pixelSize: Style.smallFontSize
                        text: "AVAILABLE"
                    }
                    Label {
                        color: Style.accentColor
                        font.family: Style.monoFontFamily
                        text: Updater.latestVersion
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                CompactButton {
                    enabled: Updater.available && !Updater.busy
                    text: "Update"
                    visible: Updater.supported && Updater.available

                    onClicked: Updater.install()
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            border.color: Style.borderColor
            border.width: 1
            color: Style.surfaceColor
            implicitHeight: associationColumn.implicitHeight + 32
            radius: 6
            visible: Updater.associationCount > 0

            ColumnLayout {
                id: associationColumn

                anchors.fill: parent
                anchors.margins: 16
                spacing: 4

                Label {
                    font.bold: true
                    text: "File associations"
                }
                Repeater {
                    model: Updater.associationCount

                    delegate: CheckBox {
                        id: preferenceAssociation

                        required property int index

                        checked: Updater.associationEnabled(preferenceAssociation.index)
                        text: Updater.associationLabel(preferenceAssociation.index)

                        onToggled: Updater.setAssociationEnabled(preferenceAssociation.index, checked)
                    }
                }
            }
        }
        Item {
            Layout.fillHeight: true
        }
    }
}
