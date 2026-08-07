pragma ComponentBehavior: Bound
import Omatrack
import Qt.labs.platform as Platform

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: root

    property string activeSessionKey: ""
    property string activeSessionName: ""

    // Supplied by QQmlApplicationEngine::setInitialProperties in main().
    required property bool autotestWindows

    // Caches the root itself renders: the lap filmstrip, the damper-alignment
    // traces, and the telemetry directories listed in the drawer. Each is
    // refreshed from the matching Store signal in the Connections block below.
    property bool deltaTraceVisible: false
    property var directoryRows: []
    // Side by side only when the reference lap has its own recording and the
    // primary video is telemetry-linked, so both can be distance-aligned.
    readonly property bool dualVideo: telemetryVideoActive && Store.compareVideoSource.toString() !== "" && Store.compareVideoSource.toString() !== Store.primaryVideoSource.toString()
    property var expandedDates: ({})

    // Session-tree expansion state, keyed by track name and by track+date.
    property var expandedTracks: ({})
    property var filmstripSessions: []
    // Rapid selection changes update one pending source instead of queuing
    // stale callLater closures that can reopen the previous recording.
    property url pendingVideoSource: ""
    property string pointerTooltipOwner: ""
    property string pointerTooltipText: ""
    property real pointerTooltipX: 0
    property real pointerTooltipY: 0
    property string referenceSessionKey: ""
    property string referenceSessionName: ""
    property real referenceSyncBaseRate: 1
    property real referenceSyncError: 0
    property real referenceSyncLastPrimary: -1
    property real referenceSyncLastTarget: -1
    property string referenceSyncState: "WAIT"
    property string sessionFilter: ""
    property bool sidebarVisible: true
    required property url startupVideo
    property bool telemetryVideoActive: false
    property bool videoFullscreen: false
    property int videoRestoreVisibility: Window.Windowed
    property bool videoVisible: false

    function addDir(p): void {
        const path = root.toLocalPath(p);
        if (path === "")
            return;
        Store.addSessionDirectory(path);
        dirField.text = "";
        root.directoryRows = Store.sessionDirectories();
    }
    function bestLapForSession(key) {
        const laps = key !== "" ? Store.lapsForSession(key) : [];
        for (let i = 0; i < laps.length; ++i)
            if (laps[i].isFastest)
                return laps[i];
        for (let i = 0; i < laps.length; ++i)
            if (laps[i].countsForBest)
                return laps[i];
        for (let i = 0; i < laps.length; ++i)
            if (laps[i].isComplete)
                return laps[i];
        return laps.length > 0 ? laps[0] : null;
    }
    function dateExpanded(key) {
        return expandedDates[key] !== false;
    }
    function dateKey(trackName, dateName) {
        return trackName + "|" + dateName;
    }
    function defaultTelemetryFolder() {
        const home = toLocalPath(Platform.StandardPaths.writableLocation(Platform.StandardPaths.HomeLocation));
        const preferred = home + "/Documents/Telemetry";
        return "file://" + (Store.directoryExists(preferred) ? preferred : home);
    }
    function dismissCornerPopover() {
        cornerWindow.hide();
    }
    function dismissPointerTooltip(owner: string): void {
        if (root.pointerTooltipOwner === owner)
            root.pointerTooltipOwner = "";
    }
    function formatMediaTime(seconds) {
        if (!isFinite(seconds) || seconds < 0)
            seconds = 0;
        const total = Math.floor(seconds);
        const hours = Math.floor(total / 3600);
        const minutes = Math.floor((total % 3600) / 60);
        const secs = total % 60;
        const paddedMinutes = (hours > 0 && minutes < 10 ? "0" : "") + minutes;
        const paddedSeconds = (secs < 10 ? "0" : "") + secs;
        return hours > 0 ? hours + ":" + paddedMinutes + ":" + paddedSeconds : minutes + ":" + paddedSeconds;
    }
    function lapStripEntry(key, reference) {
        const laps = Store.lapsForSession(key);
        let total = 0;
        for (let i = 0; i < laps.length; ++i)
            total += Math.max(1, laps[i].timeMs);
        return {
            sessionKey: key,
            driverName: Store.driverDisplayName(key),
            bestTime: sessionInfoForKey(key)?.bestTime || "",
            reference: reference,
            laps: laps,
            totalTimeMs: Math.max(1, total)
        };
    }
    function movePointerTooltip(owner: string, x: real, y: real): void {
        if (root.pointerTooltipOwner !== owner)
            return;
        root.pointerTooltipX = Math.max(8, Math.min(root.width - pointerTooltip.implicitWidth - 8, x + 14));
        root.pointerTooltipY = Math.max(8, Math.min(root.height - pointerTooltip.implicitHeight - 8, y + 14));
    }
    function openCornerRename(index: int): void {
        const zones = Store.cornerList();
        if (index < 0 || index >= zones.length)
            return;
        cornerRenameDialog.cornerIndex = index;
        cornerRenameField.text = zones[index].name || "";
        cornerRenameDialog.open();
    }
    function openDriverRename(mappingKey, displayName) {
        if (!mappingKey)
            return;
        driverRenameDialog.mappingKey = mappingKey;
        driverRenameField.text = displayName || "";
        driverRenameDialog.open();
    }
    function openPendingVideo() {
        const source = pendingVideoSource;
        if (source.toString() === "" || videoPlayer.source.toString() === source.toString())
            return;
        videoPlayer.openMedia(source);
    }
    function rebuildTree() {
        if (!Store.ready)
            return;
        treeModel.clear();
        const groups = Store.trackGroups();
        const query = sessionFilter.trim().toLowerCase();
        for (let t = 0; t < groups.length; ++t) {
            const trackName = groups[t].track;
            const dates = groups[t].dates;
            let matchingDates = [];
            for (let d = 0; d < dates.length; ++d) {
                let matchingSessions = [];
                const sessions = dates[d].sessions;
                for (let s = 0; s < sessions.length; ++s) {
                    if (sessionMatches(sessions[s], trackName, dates[d].date, query))
                        matchingSessions.push(sessions[s]);
                }
                if (matchingSessions.length > 0)
                    matchingDates.push({
                        date: dates[d].date,
                        sessions: matchingSessions
                    });
            }
            if (matchingDates.length === 0)
                continue;
            const showTrack = query !== "" || trackExpanded(trackName);
            treeModel.append({
                role: "track",
                name: trackName,
                stem: "",
                driver: "",
                driverId: "",
                mappingKey: "",
                sessionTime: "",
                bestTime: "",
                isDriverBest: false,
                isDayBest: false,
                isVideo: false,
                indent: 0,
                key: "",
                expanded: showTrack
            });
            if (!showTrack)
                continue;
            for (let d = 0; d < matchingDates.length; ++d) {
                const dateName = matchingDates[d].date;
                const sessions = matchingDates[d].sessions;
                const dk = dateKey(trackName, dateName);
                const showDate = query !== "" || dateExpanded(dk);
                treeModel.append({
                    role: "date",
                    name: dateName,
                    stem: "",
                    driver: "",
                    driverId: "",
                    mappingKey: "",
                    sessionTime: "",
                    bestTime: "",
                    isDriverBest: false,
                    isDayBest: false,
                    isVideo: false,
                    indent: 1,
                    key: dk,
                    expanded: showDate
                });
                if (!showDate)
                    continue;
                for (let s = 0; s < sessions.length; ++s) {
                    const session = sessions[s];
                    const display = session.driver !== "" ? session.driver : "Unknown";
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
                    });
                }
            }
        }
    }
    function refreshDeltaTraceVisible(): void {
        root.deltaTraceVisible = Store.channelVisible("delta");
    }
    function refreshLapStrip() {
        activeSessionKey = Store.primarySessionKey;
        referenceSessionKey = Store.compareSessionKey;
        activeSessionName = sessionNameForKey(activeSessionKey);
        referenceSessionName = sessionNameForKey(referenceSessionKey);
        let strips = [];
        if (activeSessionKey !== "")
            strips.push(lapStripEntry(activeSessionKey, false));
        if (referenceSessionKey !== "" && referenceSessionKey !== activeSessionKey)
            strips.push(lapStripEntry(referenceSessionKey, true));
        filmstripSessions = strips;
    }
    function revealSession(key) {
        if (key === "" || sessionFilter !== "")
            return;
        const groups = Store.trackGroups();
        for (let t = 0; t < groups.length; ++t) {
            const dates = groups[t].dates;
            for (let d = 0; d < dates.length; ++d) {
                const sessions = dates[d].sessions;
                for (let s = 0; s < sessions.length; ++s) {
                    if (sessions[s].key !== key)
                        continue;
                    let tracks = Object.assign({}, expandedTracks);
                    tracks[groups[t].track] = true;
                    expandedTracks = tracks;
                    let expanded = Object.assign({}, expandedDates);
                    expanded[dateKey(groups[t].track, dates[d].date)] = true;
                    expandedDates = expanded;
                    return;
                }
            }
        }
    }
    function seekVideoRelative(seconds) {
        if (telemetryVideoActive)
            Store.seekCursorSeconds(seconds);
        else
            videoPlayer.seekRelative(seconds);
    }
    function seekVideoToTelemetry() {
        if (!telemetryVideoActive || !videoPlayer.loaded)
            return;
        const target = Store.primaryVideoTime;
        if (Math.abs(videoPlayer.position - target) > 0.025)
            videoPlayer.seek(target);
    }
    function sessionInfoForKey(key) {
        if (key === "")
            return null;
        const groups = Store.trackGroups();
        for (let t = 0; t < groups.length; ++t) {
            const dates = groups[t].dates;
            for (let d = 0; d < dates.length; ++d) {
                const sessions = dates[d].sessions;
                for (let s = 0; s < sessions.length; ++s)
                    if (sessions[s].key === key)
                        return sessions[s];
            }
        }
        return null;
    }
    function sessionMatches(session, trackName, dateName, query) {
        if (query === "")
            return true;
        const searchable = [trackName, dateName, session.stem, session.driver, session.driverId, session.carNumber, session.carClass, session.sessionTime, session.bestTime].join(" ").toLowerCase();
        return searchable.includes(query);
    }
    function sessionNameForKey(key) {
        for (let i = 0; i < treeModel.count; ++i) {
            const row = treeModel.get(i);
            if (row.role === "session" && row.key === key)
                return row.name;
        }
        return "";
    }
    function setSessionActive(key) {
        const lap = bestLapForSession(key);
        if (!lap)
            return;
        const referenceKey = referenceSessionKey;
        const referenceLap = bestLapForSession(referenceKey);
        Store.selectLap(key, lap.lapId);
        if (referenceKey !== "" && referenceKey !== key && referenceLap)
            Store.compareLap(referenceKey, referenceLap.lapId);
        else
            Store.clearCompare();
    }
    function setSessionReference(key) {
        if (key === root.referenceSessionKey) {
            Store.clearCompare();
            return;
        }
        if (activeSessionKey === "" || key === activeSessionKey) {
            useSessionAlone(key);
            return;
        }
        const lap = bestLapForSession(key);
        if (lap)
            Store.compareLap(key, lap.lapId);
    }
    function showPointerTooltip(owner: string, text: string, x: real, y: real): void {
        root.pointerTooltipOwner = owner;
        root.pointerTooltipText = text;
        root.movePointerTooltip(owner, x, y);
    }
    function showVideo(source, telemetryLinked) {
        telemetryVideoActive = telemetryLinked === true;
        videoVisible = true;
        if (root.width < 1000)
            sidebarVisible = false;
        pendingVideoSource = source;
        Qt.callLater(root.openPendingVideo);
    }
    function syncReferenceSource(): void {
        const ref = videoReference();
        if (!ref)
            return;
        const source = Store.compareVideoSource;
        if (source.toString() === "") {
            if (ref.source.toString() !== "")
                ref.closeMedia();
            return;
        }
        if (ref.source.toString() !== source.toString())
            ref.openMedia(source);
        else
            syncReferenceVideo(true);
    }
    function syncReferenceVideo(force: bool): void {
        const ref = videoReference();
        if (!ref || !ref.loaded) {
            referenceSyncState = "WAIT";
            return;
        }
        const target = Store.compareVideoTime;
        if (target <= 0) {
            ref.playbackRate = 1;
            referenceSyncState = "NO MAP";
            return;
        }

        const error = target - ref.position;
        const primaryDelta = videoPlayer.position - referenceSyncLastPrimary;
        referenceSyncError = error;
        if (force) {
            ref.playbackRate = Store.comparisonVideoRate;
            if (Math.abs(error) > 0.025)
                ref.seek(target);
            referenceSyncError = 0;
            referenceSyncBaseRate = Store.comparisonVideoRate;
            referenceSyncLastPrimary = videoPlayer.position;
            referenceSyncLastTarget = target;
            referenceSyncState = "LOCKED";
            return;
        }
        if (videoPlayer.paused) {
            ref.playbackRate = 1;
            if (Math.abs(error) > 0.025)
                ref.seek(target);
            referenceSyncError = 0;
            referenceSyncLastPrimary = videoPlayer.position;
            referenceSyncLastTarget = target;
            referenceSyncState = "LOCKED";
            return;
        }
        if (Math.abs(primaryDelta) > 0.5) {
            // Follow an explicit primary seek exactly. This is navigation, not
            // the periodic correction that previously made playback jump.
            referenceSyncBaseRate = Store.comparisonVideoRate;
            ref.playbackRate = referenceSyncBaseRate;
            ref.seek(target);
            referenceSyncError = 0;
            referenceSyncLastPrimary = videoPlayer.position;
            referenceSyncLastTarget = target;
            referenceSyncState = "LOCKED";
            return;
        }
        if (primaryDelta > 0.02) {
            const mappedRate = Store.comparisonVideoRate;
            referenceSyncBaseRate = mappedRate;
            referenceSyncLastPrimary = videoPlayer.position;
            referenceSyncLastTarget = target;
        }

        // Feed forward the track-map rate, then trim residual clock error.
        // No periodic seeks: continuous playback remains visually continuous.
        const correction = Math.max(-0.3, Math.min(0.3, error * 0.8));
        ref.playbackRate = Math.max(0.5, Math.min(2, referenceSyncBaseRate + correction));
        referenceSyncState = Math.abs(error) < 0.08 ? "LOCKED" : "TRIMMING";
    }
    function syncTelemetryVideo() {
        const source = Store.primaryVideoSource;
        if (source.toString() === "") {
            if (telemetryVideoActive) {
                videoPlayer.closeMedia();
                videoVisible = false;
                telemetryVideoActive = false;
            }
            return;
        }
        if (!telemetryVideoActive || videoPlayer.source.toString() !== source.toString()) {
            showVideo(source, true);
        } else {
            // Cancel any deferred open queued by an intermediate selection.
            pendingVideoSource = source;
            seekVideoToTelemetry();
        }
        syncReferenceSource();
    }
    function toLocalPath(value) {
        const text = value.toString();
        return text.startsWith("file://") ? decodeURIComponent(text.substring(7)) : text;
    }
    function trackExpanded(name) {
        return expandedTracks[name] === true;
    }
    function useSessionAlone(key) {
        const lap = bestLapForSession(key);
        if (!lap)
            return;
        Store.clearCompare();
        Store.selectLap(key, lap.lapId);
    }
    function videoFileName(source) {
        const text = source.toString();
        if (text === "")
            return "";
        return decodeURIComponent(text.substring(text.lastIndexOf("/") + 1));
    }
    // ── reference recording ─────────────────────────────────────────
    // The reference video is driven by the telemetry cursor and a cached,
    // monotonic track-position map. GPS fixes remove slow distance drift;
    // bounded playback-rate trim removes clock drift without periodic seeks.
    function videoReference(): MpvVideoItem {
        return videoReferenceLoader.player;
    }
    function videoSetFullscreen(on) {
        if (on === videoFullscreen)
            return;
        if (on) {
            videoVisible = true;
            videoRestoreVisibility = root.visibility;
            videoFullscreen = true;
            root.visibility = Window.FullScreen;
        } else {
            videoFullscreen = false;
            root.visibility = videoRestoreVisibility;
        }
    }
    function videoToggleMuted() {
        if (videoPlayer.loaded)
            Store.videoMuted = !Store.videoMuted;
    }
    function videoTogglePaused() {
        if (!videoPlayer.loaded)
            return;
        videoPlayer.togglePaused();
        if (!videoPlayer.paused)
            syncReferenceVideo(true);
    }

    Material.accent: Style.accentColor
    Material.background: Style.backgroundColor
    Material.foreground: Style.foregroundColor
    Material.primary: Style.surfaceColor
    Material.theme: Material.Dark
    color: Style.backgroundColor
    font.family: Style.uiFontFamily
    font.pixelSize: 11
    height: 800
    minimumHeight: 480
    minimumWidth: 720
    title: "Omatrack"
    visible: true
    width: 1280

    // ══ header ══════════════════════════════════════════════════════
    header: AppHeader {
        sidebarVisible: root.sidebarVisible
        visible: !root.videoFullscreen

        onChannelsRequested: {
            channelsWindow.refresh();
            channelsWindow.show();
            channelsWindow.raise();
        }
        onCornersRequested: {
            cornerWindow.refresh();
            cornerWindow.show();
            cornerWindow.requestActivate();
        }
        onDriverRenameRequested: (key, name) => root.openDriverRename(key, name)
        onOpenTelemetryRequested: drawer.open()
        onOpenVideoRequested: videoFileDialog.open()
        onPreferencesRequested: {
            settingsWindow.refresh();
            settingsWindow.show();
            settingsWindow.raise();
        }
        onSidebarToggleRequested: root.sidebarVisible = !root.sidebarVisible
    }

    Component.onCompleted: {
        root.refreshDeltaTraceVisible();
        root.rebuildTree();
        root.refreshLapStrip();
        root.syncTelemetryVideo();
        if (root.startupVideo.toString() !== "" && Store.primaryVideoSource.toString() === "")
            root.showVideo(root.startupVideo);
        if (root.autotestWindows) {
            cornerWindow.show();
            channelsWindow.show();
            settingsWindow.show();
        }
    }
    onWidthChanged: {
        if (root.videoVisible && root.width < 1000)
            root.sidebarVisible = false;
    }

    // One item tree with two homes: docked above the traces, or filling the
    // window. Reparenting stays inside this window so both libmpv items retain
    // the shared OpenGL scene-graph context.
    Item {
        id: videoFullscreenSlot

        anchors.fill: parent
        visible: root.videoFullscreen
        z: 9000
    }
    Rectangle {
        id: videoStage

        anchors.fill: parent
        clip: true
        color: Style.videoLetterboxColor
        objectName: "videoStage"
        parent: root.videoFullscreen ? videoFullscreenSlot : videoStageSlot

        HoverHandler {
            id: videoStageHover
        }
        Timer {
            interval: 100
            repeat: true
            running: root.dualVideo && videoPlayer.loaded && !videoPlayer.paused

            onTriggered: root.syncReferenceVideo(false)
        }
        RowLayout {
            anchors.fill: parent
            spacing: root.dualVideo ? 2 : 0

            Item {
                Layout.fillHeight: true
                Layout.fillWidth: true

                MpvVideoItem {
                    id: videoPlayer

                    anchors.fill: parent
                    muted: Store.videoMuted
                    objectName: "videoPlayer"

                    onLoadedChanged: {
                        if (loaded && root.telemetryVideoActive) {
                            Qt.callLater(() => {
                                videoPlayer.paused = true;
                                root.seekVideoToTelemetry();
                            });
                        }
                    }
                    onPositionChanged: {
                        if (root.telemetryVideoActive && loaded && !paused)
                            Store.setCursorFromVideoTime(position);
                    }
                }
                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: root.dualVideo ? 44 : 6
                    anchors.top: parent.top
                    bottomPadding: 2
                    color: Style.accentColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: 8
                    leftPadding: 4
                    rightPadding: 4
                    text: "ACTIVE  " + root.videoFileName(Store.primaryVideoSource)
                    topPadding: 2
                    visible: root.dualVideo

                    background: Rectangle {
                        color: Qt.rgba(0, 0, 0, 0.55)
                    }
                }
                Column {
                    anchors.centerIn: parent
                    spacing: 8
                    visible: videoPlayer.errorString !== "" || !videoPlayer.loaded
                    width: Math.min(parent.width - 32, 360)

                    Label {
                        color: videoPlayer.errorString !== "" ? Style.redColor : Style.mutedTextColor
                        font.family: Style.monoFontFamily
                        font.pixelSize: 10
                        horizontalAlignment: Text.AlignHCenter
                        text: videoPlayer.errorString !== "" ? videoPlayer.errorString : videoPlayer.ready ? "Loading video…" : "Preparing video renderer…"
                        width: parent.width
                        wrapMode: Text.Wrap
                    }
                    Button {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: "Open video"
                        visible: videoPlayer.source.toString() === ""

                        onClicked: videoFileDialog.open()
                    }
                }
            }
            Item {
                Layout.fillHeight: true
                Layout.fillWidth: true
                visible: root.dualVideo

                Loader {
                    id: videoReferenceLoader

                    readonly property MpvVideoItem player: item as MpvVideoItem

                    active: root.dualVideo
                    anchors.fill: parent

                    sourceComponent: Component {
                        MpvVideoItem {
                            muted: true
                            objectName: "videoPlayerReference"

                            onLoadedChanged: {
                                if (loaded)
                                    Qt.callLater(() => {
                                        paused = videoPlayer.paused;
                                        root.syncReferenceVideo(true);
                                    });
                            }
                        }
                    }

                    onLoaded: root.syncReferenceSource()
                }
                Connections {
                    function onPausedChanged(): void {
                        const ref = videoReferenceLoader.player;
                        if (ref && ref.paused !== videoPlayer.paused)
                            ref.paused = videoPlayer.paused;
                    }

                    target: videoReferenceLoader.player
                }
                Connections {
                    function onPausedChanged(): void {
                        const ref = root.videoReference();
                        if (!ref || !ref.loaded)
                            return;
                        ref.paused = videoPlayer.paused;
                        root.syncReferenceVideo(!videoPlayer.paused);
                    }

                    target: videoPlayer
                }
                Label {
                    anchors.left: parent.left
                    anchors.margins: 6
                    anchors.top: parent.top
                    bottomPadding: 2
                    color: Style.orangeColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: 8
                    leftPadding: 4
                    rightPadding: 4
                    text: "⇄ REFERENCE  " + root.videoFileName(Store.compareVideoSource)
                    topPadding: 2

                    background: Rectangle {
                        color: Qt.rgba(0, 0, 0, 0.55)
                    }
                }
                Label {
                    anchors.margins: 6
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 22
                    bottomPadding: 2
                    color: root.referenceSyncState === "LOCKED" && Store.comparisonAlignmentConfidence !== "LOW" ? Style.greenColor : Style.yellowColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 8
                    leftPadding: 4
                    rightPadding: 4
                    text: (Store.comparisonGpsAnchors > 0 ? "GPS×" + Store.comparisonGpsAnchors : "SPEED") + " " + Store.comparisonAlignmentConfidence + " · " + root.referenceSyncState + " · " + Math.round(Math.abs(root.referenceSyncError) * 1000) + "ms" + " · ×" + (videoReferenceLoader.player !== null ? videoReferenceLoader.player.playbackRate.toFixed(2) : "1.00")
                    topPadding: 2
                    visible: videoReferenceLoader.player !== null

                    background: Rectangle {
                        color: Qt.rgba(0, 0, 0, 0.55)
                    }
                }
                Label {
                    anchors.centerIn: parent
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: videoReferenceLoader.player !== null && videoReferenceLoader.player.errorString !== "" ? videoReferenceLoader.player.errorString : "Loading reference video…"
                    visible: videoReferenceLoader.player !== null && !videoReferenceLoader.player.loaded
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            enabled: videoPlayer.loaded

            onClicked: {
                videoPane.forceActiveFocus();
                root.videoTogglePaused();
            }
        }
        ToolButton {
            ToolTip.text: videoPlayer.muted ? "Enable audio (M)" : "Mute audio (M)"
            ToolTip.visible: hovered
            anchors.left: parent.left
            anchors.margins: 6
            anchors.top: parent.top
            enabled: videoPlayer.loaded
            height: 28
            objectName: "videoMuteButton"
            text: videoPlayer.muted ? "🔇" : "🔊"
            width: 32
            z: 2

            onClicked: root.videoToggleMuted()
        }
        Rectangle {
            id: videoControls

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            color: Qt.rgba(0, 0, 0, 0.78)
            height: 40
            objectName: "videoControls"
            opacity: videoPlayer.loaded && (videoStageHover.hovered || videoControlsHover.hovered) ? 1 : 0
            visible: opacity > 0.01

            Behavior on opacity {
                NumberAnimation {
                    duration: 110
                }
            }

            HoverHandler {
                id: videoControlsHover
            }
            MouseArea {
                anchors.fill: parent
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 4

                ToolButton {
                    Layout.preferredWidth: 54
                    ToolTip.text: root.dualVideo ? "Play/pause both recordings (Space)" : "Play/pause (Space)"
                    ToolTip.visible: hovered
                    implicitWidth: 54
                    leftPadding: 2
                    objectName: "videoPlayPauseButton"
                    rightPadding: 2
                    text: videoPlayer.paused ? "Play" : "Pause"

                    onClicked: root.videoTogglePaused()
                }
                Item {
                    Layout.fillWidth: true
                }
                Label {
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 9
                    text: root.formatMediaTime(videoPlayer.position) + " / " + root.formatMediaTime(videoPlayer.duration)
                }
                Item {
                    Layout.fillWidth: true
                }
                ToolButton {
                    Layout.preferredWidth: 48
                    ToolTip.text: root.videoFullscreen ? "Leave fullscreen (Esc)" : "Fullscreen (F)"
                    ToolTip.visible: hovered
                    implicitWidth: 48
                    leftPadding: 2
                    objectName: "videoFullscreenButton"
                    rightPadding: 2
                    text: root.videoFullscreen ? "Exit" : "Full"

                    onClicked: root.videoSetFullscreen(!root.videoFullscreen)
                }
            }
        }
    }
    ListModel {
        id: treeModel
    }
    Connections {
        function onChannelConfigChanged(): void {
            root.refreshDeltaTraceVisible();
        }
        function onDriverMappingsChanged(): void {
            root.rebuildTree();
        }
        function onSelectionChanged(): void {
            root.refreshLapStrip();
            root.revealSession(Store.primarySessionKey);
            root.rebuildTree();
            root.syncTelemetryVideo();
        }
        function onSessionsChanged(): void {
            root.rebuildTree();
            root.refreshLapStrip();
            root.directoryRows = Store.sessionDirectories();
        }
        function onStandaloneVideoRequested(source: url): void {
            root.showVideo(source, false);
        }
        function onVideoTimeChanged(): void {
            root.seekVideoToTelemetry();
            root.syncReferenceVideo(videoPlayer.paused);
        }

        target: Store
    }

    // ══ drawer (file open) ══════════════════════════════════════════
    Drawer {
        id: drawer

        edge: Qt.LeftEdge
        height: root.height
        width: Math.min(360, root.width * 0.86)

        background: Rectangle {
            color: Style.backgroundColor
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            Label {
                font.bold: true
                font.pixelSize: 15
                text: "Open session"
            }
            Label {
                color: Style.mutedTextColor
                text: "Scan a directory of .pds / .ld / .ldx / .vbo / .mp4 files"
                wrapMode: Text.Wrap
            }
            CompactTextField {
                id: dirField

                Layout.fillWidth: true
                placeholderText: "/path/to/telemetry"

                onAccepted: root.addDir(dirField.text)
            }
            RowLayout {
                Layout.fillWidth: true

                Button {
                    text: "Add"

                    onClicked: root.addDir(dirField.text)
                }
                Button {
                    text: "Choose…"

                    onClicked: folderDialog.open()
                }
                Button {
                    text: "Open file"

                    onClicked: fileDialog.open()
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: Store.loading

                BusyIndicator {
                    Layout.preferredHeight: 22
                    Layout.preferredWidth: 22
                    running: Store.loading
                }
                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    font.pixelSize: 10
                    text: "Scanning telemetry files…"
                }
            }
            Label {
                color: Style.mutedTextColor
                font.pixelSize: 12
                text: "Directories:"
                visible: Store.ready
            }
            ListView {
                id: drawerDirectories

                Layout.fillHeight: true
                Layout.fillWidth: true
                model: root.directoryRows

                delegate: RowLayout {
                    id: directoryRow

                    required property string modelData

                    width: drawerDirectories.width

                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        font.pixelSize: 10
                        text: directoryRow.modelData
                    }
                }
            }
        }
    }
    Platform.FolderDialog {
        id: folderDialog

        acceptLabel: "Add"
        folder: root.defaultTelemetryFolder()
        title: "Choose telemetry directory"

        onAccepted: root.addDir(folderDialog.folder)
    }
    Platform.FileDialog {
        id: fileDialog

        fileMode: Platform.FileDialog.OpenFile
        nameFilters: ["Telemetry (*.pds *.ld *.ldx *.vbo *.mp4 *.MP4)", "All files (*)"]
        title: "Open telemetry file"

        onAccepted: Store.openFile(root.toLocalPath(fileDialog.file))
    }
    Platform.FileDialog {
        id: videoFileDialog

        fileMode: Platform.FileDialog.OpenFile
        nameFilters: ["Video (*.mp4 *.MP4 *.mov *.MOV *.mkv *.MKV *.avi *.AVI *.m4v *.webm)", "All files (*)"]
        title: "Open onboard video"

        onAccepted: Store.openFile(root.toLocalPath(videoFileDialog.file))
    }
    Dialog {
        id: driverRenameDialog

        property string mappingKey: ""

        closePolicy: Popup.CloseOnEscape
        focus: true
        modal: true
        objectName: "driverRenameDialog"
        parent: Overlay.overlay
        standardButtons: Dialog.Save | Dialog.Cancel
        title: "Rename driver"
        width: Math.min(360, root.width - 32)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)

        contentItem: CompactTextField {
            id: driverRenameField

            objectName: "driverRenameField"
            placeholderText: "Driver name"

            onAccepted: driverRenameDialog.accept()
        }

        onAccepted: {
            Store.setDriverMapping(mappingKey, driverRenameField.text);
            settingsWindow.refresh();
        }
        onOpened: {
            driverRenameField.forceActiveFocus();
            driverRenameField.selectAll();
        }
    }
    TrackAssignmentDialog {
        id: trackAssignmentDialog

        parent: Overlay.overlay
        width: Math.min(440, root.width - 32)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
    }
    Dialog {
        id: cornerRenameDialog

        property int cornerIndex: -1

        closePolicy: Popup.CloseOnEscape
        focus: true
        modal: true
        objectName: "cornerRenameDialog"
        parent: Overlay.overlay
        standardButtons: Dialog.Save | Dialog.Cancel
        title: "Rename corner zone"
        width: Math.min(360, root.width - 32)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)

        contentItem: CompactTextField {
            id: cornerRenameField

            objectName: "cornerRenameField"
            placeholderText: "Corner name"

            onAccepted: cornerRenameDialog.accept()
        }

        onAccepted: Store.setCornerName(cornerIndex, cornerRenameField.text)
        onOpened: {
            cornerRenameField.forceActiveFocus();
            cornerRenameField.selectAll();
        }
    }

    // ══ corner inspector (separate Material window) ════════════════
    CornerInspectorWindow {
        id: cornerWindow

        onCornerDismissRequested: root.dismissCornerPopover()
        onCornerRenameRequested: index => root.openCornerRename(index)
    }
    ChannelsWindow {
        id: channelsWindow
    }
    PreferencesWindow {
        id: settingsWindow

        onDriverRenameRequested: (mappingKey, displayName) => root.openDriverRename(mappingKey, displayName)
    }
    ToolTip {
        id: pointerTooltip

        parent: Overlay.overlay
        text: root.pointerTooltipText
        visible: root.pointerTooltipOwner !== ""
        x: root.pointerTooltipX
        y: root.pointerTooltipY
    }
    Menu {
        id: lapMenu

        property int lapId: -1
        property string sessionKey: ""

        MenuItem {
            enabled: lapMenu.sessionKey !== Store.primarySessionKey || lapMenu.lapId !== Store.primaryLapIndex
            text: "Set as active lap"

            onTriggered: Store.selectLap(lapMenu.sessionKey, lapMenu.lapId)
        }
        MenuItem {
            enabled: lapMenu.sessionKey !== Store.primarySessionKey || lapMenu.lapId !== Store.primaryLapIndex
            text: "Set as reference lap"

            onTriggered: Store.compareLap(lapMenu.sessionKey, lapMenu.lapId)
        }
    }

    // ══ body ════════════════════════════════════════════════════════
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? root.filmstripSessions.length * 33 + 9 : 0
            color: Style.darkBackgroundColor
            visible: root.filmstripSessions.length > 0

            Column {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 3

                Repeater {
                    model: root.filmstripSessions

                    delegate: Rectangle {
                        id: sessionStrip

                        required property var modelData
                        property string selectedLapTime: {
                            const laps = strip.laps;
                            const key = strip.reference ? Store.compareSessionKey : Store.primarySessionKey;
                            const idx = strip.reference ? Store.compareLapIndex : Store.primaryLapIndex;
                            if (key === strip.sessionKey)
                                for (let i = 0; i < laps.length; ++i)
                                    if (laps[i].lapId === idx)
                                        return laps[i].timeText;
                            return strip.bestTime;
                        }
                        property var strip: modelData

                        border.color: Style.borderColor
                        color: strip.reference ? Qt.rgba(224 / 255, 157 / 255, 127 / 255, 0.06) : "transparent"
                        height: 30
                        radius: 4
                        width: parent.width

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 4
                            spacing: 5

                            Label {
                                Layout.maximumWidth: 190
                                Layout.minimumWidth: 190
                                Layout.preferredWidth: 190
                                color: sessionStrip.strip.reference ? Style.orangeColor : Style.accentColor
                                elide: Text.ElideRight
                                font.bold: true
                                font.family: Style.monoFontFamily
                                font.pixelSize: 9
                                text: (sessionStrip.strip.driverName !== "" && sessionStrip.strip.driverName !== "Unknown" ? sessionStrip.strip.driverName : "Unknown driver") + (sessionStrip.selectedLapTime !== "" ? " · " + sessionStrip.selectedLapTime : "")
                            }
                            ToolButton {
                                Layout.preferredHeight: 24
                                Layout.preferredWidth: 24
                                ToolTip.text: sessionStrip.strip.reference ? "Remove reference session" : "Clear active session"
                                ToolTip.visible: hovered
                                text: "×"

                                onClicked: sessionStrip.strip.reference ? Store.clearCompare() : Store.clearPrimary()
                            }
                            Item {
                                Layout.preferredWidth: 7
                            }
                            Item {
                                id: proportionalLapLane

                                Layout.fillHeight: true
                                Layout.fillWidth: true

                                Row {
                                    id: proportionalLapRow

                                    anchors.fill: parent
                                    spacing: 3

                                    Repeater {
                                        model: sessionStrip.strip.laps

                                        delegate: Rectangle {
                                            id: proportionalLap

                                            required property var modelData
                                            property bool selectedLap: sessionStrip.strip.reference ? sessionStrip.strip.sessionKey === Store.compareSessionKey && proportionalLap.modelData.lapId === Store.compareLapIndex : sessionStrip.strip.sessionKey === Store.primarySessionKey && proportionalLap.modelData.lapId === Store.primaryLapIndex
                                            readonly property string tooltipOwner: "lap:" + sessionStrip.strip.sessionKey + ":" + proportionalLap.modelData.lapId + ":" + sessionStrip.strip.reference

                                            // Bound to the Row, not `parent`:
                                            // a delegate evaluates its
                                            // bindings before it is reparented,
                                            // so `parent` is null on creation.
                                            anchors.verticalCenter: proportionalLapRow.verticalCenter
                                            border.color: sessionStrip.strip.reference ? Style.orangeColor : Style.accentColor
                                            border.width: proportionalLap.selectedLap ? 1 : 0
                                            color: proportionalLap.selectedLap ? Style.selectionColor : proportionalLapMouse.containsMouse ? Style.surfaceColor : Style.backgroundColor
                                            height: proportionalLapRow.height - 8
                                            radius: 3
                                            width: Math.max(1, (proportionalLapLane.width - Math.max(0, sessionStrip.strip.laps.length - 1) * 3) * proportionalLap.modelData.timeMs / sessionStrip.strip.totalTimeMs)

                                            Rectangle {
                                                anchors.bottom: parent.bottom
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                color: Style.greenColor
                                                height: proportionalLap.modelData.isFastest ? 2 : 0
                                            }
                                            Label {
                                                id: proportionalLapLabel

                                                anchors.fill: parent
                                                anchors.leftMargin: 5
                                                anchors.rightMargin: 5
                                                color: proportionalLap.selectedLap ? (sessionStrip.strip.reference ? Style.orangeColor : Style.accentColor) : proportionalLap.modelData.isFastest ? Style.greenColor : Style.foregroundColor
                                                elide: Text.ElideRight
                                                font.bold: proportionalLap.selectedLap
                                                font.family: Style.monoFontFamily
                                                font.pixelSize: 9
                                                text: proportionalLap.modelData.timeText
                                                verticalAlignment: Text.AlignVCenter
                                            }
                                            MouseArea {
                                                id: proportionalLapMouse

                                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                                anchors.fill: parent
                                                hoverEnabled: true

                                                onClicked: mouse => {
                                                    if (mouse.button === Qt.RightButton) {
                                                        root.dismissPointerTooltip(proportionalLap.tooltipOwner);
                                                        lapMenu.sessionKey = sessionStrip.strip.sessionKey;
                                                        lapMenu.lapId = proportionalLap.modelData.lapId;
                                                        lapMenu.popup(proportionalLap, mouse.x, mouse.y);
                                                        return;
                                                    }
                                                    if (sessionStrip.strip.reference)
                                                        Store.compareLap(sessionStrip.strip.sessionKey, proportionalLap.modelData.lapId);
                                                    else
                                                        Store.selectLap(sessionStrip.strip.sessionKey, proportionalLap.modelData.lapId);
                                                }
                                                onEntered: {
                                                    const point = proportionalLapMouse.mapToItem(Overlay.overlay, proportionalLapMouse.mouseX, proportionalLapMouse.mouseY);
                                                    root.showPointerTooltip(proportionalLap.tooltipOwner, proportionalLap.modelData.timeText + " · " + proportionalLap.modelData.label, point.x, point.y);
                                                }
                                                onExited: root.dismissPointerTooltip(proportionalLap.tooltipOwner)
                                                onPositionChanged: mouse => {
                                                    const point = proportionalLapMouse.mapToItem(Overlay.overlay, mouse.x, mouse.y);
                                                    root.movePointerTooltip(proportionalLap.tooltipOwner, point.x, point.y);
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
            Layout.fillHeight: true
            Layout.fillWidth: true
            orientation: Qt.Horizontal

            handle: Rectangle {
                color: Style.borderColor
                implicitWidth: 1
            }

            Pane {
                id: sidebarPane

                SplitView.fillHeight: true
                SplitView.maximumWidth: root.sidebarVisible ? 420 : 0
                SplitView.minimumWidth: root.sidebarVisible ? 185 : 0
                SplitView.preferredWidth: root.sidebarVisible ? Math.min(280, Math.max(210, root.width * 0.32)) : 0
                padding: 0
                visible: root.sidebarVisible

                background: Rectangle {
                    color: Style.darkBackgroundColor
                }

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 66
                        color: Style.surfaceColor

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 4
                            spacing: 0

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 34

                                Label {
                                    color: Style.mutedTextColor
                                    font.bold: true
                                    font.family: Style.monoFontFamily
                                    font.letterSpacing: 0.8
                                    font.pixelSize: 10
                                    text: "SESSIONS"
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                BusyIndicator {
                                    Layout.preferredHeight: 22
                                    Layout.preferredWidth: 22
                                    running: Store.loading
                                    visible: running
                                }
                                ToolButton {
                                    ToolTip.text: "Rescan session directories"
                                    ToolTip.visible: hovered
                                    enabled: !Store.loading
                                    text: "↻"

                                    onClicked: Store.scan()
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 28
                                spacing: 2

                                CompactTextField {
                                    id: sessionSearch

                                    Layout.fillWidth: true
                                    color: Style.foregroundColor
                                    placeholderText: "Filter driver, run, car, time…"
                                    placeholderTextColor: Style.dimTextColor

                                    onTextEdited: {
                                        root.sessionFilter = text;
                                        sessionFilterTimer.restart();
                                    }
                                }
                                ToolButton {
                                    Layout.preferredHeight: 24
                                    Layout.preferredWidth: 24
                                    ToolTip.text: "Clear session filter"
                                    ToolTip.visible: hovered
                                    text: "×"
                                    visible: sessionSearch.text !== ""

                                    onClicked: {
                                        sessionSearch.clear();
                                        root.sessionFilter = "";
                                        root.rebuildTree();
                                    }
                                }
                            }
                        }
                    }
                    Timer {
                        id: sessionFilterTimer

                        interval: 120
                        repeat: false

                        onTriggered: root.rebuildTree()
                    }
                    Item {
                        Layout.fillHeight: true
                        Layout.fillWidth: true

                        ListView {
                            id: tree

                            anchors.fill: parent
                            boundsBehavior: Flickable.StopAtBounds
                            clip: true
                            model: treeModel

                            ScrollBar.vertical: ThinScrollBar {
                            }
                            delegate: SessionTreeDelegate {
                                activeSessionKey: root.activeSessionKey
                                referenceSessionKey: root.referenceSessionKey

                                onDriverRenameRequested: (mappingKey, driver) => root.openDriverRename(mappingKey, driver)
                                onPointerTooltipDismissed: owner => root.dismissPointerTooltip(owner)
                                onPointerTooltipMoved: (owner, x, y) => root.movePointerTooltip(owner, x, y)
                                onPointerTooltipRequested: (owner, text, x, y) => root.showPointerTooltip(owner, text, x, y)
                                onSessionActivated: key => root.setSessionActive(key)
                                onSessionIsolated: key => root.useSessionAlone(key)
                                onSetActiveRequested: key => root.setSessionActive(key)
                                onSetReferenceRequested: key => root.setSessionReference(key)
                                onToggleDateRequested: key => {
                                    root.expandedDates[key] = !root.dateExpanded(key);
                                    root.rebuildTree();
                                }
                                onToggleTrackRequested: name => {
                                    root.expandedTracks[name] = !root.trackExpanded(name);
                                    root.rebuildTree();
                                }
                                onTrackAssignmentRequested: key => trackAssignmentDialog.openForSession(key)
                            }
                        }
                        Column {
                            anchors.centerIn: parent
                            spacing: 4
                            visible: Store.loading
                            z: 2

                            BusyIndicator {
                                anchors.horizontalCenter: parent.horizontalCenter
                                objectName: "sessionLoadingIndicator"
                                running: Store.loading
                            }
                            Label {
                                color: Style.mutedTextColor
                                font.family: Style.monoFontFamily
                                font.pixelSize: 10
                                text: "LOADING SESSIONS"
                            }
                        }
                    }
                }
            }
            SplitView {
                id: analysisSplit

                SplitView.fillHeight: true
                SplitView.fillWidth: true
                orientation: Qt.Vertical

                handle: Item {
                    id: analysisSplitHandle

                    implicitHeight: 7

                    HoverHandler {
                        cursorShape: Qt.SplitVCursor
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        color: analysisSplitHandle.SplitHandle.pressed || analysisSplitHandle.SplitHandle.hovered ? Style.accentColor : Style.borderColor
                        height: analysisSplitHandle.SplitHandle.pressed ? 3 : 1
                        width: parent.width
                    }
                }

                Rectangle {
                    id: videoPane

                    SplitView.fillWidth: true
                    SplitView.maximumHeight: visible ? root.height * 0.72 : 0
                    SplitView.minimumHeight: visible ? 180 : 0
                    SplitView.preferredHeight: visible ? Math.min(480, Math.max(220, root.height * 0.42)) : 0
                    border.color: Style.borderColor
                    border.width: 1
                    color: Style.traceBackgroundColor
                    focus: visible
                    objectName: "videoPane"
                    visible: root.videoVisible

                    Shortcut {
                        enabled: videoPane.visible && videoPlayer.loaded
                        sequence: "Space"

                        onActivated: root.videoTogglePaused()
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
                    Shortcut {
                        enabled: videoPane.visible && videoPlayer.loaded
                        sequence: "M"

                        onActivated: root.videoToggleMuted()
                    }
                    Shortcut {
                        enabled: videoPane.visible && videoPlayer.loaded
                        sequence: "F"

                        onActivated: root.videoSetFullscreen(!root.videoFullscreen)
                    }
                    Shortcut {
                        enabled: root.videoFullscreen
                        sequence: "Escape"

                        onActivated: root.videoSetFullscreen(false)
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            border.color: Style.borderColor
                            color: Style.surfaceColor

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 4
                                spacing: 6

                                Label {
                                    color: Style.accentColor
                                    font.bold: true
                                    font.family: Style.monoFontFamily
                                    font.letterSpacing: 0.8
                                    font.pixelSize: 9
                                    text: "VIDEO"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    Layout.preferredWidth: 1
                                    clip: true
                                    color: Style.foregroundColor
                                    elide: Text.ElideMiddle
                                    font.pixelSize: 10
                                    text: videoPlayer.title || "No video loaded"
                                }
                                Label {
                                    color: videoPlayer.seeking ? Style.yellowColor : Style.mutedTextColor
                                    font.family: Style.monoFontFamily
                                    font.pixelSize: 8
                                    text: videoPlayer.seeking ? "SEEK" : videoPlayer.loaded ? (videoPlayer.paused ? "PAUSED" : "PLAYING") : ""
                                    visible: videoPane.width >= 470
                                }
                                ToolButton {
                                    ToolTip.text: "Open another video"
                                    ToolTip.visible: hovered
                                    text: "Open"
                                    visible: videoPane.width >= 410

                                    onClicked: videoFileDialog.open()
                                }
                                ToolButton {
                                    Layout.preferredWidth: 28
                                    ToolTip.text: "Close video"
                                    ToolTip.visible: hovered
                                    implicitWidth: 28
                                    leftPadding: 2
                                    rightPadding: 2
                                    text: "×"

                                    onClicked: {
                                        if (root.videoFullscreen)
                                            root.videoSetFullscreen(false);
                                        videoPlayer.closeMedia();
                                        root.telemetryVideoActive = false;
                                        root.videoVisible = false;
                                    }
                                }
                            }
                        }
                        // Docked home of videoStage; the stage itself is at
                        // window scope so it can move fullscreen without
                        // rebuilding either libmpv render context.
                        Item {
                            id: videoStageSlot

                            Layout.fillHeight: true
                            Layout.fillWidth: true
                            objectName: "videoStageSlot"
                        }
                    }
                }
                Rectangle {
                    SplitView.fillHeight: true
                    SplitView.fillWidth: true
                    clip: true
                    color: Style.traceBackgroundColor

                    TraceView {
                        id: trace

                        anchors.fill: parent
                        focus: true
                        objectName: "traceView"
                        store: Store

                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_C) {
                                Store.clearCompare();
                                event.accepted = true;
                            } else if (event.key === Qt.Key_A) {
                                Store.setEditingCorners(!Store.editingCorners);
                                event.accepted = true;
                            }
                        }
                        onChannelMenuRequested: (key, title, pinned, x, y) => {
                            channelMenu.channelKey = key;
                            channelMenu.channelTitle = title;
                            channelMenu.pinned = pinned;
                            channelMenu.popup(trace, x, y);
                        }
                        onChannelsRequested: {
                            channelsWindow.refresh();
                            channelsWindow.show();
                            channelsWindow.raise();
                        }
                        onCornerActivated: index => {
                            cornerWindow.refresh();
                            cornerWindow.selectedCornerIndex = index;
                            cornerWindow.show();
                            cornerWindow.requestActivate();
                        }
                        onCornerMenuRequested: (cornerIndex, cornerName, fraction, x, y) => {
                            cornerMenu.cornerIndex = cornerIndex;
                            cornerMenu.cornerName = cornerName;
                            cornerMenu.fraction = fraction;
                            cornerMenu.popup(trace, x, y);
                        }
                        onCornerRenameRequested: index => root.openCornerRename(index)

                        Menu {
                            id: cornerMenu

                            property int cornerIndex: -1
                            property string cornerName: ""
                            property real fraction: 0

                            objectName: "cornerMenu"

                            MenuItem {
                                text: "Add zone here"

                                onTriggered: {
                                    const index = trace.addCornerAt(cornerMenu.fraction);
                                    if (index >= 0)
                                        root.openCornerRename(index);
                                }
                            }
                            MenuItem {
                                enabled: cornerMenu.cornerIndex >= 0
                                height: enabled ? implicitHeight : 0
                                text: "Rename " + cornerMenu.cornerName + "…"
                                visible: enabled

                                onTriggered: root.openCornerRename(cornerMenu.cornerIndex)
                            }
                            MenuItem {
                                enabled: cornerMenu.cornerIndex >= 0
                                height: enabled ? implicitHeight : 0
                                text: "Delete " + cornerMenu.cornerName
                                visible: enabled

                                onTriggered: Store.deleteCorner(cornerMenu.cornerIndex)
                            }
                        }
                        Menu {
                            id: channelMenu

                            property string channelKey: ""
                            property string channelTitle: ""
                            property bool pinned: false

                            objectName: "channelMenu"

                            MenuItem {
                                text: channelMenu.pinned ? "Unpin " + channelMenu.channelTitle : "Pin " + channelMenu.channelTitle + " to top"

                                onTriggered: trace.toggleSticky(channelMenu.channelKey)
                            }
                            MenuItem {
                                text: "Hide " + channelMenu.channelTitle

                                onTriggered: trace.hideChannel(channelMenu.channelKey)
                            }
                            MenuSeparator {
                            }
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
                                    channelsWindow.refresh();
                                    channelsWindow.show();
                                    channelsWindow.raise();
                                }
                            }
                        }
                    }
                    TraceCursorOverlay {
                        anchors.fill: parent
                        objectName: "traceOverlay"
                        trace: trace
                        z: 1
                    }
                    ToolButton {
                        ToolTip.text: Store.comparing ? (root.deltaTraceVisible ? "Hide comparison delta-time trace" : "Show comparison delta-time trace") : "Select a comparison lap to show delta time"
                        ToolTip.visible: hovered
                        anchors.left: parent.left
                        anchors.top: parent.top
                        checked: root.deltaTraceVisible
                        enabled: Store.comparing
                        height: 22
                        objectName: "deltaTraceButton"
                        text: "Δt"
                        width: 58
                        z: 2

                        onClicked: Store.setChannelVisible("delta", !root.deltaTraceVisible)
                    }
                    Rectangle {
                        anchors.fill: parent
                        color: Qt.rgba(0, 0, 0, 0.32)
                        visible: Store.lapLoading
                        z: 3

                        Rectangle {
                            anchors.centerIn: parent
                            border.color: Style.borderColor
                            border.width: 1
                            color: Style.surfaceColor
                            height: 76
                            radius: 4
                            width: 132

                            Column {
                                anchors.centerIn: parent
                                spacing: 4

                                BusyIndicator {
                                    Material.accent: Style.accentColor
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    objectName: "lapLoadingIndicator"
                                    running: Store.lapLoading
                                }
                                Label {
                                    color: Style.foregroundColor
                                    font.family: Style.monoFontFamily
                                    font.pixelSize: 10
                                    text: "LOADING LAP"
                                }
                            }
                        }
                    }
                    ToolButton {
                        ToolTip.text: "Add or hide source channels"
                        ToolTip.visible: hovered
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 4
                        anchors.left: parent.left
                        anchors.leftMargin: 4
                        font.family: Style.monoFontFamily
                        font.pixelSize: 8
                        text: "Channels…"
                        z: 2

                        onClicked: {
                            channelsWindow.refresh();
                            channelsWindow.show();
                            channelsWindow.raise();
                        }
                    }
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 64 : 0
            border.color: Style.borderColor
            color: Style.surfaceColor
            visible: !Store.hasGpsData && Store.comparing && Store.hasDamperAlignment()

            RowLayout {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 8

                Label {
                    Layout.preferredWidth: 66
                    color: Style.mutedTextColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: 8
                    lineHeight: 0.9
                    text: "DAMPER\nALIGN"
                }
                Rectangle {
                    id: damperAlignmentRail

                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    border.color: Style.borderColor
                    border.width: 1
                    color: referenceAlignmentMouse.containsMouse || referenceAlignmentMouse.pressed ? Qt.rgba(224 / 255, 157 / 255, 127 / 255, 0.13) : Style.traceBackgroundColor
                    radius: 3

                    Label {
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        anchors.top: parent.top
                        anchors.topMargin: 5
                        color: Style.accentColor
                        elide: Text.ElideRight
                        font.family: Style.monoFontFamily
                        font.pixelSize: 8
                        text: "ACTIVE  " + root.activeSessionName
                        width: 130
                    }
                    Label {
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 5
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        color: Style.orangeColor
                        elide: Text.ElideRight
                        font.family: Style.monoFontFamily
                        font.pixelSize: 8
                        text: "REF     " + root.referenceSessionName
                        width: 130
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.leftMargin: 138
                        anchors.top: parent.top
                        color: Style.borderColor
                        width: 1
                    }
                    DamperStripView {
                        id: referenceDamper

                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.leftMargin: 138
                        anchors.right: dragLabel.left
                        anchors.rightMargin: 4
                        anchors.top: parent.top
                        color: Style.orangeColor
                        series: DamperStripView.Compare
                        shift: Store.referenceAlignment
                        store: Store
                        strokeOpacity: 0.52
                    }
                    DamperStripView {
                        id: primaryDamper

                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.leftMargin: 138
                        anchors.right: dragLabel.left
                        anchors.rightMargin: 4
                        anchors.top: parent.top
                        color: Style.accentColor
                        series: DamperStripView.Primary
                        store: Store
                        strokeOpacity: 0.78
                        z: 1
                    }
                    Label {
                        id: dragLabel

                        anchors.right: parent.right
                        anchors.rightMargin: 5
                        anchors.verticalCenter: parent.verticalCenter
                        color: Style.orangeColor
                        font.bold: true
                        font.family: Style.monoFontFamily
                        font.pixelSize: 8
                        horizontalAlignment: Text.AlignRight
                        text: "DRAG ↔"
                        width: 62
                    }
                    MouseArea {
                        id: referenceAlignmentMouse

                        property real pressX: 0
                        property real startOffset: 0

                        ToolTip.text: "Drag the orange reference trace; double-click to reset"
                        ToolTip.visible: containsMouse
                        anchors.fill: parent
                        cursorShape: Qt.SizeHorCursor
                        hoverEnabled: true

                        onDoubleClicked: Store.resetReferenceAlignment()
                        onPositionChanged: mouse => {
                            if (!pressed || width <= 0)
                                return;
                            Store.referenceAlignment = startOffset + (mouse.x - pressX) / width;
                        }
                        onPressed: mouse => {
                            pressX = mouse.x;
                            startOffset = Store.referenceAlignment;
                        }
                    }
                }
                Column {
                    Layout.preferredWidth: 68
                    spacing: 2

                    Label {
                        color: Math.abs(Store.referenceAlignment) > 0.00001 ? Style.orangeColor : Style.mutedTextColor
                        font.bold: true
                        font.family: Style.monoFontFamily
                        font.pixelSize: 9
                        text: {
                            const alignment = Store.referenceAlignment;
                            const seconds = Store.referenceAlignmentSeconds();
                            return (seconds >= 0 ? "+" : "") + seconds.toFixed(3) + "s";
                        }
                    }
                    ToolButton {
                        enabled: Math.abs(Store.referenceAlignment) > 0.00001
                        text: "Reset"

                        onClicked: Store.resetReferenceAlignment()
                    }
                }
            }
        }
    }

    // ══ sidebar delegate: collapsible rows ══════════════════════════
}
