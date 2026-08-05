import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Qt.labs.platform as Platform
import QtQuick.Window
import Racecraft

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: "Racecraft"
    SystemPalette {
        id: desktopPalette
        colorGroup: SystemPalette.Active
    }

    readonly property color backgroundColor:
        omarchyColors.background || desktopPalette.window
    readonly property color darkBackgroundColor:
        omarchyColors.dark_background || desktopPalette.alternateBase
    readonly property color traceBackgroundColor:
        omarchyColors.darker_background || desktopPalette.base
    readonly property color surfaceColor:
        omarchyColors.lighter_background || desktopPalette.button
    readonly property color selectionColor:
        omarchyColors.selection || desktopPalette.highlight
    readonly property color borderColor:
        omarchyColors.muted || desktopPalette.mid
    readonly property color foregroundColor:
        omarchyColors.foreground || desktopPalette.windowText
    readonly property color mutedTextColor:
        omarchyColors.light_foreground || desktopPalette.midlight
    readonly property color dimTextColor:
        omarchyColors.dark_foreground || desktopPalette.mid
    readonly property color accentColor:
        omarchyColors.accent || desktopPalette.highlight
    readonly property color greenColor:
        omarchyColors.green || "#a7c080"
    readonly property color yellowColor:
        omarchyColors.yellow || "#dbbc7f"
    readonly property color orangeColor:
        omarchyColors.orange || "#e09d7f"
    readonly property color redColor:
        omarchyColors.red || "#e67e80"
    readonly property color magentaColor:
        omarchyColors.magenta || "#d699b6"

    Material.theme: Material.Dark
    Material.primary: surfaceColor
    Material.accent: accentColor
    Material.background: backgroundColor
    Material.foreground: foregroundColor
    color: backgroundColor
    readonly property string uiFontFamily: "Geist"
    font.family: uiFontFamily
    font.pixelSize: 11

    // alias so TraceView can bind without its own `store` property shadowing
    // the context property
    property var appStore: store
    property bool sidebarVisible: true
    property string activeSessionKey: ""
    property string activeSessionName: ""
    property string referenceSessionKey: ""
    property string referenceSessionName: ""
    property var expandedDates: ({})
    property var channelRows: []
    property var cornerRows: []
    property var cornerZoneRows: []
    property var aliasRows: []
    property var mappingRows: []
    property var filmstripSessions: []
    property var alignmentRows: ({})
    readonly property var colorChoices: ["#a7c080", "#7fbbb3", "#e67e80",
                                         "#dbbc7f", "#d699b6", "#e09d7f",
                                         "#d3c6aa", "#9da9a0"]
    property var directoryRows: []
    property bool videoVisible: false
    property bool telemetryVideoActive: false
    onWidthChanged: {
        if (videoVisible && width < 1000) sidebarVisible = false
    }

    ListModel { id: treeModel }
    property var expandedTracks: ({})

    function trackExpanded(name) { return expandedTracks[name] !== false }
    function dateExpanded(key) { return expandedDates[key] !== false }
    function refreshChannelRows() {
        const settings = store.channelSettings()
        let rows = []
        for (let i = 0; i < settings.length; ++i)
            if (settings[i].key !== "delta") rows.push(settings[i])
        channelRows = rows
    }
    function refreshCornerRows() { cornerRows = store.cornerComparison() }
    function refreshCornerZones() { cornerZoneRows = store.cornerList() }
    function openCornerRename(index) {
        const zones = store.cornerList()
        if (index < 0 || index >= zones.length) return
        cornerRenameDialog.cornerIndex = index
        cornerRenameField.text = zones[index].name || ""
        cornerRenameDialog.open()
    }
    function refreshAliasRows() { aliasRows = store.driverAliases() }
    function refreshMappingRows() { mappingRows = store.driverMappings() }
    function refreshDirectoryRows() { directoryRows = store.sessionDirectories() }
    function openDriverRename(mappingKey, displayName) {
        if (!mappingKey) return
        driverRenameDialog.mappingKey = mappingKey
        driverRenameField.text = displayName || ""
        driverRenameDialog.open()
    }
    function dismissCornerPopover() {
        cornerWindow.hide()
    }
    function sessionNameForKey(key) {
        for (let i = 0; i < treeModel.count; ++i) {
            const row = treeModel.get(i)
            if (row.role === "session" && row.key === key) return row.name
        }
        return ""
    }
    function sessionInfoForKey(key) {
        if (key === "") return null
        const groups = store.trackGroups()
        for (let t = 0; t < groups.length; ++t) {
            const dates = groups[t].dates
            for (let d = 0; d < dates.length; ++d) {
                const sessions = dates[d].sessions
                for (let s = 0; s < sessions.length; ++s)
                    if (sessions[s].key === key) return sessions[s]
            }
        }
        return null
    }
    function stripBadgeText(strip, lapTime) {
        let parts = [strip.reference ? "⇄ REF" : "RUN"]
        if (strip.driverName !== "" && strip.driverName !== "Unknown")
            parts.push(strip.driverName)
        if (lapTime !== "") parts.push(lapTime)
        return parts.join(" · ")
    }
    function bestLapForSession(key) {
        const laps = key !== "" ? store.lapsForSession(key) : []
        for (let i = 0; i < laps.length; ++i)
            if (laps[i].isFastest) return laps[i]
        for (let i = 0; i < laps.length; ++i)
            if (laps[i].countsForBest) return laps[i]
        for (let i = 0; i < laps.length; ++i)
            if (laps[i].isComplete) return laps[i]
        return laps.length > 0 ? laps[0] : null
    }
    function refreshLapStrip() {
        activeSessionKey = store.primarySessionKey
        referenceSessionKey = store.compareSessionKey
        activeSessionName = sessionNameForKey(activeSessionKey)
        referenceSessionName = sessionNameForKey(referenceSessionKey)
        let strips = []
        function appendStrip(key, reference) {
            if (key === "") return
            const laps = store.lapsForSession(key)
            const info = sessionInfoForKey(key)
            let total = 0
            for (let i = 0; i < laps.length; ++i)
                total += Math.max(1, laps[i].timeMs)
            strips.push({
                sessionKey: key,
                runName: info && info.stem !== "" ? info.stem
                                                  : sessionNameForKey(key),
                driverName: store.driverDisplayName(key),
                bestTime: info ? info.bestTime : "",
                reference: reference,
                laps: laps,
                totalTimeMs: Math.max(1, total)
            })
        }
        if (referenceSessionKey !== "" &&
            referenceSessionKey !== activeSessionKey)
            appendStrip(referenceSessionKey, true)
        appendStrip(activeSessionKey, false)
        filmstripSessions = strips
    }
    function refreshAlignmentData() { alignmentRows = store.alignmentData() }
    function paintDamperStrip(canvas, samples, color, shift, opacity) {
        const ctx = canvas.getContext("2d")
        ctx.clearRect(0, 0, canvas.width, canvas.height)
        if (!samples || samples.length < 2) return
        const low = alignmentRows.min
        const high = alignmentRows.max
        const span = Math.max(0.000001, high - low)
        ctx.globalAlpha = opacity === undefined ? 0.7 : opacity
        ctx.strokeStyle = color
        ctx.lineWidth = 1.25
        ctx.beginPath()
        for (let i = 0; i < samples.length; ++i) {
            const x = i / (samples.length - 1) * canvas.width +
                      shift * canvas.width
            const y = canvas.height -
                      (samples[i] - low) / span * canvas.height
            if (i === 0) ctx.moveTo(x, y)
            else ctx.lineTo(x, y)
        }
        ctx.stroke()
        ctx.globalAlpha = 1
    }
    function paintCornerDamperStrip(canvas, primary, compare, shiftMeters,
                                    windowMeters, cornerStartMeters) {
        const ctx = canvas.getContext("2d")
        ctx.clearRect(0, 0, canvas.width, canvas.height)
        const a = primary || []
        const b = compare || []
        if (a.length < 2 && b.length < 2) return
        let low = Number.POSITIVE_INFINITY
        let high = Number.NEGATIVE_INFINITY
        for (let i = 0; i < a.length; ++i) {
            low = Math.min(low, a[i])
            high = Math.max(high, a[i])
        }
        for (let i = 0; i < b.length; ++i) {
            low = Math.min(low, b[i])
            high = Math.max(high, b[i])
        }
        if (!isFinite(low) || !isFinite(high) || high <= low) {
            low = 0
            high = 1
        }
        const span = Math.max(0.000001, high - low)
        ctx.fillStyle = root.traceBackgroundColor
        ctx.fillRect(0, 0, canvas.width, canvas.height)
        ctx.strokeStyle = root.borderColor
        ctx.lineWidth = 1
        for (let g = 1; g < 4; ++g) {
            const y = canvas.height * g / 4
            ctx.beginPath()
            ctx.moveTo(0, y)
            ctx.lineTo(canvas.width, y)
            ctx.stroke()
        }
        const cornerX = Math.max(0, Math.min(1,
            cornerStartMeters / Math.max(1, windowMeters))) * canvas.width
        ctx.strokeStyle = root.accentColor
        ctx.setLineDash([4, 3])
        ctx.beginPath()
        ctx.moveTo(cornerX, 0)
        ctx.lineTo(cornerX, canvas.height)
        ctx.stroke()
        ctx.setLineDash([])
        ctx.fillStyle = root.accentColor
        ctx.font = "bold 8px 'Geist Mono'"
        ctx.fillText("CORNER START", Math.min(canvas.width - 86,
                                                cornerX + 4), 11)

        function draw(values, color, shift, opacity) {
            if (!values || values.length < 2) return
            ctx.globalAlpha = opacity
            ctx.strokeStyle = color
            ctx.lineWidth = 1.35
            ctx.beginPath()
            for (let i = 0; i < values.length; ++i) {
                const x = i / (values.length - 1) * canvas.width +
                          shift / Math.max(1, windowMeters) * canvas.width
                const y = canvas.height -
                          (values[i] - low) / span * canvas.height
                if (i === 0) ctx.moveTo(x, y)
                else ctx.lineTo(x, y)
            }
            ctx.stroke()
        }
        draw(b, root.orangeColor, shiftMeters, 0.58)
        draw(a, root.accentColor, 0, 0.82)
        ctx.globalAlpha = 1
    }
    function useSessionAlone(key) {
        const lap = bestLapForSession(key)
        if (!lap) return
        store.clearCompare()
        store.selectLap(key, lap.lapId)
    }
    function setSessionActive(key) {
        const lap = bestLapForSession(key)
        if (!lap) return
        const referenceKey = referenceSessionKey
        const referenceLap = bestLapForSession(referenceKey)
        store.selectLap(key, lap.lapId)
        if (referenceKey !== "" && referenceKey !== key && referenceLap)
            store.compareLap(referenceKey, referenceLap.lapId)
        else
            store.clearCompare()
    }
    function setSessionReference(key) {
        if (key === referenceSessionKey) {
            store.clearCompare()
            return
        }
        if (activeSessionKey === "" || key === activeSessionKey) {
            useSessionAlone(key)
            return
        }
        const lap = bestLapForSession(key)
        if (lap) store.compareLap(key, lap.lapId)
    }
    function dateKey(trackName, dateName) { return trackName + "|" + dateName }
    function showVideo(source, telemetryLinked) {
        telemetryVideoActive = telemetryLinked === true
        videoVisible = true
        if (root.width < 1000) sidebarVisible = false
        Qt.callLater(() => videoPlayer.openMedia(source))
    }
    function seekVideoToTelemetry() {
        if (!telemetryVideoActive || !videoPlayer.loaded) return
        const target = store.primaryVideoTime
        if (Math.abs(videoPlayer.position - target) > 0.025)
            videoPlayer.seek(target)
    }
    function seekVideoRelative(seconds) {
        if (telemetryVideoActive)
            store.seekCursorSeconds(seconds)
        else
            videoPlayer.seekRelative(seconds)
    }
    function syncTelemetryVideo() {
        const source = store.primaryVideoSource
        if (source.toString() === "") {
            if (telemetryVideoActive) {
                videoPlayer.closeMedia()
                videoVisible = false
                telemetryVideoActive = false
            }
            return
        }
        if (!telemetryVideoActive ||
            videoPlayer.source.toString() !== source.toString())
            showVideo(source, true)
        else
            seekVideoToTelemetry()
    }

    function rebuildTree() {
        if (!store.ready) return
        treeModel.clear()
        const groups = store.trackGroups()
        for (let t = 0; t < groups.length; ++t) {
            const trackName = groups[t].track
            const dates = groups[t].dates
            treeModel.append({ role: "track", name: trackName, indent: 0,
                               key: "", expanded: trackExpanded(trackName) })
            if (!trackExpanded(trackName)) continue
            for (let d = 0; d < dates.length; ++d) {
                const dateName = dates[d].date
                const sessions = dates[d].sessions
                const dk = dateKey(trackName, dateName)
                treeModel.append({ role: "date", name: dateName, indent: 1,
                                   key: dk, expanded: dateExpanded(dk) })
                if (!dateExpanded(dk)) continue
                for (let s = 0; s < sessions.length; ++s) {
                    const session = sessions[s]
                    const display = session.driver !== ""
                                    ? session.driver : "Unknown"
                    treeModel.append({
                        role: "session",
                        name: display,
                        stem: session.stem,
                        driver: session.driver,
                        driverId: session.driverId,
                        mappingKey: session.mappingKey,
                        carNumber: session.carNumber,
                        carClass: session.carClass,
                        sessionTime: session.sessionTime,
                        bestTime: session.bestTime,
                        bestTimeMs: session.bestTimeMs,
                        isDriverBest: session.isDriverBest,
                        isDayBest: session.isDayBest,
                        isVideo: session.isVideo === true,
                        indent: 2,
                        key: session.key,
                        expanded: false
                    })
                }
            }
        }
    }

    Component.onCompleted: {
        store.sessionsChanged.connect(() => {
            rebuildTree()
            refreshLapStrip()
            refreshAliasRows()
            refreshMappingRows()
            refreshDirectoryRows()
        })
        store.selectionChanged.connect(() => {
            readout.refresh()
            refreshCornerRows()
            refreshLapStrip()
            refreshAlignmentData()
            syncTelemetryVideo()
        })
        store.channelConfigChanged.connect(refreshChannelRows)
        store.cornersChanged.connect(() => {
            refreshCornerRows()
            refreshCornerZones()
        })
        store.driverMappingsChanged.connect(() => {
            refreshMappingRows()
            rebuildTree()
        })
        refreshChannelRows()
        refreshCornerRows()
        refreshCornerZones()
        refreshAliasRows()
        refreshMappingRows()
        refreshDirectoryRows()
        refreshLapStrip()
        refreshAlignmentData()
        readout.refresh()
        rebuildTree()
        syncTelemetryVideo()
        if (typeof startupVideo !== "undefined" &&
            startupVideo.toString() !== "" &&
            store.primaryVideoSource.toString() === "")
            showVideo(startupVideo)
        if (typeof autotestWindows !== "undefined" && autotestWindows) {
            cornerWindow.show()
            channelsWindow.show()
            settingsWindow.show()
        }
    }

    Connections {
        target: store
        function onCursorFracChanged() { readout.refresh() }
        function onVideoTimeChanged() { root.seekVideoToTelemetry() }
        function onReferenceAlignmentChanged() {
            referenceDamper.requestPaint()
        }
    }

    // ══ header ══════════════════════════════════════════════════════
    header: ToolBar {
        id: appBar
        height: 48
        Material.elevation: 2
        background: Rectangle {
            color: root.surfaceColor
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: root.borderColor
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            spacing: 8

            ToolButton {
                text: "☰"
                font.pixelSize: 16
                onClicked: sidebarVisible = !sidebarVisible
                ToolTip.visible: hovered
                ToolTip.text: sidebarVisible ? "Hide sessions" : "Show sessions"
            }
            Label {
                text: "RACECRAFT"
                font.family: "Geist Mono"
                font.pixelSize: 13
                font.bold: true
                font.letterSpacing: 1.2
                color: root.accentColor
            }
            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: 28
                color: root.borderColor
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 120
                spacing: 0
                Label {
                    Layout.fillWidth: true
                    text: store.primaryLabel || "No lap selected"
                    elide: Text.ElideRight
                    font.pixelSize: 12
                    font.bold: true
                    color: root.foregroundColor
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: store.primaryDriverName
                        visible: text !== ""
                        Layout.maximumWidth: Math.min(220, implicitWidth)
                        elide: Text.ElideRight
                        font.pixelSize: 10
                        color: store.comparing
                               ? root.orangeColor : root.mutedTextColor
                    }
                    ToolButton {
                        id: headerDriverEdit
                        objectName: "headerDriverEdit"
                        visible: store.primaryDriverMappingKey !== ""
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 20
                        text: "✎"
                        font.pixelSize: 10
                        onClicked: root.openDriverRename(
                            store.primaryDriverMappingKey,
                            store.primaryDriverName)
                        ToolTip.visible: hovered
                        ToolTip.text: "Rename driver"
                    }
                    Label {
                        Layout.fillWidth: true
                        text: {
                            const driver = store.primaryDriverName
                            const detail = store.primaryDetail
                            const suffix = driver !== "" &&
                                           detail.indexOf(driver) === 0
                                         ? detail.substring(driver.length)
                                         : detail
                            return suffix +
                                   (store.comparing
                                    ? "  ·  vs " + store.compareLabel : "")
                        }
                        visible: text !== ""
                        elide: Text.ElideRight
                        font.pixelSize: 10
                        color: store.comparing
                               ? root.orangeColor : root.mutedTextColor
                    }
                }
            }
            Label {
                id: readout
                Layout.maximumWidth: Math.max(120, root.width * 0.34)
                visible: root.width >= 650
                elide: Text.ElideLeft
                horizontalAlignment: Text.AlignRight
                font.family: "Geist Mono"
                font.pixelSize: 10
                color: root.mutedTextColor
                function refresh() {
                    if (!store.ready) { text = ""; return }
                    const r = store.cursorReadout()
                    let parts = []
                    if (r.time !== undefined) parts.push(r.time.toFixed(1) + "s")
                    if (r.dist !== undefined) parts.push(Math.round(r.dist) + "m")
                    if (r.speed !== undefined) parts.push(Math.round(r.speed) + " km/h")
                    if (r.gear !== undefined) parts.push("G" + r.gear)
                    if (r.corner) parts.push(r.corner)
                    if (store.comparing && r.delta !== undefined)
                        parts.push("Δ " + r.delta.toFixed(2) + "s")
                    text = parts.join("  ·  ")
                }
            }
            ToolButton {
                text: "•••"
                font.pixelSize: 14
                onClicked: actionsMenu.open()
                ToolTip.visible: hovered
                ToolTip.text: "Actions"
            }
        }

        Menu {
            id: actionsMenu
            y: appBar.height
            x: Math.max(0, appBar.width - width - 8)
            MenuItem {
                text: "Open telemetry…"
                onTriggered: drawer.open()
            }
            MenuItem {
                text: "Open video…"
                onTriggered: videoFileDialog.open()
            }
            MenuSeparator {}
            MenuItem {
                text: "Corner inspector"
                onTriggered: {
                    refreshCornerRows()
                    cornerWindow.show()
                    cornerWindow.requestActivate()
                }
            }
            MenuItem {
                text: "Edit corner zones"
                checkable: true
                checked: store.editingCorners
                onTriggered: store.setEditingCorners(checked)
            }
            MenuItem {
                text: "Auto-generate corners"
                onTriggered: store.autoGenerateCorners()
            }
            MenuItem {
                text: "Save corners"
                onTriggered: store.saveCorners()
            }
            MenuSeparator {}
            MenuItem {
                text: "Trace channels"
                onTriggered: {
                    refreshChannelRows()
                    channelsWindow.show()
                    channelsWindow.raise()
                }
            }
            MenuItem {
                text: "Preferences"
                onTriggered: {
                    refreshMappingRows()
                    refreshDirectoryRows()
                    settingsWindow.show()
                    settingsWindow.raise()
                }
            }
            MenuItem {
                text: "Larger trace lanes"
                checkable: true
                checked: store.channelHeight > 110
                onTriggered: store.channelHeight = checked ? 150 : 110
            }
            MenuSeparator { visible: store.comparing }
            MenuItem {
                text: "Clear comparison"
                visible: store.comparing
                onTriggered: store.clearCompare()
            }
        }
    }

    // ══ drawer (file open) ══════════════════════════════════════════
    Drawer {
        id: drawer
        width: Math.min(360, root.width * 0.86)
        height: root.height
        edge: Qt.LeftEdge
        background: Rectangle { color: root.backgroundColor }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10
            Label { text: "Open session"; font.bold: true; font.pixelSize: 15 }
            Label { text: "Scan a directory of .pds / .ld / .ldx / .vbo / .mp4 files"; wrapMode: Text.Wrap; color: root.mutedTextColor }
            TextField {
                id: dirField
                Layout.fillWidth: true
                placeholderText: "/path/to/telemetry"
                onAccepted: addDir(dirField.text)
            }
            RowLayout {
                Layout.fillWidth: true
                Button { text: "Add"; onClicked: addDir(dirField.text) }
                Button { text: "Choose…"; onClicked: folderDialog.open() }
                Button { text: "Open file"; onClicked: fileDialog.open() }
            }
            Label {
                text: "Directories:"
                font.pixelSize: 12
                color: root.mutedTextColor
                visible: store.ready
            }
            ListView {
                id: drawerDirectories
                Layout.fillWidth: true
                Layout.fillHeight: true
                model: directoryRows
                delegate: RowLayout {
                    width: drawerDirectories.width
                    Label { text: modelData; elide: Text.ElideMiddle; Layout.fillWidth: true; font.pixelSize: 10 }
                }
            }
        }
    }

    function toLocalPath(value) {
        const text = value.toString()
        return text.startsWith("file://")
               ? decodeURIComponent(text.substring(7)) : text
    }
    function defaultTelemetryFolder() {
        const home = toLocalPath(Platform.StandardPaths.writableLocation(
            Platform.StandardPaths.HomeLocation))
        const preferred = home + "/Documents/Telemetry"
        return "file://" + (store.directoryExists(preferred) ? preferred : home)
    }
    function addDir(p) {
        const path = toLocalPath(p)
        if (path === "") return
        store.addSessionDirectory(path)
        dirField.text = ""
        rebuildTree()
        refreshDirectoryRows()
    }

    Platform.FolderDialog {
        id: folderDialog
        title: "Choose telemetry directory"
        acceptLabel: "Add"
        folder: root.defaultTelemetryFolder()
        onAccepted: root.addDir(folderDialog.folder)
    }

    Platform.FileDialog {
        id: fileDialog
        title: "Open telemetry file"
        fileMode: Platform.FileDialog.OpenFile
        nameFilters: ["Telemetry (*.pds *.ld *.ldx *.vbo *.mp4 *.MP4)", "All files (*)"]
        onAccepted: {
            store.openFile(fileDialog.file.toLocalFile())
            refreshCornerRows()
            rebuildTree()
        }
    }

    Platform.FileDialog {
        id: videoFileDialog
        title: "Open onboard video"
        fileMode: Platform.FileDialog.OpenFile
        nameFilters: [
            "Video (*.mp4 *.MP4 *.mov *.MOV *.mkv *.MKV *.avi *.AVI *.m4v *.webm)",
            "All files (*)"
        ]
        onAccepted: showVideo(videoFileDialog.file)
    }

    Dialog {
        id: driverRenameDialog
        objectName: "driverRenameDialog"
        parent: Overlay.overlay
        width: Math.min(360, root.width - 32)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        modal: true
        focus: true
        title: "Rename driver"
        standardButtons: Dialog.Save | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape
        property string mappingKey: ""
        onOpened: {
            driverRenameField.forceActiveFocus()
            driverRenameField.selectAll()
        }
        onAccepted: {
            store.setDriverMapping(mappingKey, driverRenameField.text)
            refreshMappingRows()
        }
        contentItem: TextField {
            id: driverRenameField
            objectName: "driverRenameField"
            placeholderText: "Driver name"
            selectByMouse: true
            onAccepted: driverRenameDialog.accept()
        }
    }

    Dialog {
        id: cornerRenameDialog
        objectName: "cornerRenameDialog"
        parent: Overlay.overlay
        width: Math.min(360, root.width - 32)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        modal: true
        focus: true
        title: "Rename corner zone"
        standardButtons: Dialog.Save | Dialog.Cancel
        closePolicy: Popup.CloseOnEscape
        property int cornerIndex: -1
        onOpened: {
            cornerRenameField.forceActiveFocus()
            cornerRenameField.selectAll()
        }
        onAccepted: store.setCornerName(cornerIndex, cornerRenameField.text)
        contentItem: TextField {
            id: cornerRenameField
            objectName: "cornerRenameField"
            placeholderText: "Corner name"
            selectByMouse: true
            onAccepted: cornerRenameDialog.accept()
        }
    }

    // ══ corner inspector (separate Material window) ════════════════
    ApplicationWindow {
        id: cornerWindow
        objectName: "cornerWindow"
        width: 900
        height: 700
        minimumWidth: 700
        minimumHeight: 520
        visible: false
        title: store.comparing ? "Corner Analysis — primary vs reference" : "Corner Analysis"
        color: root.backgroundColor
        font.family: root.uiFontFamily
        font.pixelSize: 11
        Material.theme: Material.Dark
        Material.primary: root.surfaceColor
        Material.accent: root.accentColor
        Material.background: root.backgroundColor
        Material.foreground: root.foregroundColor
        property int selectedCornerIndex: 0
        readonly property var selectedCorner:
            cornerRows.length > 0
            ? cornerRows[Math.min(selectedCornerIndex, cornerRows.length - 1)]
            : ({})
        property real cornerDamperShift: 0
        onSelectedCornerChanged:
            cornerDamperShift = Number(selectedCorner.damperAlignment || 0)
        function commitZoneRange(index, startText, endText) {
            let start = Number(startText) / 100
            let end = Number(endText) / 100
            if (!isFinite(start) || !isFinite(end)) return
            start = Math.max(0, Math.min(1, start))
            end = Math.max(start + 0.001, Math.min(1, end))
            store.updateCorner(index, start, end)
            store.saveCorners()
        }

        Shortcut {
            sequence: StandardKey.Cancel
            context: Qt.ApplicationShortcut
            onActivated: root.dismissCornerPopover()
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: cornerWindow.selectedCorner.name || "Corner Analysis"
                    font.pixelSize: 17
                    font.bold: true
                    color: root.foregroundColor
                }
                Label {
                    text: store.comparing
                          ? "distance-aligned primary vs reference"
                          : store.primaryDetail
                    font.pixelSize: 11
                    color: root.mutedTextColor
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: store.trackAtlasReady
                    text: "TRACK ATLAS"
                    font.family: "Geist Mono"
                    font.pixelSize: 9
                    color: root.accentColor
                }
            }

            ListView {
                id: cornerPicker
                Layout.fillWidth: true
                Layout.preferredHeight: 34
                orientation: ListView.Horizontal
                spacing: 4
                clip: true
                model: cornerRows
                delegate: Rectangle {
                    width: Math.max(86, cornerName.implicitWidth + 20)
                    height: 30
                    radius: 4
                    color: index === cornerWindow.selectedCornerIndex
                           ? root.selectionColor : root.darkBackgroundColor
                    border.width: index === cornerWindow.selectedCornerIndex ? 1 : 0
                    border.color: root.accentColor
                    Label {
                        id: cornerName
                        anchors.centerIn: parent
                        text: modelData.name
                        font.pixelSize: 10
                        font.bold: index === cornerWindow.selectedCornerIndex
                        color: index === cornerWindow.selectedCornerIndex
                               ? root.accentColor : root.mutedTextColor
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: cornerWindow.selectedCornerIndex = index
                    }
                }
                ScrollBar.horizontal: ScrollBar {}
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Button {
                    text: store.editingCorners ? "Editing zones" : "Edit zones"
                    checkable: true
                    checked: store.editingCorners
                    onClicked: store.setEditingCorners(checked)
                }
                Button {
                    text: "Add zone"
                    enabled: store.editingCorners
                    onClicked: {
                        store.setEditingCorners(true)
                        const width = 0.04
                        const start = Math.max(
                            0, Math.min(1 - width, store.cursorFrac - width / 2))
                        const index = store.addCorner(start, start + width)
                        if (index >= 0) {
                            cornerWindow.selectedCornerIndex = index
                            root.openCornerRename(index)
                        }
                    }
                }
                Button {
                    text: "Auto-generate"
                    enabled: store.editingCorners
                    onClicked: {
                        store.autoGenerateCorners()
                        store.saveCorners()
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: store.editingCorners
                          ? "Drag zone edges on the trace; edits save automatically."
                          : "Enable editing to add, rename, or delete zones."
                    elide: Text.ElideRight
                    font.pixelSize: 10
                    color: root.mutedTextColor
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 158 : 0
                visible: store.editingCorners
                radius: 4
                color: root.darkBackgroundColor
                border.color: root.borderColor
                ListView {
                    id: zoneEditor
                    anchors.fill: parent
                    anchors.margins: 5
                    clip: true
                    spacing: 3
                    model: cornerZoneRows
                    ScrollBar.vertical: ScrollBar {}
                    delegate: RowLayout {
                        required property var modelData
                        required property int index
                        width: zoneEditor.width - 12
                        height: 34
                        spacing: 5
                        TextField {
                            id: zoneNameField
                            Layout.fillWidth: true
                            Layout.preferredHeight: 28
                            font.pixelSize: 10
                            text: modelData.name
                            selectByMouse: true
                            onEditingFinished: {
                                store.setCornerName(index, text)
                                const zones = store.cornerList()
                                if (index < zones.length)
                                    text = zones[index].name
                            }
                        }
                        TextField {
                            id: zoneStartField
                            Layout.preferredWidth: 74
                            Layout.preferredHeight: 28
                            text: (modelData.start * 100).toFixed(2)
                            selectByMouse: true
                            horizontalAlignment: Text.AlignRight
                            font.family: "Geist Mono"
                            font.pixelSize: 10
                            validator: DoubleValidator { bottom: 0; top: 100 }
                            onEditingFinished: cornerWindow.commitZoneRange(
                                index, zoneStartField.text, zoneEndField.text)
                        }
                        Label {
                            text: "→"
                            font.pixelSize: 10
                            color: root.mutedTextColor
                        }
                        TextField {
                            id: zoneEndField
                            Layout.preferredWidth: 74
                            Layout.preferredHeight: 28
                            text: (modelData.end * 100).toFixed(2)
                            selectByMouse: true
                            horizontalAlignment: Text.AlignRight
                            font.family: "Geist Mono"
                            font.pixelSize: 10
                            validator: DoubleValidator { bottom: 0; top: 100 }
                            onEditingFinished: cornerWindow.commitZoneRange(
                                index, zoneStartField.text, zoneEndField.text)
                        }
                        Label {
                            text: "% lap"
                            font.family: "Geist Mono"
                            font.pixelSize: 9
                            color: root.mutedTextColor
                        }
                        ToolButton {
                            text: "×"
                            implicitWidth: 28
                            onClicked: store.deleteCorner(index)
                            ToolTip.visible: hovered
                            ToolTip.text: "Delete zone"
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: [
                        {
                            label: "TIME",
                            value: cornerWindow.selectedCorner.time !== undefined
                                   ? cornerWindow.selectedCorner.time.toFixed(3) + "s"
                                   : "—",
                            delta: cornerWindow.selectedCorner.hasCompare
                                   ? "Δ " + (cornerWindow.selectedCorner.delta >= 0 ? "+" : "") +
                                     cornerWindow.selectedCorner.delta.toFixed(3) + "s"
                                   : ""
                        },
                        {
                            label: "SPEED",
                            value: cornerWindow.selectedCorner.apexSpeed !== undefined
                                   ? Math.round(cornerWindow.selectedCorner.entrySpeed) + " → " +
                                     Math.round(cornerWindow.selectedCorner.apexSpeed) + " → " +
                                     Math.round(cornerWindow.selectedCorner.exitSpeed)
                                   : "—",
                            delta: cornerWindow.selectedCorner.hasCompare
                                   ? "exit " +
                                     (cornerWindow.selectedCorner.exitDelta >= 0 ? "+" : "") +
                                     cornerWindow.selectedCorner.exitDelta.toFixed(1) + " km/h"
                                   : ""
                        },
                        {
                            label: "CONTROL",
                            value: cornerWindow.selectedCorner.minGear !== undefined
                                   ? "G" + cornerWindow.selectedCorner.minGear +
                                     "  steer " +
                                     Math.round(cornerWindow.selectedCorner.maxSteering) + "°"
                                   : "—",
                            delta: cornerWindow.selectedCorner.turnInPoint !== undefined
                                   ? "turn-in " +
                                     Math.round(cornerWindow.selectedCorner.turnInPoint) +
                                     "m  apex " +
                                     Math.round(cornerWindow.selectedCorner.apexPoint) + "m"
                                   : ""
                        },
                        {
                            label: "SCORE",
                            value: cornerWindow.selectedCorner.hasCompare
                                   ? Math.round(cornerWindow.selectedCorner.score) + " / 100"
                                   : "single lap",
                            delta: cornerWindow.selectedCorner.hasCompare
                                   ? (cornerWindow.selectedCorner.delta < 0
                                      ? "primary ahead" : "reference ahead")
                                   : ""
                        }
                    ]
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 58
                        radius: 4
                        color: root.surfaceColor
                        Column {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 2
                            Label {
                                text: modelData.label
                                font.family: "Geist Mono"
                                font.pixelSize: 8
                                font.bold: true
                                color: root.dimTextColor
                            }
                            Label {
                                text: modelData.value
                                font.family: "Geist Mono"
                                font.pixelSize: 12
                                font.bold: true
                                color: root.foregroundColor
                            }
                            Label {
                                text: modelData.delta
                                font.family: "Geist Mono"
                                font.pixelSize: 9
                                color: root.mutedTextColor
                            }
                        }
                    }
                }
            }

            Canvas {
                id: cornerGraphs
                Layout.fillWidth: true
                Layout.fillHeight: true
                property var corner: cornerWindow.selectedCorner
                onCornerChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    if (!corner || !corner.speedSeries) return
                    const panelGap = 8
                    const panelHeight = (height - panelGap * 2) / 3

                    function range(seriesA, seriesB, symmetric) {
                        let lo = Number.POSITIVE_INFINITY
                        let hi = Number.NEGATIVE_INFINITY
                        const arrays = [seriesA || [], seriesB || []]
                        for (let a = 0; a < arrays.length; ++a)
                            for (let i = 0; i < arrays[a].length; ++i) {
                                lo = Math.min(lo, arrays[a][i])
                                hi = Math.max(hi, arrays[a][i])
                            }
                        if (!isFinite(lo) || !isFinite(hi) || hi <= lo) {
                            lo = 0
                            hi = 1
                        }
                        if (symmetric) {
                            const m = Math.max(Math.abs(lo), Math.abs(hi), 1)
                            lo = -m
                            hi = m
                        }
                        return [lo, hi]
                    }
                    function panel(top, label) {
                        ctx.fillStyle = root.traceBackgroundColor
                        ctx.fillRect(0, top, width, panelHeight)
                        ctx.strokeStyle = root.borderColor
                        ctx.lineWidth = 1
                        for (let g = 1; g < 4; ++g) {
                            const y = top + panelHeight * g / 4
                            ctx.beginPath()
                            ctx.moveTo(0, y)
                            ctx.lineTo(width, y)
                            ctx.stroke()
                        }
                        ctx.fillStyle = root.mutedTextColor
                        ctx.font = "10px 'Geist Mono'"
                        ctx.fillText(label, 7, top + 13)
                    }
                    function series(values, top, lo, hi, color, lineWidth) {
                        if (!values || values.length < 2) return
                        ctx.strokeStyle = color
                        ctx.lineWidth = lineWidth
                        ctx.beginPath()
                        for (let i = 0; i < values.length; ++i) {
                            const x = i / (values.length - 1) * width
                            const y = top + panelHeight -
                                      (values[i] - lo) / Math.max(0.000001, hi - lo) *
                                      panelHeight
                            if (i === 0) ctx.moveTo(x, y)
                            else ctx.lineTo(x, y)
                        }
                        ctx.stroke()
                    }
                    function annotation(position, label, color, labelY, dashed) {
                        if (position === undefined || position === null) return
                        const x = Math.max(0, Math.min(1, position)) * width
                        ctx.save()
                        ctx.strokeStyle = color
                        ctx.lineWidth = dashed ? 1 : 1.4
                        ctx.setLineDash(dashed ? [4, 4] : [])
                        ctx.beginPath()
                        ctx.moveTo(x, 0)
                        ctx.lineTo(x, height)
                        ctx.stroke()
                        ctx.setLineDash([])
                        if (label) {
                            ctx.fillStyle = color
                            ctx.font = "bold 9px 'Geist Mono'"
                            ctx.fillText(label, Math.min(width - 58, x + 4), labelY)
                        }
                        ctx.restore()
                    }

                    const speedTop = 0
                    const pedalTop = panelHeight + panelGap
                    const steeringTop = (panelHeight + panelGap) * 2
                    panel(speedTop, "SPEED")
                    panel(pedalTop, "THROTTLE / BRAKE")
                    panel(steeringTop, "STEERING")

                    let r = range(corner.speedSeries,
                                  corner.compareSpeedSeries, false)
                    series(corner.compareSpeedSeries, speedTop, r[0], r[1],
                           root.mutedTextColor, 1.2)
                    series(corner.speedSeries, speedTop, r[0], r[1],
                           root.greenColor, 1.8)

                    series(corner.compareThrottleSeries, pedalTop, 0, 1,
                           root.mutedTextColor, 1.1)
                    series(corner.throttleSeries, pedalTop, 0, 1,
                           root.greenColor, 1.7)
                    const brakeMax = Math.max(
                        corner.maxBrake || 1,
                        corner.compareMaxBrake || 1, 1)
                    series(corner.compareBrakeSeries, pedalTop, 0, brakeMax,
                           root.orangeColor, 1.1)
                    series(corner.brakeSeries, pedalTop, 0, brakeMax,
                           root.redColor, 1.7)

                    r = range(corner.steeringSeries,
                              corner.compareSteeringSeries, true)
                    series(corner.compareSteeringSeries, steeringTop,
                           r[0], r[1], root.mutedTextColor, 1.1)
                    series(corner.steeringSeries, steeringTop,
                           r[0], r[1], root.yellowColor, 1.7)

                    // Context is visible but recessed; the selected zone keeps
                    // full contrast between its boundary lines.
                    const zoneStart = Math.max(
                        0, Math.min(1, corner.cornerStartPosition !== undefined
                                       ? corner.cornerStartPosition : 0)) * width
                    const zoneEnd = Math.max(
                        0, Math.min(1, corner.cornerEndPosition !== undefined
                                       ? corner.cornerEndPosition : 1)) * width
                    ctx.save()
                    ctx.fillStyle = Qt.rgba(0, 0, 0, 0.58)
                    if (zoneStart > 0) ctx.fillRect(0, 0, zoneStart, height)
                    if (zoneEnd < width)
                        ctx.fillRect(zoneEnd, 0, width - zoneEnd, height)
                    ctx.strokeStyle = root.accentColor
                    ctx.globalAlpha = 0.55
                    ctx.lineWidth = 1
                    for (const edge of [zoneStart, zoneEnd]) {
                        ctx.beginPath()
                        ctx.moveTo(edge, 0)
                        ctx.lineTo(edge, height)
                        ctx.stroke()
                    }
                    ctx.restore()
                    ctx.fillStyle = root.mutedTextColor
                    ctx.font = "9px 'Geist Mono'"
                    ctx.fillText(
                        Math.round(corner.contextWindowMeters || 0) + "m window · " +
                        Math.round(corner.cornerLengthMeters || 0) + "m zone",
                        7, height - 6)

                    annotation(corner.compareTurnInPosition, "",
                               root.dimTextColor, 0, true)
                    annotation(corner.compareApexPosition, "",
                               root.dimTextColor, 0, true)
                    annotation(corner.compareThrottlePosition, "",
                               root.dimTextColor, 0, true)
                    annotation(corner.turnInPosition, "TURN-IN",
                               root.accentColor, 30, false)
                    annotation(corner.apexPosition, "APEX",
                               root.magentaColor, 42, false)
                    annotation(corner.throttlePosition, "PICKUP",
                               root.greenColor, 54, false)
                }
            }

            Rectangle {
                id: cornerDamperPanel
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? 76 : 0
                visible: !store.hasGpsData &&
                         !!cornerWindow.selectedCorner.hasCompare &&
                         cornerWindow.selectedCorner.damperPrimarySeries !== undefined &&
                         cornerWindow.selectedCorner.damperCompareSeries !== undefined &&
                         cornerWindow.selectedCorner.damperCompareSeries.length > 1
                radius: 4
                color: root.surfaceColor
                border.color: root.borderColor
                Label {
                    width: 82
                    anchors.left: parent.left
                    anchors.leftMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: "DAMPER\nALIGN"
                    font.family: "Geist Mono"
                    font.pixelSize: 8
                    font.bold: true
                    lineHeight: 0.9
                    color: root.mutedTextColor
                }
                Canvas {
                    id: cornerDamperCanvas
                    anchors.left: parent.left
                    anchors.leftMargin: 90
                    anchors.right: cornerDamperDragLabel.left
                    anchors.rightMargin: 6
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    property var corner: cornerWindow.selectedCorner
                    property var primarySamples:
                        corner.damperPrimarySeries || []
                    property var compareSamples:
                        corner.damperCompareSeries || []
                    property real shiftMeters:
                        cornerWindow.cornerDamperShift
                    property real windowMeters:
                        Number(corner.damperWindowMeters || 1)
                    property real cornerStartMeters:
                        Number(corner.damperCornerStartMeters || 0)
                    onCornerChanged: requestPaint()
                    onPrimarySamplesChanged: requestPaint()
                    onCompareSamplesChanged: requestPaint()
                    onShiftMetersChanged: requestPaint()
                    onPaint: root.paintCornerDamperStrip(
                        this, primarySamples, compareSamples,
                        shiftMeters, windowMeters, cornerStartMeters)
                }
                Label {
                    id: cornerDamperDragLabel
                    width: 104
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    text: (cornerWindow.selectedCorner.damperAlignmentValid
                           ? "AUTO " : "MANUAL ") +
                          (cornerWindow.cornerDamperShift >= 0 ? "+" : "") +
                          cornerWindow.cornerDamperShift.toFixed(1) + "m  ↔"
                    horizontalAlignment: Text.AlignRight
                    font.family: "Geist Mono"
                    font.pixelSize: 8
                    font.bold: true
                    color: cornerWindow.selectedCorner.damperAlignmentValid
                           ? root.accentColor : root.orangeColor
                }
                MouseArea {
                    id: cornerDamperAlignmentMouse
                    anchors.left: cornerDamperCanvas.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    cursorShape: Qt.SizeHorCursor
                    property real pressX: 0
                    property real startShift: 0
                    onPressed: (mouse) => {
                        pressX = mouse.x
                        startShift = cornerWindow.cornerDamperShift
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed || width <= 0) return
                        const span = Number(
                            cornerWindow.selectedCorner.damperWindowMeters || 1)
                        cornerWindow.cornerDamperShift = Math.max(
                            -50, Math.min(50,
                                startShift + (mouse.x - pressX) / width * span))
                    }
                    onDoubleClicked:
                        cornerWindow.cornerDamperShift =
                            Number(cornerWindow.selectedCorner.damperAlignment || 0)
                    ToolTip.visible: containsMouse
                    ToolTip.text:
                        "Auto-aligned from damper peaks in the prior 300m; drag to adjust"
                }
            }

            Label {
                Layout.fillWidth: true
                visible: cornerWindow.selectedCorner.note !== undefined
                text: cornerWindow.selectedCorner.note || ""
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: root.mutedTextColor
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Button {
                    text: "Edit zones"
                    checkable: true
                    checked: store.editingCorners
                    onClicked: store.setEditingCorners(checked)
                }
                Item { Layout.fillWidth: true }
                Button { text: "Close"; onClicked: cornerWindow.hide() }
            }
        }
    }

    ApplicationWindow {
        id: channelsWindow
        objectName: "channelsWindow"
        width: 640
        height: 480
        minimumWidth: 520
        minimumHeight: 400
        visible: false
        title: "Trace Channels"
        color: root.backgroundColor
        font.family: root.uiFontFamily
        font.pixelSize: 11
        Material.theme: Material.Dark
        Material.primary: root.surfaceColor
        Material.accent: root.accentColor
        Material.background: root.backgroundColor
        Material.foreground: root.foregroundColor

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 6
            Label { text: "Trace channels"; font.pixelSize: 16; font.bold: true }
            Label {
                text: "Source channels; visibility, color, and lane height"
                font.pixelSize: 11
                color: root.mutedTextColor
            }
            ListView {
                id: channelListView
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: channelRows
                delegate: RowLayout {
                    property var chan: modelData
                    width: channelListView.width
                    height: 36
                    spacing: 8
                    Switch {
                        checked: chan.visible
                        onToggled: store.setChannelVisible(chan.key, checked)
                    }
                    Label {
                        text: chan.title
                        Layout.preferredWidth: 92
                        font.bold: true
                    }
                    Label {
                        text: chan.unit
                        Layout.preferredWidth: 40
                        font.family: "Geist Mono"
                        color: root.mutedTextColor
                    }
                    ComboBox {
                        Layout.preferredWidth: 120
                        model: root.colorChoices
                        currentIndex: Math.max(0, root.colorChoices.indexOf(chan.color))
                        delegate: ItemDelegate {
                            width: parent.width
                            contentItem: Rectangle {
                                color: modelData
                                border.color: root.borderColor
                                radius: 3
                            }
                        }
                        onActivated: store.setChannelColor(chan.key, currentText)
                    }
                    ComboBox {
                        Layout.preferredWidth: 88
                        model: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]
                        currentIndex: Math.max(0, Math.round((chan.weight - 0.5) / 0.25))
                        onActivated: store.setChannelWeight(chan.key, Number(currentText))
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: "Close"; onClicked: channelsWindow.hide() }
            }
        }
    }

    ApplicationWindow {
        id: settingsWindow
        objectName: "settingsWindow"
        width: 820
        height: 680
        minimumWidth: 700
        minimumHeight: 560
        visible: false
        title: "Racecraft Preferences"
        color: root.backgroundColor
        font.family: root.uiFontFamily
        font.pixelSize: 11
        property string mappingEditKey: ""
        property string mappingEditName: ""
        Material.theme: Material.Dark
        Material.primary: root.surfaceColor
        Material.accent: root.accentColor
        Material.background: root.backgroundColor
        Material.foreground: root.foregroundColor

        Platform.FolderDialog {
            id: settingsFolderDialog
            title: "Choose telemetry directory"
            acceptLabel: "Add"
            folder: root.defaultTelemetryFolder()
            onAccepted: {
                root.addDir(settingsFolderDialog.folder)
                root.refreshDirectoryRows()
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 6
            Label { text: "Preferences"; font.pixelSize: 16; font.bold: true }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "Track atlas"
                    font.bold: true
                    color: root.accentColor
                }
                Label {
                    Layout.fillWidth: true
                    text: store.trackAtlasStatus
                    elide: Text.ElideRight
                    font.family: "Geist Mono"
                    font.pixelSize: 9
                    color: root.mutedTextColor
                }
                Button {
                    text: "Update now"
                    onClicked: store.refreshTrackAtlas()
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: "Telemetry directories"
                    font.bold: true
                    color: root.accentColor
                }
                Label {
                    Layout.fillWidth: true
                    text: "stored in " + store.configFilePath()
                    elide: Text.ElideMiddle
                    font.family: "Geist Mono"
                    font.pixelSize: 9
                    color: root.mutedTextColor
                }
                Button {
                    text: "Rescan"
                    onClicked: { store.scan(); root.rebuildTree() }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: settingsDir
                    Layout.fillWidth: true
                    placeholderText: "/path/to/telemetry"
                    onAccepted: settingsAddDir.clicked()
                }
                Button {
                    text: "Browse…"
                    onClicked: settingsFolderDialog.open()
                }
                Button {
                    id: settingsAddDir
                    text: "Add"
                    enabled: settingsDir.text !== ""
                    onClicked: {
                        root.addDir(settingsDir.text)
                        settingsDir.text = ""
                        root.refreshDirectoryRows()
                    }
                }
            }
            ListView {
                id: settingsDirectories
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(
                    160, Math.max(40, directoryRows.length * 38))
                clip: true
                model: directoryRows
                delegate: RowLayout {
                    required property var modelData
                    width: settingsDirectories.width
                    height: 38
                    spacing: 6
                    Label {
                        text: modelData
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                        font.family: "Geist Mono"
                        font.pixelSize: 10
                        color: store.directoryExists(modelData)
                               ? root.foregroundColor : root.redColor
                    }
                    Label {
                        visible: !store.directoryExists(modelData)
                        text: "MISSING"
                        font.family: "Geist Mono"
                        font.pixelSize: 9
                        font.bold: true
                        color: root.redColor
                    }
                    Button {
                        text: "Remove"
                        Layout.preferredHeight: 30
                        onClicked: {
                            store.removeSessionDirectory(modelData)
                            root.refreshDirectoryRows()
                        }
                    }
                }
            }
            Label {
                text: "Driver mappings"
                font.bold: true
                color: root.accentColor
            }
            Label {
                text: "Names apply to the same car number, class, and driver ID across sessions."
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                font.pixelSize: 9
                color: root.mutedTextColor
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: mappingNameField
                    Layout.fillWidth: true
                    text: settingsWindow.mappingEditName
                    placeholderText: "Select a mapping or edit a driver name"
                    onTextChanged: {
                        if (activeFocus)
                            settingsWindow.mappingEditName = text
                    }
                }
                Button {
                    text: "Save"
                    enabled: settingsWindow.mappingEditKey !== "" &&
                             mappingNameField.text.trim() !== ""
                    onClicked: {
                        store.setDriverMapping(
                            settingsWindow.mappingEditKey,
                            mappingNameField.text)
                        settingsWindow.mappingEditKey = ""
                        settingsWindow.mappingEditName = ""
                        root.refreshMappingRows()
                    }
                }
            }
            ListView {
                id: mappingListView
                Layout.fillWidth: true
                Layout.preferredHeight: 250
                clip: true
                model: mappingRows
                delegate: RowLayout {
                    width: mappingListView.width
                    height: 38
                    spacing: 6
                    Label {
                        Layout.preferredWidth: 92
                        text: "Car " + modelData.carNumber
                        font.family: "Geist Mono"
                        font.pixelSize: 9
                        color: root.foregroundColor
                    }
                    Label {
                        Layout.preferredWidth: 88
                        text: modelData.carClass || "Unknown class"
                        font.family: "Geist Mono"
                        font.pixelSize: 9
                        color: root.mutedTextColor
                    }
                    Label {
                        Layout.preferredWidth: 72
                        text: "ID " + (modelData.driverId || "—")
                        font.family: "Geist Mono"
                        font.pixelSize: 9
                        color: root.mutedTextColor
                    }
                    Label {
                        visible: settingsWindow.mappingEditKey !== modelData.key
                        Layout.fillWidth: true
                        text: modelData.display
                        elide: Text.ElideRight
                        color: root.accentColor
                    }
                    TextField {
                        visible: settingsWindow.mappingEditKey === modelData.key
                        Layout.fillWidth: true
                        text: settingsWindow.mappingEditName
                        onTextChanged: {
                            if (activeFocus)
                                settingsWindow.mappingEditName = text
                        }
                    }
                    ToolButton {
                        text: settingsWindow.mappingEditKey === modelData.key
                               ? "✓" : "✎"
                        onClicked: {
                            if (settingsWindow.mappingEditKey === modelData.key) {
                                store.setDriverMapping(
                                    modelData.key,
                                    settingsWindow.mappingEditName)
                                settingsWindow.mappingEditKey = ""
                                settingsWindow.mappingEditName = ""
                                root.refreshMappingRows()
                            } else {
                                settingsWindow.mappingEditKey = modelData.key
                                settingsWindow.mappingEditName = modelData.display
                            }
                        }
                    }
                    ToolButton {
                        text: "×"
                        onClicked: {
                            store.setDriverMapping(modelData.key, "")
                            root.refreshMappingRows()
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: "Close"; onClicked: settingsWindow.hide() }
            }
        }
    }

    // ══ body ════════════════════════════════════════════════════════
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight:
                visible ? filmstripSessions.length * 33 + 9 : 0
            visible: filmstripSessions.length > 0
            color: root.darkBackgroundColor
            Column {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 3
                Repeater {
                    model: filmstripSessions
                    delegate: Rectangle {
                        id: sessionStrip
                        required property var modelData
                        property var strip: modelData
                        width: parent.width
                        height: 30
                        radius: 4
                        color: strip.reference
                               ? Qt.rgba(224 / 255, 157 / 255, 127 / 255, 0.06)
                               : "transparent"
                        border.color: root.borderColor
                        property string selectedLapTime: {
                            const laps = strip.laps
                            const key = strip.reference
                                        ? store.compareSessionKey
                                        : store.primarySessionKey
                            const idx = strip.reference
                                        ? store.compareLapIndex
                                        : store.primaryLapIndex
                            if (key === strip.sessionKey)
                                for (let i = 0; i < laps.length; ++i)
                                    if (laps[i].lapId === idx)
                                        return laps[i].timeText
                            return strip.bestTime
                        }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 4
                            spacing: 5
                            RowLayout {
                                Layout.fillWidth: false
                                Layout.maximumWidth: 400
                                Layout.preferredWidth: 400
                                spacing: 5
                                Label {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 60
                                    text: sessionStrip.strip.runName
                                    elide: Text.ElideMiddle
                                    font.family: "Geist Mono"
                                    font.pixelSize: 9
                                    font.bold: true
                                    color: root.foregroundColor
                                }
                                Rectangle {
                                    Layout.preferredWidth:
                                        stripBadgeLabel.implicitWidth + 10
                                    Layout.preferredHeight: 16
                                    radius: 3
                                    color: "transparent"
                                    border.width: 1
                                    border.color: sessionStrip.strip.reference
                                                  ? root.orangeColor
                                                  : root.accentColor
                                    Label {
                                        id: stripBadgeLabel
                                        anchors.centerIn: parent
                                        text: root.stripBadgeText(
                                                  sessionStrip.strip,
                                                  sessionStrip.selectedLapTime)
                                        font.family: "Geist Mono"
                                        font.pixelSize: 9
                                        font.bold: true
                                        color: sessionStrip.strip.reference
                                               ? root.orangeColor
                                               : root.accentColor
                                    }
                                }
                            }
                            ToolButton {
                                Layout.preferredWidth: 24
                                Layout.preferredHeight: 24
                                text: "×"
                                onClicked: strip.reference
                                          ? store.clearCompare()
                                          : store.clearPrimary()
                                ToolTip.visible: hovered
                                ToolTip.text: strip.reference
                                               ? "Remove reference session"
                                               : "Clear active session"
                            }
                            Item {
                                id: proportionalLapLane
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Row {
                                    anchors.fill: parent
                                    spacing: 3
                                    Repeater {
                                        model: sessionStrip.strip.laps
                                        delegate: Rectangle {
                                            id: proportionalLap
                                            required property var modelData
                                            property bool selectedLap:
                                                sessionStrip.strip.reference
                                                ? sessionStrip.strip.sessionKey ===
                                                      store.compareSessionKey &&
                                                  modelData.lapId ===
                                                      store.compareLapIndex
                                                : sessionStrip.strip.sessionKey ===
                                                      store.primarySessionKey &&
                                                  modelData.lapId ===
                                                      store.primaryLapIndex
                                            width: Math.max(
                                                1,
                                                (proportionalLapLane.width -
                                                 Math.max(
                                                     0,
                                                     sessionStrip.strip.laps.length - 1) *
                                                     3) *
                                                    modelData.timeMs /
                                                    sessionStrip.strip.totalTimeMs)
                                            anchors.verticalCenter:
                                                parent.verticalCenter
                                            height: parent.height - 8
                                            radius: 3
                                            color: selectedLap
                                                   ? root.selectionColor
                                                   : proportionalLapMouse.containsMouse
                                                     ? root.surfaceColor
                                                     : root.backgroundColor
                                            border.width: selectedLap ? 1 : 0
                                            border.color: sessionStrip.strip.reference
                                                          ? root.orangeColor
                                                          : root.accentColor
                                            Rectangle {
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.bottom: parent.bottom
                                                height: modelData.isFastest ? 2 : 0
                                                color: root.greenColor
                                            }
                                            Label {
                                                anchors.fill: parent
                                                anchors.leftMargin: 5
                                                anchors.rightMargin: 5
                                                text: modelData.label + "  " +
                                                      modelData.timeText
                                                elide: Text.ElideRight
                                                verticalAlignment: Text.AlignVCenter
                                                font.family: "Geist Mono"
                                                font.pixelSize: 9
                                                font.bold: selectedLap
                                                color: selectedLap
                                                       ? (sessionStrip.strip.reference
                                                          ? root.orangeColor
                                                          : root.accentColor)
                                                       : modelData.isFastest
                                                         ? root.greenColor
                                                         : root.foregroundColor
                                            }
                                            MouseArea {
                                                id: proportionalLapMouse
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                onClicked: {
                                                    if (sessionStrip.strip.reference)
                                                        store.compareLap(
                                                            sessionStrip.strip.sessionKey,
                                                            modelData.lapId)
                                                    else
                                                        store.selectLap(
                                                            sessionStrip.strip.sessionKey,
                                                            modelData.lapId)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            handle: Rectangle {
                implicitWidth: 1
                color: root.borderColor
            }

            Pane {
                id: sidebarPane
                visible: sidebarVisible
                SplitView.preferredWidth:
                    sidebarVisible
                    ? Math.min(280, Math.max(210, root.width * 0.32)) : 0
                SplitView.minimumWidth: sidebarVisible ? 185 : 0
                SplitView.maximumWidth: sidebarVisible ? 420 : 0
                SplitView.fillHeight: true
                padding: 0
                background: Rectangle { color: root.darkBackgroundColor }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 38
                        color: root.surfaceColor
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 4
                            Label {
                                text: "SESSIONS"
                                font.pixelSize: 10
                                font.family: "Geist Mono"
                                font.bold: true
                                font.letterSpacing: 0.8
                                color: root.mutedTextColor
                            }
                            Item { Layout.fillWidth: true }
                            ToolButton {
                                text: "↻"
                                onClicked: { store.scan(); rebuildTree() }
                                ToolTip.visible: hovered
                                ToolTip.text: "Rescan session directories"
                            }
                        }
                    }
                    ListView {
                        id: tree
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: treeModel
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}
                        delegate: sidebarDelegate
                    }
                }
            }

            SplitView {
                id: analysisSplit
                SplitView.fillWidth: true
                SplitView.fillHeight: true
                orientation: Qt.Vertical

                handle: Rectangle {
                    implicitHeight: 1
                    color: root.borderColor
                }

            Rectangle {
                id: videoPane
                objectName: "videoPane"
                visible: videoVisible
                SplitView.fillWidth: true
                SplitView.preferredHeight:
                    visible ? Math.min(480, Math.max(220, root.height * 0.42)) : 0
                SplitView.minimumHeight: visible ? 180 : 0
                SplitView.maximumHeight: visible ? root.height * 0.72 : 0
                color: root.traceBackgroundColor
                border.width: 1
                border.color: root.borderColor
                focus: visible

                Shortcut {
                    enabled: videoPane.visible && videoPlayer.loaded
                    sequence: "Space"
                    onActivated: videoPlayer.togglePaused()
                }
                Shortcut {
                    enabled: videoPane.visible && videoPlayer.loaded
                    sequence: "Left"
                    onActivated: root.seekVideoRelative(-5)
                }
                Shortcut {
                    enabled: videoPane.visible && videoPlayer.loaded
                    sequence: "Right"
                    onActivated: root.seekVideoRelative(15)
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        color: root.surfaceColor
                        border.color: root.borderColor
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 4
                            spacing: 6
                            Label {
                                text: "VIDEO"
                                font.family: "Geist Mono"
                                font.pixelSize: 9
                                font.bold: true
                                font.letterSpacing: 0.8
                                color: root.accentColor
                            }
                            Label {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                Layout.preferredWidth: 1
                                clip: true
                                text: videoPlayer.title || "No video loaded"
                                elide: Text.ElideMiddle
                                font.pixelSize: 10
                                color: root.foregroundColor
                            }
                            Label {
                                visible: videoPane.width >= 470
                                text: videoPlayer.seeking
                                      ? "SEEK"
                                      : videoPlayer.loaded
                                        ? (videoPlayer.paused ? "PAUSED" : "PLAYING")
                                        : ""
                                font.family: "Geist Mono"
                                font.pixelSize: 8
                                color: videoPlayer.seeking
                                       ? root.yellowColor : root.mutedTextColor
                            }
                            ToolButton {
                                visible: videoPane.width >= 410
                                text: "Open"
                                onClicked: videoFileDialog.open()
                                ToolTip.visible: hovered
                                ToolTip.text: "Open another video"
                            }
                            ToolButton {
                                implicitWidth: 28
                                Layout.preferredWidth: 28
                                leftPadding: 2
                                rightPadding: 2
                                text: "×"
                                onClicked: {
                                    videoPlayer.closeMedia()
                                    telemetryVideoActive = false
                                    videoVisible = false
                                }
                                ToolTip.visible: hovered
                                ToolTip.text: "Close video"
                            }
                        }
                    }

                    Rectangle {
                        id: videoSurface
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: "#000000"
                        clip: true

                        MpvVideoItem {
                            id: videoPlayer
                            objectName: "videoPlayer"
                            anchors.fill: parent
                            onLoadedChanged: {
                                if (loaded && root.telemetryVideoActive) {
                                    Qt.callLater(() => {
                                        videoPlayer.paused = true
                                        root.seekVideoToTelemetry()
                                    })
                                }
                            }
                            onPositionChanged: {
                                if (root.telemetryVideoActive && loaded &&
                                    !paused)
                                    store.setCursorFromVideoTime(position)
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            enabled: videoPlayer.loaded
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                videoPane.forceActiveFocus()
                                videoPlayer.togglePaused()
                            }
                        }

                        Column {
                            anchors.centerIn: parent
                            width: Math.min(parent.width - 32, 360)
                            spacing: 8
                            visible: videoPlayer.errorString !== "" ||
                                     !videoPlayer.loaded
                            Label {
                                width: parent.width
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                                text: videoPlayer.errorString !== ""
                                      ? videoPlayer.errorString
                                      : videoPlayer.ready
                                        ? "Loading video…"
                                        : "Preparing video renderer…"
                                color: videoPlayer.errorString !== ""
                                       ? root.redColor : root.mutedTextColor
                                font.family: "Geist Mono"
                                font.pixelSize: 10
                            }
                            Button {
                                anchors.horizontalCenter: parent.horizontalCenter
                                visible: videoPlayer.source.toString() === ""
                                text: "Open video"
                                onClicked: videoFileDialog.open()
                            }
                        }
                    }

                }
            }
            Rectangle {
                SplitView.fillWidth: true
                SplitView.fillHeight: true
                color: root.traceBackgroundColor
                clip: true
                TraceView {
                    id: trace
                    objectName: "traceView"
                    anchors.fill: parent
                    store: appStore
                    focus: true
                    onCornerActivated: (index) => {
                        refreshCornerRows()
                        cornerWindow.selectedCornerIndex = index
                        cornerWindow.show()
                        cornerWindow.requestActivate()
                    }
                    onCornerRenameRequested: (index) => openCornerRename(index)
                    onChannelsRequested: {
                        refreshChannelRows()
                        channelsWindow.show()
                        channelsWindow.raise()
                    }
                    onCornerMenuRequested: (cornerIndex, cornerName,
                                            fraction, x, y) => {
                        cornerMenu.cornerIndex = cornerIndex
                        cornerMenu.cornerName = cornerName
                        cornerMenu.fraction = fraction
                        cornerMenu.popup(trace, x, y)
                    }
                    onChannelMenuRequested: (key, title, pinned, x, y) => {
                        channelMenu.channelKey = key
                        channelMenu.channelTitle = title
                        channelMenu.pinned = pinned
                        channelMenu.popup(trace, x, y)
                    }

                    Menu {
                        objectName: "cornerMenu"
                        id: cornerMenu
                        property int cornerIndex: -1
                        property string cornerName: ""
                        property real fraction: 0
                        MenuItem {
                            text: "Add zone here"
                            onTriggered: {
                                const index = trace.addCornerAt(
                                    cornerMenu.fraction)
                                if (index >= 0) root.openCornerRename(index)
                            }
                        }
                        MenuItem {
                            text: "Rename " + cornerMenu.cornerName + "…"
                            enabled: cornerMenu.cornerIndex >= 0
                            height: enabled ? implicitHeight : 0
                            visible: enabled
                            onTriggered:
                                root.openCornerRename(cornerMenu.cornerIndex)
                        }
                        MenuItem {
                            text: "Delete " + cornerMenu.cornerName
                            enabled: cornerMenu.cornerIndex >= 0
                            height: enabled ? implicitHeight : 0
                            visible: enabled
                            onTriggered:
                                store.deleteCorner(cornerMenu.cornerIndex)
                        }
                    }

                    Menu {
                        id: channelMenu
                        property string channelKey: ""
                        property string channelTitle: ""
                        property bool pinned: false
                        objectName: "channelMenu"
                        MenuItem {
                            text: channelMenu.pinned
                                  ? "Unpin " + channelMenu.channelTitle
                                  : "Pin " + channelMenu.channelTitle +
                                    " to top"
                            onTriggered:
                                trace.toggleSticky(channelMenu.channelKey)
                        }
                        MenuItem {
                            text: "Hide " + channelMenu.channelTitle
                            onTriggered:
                                trace.hideChannel(channelMenu.channelKey)
                        }
                        MenuSeparator {}
                        MenuItem {
                            text: "Show all standard channels"
                            onTriggered: trace.showAllStandardChannels()
                        }
                        MenuItem {
                            text: "Unpin all channels"
                            onTriggered: trace.unpinAllChannels()
                        }
                        MenuItem {
                            text: "More channels…"
                            onTriggered: {
                                root.refreshChannelRows()
                                channelsWindow.show()
                                channelsWindow.raise()
                            }
                        }
                    }
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_C) {
                            store.clearCompare()
                            event.accepted = true
                        } else if (event.key === Qt.Key_A) {
                            store.setEditingCorners(!store.editingCorners)
                            event.accepted = true
                        }
                    }
                }
                TraceCursorOverlay {
                    objectName: "traceOverlay"
                    anchors.fill: parent
                    trace: trace
                    z: 1
                }
                ToolButton {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 4
                    anchors.bottomMargin: 4
                    z: 2
                    text: "Channels…"
                    font.family: "Geist Mono"
                    font.pixelSize: 8
                    onClicked: {
                        refreshChannelRows()
                        channelsWindow.show()
                        channelsWindow.raise()
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: "Add or hide source channels"
                }
            }
            }

        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 64 : 0
            visible: !store.hasGpsData && store.comparing &&
                     alignmentRows.primary !== undefined &&
                     alignmentRows.primary.length > 1
            color: root.surfaceColor
            border.color: root.borderColor
            RowLayout {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 8
                Label {
                    Layout.preferredWidth: 66
                    text: "DAMPER\nALIGN"
                    font.family: "Geist Mono"
                    font.pixelSize: 8
                    font.bold: true
                    lineHeight: 0.9
                    color: root.mutedTextColor
                }
                Rectangle {
                    id: damperAlignmentRail
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 3
                    color: referenceAlignmentMouse.containsMouse ||
                           referenceAlignmentMouse.pressed
                           ? Qt.rgba(224 / 255, 157 / 255, 127 / 255, 0.13)
                           : root.traceBackgroundColor
                    border.width: 1
                    border.color: root.borderColor
                    Label {
                        width: 130
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        anchors.top: parent.top
                        anchors.topMargin: 5
                        text: "ACTIVE  " + activeSessionName
                        elide: Text.ElideRight
                        font.family: "Geist Mono"
                        font.pixelSize: 8
                        color: root.accentColor
                    }
                    Label {
                        width: 130
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 5
                        text: "REF     " + referenceSessionName
                        elide: Text.ElideRight
                        font.family: "Geist Mono"
                        font.pixelSize: 8
                        color: root.orangeColor
                    }
                    Rectangle {
                        width: 1
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.leftMargin: 138
                        color: root.borderColor
                    }
                    Canvas {
                        id: referenceDamper
                        anchors.left: parent.left
                        anchors.leftMargin: 138
                        anchors.right: dragLabel.left
                        anchors.rightMargin: 4
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        property var samples: alignmentRows.compare || []
                        property real shift: store.referenceAlignment
                        onSamplesChanged: requestPaint()
                        onShiftChanged: requestPaint()
                        onPaint: root.paintDamperStrip(
                            this, samples, root.orangeColor, shift, 0.52)
                    }
                    Canvas {
                        id: primaryDamper
                        anchors.left: parent.left
                        anchors.leftMargin: 138
                        anchors.right: dragLabel.left
                        anchors.rightMargin: 4
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        property var samples: alignmentRows.primary || []
                        onSamplesChanged: requestPaint()
                        onPaint: root.paintDamperStrip(
                            this, samples, root.accentColor, 0, 0.78)
                        z: 1
                    }
                    Label {
                        id: dragLabel
                        width: 62
                        anchors.right: parent.right
                        anchors.rightMargin: 5
                        anchors.verticalCenter: parent.verticalCenter
                        text: "DRAG ↔"
                        horizontalAlignment: Text.AlignRight
                        font.family: "Geist Mono"
                        font.pixelSize: 8
                        font.bold: true
                        color: root.orangeColor
                    }
                    MouseArea {
                        id: referenceAlignmentMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeHorCursor
                        property real pressX: 0
                        property real startOffset: 0
                        onPressed: (mouse) => {
                            pressX = mouse.x
                            startOffset = store.referenceAlignment
                        }
                        onPositionChanged: (mouse) => {
                            if (!pressed || width <= 0) return
                            store.referenceAlignment =
                                startOffset + (mouse.x - pressX) / width
                        }
                        onDoubleClicked: store.resetReferenceAlignment()
                        ToolTip.visible: containsMouse
                        ToolTip.text:
                            "Drag the orange reference trace; double-click to reset"
                    }
                }
                Column {
                    Layout.preferredWidth: 68
                    spacing: 2
                    Label {
                        text: {
                            const alignment = store.referenceAlignment
                            const seconds = store.referenceAlignmentSeconds()
                            return (seconds >= 0 ? "+" : "") +
                                   seconds.toFixed(3) + "s"
                        }
                        font.family: "Geist Mono"
                        font.pixelSize: 9
                        font.bold: true
                        color: Math.abs(store.referenceAlignment) > 0.00001
                               ? root.orangeColor : root.mutedTextColor
                    }
                    ToolButton {
                        text: "Reset"
                        enabled: Math.abs(store.referenceAlignment) > 0.00001
                        onClicked: store.resetReferenceAlignment()
                    }
                }
            }
        }
    }

    // ══ current / reference selector dot ════════════════════════════
    component RoleDot: Item {
        id: dot
        property bool selected: false
        property color activeColor: root.accentColor
        property string tip: ""
        signal activated()
        implicitWidth: 14
        implicitHeight: 14
        Rectangle {
            anchors.centerIn: parent
            width: dot.selected ? 12 : dotMouse.containsMouse ? 8 : 5
            height: width
            radius: width / 2
            color: dot.selected ? dot.activeColor
                   : dotMouse.containsMouse ? root.foregroundColor
                   : root.dimTextColor
            opacity: dot.selected ? 1.0 : dotMouse.containsMouse ? 0.85 : 0.45
            Behavior on width {
                NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
            }
            Behavior on opacity { NumberAnimation { duration: 110 } }
        }
        MouseArea {
            id: dotMouse
            anchors.fill: parent
            hoverEnabled: true
            onClicked: dot.activated()
            ToolTip.visible: containsMouse && dot.tip !== ""
            ToolTip.text: dot.tip
        }
    }

    // ══ sidebar delegate: collapsible rows ══════════════════════════
    Component {
        id: sidebarDelegate
        Item {
            id: row
            width: tree.width
            height: model.role === "track" ? 28
                    : model.role === "date" ? 24 : 38

            property string sessionKey: model.key || ""
            property bool activeSession:
                model.role === "session" && model.key === activeSessionKey
            property bool referenceSession:
                model.role === "session" && model.key === referenceSessionKey

            Rectangle {
                anchors.fill: parent
                color: activeSession ? root.selectionColor
                      : referenceSession
                        ? Qt.rgba(224 / 255, 157 / 255, 127 / 255, 0.14)
                      : model.role === "track" ? root.surfaceColor
                      : rowMouse.containsMouse ? root.backgroundColor
                      : "transparent"
            }

            RowLayout {
                anchors.fill: parent
                z: 1
                anchors.leftMargin: 4 + model.indent * 8
                anchors.rightMargin: model.role === "session" ? 46 : 4
                spacing: 4
                Label {
                    text: model.role === "track" || model.role === "date"
                          ? (model.expanded ? "▾" : "▸") : ""
                    Layout.preferredWidth: 10
                    color: root.dimTextColor
                    font.pixelSize: 8
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 0
                    RowLayout {
                        visible: model.role === "session"
                        Layout.fillWidth: true
                        spacing: 4
                        Rectangle {
                            visible: model.isVideo === true
                            Layout.preferredWidth: 15
                            Layout.preferredHeight: 11
                            radius: 2
                            color: "transparent"
                            border.width: 1
                            border.color: sessionRowLabel.color
                            Label {
                                anchors.centerIn: parent
                                text: "▶"
                                font.family: "Geist Mono"
                                font.pixelSize: 6
                                color: sessionRowLabel.color
                            }
                        }
                        Label {
                            id: sessionRowLabel
                            text: (model.driver || "Unknown") +
                                  (model.driverId !== "" ? "  ID " + model.driverId : "")
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.family: "Geist"
                            font.pixelSize: 10
                            font.bold: activeSession
                            color: activeSession ? root.accentColor
                                   : referenceSession ? root.orangeColor
                                   : root.foregroundColor
                        }
                    }
                    Label {
                        visible: model.role === "session"
                        text: (model.stem || model.name) +
                              "  ·  " + (model.sessionTime || "--:--:--") +
                              "  ·  best " + (model.bestTime || "—")
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        font.family: "Geist Mono"
                        font.pixelSize: 8
                        color: model.isDayBest
                               ? root.magentaColor
                               : model.isDriverBest
                                 ? root.greenColor
                                 : root.mutedTextColor
                    }
                    Label {
                        visible: model.role !== "session"
                        text: model.name
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        font.family: "Geist"
                        font.pixelSize: model.role === "track" ? 10 : 9
                        font.bold: model.role === "track"
                        color: model.role === "track"
                               ? root.foregroundColor : root.mutedTextColor
                    }
                }
                ToolButton {
                    visible: model.role === "track"
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                    text: "×"
                    onClicked: store.closeTrack(model.name)
                    ToolTip.visible: hovered
                    ToolTip.text: "Close track"
                }
            }

            Row {
                visible: model.role === "session"
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                z: 2
                RoleDot {
                    selected: row.activeSession
                    activeColor: root.accentColor
                    tip: "Make current lap"
                    onActivated: setSessionActive(row.sessionKey)
                }
                RoleDot {
                    selected: row.referenceSession
                    activeColor: root.orangeColor
                    tip: row.referenceSession ? "Clear reference"
                                              : "Make reference lap"
                    onActivated: setSessionReference(row.sessionKey)
                }
            }

            Menu {
                id: sessionMenu
                MenuItem {
                    objectName: "renameDriverMenuItem"
                    text: "Rename driver"
                    enabled: (model.mappingKey || "") !== ""
                    onTriggered: root.openDriverRename(
                        model.mappingKey || "", model.driver || "")
                }
                MenuSeparator {}
                MenuItem {
                    text: "Set active session (best lap)"
                    onTriggered: setSessionActive(model.key)
                }
                MenuItem {
                    text: "Set as reference (best lap)"
                    enabled: activeSessionKey !== "" &&
                             model.key !== activeSessionKey
                    onTriggered: setSessionReference(model.key)
                }
                MenuItem {
                    text: "Use this session only"
                    onTriggered: useSessionAlone(model.key)
                }
                MenuSeparator { visible: store.comparing }
                MenuItem {
                    text: "Clear reference"
                    visible: store.comparing
                    onTriggered: store.clearCompare()
                }
            }

            MouseArea {
                id: rowMouse
                z: 0
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (mouse) => {
                    if (mouse.button === Qt.RightButton) {
                        if (model.role === "session") {
                            sessionMenu.x = mouse.x
                            sessionMenu.y = mouse.y
                            sessionMenu.open()
                        }
                        return
                    }
                    if (model.role === "track") {
                        expandedTracks[model.name] = !trackExpanded(model.name)
                        rebuildTree()
                    } else if (model.role === "date") {
                        expandedDates[model.key] = !dateExpanded(model.key)
                        rebuildTree()
                    } else if (model.role === "session") {
                        setSessionActive(model.key)
                    }
                }
            }
        }
    }
}
