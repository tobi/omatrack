pragma ComponentBehavior: Bound
import Omatrack
import Qt.labs.platform as Platform

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: root

    property bool _lapStripDirty: true
    property var _sessionInfoCache: ({})
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
    property var filmstripSessions: []
    property int lapAdvanceCount: 0
    property int lapAdvanceNextId: -1
    property string lapAdvanceNextLabel: ""
    property bool lapAdvanceResume: false
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
    property int referenceSyncPauseAttempts: 0
    property string referenceSyncState: "WAIT"
    property bool sidebarVisible: true
    readonly property bool standaloneVideoActive: root.videoVisible && !root.telemetryVideoActive
    required property url startupVideo
    property bool telemetryVideoActive: false
    property bool videoControlsRequested: true
    property bool videoFullscreen: false
    // Fullscreen compose: 1 split, 2 active+pip, 3 ref+pip, 4 active,
    // 5 reference. Docked playback always uses split when both exist.
    property int videoFullscreenLayout: 1
    readonly property string videoFullscreenLayoutTitle: root.videoFullscreenLayout === 1 ? "SPLIT" : root.videoFullscreenLayout === 2 ? "ACTIVE + REF" : root.videoFullscreenLayout === 3 ? "REF + ACTIVE" : root.videoFullscreenLayout === 4 ? "ACTIVE" : root.videoFullscreenLayout === 5 ? "REFERENCE" : ""
    property bool videoOverlayVisible: true
    property int videoRestoreVisibility: Window.Windowed
    property bool videoSlowMotion: false
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
    function cancelLapAdvance(): void {
        lapAdvanceTimer.stop();
        root.lapAdvanceCount = 0;
        root.lapAdvanceNextId = -1;
        root.lapAdvanceNextLabel = "";
        root.lapAdvanceResume = false;
    }
    function defaultTelemetryFolder() {
        return "file://" + Store.defaultTelemetryDirectory();
    }
    function dismissCornerPopover() {
        Store.clearCornerFocus();
    }
    function dismissPointerTooltip(owner: string): void {
        if (root.pointerTooltipOwner === owner)
            root.pointerTooltipOwner = "";
    }
    function finishLapAdvance(): void {
        const sessionKey = Store.primarySessionKey;
        const nextId = root.lapAdvanceNextId;
        root.cancelLapAdvance();
        if (sessionKey === "" || nextId < 0)
            return;
        Store.selectLap(sessionKey, nextId);
        root.lapAdvanceResume = true;
        root.tryResumeLapAdvance();
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
    function hideVideo(): void {
        root.cancelLapAdvance();
        root.pendingVideoSource = "";
        if (root.videoFullscreen)
            root.videoSetFullscreen(false);
        const reference = root.videoReference();
        if (reference && reference.source.toString() !== "")
            reference.closeMedia();
        if (videoPlayer.source.toString() !== "")
            videoPlayer.closeMedia();
        root.telemetryVideoActive = false;
        root.videoVisible = false;
    }
    function lapStripEntry(key, reference) {
        const laps = Store.lapsForSession(key);
        let fixedLapCount = 0;
        let flexibleTimeMs = 0;
        for (let i = 0; i < laps.length; ++i) {
            if (!laps[i].countsForBest)
                ++fixedLapCount;
            else
                flexibleTimeMs += Math.max(1, laps[i].timeMs);
        }
        return {
            sessionKey: key,
            driverName: Store.driverDisplayName(key),
            bestTime: sessionInfoForKey(key)?.bestTime || "",
            reference: reference,
            laps: laps,
            fixedLapCount: fixedLapCount,
            flexibleTimeMs: Math.max(1, flexibleTimeMs)
        };
    }
    function lockPrimaryRealtime(): void {
        const rate = root.videoSlowMotion && root.videoFullscreen ? 0.25 : 1;
        if (videoPlayer.loaded && videoPlayer.playbackRate !== rate)
            videoPlayer.playbackRate = rate;
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
    function openDroppedFiles(urls): void {
        for (let i = 0; i < urls.length; ++i)
            Store.openFile(root.toLocalPath(urls[i]));
    }
    function openPendingVideo() {
        const source = pendingVideoSource;
        if (source.toString() === "" || videoPlayer.source.toString() === source.toString())
            return;
        videoPlayer.openMedia(source);
    }
    function primaryClockRate(): real {
        return root.videoSlowMotion && root.videoFullscreen ? 0.25 : 1;
    }
    function realignPausedVideos(): void {
        const ref = root.videoReference();
        if (!root.dualVideo || !videoPlayer.loaded || !videoPlayer.paused || !ref || !ref.loaded)
            return;

        // Use the primary player's final stopped time as truth. The store maps
        // that station onto the reference with its shared GPS/speed alignment.
        Store.setCursorFromVideoTime(videoPlayer.position);
        const target = Store.compareVideoTime;
        root.referenceSyncPauseAttempts = 0;
        if (target <= 0) {
            ref.playbackRate = 1;
            root.referenceSyncState = "NO MAP";
            return;
        }

        ref.paused = true;
        ref.playbackRate = 1;
        root.referenceSyncBaseRate = 1;
        root.referenceSyncError = target - ref.position;
        root.referenceSyncLastPrimary = videoPlayer.position;
        root.referenceSyncLastTarget = target;
        root.referenceSyncPauseAttempts = 1;
        root.referenceSyncState = "ALIGNING";
        // A pause is an explicit synchronization checkpoint, so request an
        // exact seek even when both reported positions are already close.
        ref.seek(target);
        pausedVideoAlignmentTimer.restart();
    }
    function refreshDeltaTraceVisible(): void {
        root.deltaTraceVisible = Store.channelVisible("delta");
    }
    function refreshLapStrip() {
        const newActive = Store.primarySessionKey;
        const newReference = Store.compareSessionKey;
        if (!root._lapStripDirty && newActive === activeSessionKey && newReference === referenceSessionKey)
            return;
        root._lapStripDirty = false;
        activeSessionKey = newActive;
        referenceSessionKey = newReference;
        activeSessionName = sessionNameForKey(activeSessionKey);
        referenceSessionName = sessionNameForKey(referenceSessionKey);
        let strips = [];
        if (activeSessionKey !== "")
            strips.push(lapStripEntry(activeSessionKey, false));
        if (referenceSessionKey !== "" && referenceSessionKey !== activeSessionKey)
            strips.push(lapStripEntry(referenceSessionKey, true));
        filmstripSessions = strips;
    }
    // A streamed recording is reached through a signature that expires, and a
    // laptop closed at the circuit and opened on the plane home wakes up
    // holding a dead one. The store can sign a new address for the same file;
    // the player picks up where it was. If it has nothing to offer — a local
    // file, or a recording it never produced — the error already on screen is
    // the right answer.
    function reopenExpiredVideo(player: MpvVideoItem): void {
        if (!player)
            return;
        const fresh = Store.refreshedVideoSource(player.source);
        if (fresh.toString() !== "")
            player.reopenMedia(fresh);
    }
    function revealVideoControls(): void {
        if (!root.videoVisible)
            return;
        root.videoControlsRequested = true;
        videoControlsHideTimer.restart();
    }
    function seekVideoRelative(seconds) {
        if (!videoPlayer.loaded)
            return;
        if (root.lapAdvanceCount > 0)
            root.cancelLapAdvance();
        const target = Math.max(0, videoPlayer.position + seconds);
        videoPlayer.seek(target);
        if (root.telemetryVideoActive)
            Store.setCursorFromVideoTime(target);
        if (root.dualVideo)
            root.syncReferenceVideo(true);
        root.revealVideoControls();
    }
    function seekVideoToTelemetry() {
        if (!telemetryVideoActive || !videoPlayer.loaded)
            return;
        root.lockPrimaryRealtime();
        const target = Store.primaryVideoTime;
        const error = Math.abs(videoPlayer.position - target);
        if (videoPlayer.paused) {
            if (error > 0.025)
                videoPlayer.seek(target);
            return;
        }
        // Playing: the primary file is the clock. Only honor an explicit
        // cursor jump — never tug the recording back onto a telemetry sample.
        if (error > 0.2)
            videoPlayer.seek(target);
    }
    function sessionInfoForKey(key) {
        if (key === "")
            return null;
        const cached = root._sessionInfoCache[key];
        if (cached)
            return cached;
        // Cache miss: rebuild from track groups, then retry.
        const cache = {};
        const groups = Store.trackGroups();
        for (let t = 0; t < groups.length; ++t) {
            const dates = groups[t].dates;
            for (let d = 0; d < dates.length; ++d) {
                const sessions = dates[d].sessions;
                for (let s = 0; s < sessions.length; ++s)
                    cache[sessions[s].key] = sessions[s];
            }
        }
        root._sessionInfoCache = cache;
        return cache[key] || null;
    }
    function sessionNameForKey(key) {
        const session = root.sessionInfoForKey(key);
        return session ? (session.driver || session.stem || "") : "";
    }
    function setSessionActive(key) {
        if (key !== "" && key === Store.primarySessionKey)
            return;
        const lap = bestLapForSession(key);
        if (!lap) {
            Store.clearCompare();
            Store.selectSession(key, false);
            return;
        }
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
        else
            Store.selectSession(key, true);
    }
    function setVideoFullscreenLayout(layout: int): void {
        if (layout < 1 || layout > 5)
            return;
        if (!root.dualVideo && layout !== 4)
            return;
        root.videoFullscreenLayout = layout;
        root.revealVideoControls();
    }
    function showPointerTooltip(owner: string, text: string, x: real, y: real): void {
        root.pointerTooltipOwner = owner;
        root.pointerTooltipText = text;
        root.movePointerTooltip(owner, x, y);
    }
    function showVideo(source, telemetryLinked) {
        telemetryVideoActive = telemetryLinked === true;
        videoVisible = true;
        root.revealVideoControls();
        if (root.width < 1000)
            sidebarVisible = false;
        pendingVideoSource = source;
        Qt.callLater(root.openPendingVideo);
    }
    function startLapAdvance(): void {
        if (root.lapAdvanceCount > 0)
            return;
        const nextId = Store.nextPrimaryLapId();
        videoPlayer.paused = true;
        if (nextId < 0)
            return;
        root.lapAdvanceNextId = nextId;
        root.lapAdvanceNextLabel = Store.lapLabel(Store.primarySessionKey, nextId);
        root.lapAdvanceCount = 3;
        Store.prefetchLap(Store.primarySessionKey, nextId);
        lapAdvanceTimer.start();
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
        if (force || videoPlayer.paused || Math.abs(primaryDelta) > 0.5) {
            // Pause, play-start, and an explicit primary jump all snap the
            // reference onto the mapped station. Continuous play never seeks.
            ref.playbackRate = videoPlayer.paused ? 1 : root.primaryClockRate();
            ref.seek(target);
            referenceSyncError = 0;
            referenceSyncBaseRate = 1;
            referenceSyncLastPrimary = videoPlayer.position;
            referenceSyncLastTarget = target;
            referenceSyncState = videoPlayer.paused ? "ALIGNING" : "LOCKED";
            if (videoPlayer.paused)
                pausedVideoAlignmentTimer.restart();
            return;
        }

        referenceSyncLastPrimary = videoPlayer.position;
        referenceSyncLastTarget = target;
        const rate = Store.referencePlaybackRate(ref.position) * root.primaryClockRate();
        referenceSyncBaseRate = rate;
        ref.playbackRate = rate;
        if (Store.cursorInCorner())
            referenceSyncState = Math.abs(error) < 0.08 ? "CORNER" : "HOLD";
        else
            referenceSyncState = Math.abs(error) < 0.08 ? "LOCKED" : "STRAIGHT";
    }
    function syncTelemetryVideo() {
        const source = Store.primaryVideoSource;
        if (source.toString() === "") {
            root.hideVideo();
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
    function tickLapAdvance(): void {
        if (root.lapAdvanceCount <= 1) {
            root.finishLapAdvance();
            return;
        }
        root.lapAdvanceCount = root.lapAdvanceCount - 1;
    }
    function toLocalPath(value) {
        const text = value.toString();
        return text.startsWith("file://") ? decodeURIComponent(text.substring(7)) : text;
    }
    function toggleVideoSlowMotion(): void {
        if (!root.videoFullscreen || !videoPlayer.loaded)
            return;
        root.videoSlowMotion = !root.videoSlowMotion;
        root.lockPrimaryRealtime();
        if (root.dualVideo && !videoPlayer.paused)
            root.syncReferenceVideo(false);
        root.revealVideoControls();
    }
    function tryResumeLapAdvance(): void {
        if (!root.lapAdvanceResume)
            return;
        if (Store.lapLoading || videoPlayer.seeking)
            return;
        root.lapAdvanceResume = false;
        if (!videoPlayer.loaded)
            return;
        root.lockPrimaryRealtime();
        videoPlayer.paused = false;
        if (root.dualVideo)
            root.syncReferenceVideo(true);
    }
    function useSessionAlone(key) {
        const lap = bestLapForSession(key);
        Store.clearCompare();
        if (lap)
            Store.selectLap(key, lap.lapId);
        else
            Store.selectSession(key, false);
    }
    function verifyPausedVideoAlignment(): void {
        const ref = root.videoReference();
        if (!root.dualVideo || !videoPlayer.paused || !ref || !ref.loaded)
            return;
        if (ref.seeking) {
            pausedVideoAlignmentTimer.restart();
            return;
        }

        // time-pos can advance once more while the pause command settles.
        // Re-sample it before checking the reference player's exact seek.
        Store.setCursorFromVideoTime(videoPlayer.position);
        const target = Store.compareVideoTime;
        if (target <= 0) {
            root.referenceSyncState = "NO MAP";
            return;
        }
        const error = target - ref.position;
        root.referenceSyncError = error;
        root.referenceSyncLastPrimary = videoPlayer.position;
        root.referenceSyncLastTarget = target;
        if (Math.abs(error) > 0.025 && root.referenceSyncPauseAttempts < 3) {
            ++root.referenceSyncPauseAttempts;
            root.referenceSyncState = "ALIGNING";
            ref.seek(target);
            pausedVideoAlignmentTimer.restart();
            return;
        }
        root.referenceSyncPauseAttempts = 0;
        root.referenceSyncState = Math.abs(error) <= 0.05 ? "LOCKED" : "BEST";
    }
    function videoFileName(source) {
        const text = source.toString();
        if (text === "")
            return "";
        // A streamed recording arrives as a signed https URL, and for WebDAV
        // that URL carries the password. Only the last path segment is ever
        // put on screen.
        const path = text.split("?")[0];
        return decodeURIComponent(path.substring(path.lastIndexOf("/") + 1));
    }
    // ── reference recording ─────────────────────────────────────────
    // The primary recording is the clock: it always plays at 1× and each
    // frame moves the telemetry cursor. The reference snaps to the mapped
    // station on pause, then uses the next straight to arrive with it at
    // turn-in. Corners stay at 1× so a turn is never time-warped.
    function videoReference(): MpvVideoItem {
        return videoReferenceLoader.player;
    }
    function videoSetFullscreen(on) {
        if (on === videoFullscreen)
            return;
        if (on) {
            videoVisible = true;
            videoFullscreenLayout = dualVideo ? 1 : 4;
            videoRestoreVisibility = root.visibility;
            videoFullscreen = true;
            root.visibility = Window.FullScreen;
        } else {
            videoFullscreen = false;
            root.videoSlowMotion = false;
            root.lockPrimaryRealtime();
            root.visibility = videoRestoreVisibility;
        }
    }
    function videoSourceAt(x: real, y: real): url {
        const host = videoComposeHost;
        if (!root.dualVideo || host.composeMode === 4)
            return videoPlayer.source;
        if (host.composeMode === 5)
            return Store.compareVideoSource;
        if (host.composeMode === 2) {
            const left = host.width - host.pipWidth - host.pipMargin;
            const top = host.height - host.pipHeight - host.pipMargin;
            if (x >= left && x <= left + host.pipWidth && y >= top && y <= top + host.pipHeight)
                return Store.compareVideoSource;
            return videoPlayer.source;
        }
        if (host.composeMode === 3) {
            const left = host.pipMargin;
            const top = host.height - host.pipHeight - host.pipMargin;
            if (x >= left && x <= left + host.pipWidth && y >= top && y <= top + host.pipHeight)
                return videoPlayer.source;
            return Store.compareVideoSource;
        }
        return x > videoStageMouse.width / 2 ? Store.compareVideoSource : videoPlayer.source;
    }
    function videoToggleMuted() {
        if (videoPlayer.loaded)
            Store.videoMuted = !Store.videoMuted;
    }
    function videoTogglePaused() {
        if (!videoPlayer.loaded)
            return;
        if (root.lapAdvanceCount > 0) {
            root.cancelLapAdvance();
            return;
        }
        root.lockPrimaryRealtime();
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

        onAddTelemetryDirectoryRequested: {
            settingsWindow.refresh();
            settingsWindow.show();
            settingsWindow.raise();
        }
        onChannelsRequested: {
            channelsWindow.refresh();
            channelsWindow.show();
            channelsWindow.raise();
        }
        onCornersRequested: {
            Store.focusCornerAtCursor();
        }
        onDriverRenameRequested: (key, name) => root.openDriverRename(key, name)
        onMetadataRequested: (path, folderScope) => {
            if (folderScope)
                videoMetadataDialog.openForFolder(path);
            else
                videoMetadataDialog.openForVideo(path);
        }
        onOpenFileRequested: fileDialog.open()
        onPreferencesRequested: {
            settingsWindow.refresh();
            settingsWindow.show();
            settingsWindow.raise();
        }
        onSidebarToggleRequested: root.sidebarVisible = !root.sidebarVisible
    }

    Component.onCompleted: {
        root.refreshDeltaTraceVisible();
        root.refreshLapStrip();
        root.syncTelemetryVideo();
        if (root.startupVideo.toString() !== "" && Store.primaryVideoSource.toString() === "")
            root.showVideo(root.startupVideo);
        if (root.autotestWindows) {
            channelsWindow.refresh();
            channelsWindow.show();
            settingsWindow.show();
        }
        if (root.videoVisible && root.width < 1000)
            root.sidebarVisible = false;
        if (Updater.associationPrompt)
            associationDialog.open();
    }

    Shortcut {
        enabled: root.videoFullscreen || Store.focusedCorner >= 0
        sequence: "Escape"

        onActivated: {
            if (root.videoFullscreen)
                root.videoSetFullscreen(false);
            else
                Store.clearCornerFocus();
        }
    }
    DropArea {
        id: fileDropArea

        anchors.fill: parent
        z: root.videoFullscreen ? 0 : 10000

        onDropped: drop => {
            if (!drop.hasUrls)
                return;
            root.openDroppedFiles(drop.urls);
            drop.acceptProposedAction();
        }

        Rectangle {
            anchors.fill: parent
            border.color: Style.accentColor
            border.width: 2
            color: Style.dropOverlayColor
            visible: fileDropArea.containsDrag

            Label {
                anchors.centerIn: parent
                color: Style.foregroundColor
                font.bold: true
                font.pixelSize: 16
                text: "Drop telemetry, video, or sidecar"
            }
        }
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

        Timer {
            id: videoControlsHideTimer

            interval: 1800

            onTriggered: root.videoControlsRequested = false
        }
        Timer {
            interval: 100
            repeat: true
            running: root.dualVideo && videoPlayer.loaded && !videoPlayer.paused

            onTriggered: root.syncReferenceVideo(false)
        }
        Timer {
            id: pausedVideoAlignmentTimer

            interval: 120

            onTriggered: root.verifyPausedVideoAlignment()
        }
        Timer {
            id: lapAdvanceTimer

            interval: 500
            repeat: true

            onTriggered: root.tickLapAdvance()
        }
        Rectangle {
            id: fullscreenChrome

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            color: Qt.rgba(0, 0, 0, 0.72)
            height: 28
            visible: root.videoFullscreen
            z: 10

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 8
                spacing: 10

                Label {
                    color: Style.foregroundColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.letterSpacing: 0.8
                    font.pixelSize: 10
                    text: root.videoFullscreenLayoutTitle
                }
                Label {
                    color: Style.yellowColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: "0.25×"
                    visible: root.videoSlowMotion
                }
                Label {
                    color: Style.accentColor
                    font.family: Style.uiFontFamily
                    font.pixelSize: 10
                    text: Store.primaryDriverName || "—"
                    visible: root.telemetryVideoActive
                }
                Label {
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: "LAP " + Store.primaryLapOrdinal + "/" + Store.primaryLapTotal
                    visible: root.telemetryVideoActive
                }
                Label {
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: "FUEL " + (Store.primaryFuelLoad || "—")
                    visible: root.telemetryVideoActive
                }
                Label {
                    color: Style.orangeColor
                    font.family: Style.uiFontFamily
                    font.pixelSize: 10
                    text: Store.compareDriverName || "—"
                    visible: root.telemetryVideoActive && root.dualVideo
                }
                Label {
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: "LAP " + Store.compareLapOrdinal + "/" + Store.compareLapTotal
                    visible: root.telemetryVideoActive && root.dualVideo
                }
                Label {
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 10
                    text: "FUEL " + (Store.compareFuelLoad || "—")
                    visible: root.telemetryVideoActive && root.dualVideo
                }
                Item {
                    Layout.fillWidth: true
                }
                Repeater {
                    model: root.dualVideo ? 5 : 0

                    ToolButton {
                        required property int index

                        Layout.preferredHeight: 22
                        Layout.preferredWidth: 22
                        checkable: true
                        checked: root.videoFullscreenLayout === index + 1
                        font.family: Style.monoFontFamily
                        font.pixelSize: 9
                        padding: 0
                        text: String(index + 1)

                        onClicked: root.setVideoFullscreenLayout(index + 1)
                    }
                }
            }
        }
        Item {
            id: videoComposeHost

            readonly property int composeMode: !root.dualVideo ? 4 : !root.videoFullscreen ? 1 : root.videoFullscreenLayout
            readonly property real gap: 2
            readonly property real mainHeight: Math.max(0, height - 2 * videoComposeHost.mainInset)
            readonly property real mainInset: videoComposeHost.composeMode === 2 || videoComposeHost.composeMode === 3 ? Math.round(Math.min(width, height) * 0.07) : 0
            readonly property real mainWidth: Math.max(0, width - 2 * videoComposeHost.mainInset)
            readonly property real mainX: videoComposeHost.composeMode === 3 ? Math.max(0, width - videoComposeHost.mainWidth) : 0
            readonly property real pipHeight: Math.round(videoComposeHost.pipWidth * 9 / 16)
            readonly property real pipMargin: 16
            readonly property real pipWidth: Math.round(width * 0.3)
            readonly property real splitWidth: Math.max(0, (width - videoComposeHost.gap) / 2)

            anchors.fill: parent
            z: 0

            Item {
                height: videoComposeHost.composeMode === 3 ? videoComposeHost.pipHeight : videoComposeHost.composeMode === 2 ? videoComposeHost.mainHeight : videoComposeHost.height
                visible: videoComposeHost.composeMode !== 5
                width: videoComposeHost.composeMode === 1 ? videoComposeHost.splitWidth : videoComposeHost.composeMode === 3 ? videoComposeHost.pipWidth : videoComposeHost.composeMode === 2 ? videoComposeHost.mainWidth : videoComposeHost.width
                x: videoComposeHost.composeMode === 3 ? videoComposeHost.pipMargin : videoComposeHost.composeMode === 2 ? videoComposeHost.mainX : 0
                y: videoComposeHost.composeMode === 3 ? videoComposeHost.height - videoComposeHost.pipHeight - videoComposeHost.pipMargin : videoComposeHost.composeMode === 2 ? videoComposeHost.mainInset : 0
                z: videoComposeHost.composeMode === 3 ? 2 : 0

                MpvVideoItem {
                    id: videoPlayer

                    anchors.fill: parent
                    muted: Store.videoMuted
                    objectName: "videoPlayer"

                    onLoadedChanged: {
                        if (loaded && root.telemetryVideoActive) {
                            Qt.callLater(() => {
                                root.lockPrimaryRealtime();
                                videoPlayer.paused = true;
                                root.seekVideoToTelemetry();
                            });
                            root.revealVideoControls();
                        }
                    }
                    onPositionChanged: {
                        if (root.telemetryVideoActive && loaded && !paused)
                            Store.setCursorFromVideoTime(position);
                        else if (root.dualVideo && loaded && paused && root.referenceSyncPauseAttempts > 0)
                            pausedVideoAlignmentTimer.restart();
                    }
                    onSeekingChanged: {
                        if (!seeking)
                            root.tryResumeLapAdvance();
                    }
                    onSourceExpired: root.reopenExpiredVideo(videoPlayer)
                }
                Rectangle {
                    anchors.fill: parent
                    border.color: Style.accentColor
                    border.width: 1
                    color: "transparent"
                    visible: videoComposeHost.composeMode === 2 || videoComposeHost.composeMode === 3
                    z: 3
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
                Label {
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.margins: 6
                    bottomPadding: 3
                    color: Style.yellowColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 9
                    horizontalAlignment: Text.AlignHCenter
                    leftPadding: 6
                    rightPadding: 6
                    text: Store.primaryVideoSyncWarning
                    topPadding: 3
                    visible: Store.primaryVideoSyncWarning !== ""
                    width: Math.min(parent.width - 12, 440)
                    wrapMode: Text.Wrap
                    z: 4

                    background: Rectangle {
                        color: Qt.rgba(0, 0, 0, 0.78)
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
                height: videoComposeHost.composeMode === 2 ? videoComposeHost.pipHeight : videoComposeHost.composeMode === 3 ? videoComposeHost.mainHeight : videoComposeHost.height
                visible: root.dualVideo && videoComposeHost.composeMode !== 4
                width: videoComposeHost.composeMode === 1 ? videoComposeHost.splitWidth : videoComposeHost.composeMode === 2 ? videoComposeHost.pipWidth : videoComposeHost.composeMode === 3 ? videoComposeHost.mainWidth : videoComposeHost.width
                x: videoComposeHost.composeMode === 1 ? videoComposeHost.splitWidth + videoComposeHost.gap : videoComposeHost.composeMode === 2 ? videoComposeHost.width - videoComposeHost.pipWidth - videoComposeHost.pipMargin : videoComposeHost.composeMode === 3 ? videoComposeHost.mainX : 0
                y: videoComposeHost.composeMode === 2 ? videoComposeHost.height - videoComposeHost.pipHeight - videoComposeHost.pipMargin : videoComposeHost.composeMode === 3 ? videoComposeHost.mainInset : 0
                z: videoComposeHost.composeMode === 2 ? 2 : 0

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
                                        if (videoPlayer.paused)
                                            root.realignPausedVideos();
                                        else
                                            root.syncReferenceVideo(true);
                                    });
                            }
                            onSourceExpired: root.reopenExpiredVideo(this)
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
                        if (videoPlayer.paused)
                            root.realignPausedVideos();
                        else {
                            pausedVideoAlignmentTimer.stop();
                            root.referenceSyncPauseAttempts = 0;
                            root.syncReferenceVideo(true);
                        }
                    }

                    target: videoPlayer
                }
                Rectangle {
                    anchors.fill: parent
                    border.color: Style.orangeColor
                    border.width: 1
                    color: "transparent"
                    visible: videoComposeHost.composeMode === 2 || videoComposeHost.composeMode === 3
                    z: 3
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
                    anchors.topMargin: 36
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
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.margins: 6
                    bottomPadding: 3
                    color: Style.yellowColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: 9
                    horizontalAlignment: Text.AlignHCenter
                    leftPadding: 6
                    rightPadding: 6
                    text: Store.compareVideoSyncWarning
                    topPadding: 3
                    visible: Store.compareVideoSyncWarning !== ""
                    width: Math.min(parent.width - 12, 440)
                    wrapMode: Text.Wrap
                    z: 4

                    background: Rectangle {
                        color: Qt.rgba(0, 0, 0, 0.78)
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
            id: videoStageMouse

            acceptedButtons: Qt.LeftButton | Qt.RightButton
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            enabled: videoPlayer.loaded
            hoverEnabled: true

            onClicked: mouse => {
                if (mouse.button === Qt.RightButton) {
                    const source = root.videoSourceAt(mouse.x, mouse.y);
                    const point = videoStageMouse.mapToItem(Overlay.overlay, mouse.x, mouse.y);
                    videoMetadataMenu.videoPath = Store.localPathForVideoSource(source);
                    videoMetadataMenu.x = point.x;
                    videoMetadataMenu.y = point.y;
                    videoMetadataMenu.open();
                    return;
                }
                videoPane.forceActiveFocus();
                root.videoTogglePaused();
                root.revealVideoControls();
            }
            onEntered: root.revealVideoControls()
            onPositionChanged: root.revealVideoControls()
        }
        Menu {
            id: videoMetadataMenu

            property string videoPath: ""

            parent: Overlay.overlay

            MenuItem {
                enabled: videoMetadataMenu.videoPath !== ""
                text: "Edit video metadata…"

                onTriggered: videoMetadataDialog.openForVideo(videoMetadataMenu.videoPath)
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
            opacity: videoPlayer.loaded ? (root.videoControlsRequested || hovered ? 1 : 0) : 1
            text: videoPlayer.muted ? "🔇" : "🔊"
            visible: opacity > 0.01
            width: 32
            z: 2

            Behavior on opacity {
                NumberAnimation {
                    duration: 110
                }
            }

            onClicked: {
                root.videoToggleMuted();
                root.revealVideoControls();
            }
        }
        ToolButton {
            ToolTip.text: "Close video"
            ToolTip.visible: hovered
            anchors.margins: 6
            anchors.right: parent.right
            anchors.top: parent.top
            height: 28
            objectName: "videoCloseButton"
            opacity: videoPlayer.loaded ? (root.videoControlsRequested || hovered ? 1 : 0) : 1
            text: "×"
            visible: opacity > 0.01
            width: 32
            z: 3

            Behavior on opacity {
                NumberAnimation {
                    duration: 110
                }
            }

            onClicked: root.hideVideo()
        }
        Rectangle {
            id: videoControls

            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            border.color: Style.borderColor
            border.width: 1
            color: Style.videoControlBackgroundColor
            height: root.standaloneVideoActive ? 58 : 38
            objectName: "videoControls"
            opacity: videoPlayer.loaded && (root.videoControlsRequested || videoControlsHover.hovered) ? 1 : 0
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
            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 0

                Slider {
                    id: videoSeekSlider

                    Layout.fillWidth: true
                    Layout.preferredHeight: 20
                    enabled: videoPlayer.duration > 0
                    from: 0
                    objectName: "videoSeekSlider"
                    stepSize: 0.1
                    to: Math.max(0.1, videoPlayer.duration)
                    value: videoPlayer.position
                    visible: root.standaloneVideoActive

                    background: Rectangle {
                        color: Style.videoControlTrackColor
                        height: 3
                        radius: 1.5
                        width: videoSeekSlider.availableWidth
                        x: videoSeekSlider.leftPadding
                        y: videoSeekSlider.topPadding + videoSeekSlider.availableHeight / 2 - height / 2

                        Rectangle {
                            color: Style.accentColor
                            height: parent.height
                            radius: parent.radius
                            width: videoSeekSlider.visualPosition * parent.width
                        }
                    }
                    handle: Rectangle {
                        color: videoSeekSlider.pressed ? Style.foregroundColor : Style.accentColor
                        height: 10
                        radius: 5
                        width: 10
                        x: videoSeekSlider.leftPadding + videoSeekSlider.visualPosition * (videoSeekSlider.availableWidth - width)
                        y: videoSeekSlider.topPadding + videoSeekSlider.availableHeight / 2 - height / 2
                    }

                    onMoved: {
                        videoPlayer.seek(value);
                        root.revealVideoControls();
                    }
                }
                RowLayout {
                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    spacing: 3

                    ToolButton {
                        Layout.preferredWidth: 42
                        ToolTip.text: "Back 2 seconds (Left)"
                        ToolTip.visible: hovered
                        font.capitalization: Font.MixedCase
                        font.pixelSize: Style.smallFontSize
                        objectName: "videoSeekBackButton"
                        text: "−2s"
                        visible: root.standaloneVideoActive

                        onClicked: root.seekVideoRelative(-2)
                    }
                    ToolButton {
                        Layout.preferredWidth: 54
                        ToolTip.text: root.dualVideo ? "Play/pause both recordings (Space)" : "Play/pause (Space)"
                        ToolTip.visible: hovered
                        font.capitalization: Font.MixedCase
                        font.pixelSize: Style.smallFontSize
                        objectName: "videoPlayPauseButton"
                        text: videoPlayer.paused ? "Play" : "Pause"

                        onClicked: {
                            root.videoTogglePaused();
                            root.revealVideoControls();
                        }
                    }
                    ToolButton {
                        Layout.preferredWidth: 42
                        ToolTip.text: "Forward 2 seconds (Right)"
                        ToolTip.visible: hovered
                        font.capitalization: Font.MixedCase
                        font.pixelSize: Style.smallFontSize
                        objectName: "videoSeekForwardButton"
                        text: "+2s"
                        visible: root.standaloneVideoActive

                        onClicked: root.seekVideoRelative(2)
                    }
                    Row {
                        Layout.preferredHeight: 28
                        spacing: 2
                        visible: root.videoFullscreen && root.dualVideo

                        Repeater {
                            model: ["Split", "Active+", "Ref+", "Active", "Ref"]

                            ToolButton {
                                required property int index
                                required property string modelData

                                ToolTip.text: modelData + " (" + (index + 1) + ")"
                                ToolTip.visible: hovered
                                checkable: true
                                checked: root.videoFullscreenLayout === index + 1
                                font.capitalization: Font.MixedCase
                                font.pixelSize: Style.smallFontSize
                                height: 28
                                text: String(index + 1)
                                width: 28

                                onClicked: root.setVideoFullscreenLayout(index + 1)
                            }
                        }
                    }
                    ToolButton {
                        Layout.preferredWidth: 42
                        ToolTip.text: root.videoOverlayVisible ? "Hide telemetry overlay (O)" : "Show telemetry overlay (O)"
                        ToolTip.visible: hovered
                        checkable: true
                        checked: root.videoOverlayVisible
                        font.capitalization: Font.MixedCase
                        font.pixelSize: Style.smallFontSize
                        text: "HUD"
                        visible: root.videoFullscreen && root.telemetryVideoActive

                        onClicked: root.videoOverlayVisible = !root.videoOverlayVisible
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 20
                        color: Style.mutedTextColor
                        elide: Text.ElideMiddle
                        font.family: Style.uiFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: root.videoFileName(videoPlayer.source)
                        visible: root.standaloneVideoActive
                    }
                    Label {
                        color: Style.foregroundColor
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: root.formatMediaTime(videoPlayer.position) + " / " + root.formatMediaTime(videoPlayer.duration)
                    }
                    Item {
                        Layout.fillWidth: !root.standaloneVideoActive
                    }
                    ToolButton {
                        Layout.preferredWidth: 42
                        ToolTip.text: root.videoFullscreen ? "Leave fullscreen (Esc)" : "Fullscreen (F)"
                        ToolTip.visible: hovered
                        font.capitalization: Font.MixedCase
                        font.pixelSize: Style.smallFontSize
                        objectName: "videoFullscreenButton"
                        text: root.videoFullscreen ? "Exit" : "Full"

                        onClicked: {
                            root.videoSetFullscreen(!root.videoFullscreen);
                            root.revealVideoControls();
                        }
                    }
                }
            }
        }
        VideoTelemetryOverlay {
            id: videoTelemetryOverlay

            mediaTime: videoPlayer.position
            visible: root.videoFullscreen && root.videoOverlayVisible && root.telemetryVideoActive && videoPlayer.loaded
        }
        VideoDeltaBar {
            visible: root.videoFullscreen && root.videoOverlayVisible && root.telemetryVideoActive && videoPlayer.loaded && Store.comparing
        }
        Rectangle {
            anchors.centerIn: parent
            color: Style.videoControlBackgroundColor
            height: lapAdvanceColumn.implicitHeight + 20
            opacity: root.lapAdvanceCount > 0 ? 1 : 0
            radius: 6
            visible: opacity > 0.01
            width: Math.max(160, lapAdvanceColumn.implicitWidth + 32)
            z: 12

            Behavior on opacity {
                NumberAnimation {
                    duration: 80
                }
            }

            Column {
                id: lapAdvanceColumn

                anchors.centerIn: parent
                spacing: 2

                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: Style.mutedTextColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    text: "NEXT LAP"
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: Style.accentColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.fontSize + 4
                    text: root.lapAdvanceNextLabel
                    visible: root.lapAdvanceNextLabel !== ""
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: Style.foregroundColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: 42
                    text: root.lapAdvanceCount > 0 ? String(root.lapAdvanceCount) : ""
                }
            }
        }
    }
    Connections {
        function onChannelConfigChanged(): void {
            root.refreshDeltaTraceVisible();
        }
        function onLapLoadingChanged(): void {
            root.tryResumeLapAdvance();
        }
        function onOperationError(title: string, message: string): void {
            operationErrorDialog.errorTitle = title;
            operationErrorDialog.errorMessage = message;
            operationErrorDialog.open();
        }
        function onPrimaryLapPlaybackEnded(): void {
            root.startLapAdvance();
        }
        function onSelectionChanged(): void {
            if (root.lapAdvanceCount > 0)
                root.cancelLapAdvance();
            root.refreshLapStrip();
            root.syncTelemetryVideo();
        }
        function onSessionsChanged(): void {
            root._lapStripDirty = true;
            root._sessionInfoCache = ({});
            root.refreshLapStrip();
            root.directoryRows = Store.sessionDirectories();
        }
        function onStandaloneVideoRequested(source: url): void {
            root.showVideo(source, false);
        }
        function onVideoTimeChanged(): void {
            const jumped = Math.abs(videoPlayer.position - Store.primaryVideoTime) > 0.2;
            root.seekVideoToTelemetry();
            root.syncReferenceVideo(videoPlayer.paused || jumped);
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
                text: "Scan a directory of .pds / .ld / .vbo / .mp4 / .telemetry files"
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
        nameFilters: ["Telemetry and video (*.pds *.PDS *.ld *.LD *.vbo *.VBO *.telemetry *.TELEMETRY *.mp4 *.MP4 *.mov *.MOV *.mkv *.MKV *.avi *.AVI *.m4v *.M4V *.webm *.WEBM *.jsonl *.jsonl.zstd *.jsonl.zst)", "Telemetry (*.pds *.PDS *.ld *.LD *.vbo *.VBO *.telemetry *.TELEMETRY *.mp4 *.MP4)", "Sidecar (*.ext.jsonl *.mtx.jsonl *.jsonl *.jsonl.zstd *.jsonl.zst)", "Video (*.mp4 *.MP4 *.mov *.MOV *.mkv *.MKV *.avi *.AVI *.m4v *.M4V *.webm *.WEBM)", "All files (*)"]
        title: "Open telemetry or video"

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
        id: operationErrorDialog

        property string errorMessage: ""
        property string errorTitle: "Unable to complete operation"

        closePolicy: Popup.CloseOnEscape
        focus: true
        modal: true
        objectName: "operationErrorDialog"
        parent: Overlay.overlay
        standardButtons: Dialog.Ok
        title: errorTitle
        width: Math.min(520, root.width - 32)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)

        contentItem: Label {
            color: Style.foregroundColor
            font.pixelSize: 11
            text: operationErrorDialog.errorMessage
            wrapMode: Text.Wrap
        }
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
    FileAssociationsDialog {
        id: associationDialog

        parent: Overlay.overlay
        width: Math.min(480, root.width - 32)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
    }
    TrackAssignmentDialog {
        id: trackAssignmentDialog

        parent: Overlay.overlay
        width: Math.min(440, root.width - 32)
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
    }
    VideoMetadataDialog {
        id: videoMetadataDialog

        height: Math.min(760, root.height - 32)
        parent: Overlay.overlay
        width: Math.min(920, root.width - 32)
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
    ChannelsWindow {
        id: channelsWindow
    }
    PreferencesWindow {
        id: settingsWindow
    }
    ToolTip {
        id: pointerTooltip

        parent: Overlay.overlay
        text: root.pointerTooltipText
        visible: root.pointerTooltipOwner !== ""
        x: root.pointerTooltipX
        y: root.pointerTooltipY
    }
    SpanHoverCard {
        id: spanHoverCard

        accent: trace.spanHoverColor
        metaRows: trace.spanHoverMeta
        subtitleText: trace.spanHoverSubtitle
        titleText: trace.spanHoverTitle
        visible: trace.spanHoverVisible
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
            color: Style.traceBackgroundColor
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

                        color: strip.reference ? Qt.tint(Style.surfaceColor, Qt.rgba(Style.orangeColor.r, Style.orangeColor.g, Style.orangeColor.b, 0.08)) : Style.surfaceColor
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

                                            readonly property bool confidenceLap: !sessionStrip.strip.reference && Store.traceConfidenceMode && Store.traceConfidenceIncludesLap(sessionStrip.strip.sessionKey, proportionalLap.modelData.lapId)
                                            readonly property bool fixedWidthLap: !proportionalLap.modelData.countsForBest
                                            readonly property real flexibleLaneWidth: Math.max(0, proportionalLapLane.width - Math.max(0, sessionStrip.strip.laps.length - 1) * proportionalLapRow.spacing - sessionStrip.strip.fixedLapCount * 30)
                                            required property var modelData
                                            property bool selectedLap: sessionStrip.strip.reference ? sessionStrip.strip.sessionKey === Store.compareSessionKey && proportionalLap.modelData.lapId === Store.compareLapIndex : sessionStrip.strip.sessionKey === Store.primarySessionKey && proportionalLap.modelData.lapId === Store.primaryLapIndex
                                            readonly property string tooltipOwner: "lap:" + sessionStrip.strip.sessionKey + ":" + proportionalLap.modelData.lapId + ":" + sessionStrip.strip.reference

                                            // Bound to the Row, not `parent`:
                                            // a delegate evaluates its
                                            // bindings before it is reparented,
                                            // so `parent` is null on creation.
                                            anchors.verticalCenter: proportionalLapRow.verticalCenter
                                            border.color: sessionStrip.strip.reference ? Style.orangeColor : Style.accentColor
                                            border.width: proportionalLap.selectedLap || proportionalLap.confidenceLap ? 1 : 0
                                            color: proportionalLap.selectedLap ? Style.selectionColor : proportionalLap.confidenceLap ? Qt.tint(Style.traceBackgroundColor, Qt.rgba(Style.accentColor.r, Style.accentColor.g, Style.accentColor.b, 0.2)) : proportionalLapMouse.containsMouse ? Style.backgroundColor : Style.traceBackgroundColor
                                            height: proportionalLapRow.height - 8
                                            objectName: (sessionStrip.strip.reference ? "referenceFilmstripLap-" : "activeFilmstripLap-") + proportionalLap.modelData.lapId
                                            radius: 3
                                            width: proportionalLap.fixedWidthLap ? 30 : Math.max(1, proportionalLap.flexibleLaneWidth * Math.max(1, proportionalLap.modelData.timeMs) / sessionStrip.strip.flexibleTimeMs)

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
                                                font.bold: proportionalLap.selectedLap || proportionalLap.confidenceLap
                                                font.family: Style.monoFontFamily
                                                font.pixelSize: 9
                                                text: proportionalLap.fixedWidthLap ? proportionalLap.modelData.label : proportionalLap.modelData.timeText
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
                                                    root.showPointerTooltip(proportionalLap.tooltipOwner, proportionalLap.modelData.timeText + " · " + proportionalLap.modelData.label + (proportionalLap.confidenceLap ? " · Consistency cohort" : ""), point.x, point.y);
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

            FileBrowserPane {
                id: sidebarPane

                SplitView.fillHeight: true
                SplitView.maximumWidth: root.sidebarVisible ? 420 : 0
                SplitView.minimumWidth: root.sidebarVisible ? 185 : 0
                SplitView.preferredWidth: root.sidebarVisible ? Math.min(280, Math.max(210, root.width * 0.32)) : 0
                visible: root.sidebarVisible

                onDriverRenameRequested: (mappingKey, driver) => root.openDriverRename(mappingKey, driver)
                onFileActivated: (path, key, hasSession) => {
                    if (hasSession)
                        root.setSessionActive(key);
                    else
                        Store.openFile(path);
                }
                onFileIsolated: key => root.useSessionAlone(key)
                onFolderMetadataRequested: path => videoMetadataDialog.openForFolder(path)
                onPointerTooltipDismissed: owner => root.dismissPointerTooltip(owner)
                onPointerTooltipMoved: (owner, x, y) => root.movePointerTooltip(owner, x, y)
                onPointerTooltipRequested: (owner, text, x, y) => root.showPointerTooltip(owner, text, x, y)
                onSetActiveRequested: key => root.setSessionActive(key)
                onSetReferenceRequested: key => root.setSessionReference(key)
                onTrackAssignmentRequested: key => trackAssignmentDialog.openForSession(key)
                onVideoMetadataRequested: path => videoMetadataDialog.openForVideo(path)
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

                    SplitView.fillHeight: root.standaloneVideoActive
                    SplitView.fillWidth: true
                    SplitView.maximumHeight: visible ? (root.standaloneVideoActive ? 16777215 : root.height * 0.72) : 0
                    SplitView.minimumHeight: visible ? 180 : 0
                    SplitView.preferredHeight: visible ? (root.standaloneVideoActive ? root.height : Math.min(480, Math.max(220, root.height * 0.42))) : 0
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

                        onActivated: root.seekVideoRelative(-2)
                    }
                    Shortcut {
                        enabled: videoPane.visible && videoPlayer.loaded
                        sequence: "Right"

                        onActivated: root.seekVideoRelative(2)
                    }
                    Shortcut {
                        enabled: root.videoFullscreen && root.telemetryVideoActive
                        sequence: "O"

                        onActivated: root.videoOverlayVisible = !root.videoOverlayVisible
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
                        enabled: root.videoFullscreen && root.dualVideo
                        sequence: "1"

                        onActivated: root.setVideoFullscreenLayout(1)
                    }
                    Shortcut {
                        enabled: root.videoFullscreen && root.dualVideo
                        sequence: "2"

                        onActivated: root.setVideoFullscreenLayout(2)
                    }
                    Shortcut {
                        enabled: root.videoFullscreen && root.dualVideo
                        sequence: "3"

                        onActivated: root.setVideoFullscreenLayout(3)
                    }
                    Shortcut {
                        enabled: root.videoFullscreen
                        sequence: "4"

                        onActivated: root.setVideoFullscreenLayout(4)
                    }
                    Shortcut {
                        enabled: root.videoFullscreen && root.dualVideo
                        sequence: "5"

                        onActivated: root.setVideoFullscreenLayout(5)
                    }
                    Shortcut {
                        enabled: root.videoFullscreen && videoPlayer.loaded
                        sequence: "S"

                        onActivated: root.toggleVideoSlowMotion()
                    }
                    // Docked home of videoStage; the stage itself is at
                    // window scope so it can move fullscreen without
                    // rebuilding either libmpv render context.
                    Item {
                        id: videoStageSlot

                        anchors.fill: parent
                        objectName: "videoStageSlot"
                    }
                }
                Rectangle {
                    id: tracePane

                    SplitView.fillHeight: true
                    SplitView.fillWidth: true
                    SplitView.maximumHeight: visible ? 16777215 : 0
                    SplitView.minimumHeight: visible ? 120 : 0
                    clip: true
                    color: Style.traceBackgroundColor
                    objectName: "tracePane"
                    visible: !root.standaloneVideoActive

                    TraceToolbar {
                        id: traceToolbar

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        deltaTraceVisible: root.deltaTraceVisible
                        trace: trace
                        z: 2
                    }
                    TraceView {
                        id: trace

                        property bool confidenceBeforeHold: false
                        property bool confidenceHeld: false

                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: traceToolbar.bottom
                        backgroundColor: Style.traceBackgroundColor
                        focus: true
                        objectName: "traceView"
                        store: Store

                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_Period) {
                                if (!event.isAutoRepeat && !trace.confidenceHeld) {
                                    trace.confidenceBeforeHold = Store.traceConfidenceMode;
                                    trace.confidenceHeld = true;
                                    Store.traceConfidenceMode = true;
                                }
                                event.accepted = true;
                            } else if (event.key === Qt.Key_C) {
                                Store.clearCompare();
                                event.accepted = true;
                            } else if (event.key === Qt.Key_A) {
                                Store.setEditingCorners(!Store.editingCorners);
                                event.accepted = true;
                            }
                        }
                        Keys.onReleased: event => {
                            if (event.key === Qt.Key_Period) {
                                if (!event.isAutoRepeat && trace.confidenceHeld) {
                                    Store.traceConfidenceMode = trace.confidenceBeforeHold;
                                    trace.confidenceHeld = false;
                                }
                                event.accepted = true;
                            }
                        }
                        onActiveFocusChanged: {
                            if (!trace.activeFocus && trace.confidenceHeld) {
                                Store.traceConfidenceMode = trace.confidenceBeforeHold;
                                trace.confidenceHeld = false;
                            }
                        }
                        onChannelMenuRequested: (key, title, weight, x, y) => {
                            channelMenu.channelKey = key;
                            channelMenu.channelTitle = title;
                            channelMenu.channelWeight = weight;
                            channelMenu.popup(trace, x, y);
                        }
                        onChannelsRequested: {
                            channelsWindow.refresh();
                            channelsWindow.show();
                            channelsWindow.raise();
                        }
                        onCornerMenuRequested: (cornerIndex, cornerName, fraction, x, y) => {
                            cornerMenu.cornerIndex = cornerIndex;
                            cornerMenu.cornerName = cornerName;
                            cornerMenu.fraction = fraction;
                            cornerMenu.popup(trace, x, y);
                        }
                        onCornerRenameRequested: index => root.openCornerRename(index)
                        onSpanHoverChanged: {
                            if (trace.spanHoverVisible)
                                spanHoverCard.follow(trace, trace.spanHoverX, trace.spanHoverY);
                        }

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
                            MenuSeparator {
                            }
                            MenuItem {
                                text: "Auto-generate zones"

                                onTriggered: {
                                    Store.autoGenerateCorners();
                                    Store.saveCorners();
                                }
                            }
                        }
                        Menu {
                            id: channelMenu

                            property string channelKey: ""
                            property string channelTitle: ""
                            property real channelWeight: 1

                            objectName: "channelMenu"

                            MenuItem {
                                text: "Double size"

                                onTriggered: Store.setChannelWeight(channelMenu.channelKey, Math.min(4, channelMenu.channelWeight * 2))
                            }
                            MenuItem {
                                text: "Half size"

                                onTriggered: Store.setChannelWeight(channelMenu.channelKey, Math.max(0.25, channelMenu.channelWeight / 2))
                            }
                            MenuItem {
                                text: "Normal size"

                                onTriggered: Store.setChannelWeight(channelMenu.channelKey, 1)
                            }
                            MenuSeparator {
                            }
                            MenuItem {
                                text: channelMenu.channelKey.indexOf("overlay:") === 0 ? "Remove " + channelMenu.channelTitle : "Hide " + channelMenu.channelTitle

                                onTriggered: trace.hideChannel(channelMenu.channelKey)
                            }
                            MenuItem {
                                text: "Show all standard channels"

                                onTriggered: trace.showAllStandardChannels()
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
                    TraceLaneChrome {
                        anchors.fill: trace
                        trace: trace
                        z: 0.5
                    }
                    TraceCursorOverlay {
                        anchors.fill: trace
                        objectName: "traceOverlay"
                        trace: trace
                        z: 1
                    }
                    TraceCornerRuler {
                        anchors.left: trace.left
                        anchors.leftMargin: trace.labelWidth
                        anchors.right: trace.right
                        anchors.top: trace.top
                        height: trace.rulerHeight
                        objectName: "traceCornerRuler"
                        rulerHeight: trace.rulerHeight
                        z: 2
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
                    CornerFocusOverlay {
                        anchors.right: tracePane.right
                        anchors.rightMargin: 8
                        anchors.top: trace.top
                        anchors.topMargin: 8
                        height: Math.min(implicitHeight, tracePane.height - 16)
                        width: Math.min(260, Math.max(220, tracePane.width * 0.28))
                        z: 4
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
