pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: imagePage

    required property ImageModelManager modelManager

    signal chooseLocalModel

    function enableAndDownload(): void {
        Store.imageModelManaged = true;
        Store.imageTelemetryEnabled = true;
        imagePage.modelManager.downloadLatest();
    }

    clip: true
    contentWidth: imagePage.availableWidth
    objectName: "preferencesImageTelemetryPage"
    padding: 20

    ColumnLayout {
        spacing: 10
        width: imagePage.availableWidth

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.fillWidth: true
                font.bold: true
                font.pixelSize: Style.fontSize + 3
                text: "Image telemetry"
            }
            CompactButton {
                objectName: "imageLocalModelButton"
                text: "Choose local model…"

                onClicked: imagePage.chooseLocalModel()
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Read gear, displayed lap count and pedal-bar fill from the reviewed 1080p orange AiM overlay. Native telemetry stays first; unsupported layouts remain unknown."
            wrapMode: Text.Wrap
        }
        CheckBox {
            id: extractionToggle

            checked: Store.imageTelemetryEnabled
            objectName: "imageTelemetryEnabledPreference"
            text: "Extract telemetry from supported videos"

            onToggled: Store.imageTelemetryEnabled = extractionToggle.checked
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Extraction runs locally. Turning it on does not consent to downloads; a compatible local, staged or downloaded model is required."
            wrapMode: Text.Wrap
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Style.borderColor
        }
        Label {
            font.bold: true
            text: "Managed reader"
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Enable & download fetches the reader (~2.2 MB) and update metadata from Hugging Face: tobil/omatrack-telemetry-reader. No account or token is needed. Your video, images and telemetry are never uploaded."
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Managed downloads are off. Enabling them also enables extraction and allows the managed reader to replace the current model selection when applied."
            visible: !Store.imageModelManaged
            wrapMode: Text.Wrap
        }
        CompactButton {
            enabled: !imagePage.modelManager.busy
            objectName: "imageModelConsentButton"
            text: "Enable & download reader (~2.2 MB)"
            visible: !Store.imageModelManaged

            onClicked: imagePage.enableAndDownload()
        }
        CheckBox {
            id: updatesToggle

            checked: Store.imageModelUpdates
            enabled: Store.imageModelManaged
            objectName: "imageModelUpdatesToggle"
            text: "Keep reader up to date"

            onToggled: Store.imageModelUpdates = updatesToggle.checked
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "After opt-in, automatic checks run at most daily. Updates download in the background and wait until no video or extraction is active before applying. Turn this off to check and download manually."
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            font.family: Style.monoFontFamily
            text: "Installed managed version: " + (imagePage.modelManager.installedVersion || "Not downloaded")
            textFormat: Text.PlainText
            visible: Store.imageModelManaged
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.accentColor
            text: "Update available: " + imagePage.modelManager.availableVersion
            textFormat: Text.PlainText
            visible: Store.imageModelManaged && imagePage.modelManager.updateAvailable && !imagePage.modelManager.readyToApply
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            objectName: "imageModelStatus"
            text: Store.imageModelManaged ? imagePage.modelManager.status : "No managed model network requests. Local and staged models remain available."
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
        ProgressBar {
            Layout.fillWidth: true
            from: 0
            indeterminate: imagePage.modelManager.busy && imagePage.modelManager.progress < 0
            objectName: "imageModelProgress"
            to: 1
            value: Math.max(0, imagePage.modelManager.progress)
            visible: Store.imageModelManaged && imagePage.modelManager.busy
        }
        Label {
            Layout.fillWidth: true
            color: Style.redColor
            objectName: "imageModelError"
            text: imagePage.modelManager.error
            textFormat: Text.PlainText
            visible: Store.imageModelManaged && imagePage.modelManager.error !== ""
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: Store.imageModelManaged

            CompactButton {
                enabled: !imagePage.modelManager.busy
                objectName: "imageModelCheckButton"
                text: "Check now"

                onClicked: imagePage.modelManager.checkForUpdates()
            }
            CompactButton {
                enabled: !imagePage.modelManager.busy
                objectName: "imageModelDownloadButton"
                text: imagePage.modelManager.updateAvailable ? "Download update" : "Retry download"
                visible: !imagePage.modelManager.readyToApply && (imagePage.modelManager.updateAvailable || imagePage.modelManager.installedVersion === "" || imagePage.modelManager.error !== "")

                onClicked: imagePage.modelManager.downloadLatest()
            }
            CompactButton {
                objectName: "imageModelCancelButton"
                text: "Cancel"
                visible: imagePage.modelManager.busy

                onClicked: imagePage.modelManager.cancel()
            }
            Item {
                Layout.fillWidth: true
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.accentColor
            text: "Ready to apply: " + imagePage.modelManager.pendingVersion + (imagePage.modelManager.activationBlocked ? ". Automatic activation is waiting for the current video or extraction to close." : ".")
            textFormat: Text.PlainText
            visible: Store.imageModelManaged && imagePage.modelManager.readyToApply
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Apply now switches the active model, restarts extraction and uses a new prediction-cache identity if the model changed. It can interrupt collection for an open video."
            visible: Store.imageModelManaged && imagePage.modelManager.readyToApply
            wrapMode: Text.Wrap
        }
        CompactButton {
            enabled: !imagePage.modelManager.busy
            objectName: "imageModelApplyButton"
            text: "Apply now"
            visible: Store.imageModelManaged && imagePage.modelManager.readyToApply

            onClicked: imagePage.modelManager.applyPending()
        }
        CompactButton {
            objectName: "imageModelOptOutButton"
            text: "Stop managed downloads"
            visible: Store.imageModelManaged

            onClicked: Store.imageModelManaged = false
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Style.borderColor
        }
        Label {
            font.bold: true
            text: "Local model"
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            text: "Selecting a trusted local ONNX model stops managed downloads and prevents pending updates from replacing your selection. It does not rewrite the model file."
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            elide: Text.ElideMiddle
            font.family: Style.monoFontFamily
            text: Store.imageTelemetryModel || "Staged model beside the application, if installed"
            textFormat: Text.PlainText
        }
    }
}
