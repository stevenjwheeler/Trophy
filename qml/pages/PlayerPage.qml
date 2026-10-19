/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../controls"
import "../styles"
import "../views"
import QtMultimedia
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: playerPage

    property string filePath: ""
    property string fileTitle: ""
    property string fileDate: ""

    // Library entry for the open file (null when unknown): drives the
    // file-info dialog. Reactive on filePath; the entry's own NOTIFYs
    // refresh rows as the background probe lands.
    readonly property var infoRec: (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.getRecording(filePath) : null
    readonly property var bookmarks: infoRec && infoRec.bookmarks ? infoRec.bookmarks : []
    readonly property var audioRouting: infoRec && infoRec.audioRouting ? infoRec.audioRouting : ({})
    readonly property int hiddenVoiceTrack: audioRouting.v === 1 && audioRouting.separate
        && audioRouting.system && !audioRouting.voice
        && audioRouting.primary >= 0 && audioRouting.primary < playerPage.audioLanes ? audioRouting.voiceTrack : -1
    readonly property real trackHeaderWidth: audioRouting.v === 1 && audioRouting.system ? 88 : 64
    readonly property var displayedAudioTracks: {
        let tracks = [];
        for (let i = 0; i < playerPage.audioLanes; ++i)
            if (i !== playerPage.hiddenVoiceTrack) tracks.push(i);
        return tracks;
    }
    function audioTrackName(ordinal) {
        const routing = playerPage.audioRouting;
        if (routing.v !== 1) return "";
        if (ordinal === routing.primary)
            return routing.system ? (routing.game ? qsTr("Game / System") : qsTr("System")) : qsTr("Game");
        if (ordinal === routing.voiceTrack) return qsTr("Voice");
        if (ordinal === routing.micTrack) return qsTr("Mic");
        return "";
    }

    function bookmarkClipRange(ms, durationMs) {
        if (!isFinite(ms) || !isFinite(durationMs) || durationMs < 500) return [];
        const center = Math.max(0, Math.min(ms, durationMs));
        return [Math.max(0, center - 15000), Math.min(durationMs, center + 15000)];
    }

    function selectBookmarkClip(ms) {
        const range = playerPage.bookmarkClipRange(ms, playerPage.playDuration);
        if (range.length !== 2) return;
        playerPage.clipMode = true;
        playerPage.clipStartMs = range[0];
        playerPage.clipEndMs = range[1];
        playerPage.seekAll(Math.min(ms, playerPage.playDuration));
        playerPage.centerView(ms);
        bookmarksDialog.close();
    }

    function formatInfoSize(bytes) {
        if (!bytes)
            return "0 B"
        var kb = 1024
        var mb = kb * 1024
        var gb = mb * 1024
        if (bytes >= gb)
            return (bytes / gb).toFixed(1) + " GB"
        if (bytes >= mb)
            return (bytes / mb).toFixed(0) + " MB"
        if (bytes >= kb)
            return (bytes / kb).toFixed(0) + " KB"
        return bytes + " B"
    }

    function formatInfoFps(fps) {
        if (!(fps > 0))
            return "–"
        var rounded = Math.round(fps)
        return Math.abs(fps - rounded) < 0.005 ? rounded.toString() : fps.toFixed(2)
    }

    function codecDisplayName(codec) {
        if (!codec)
            return "–"
        var names = {
            "h264": "H.264", "hevc": "H.265", "av1": "AV1",
            "mpeg4": "MPEG-4", "vp9": "VP9",
            "aac": "AAC", "opus": "Opus", "mp3": "MP3",
            "flac": "FLAC", "pcm_s16le": "PCM", "vorbis": "Vorbis"
        }
        return names[codec.toLowerCase()] || codec
    }

    // Audio stream count (-1 = unavailable or pending, 0 = no audio).
    // Delivered asynchronously; lanes rebuild only for the current open.
    property int trackCount: 0
    property int audioProbeId: 0
    property bool audioProbePending: false
    // audioTrackCount returns -1 when the file can't be opened (unknown,
    // not empty). Lane math/models clamp to >= 0; the label below
    // distinguishes "unavailable" from a definitive "no audio".
    readonly property int audioLanes: Math.max(0, trackCount)

    Connections {
        target: typeof waveformCache !== "undefined" ? waveformCache : null
        function onAudioTrackCountReady(path, requestId, count) {
            if (requestId !== playerPage.audioProbeId || path !== playerPage.filePath)
                return
            playerPage.audioProbePending = false
            playerPage.trackCount = count
            // Expanding from the video-only pending state must fit every
            // newly discovered lane, without undoing a user's larger card.
            playerPage.tracksHeight = Math.max(playerPage.tracksHeight, playerPage.minTracksHeight)
        }
    }

    // True per-track mute: one audio-only pipeline per stream
    // (the video output stays silent), all driven in lockstep.
    // A muted lane is genuinely silent, not soloed away.
    property var mutedTracks: []
    property real masterVolume: 1.0
    property bool masterMuted: false

    // Zoom: factor 1 fits the whole file. The view window is
    // shared by every lane (waveforms, strip and playheads all
    // draw it); it recenters when the playhead leaves, so no
    // manual panning is needed for review.
    property real zoomFactor: 1.0
    property double viewStartMs: 0
    readonly property double viewSpanMs: (player.duration > 0 && zoomFactor >= 1) ? player.duration / zoomFactor : 0
    // Lane-facing media clock. Delegates must read these
    // (qualified) instead of `player.*` directly:
    // unqualified file ids do not resolve inside
    // `component` blocks, so a delegate-side
    // `playDuration: player.duration` binding silently
    // dies and the lanes paint blank on reopen (duration
    // stays 0 while the page-level span is valid).
    readonly property double playPosition: player.position
    readonly property bool playRunning: player.playing
    readonly property double playDuration: player.duration

    // Clip editor: the scissors toggle in the tracks header opens a
    // clip bar; two handles on every lane select the range and Snip
    // cuts it with ReplayClipper::saveRange (same stream-copy engine,
    // tags and naming as DVR clips). -1 = unset (no overlay).
    property bool clipMode: false
    property double clipStartMs: -1
    property double clipEndMs: -1
    property string clipError: ""
    // Loop the selection while reviewing it (audition before
    // snipping). On by default: the common flow is select-then-snip,
    // and a static range invites a play-press anyway.
    property bool loopClip: true
    // Source file of the in-flight snip: the player switches to the
    // new clip only while still showing it (if the player moved on or
    // closed meanwhile, the notice is enough).
    property string clipSourcePath: ""
    // Playback rate, fanned out to the video output and every
    // audio-only pipeline together (lockstep).
    property real playbackRate: 1.0
    // Scrub-throttle state (see seekTo): last issued seek and the
    // trailing position a drag burst owes.
    property double lastScrubMs: 0
    property double pendingScrubMs: -1
    // True while the library remuxes a snipped range (worker thread).
    readonly property bool clipSaving: typeof recordingLibrary !== "undefined" && recordingLibrary !== null ? recordingLibrary.clipSaving : false

    // Clip selection helpers (see the clip bar + lane handles below).
    function clipRangeValid() {
        return clipMode && clipStartMs >= 0 && clipEndMs > clipStartMs + 400
    }

    function initClipRange() {
        if (player.duration > 0) {
            clipStartMs = 0
            clipEndMs = player.duration
        }
    }

    function toggleClipMode() {
        clipMode = !clipMode
        clipError = ""
        if (clipMode)
            initClipRange()
    }

    function clearClipRange() {
        clipStartMs = -1
        clipEndMs = -1
    }

    // Dragged handle (file clock ms), clamped to the file and to a
    // 500ms minimum span so the cut always has something to keep.
    function setClipHandle(which, ms) {
        if (player.duration <= 0)
            return
        var t = Math.max(0, Math.min(player.duration, ms))
        var gap = 500
        if (which === 0)
            clipStartMs = (clipEndMs >= 0) ? Math.min(t, clipEndMs - gap) : t
        else
            clipEndMs = (clipStartMs >= 0) ? Math.max(t, clipStartMs + gap) : t
    }

    function setRate(r) {
        playbackRate = r
    }

    function snipClip() {
        if (!clipRangeValid())
            return
        clipError = ""
        clipSourcePath = filePath
        if (typeof recordingLibrary !== "undefined" && recordingLibrary !== null)
            recordingLibrary.saveClipRange(filePath, clipStartMs / 1000, clipEndMs / 1000)
    }

    function snapshotCurrent() {
        if (filePath === "")
            return
        var slash = filePath.lastIndexOf("/")
        var dir = slash >= 0 ? filePath.substring(0, slash) : "."
        var base = slash >= 0 ? filePath.substring(slash + 1) : filePath
        var stem = base.replace(/\.[^.]*$/, "")
        var ts = Qt.formatDateTime(new Date(), "yyyy-MM-dd_HH-mm-ss")
        player.snapshot(dir + "/" + stem + "_snapshot_" + ts + ".png")
    }

    // Tracks card height: 0 fits the content (every lane at
    // full height); a positive value is a compressed height
    // dragged on the handle at the card's top edge — the
    // video takes the freed room. Double-click the handle
    // to fit again.
    property real tracksHeight: 0
    // Smallest usable card: the zoom row, the video lane
    // and the card's margins.
    // Compressed lane floor: the mute button's
    // height — the header never drops below the
    // button itself.
    readonly property real minLaneHeight: 28

    // Smallest usable card: every lane at the
    // compressed floor (gaps included), so the
    // drag can never compress past the point
    // where lanes start disappearing into the
    // bottom of the card.
    readonly property real minTracksHeight: Theme.spacingMedium * 2
        + zoomRow.implicitHeight
        + (playerPage.clipError !== "" ? clipErrorLabel.implicitHeight + Theme.spacingSmall : 0)
        + (1 + displayedAudioTracks.length) * (minLaneHeight + Theme.spacingSmall)
        + (trackCount <= 0 ? noAudioLabel.implicitHeight + Theme.spacingSmall : 0)

    // Lane height while the card fits its content;
    // compressing the card shrinks every lane down
    // to minLaneHeight so all tracks stay visible
    // and denser instead of overflowing the card.
    readonly property real fullLaneHeight: 64

    readonly property real laneHeight: {
        if (tracksHeight <= 0)
            return fullLaneHeight
        var interior = tracksHeight - Theme.spacingMedium * 2
        return Math.max(minLaneHeight, Math.min(fullLaneHeight,
            (interior - zoomRow.implicitHeight) / (1 + displayedAudioTracks.length) - Theme.spacingSmall))
    }

    // Uncompressed content height, derived from the
    // layout's structure (the zoom row, every lane at
    // fullLaneHeight and the inter-lane gaps). The live
    // implicit height can't serve as the drag's upper
    // bound: it shrinks along with the lanes once the
    // card is compressed (and a Binding-captured copy
    // reverts when its `when` releases).
    readonly property real tracksNaturalHeight: Theme.spacingMedium * 2
        + zoomRow.implicitHeight
        + (playerPage.clipError !== "" ? clipErrorLabel.implicitHeight + Theme.spacingSmall : 0)
        + (1 + displayedAudioTracks.length) * (fullLaneHeight + Theme.spacingSmall)
        + (trackCount <= 0 ? noAudioLabel.implicitHeight + Theme.spacingSmall : 0)

    function setTracksHeight(height) {
        var floor = Math.min(minTracksHeight, tracksNaturalHeight)
        tracksHeight = Math.max(floor, Math.min(tracksNaturalHeight, height))
    }

    function isMuted(index) {
        return mutedTracks.indexOf(index) !== -1
    }

    function toggleMute(index) {
        var next = mutedTracks.slice()
        var at = next.indexOf(index)
        if (at === -1)
            next.push(index)
        else
            next.splice(at, 1)
        mutedTracks = next
    }

    // Open a library file (closes any current one first).
    function open(path, title, dateStr) {
        player.stop()
        mutedTracks = []
        clipMode = false
        clearClipRange()
        clipError = ""
        clipSourcePath = ""
        loopClip = true
        playbackRate = 1.0
        resetView()
        filePath = path || ""
        fileTitle = title || ""
        fileDate = dateStr || ""
        audioProbeId++
        trackCount = -1
        audioProbePending = filePath !== "" && typeof waveformCache !== "undefined" && waveformCache !== null
        if (audioProbePending)
            waveformCache.requestAudioTrackCount(filePath, audioProbeId)
        // Start with the tracks at their smallest: the
        // video gets the room, and the card expands on
        // drag (or double-click for full height).
        tracksHeight = minTracksHeight
        player.source = filePath !== "" ? recordingLibrary.localFileUrl(filePath) : ""
        if (filePath !== "")
            playAll()
    }

    function close() {
        bookmarksDialog.close()
        stopAll()
        fileInfoDialog.close()
        player.source = ""
        filePath = ""
        fileTitle = ""
        fileDate = ""
        trackCount = 0
        audioProbeId++
        audioProbePending = false
        mutedTracks = []
        clipMode = false
        clearClipRange()
        clipError = ""
        clipSourcePath = ""
        loopClip = true
        playbackRate = 1.0
        resetView()
        tracksHeight = 0
        playerFullscreen = false
        visible = false
    }

    function seekTo(ms) {
        // Scrub throttle: lane drags fire per-mousemove, and every
        // FFmpeg seek flushes the decoders and re-decodes up to a GOP
        // — an unthrottled drag aborts each seek with the next (a 2s
        // burst delivered ~60 frames headless, pure thrash). At most
        // one seek per 100ms while scrubbing, plus the exact trailing
        // position right after, so the landing frame stays precise.
        // Discrete jumps (±10s, loop wrap) bypass this via seekAll.
        var now = Date.now()
        if (now - lastScrubMs >= 100) {
            lastScrubMs = now
            pendingScrubMs = -1
            scrubTimer.stop()
            seekAll(ms)
        } else {
            pendingScrubMs = ms
            scrubTimer.restart()
        }
    }

    Timer {
        id: scrubTimer
        interval: 100
        repeat: false
        onTriggered: {
            if (playerPage.pendingScrubMs >= 0) {
                playerPage.lastScrubMs = Date.now()
                playerPage.seekAll(playerPage.pendingScrubMs)
                playerPage.pendingScrubMs = -1
            }
        }
    }

    // Fullscreen player: the window goes fullscreen and the
    // header/tracks hide so the video fills the screen, like
    // any standalone player. The page owns the flag; main.qml
    // answers it with showFullScreen()/showNormal() (a child
    // file can't reach the window id). Escape or the fullscreen
    // button exits.
    property bool playerFullscreen: false

    // Emitted by the video view's fullscreen button; the
    // page toggles the flag above and main.qml's handler
    // drives the window.
    signal fullscreenRequested()

    function resetView() {
        zoomFactor = 1.0
        viewStartMs = 0
    }

    function centerView(ms) {
        var span = viewSpanMs
        if (span <= 0 || player.duration <= 0) {
            viewStartMs = 0
            return
        }
        viewStartMs = Math.max(0, Math.min(player.duration - span, ms - span / 2))
    }

    function setZoom(f) {
        zoomFactor = Math.max(1, Math.min(16, f))
        centerView(player.position)
    }

    // Wheel zoom anchored at the cursor (not the playhead).
    function zoomAt(cursorMs, frac, zoomIn) {
        var f = Math.max(1, Math.min(16, zoomFactor * (zoomIn ? 2 : 0.5)))
        if (f === zoomFactor)
            return
        zoomFactor = f
        var span = player.duration > 0 ? player.duration / zoomFactor : 0
        if (span <= 0)
            return
        viewStartMs = Math.max(0, Math.min(player.duration - span, cursorMs - frac * span))
    }

    // Middle-drag pan: moves the window, never the playhead.
    function panViewTo(startMs) {
        var span = viewSpanMs
        if (span <= 0 || player.duration <= 0)
            return
        viewStartMs = Math.max(0, Math.min(player.duration - span, startMs))
    }

    // Called on every position tick; only recenters when the
    // playhead actually leaves the window, so lanes don't
    // repaint continuously (the overlay playhead moves freely
    // inside a settled window).
    function followPlayhead() {
        if (zoomFactor <= 1 || viewSpanMs <= 0)
            return
        var pos = player.position
        if (pos < viewStartMs || pos > viewStartMs + viewSpanMs)
            centerView(pos)
    }

    // Loop the clip selection while reviewing it (audition before
    // snipping). Called on every position tick alongside
    // followPlayhead. Wraps only while playing through the end —
    // a manual seek must never yank the head back, or the range
    // could never be extended with Set end while looping. The 1s
    // window is playback overshoot (one tick); a seek landing
    // further out escapes the loop and plays on.
    function loopCheck() {
        if (!loopClip || !clipRangeValid() || !player.playing)
            return
        var pos = player.position
        if (pos >= clipEndMs && pos < clipEndMs + 1000)
            seekAll(clipStartMs)
    }

    // Lockstep transport across the video output and every
    // audio pipeline (same event-loop turn, same file clocks).
    function playAll() {
        player.play()
        for (var i = 0; i < audioPipelines.count; i++) {
            var p = audioPipelines.objectAt(i)
            if (p)
                p.play()
        }
    }

    function pauseAll() {
        player.pause()
        for (var i = 0; i < audioPipelines.count; i++) {
            var p = audioPipelines.objectAt(i)
            if (p)
                p.pause()
        }
    }

    function stopAll() {
        player.stop()
        for (var i = 0; i < audioPipelines.count; i++) {
            var p = audioPipelines.objectAt(i)
            if (p)
                p.stop()
        }
    }

    function seekAll(ms) {
        player.seek(ms)
        for (var i = 0; i < audioPipelines.count; i++) {
            var p = audioPipelines.objectAt(i)
            if (p && p.duration > 0)
                p.position = Math.max(0, Math.min(p.duration, ms))
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: playerPage.visible
        onActivated: {
            if (playerPage.playerFullscreen)
                playerPage.playerFullscreen = false
            else
                playerPage.close()
        }
    }

    Dialog {
        id: bookmarksDialog
        objectName: "playerBookmarksDialog"
        title: qsTr("Bookmarked moments")
        width: Math.min(560, Math.max(280, playerPage.width - 40))
        anchors.centerIn: parent
        modal: true
        padding: Theme.spacingMedium
        background: Rectangle { color: Theme.cardColor; radius: Theme.radiusLarge; border.color: Theme.borderColor }
        Overlay.modal: Rectangle { color: Theme.withOpacity("#000000", 0.5) }
        header: Label { text: bookmarksDialog.title; padding: Theme.spacingMedium; color: Theme.textColor; font.pixelSize: Theme.fontSizeLarge; font.bold: true }
        footer: Item {
            implicitHeight: 48
            TrophyButton { anchors.right: parent.right; anchors.rightMargin: Theme.spacingMedium; text: qsTr("Close"); onClicked: bookmarksDialog.close() }
        }
        contentItem: ScrollView {
            implicitHeight: Math.min(320, bookmarkRows.implicitHeight)
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: bookmarkRows
                width: parent.width
                Repeater {
                    model: playerPage.bookmarks
                    delegate: RowLayout {
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: player.formatTime(modelData.positionMs)
                            color: Theme.textColor
                            font.family: "monospace"
                        }
                        TrophyButton {
                            text: qsTr("Jump")
                            onClicked: { playerPage.seekAll(modelData.positionMs); bookmarksDialog.close() }
                        }
                        TrophyButton {
                            text: qsTr("Clip ±15 s")
                            enabled: playerPage.playDuration >= 500
                            onClicked: playerPage.selectBookmarkClip(modelData.positionMs)
                        }
                        TrophyButton {
                            icon.source: "qrc:/icons/trash.svg"
                            Accessible.name: qsTr("Remove bookmark")
                            onClicked: {
                                if (!recordingLibrary.removeBookmark(playerPage.filePath, index))
                                    window.showMetadataError();
                            }
                        }
                    }
                }
            }
        }
    }

    background: Rectangle {
        color: Theme.backgroundColor
        // Same page chrome as the other pages: the top
        // tucks under the title bar and the bottom-right
        // rounds into the window corner.
        bottomRightRadius: Theme.windowRadius
        topLeftRadius: 26
    }

    ColumnLayout {
        anchors.fill: parent
        // Edge-to-edge in fullscreen (a proper player has
        // no letterboxing).
        anchors.margins: playerPage.playerFullscreen ? 0 : Theme.spacingLarge
        spacing: Theme.spacingMedium

        // Header: back, title, time
        RowLayout {
            Layout.fillWidth: true
            // Hidden in fullscreen: just the video and its
            // hover controls, like any standalone player.
            visible: !playerPage.playerFullscreen
            spacing: Theme.spacingMedium

            TrophyButton {
                text: qsTr("‹ Back")
                onClicked: playerPage.close()
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    text: playerPage.fileTitle
                    color: Theme.textColor
                    font.pixelSize: Theme.fontSizeLarge
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }

                Label {
                    Layout.fillWidth: true
                    visible: playerPage.fileDate !== ""
                    text: playerPage.fileDate
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }

            Label {
                text: player.formatTime(player.position) + " / " + player.formatTime(player.duration)
                color: Theme.textMutedColor
                font.pixelSize: Theme.fontSizeSmall
                font.family: "monospace"
            }

            TrophyButton {
                implicitHeight: 32
                implicitWidth: 32
                objectName: "playerBookmarksButton"
                padding: 0
                horizontalPadding: 0
                icon.width: 18
                icon.height: 18
                icon.source: "qrc:/icons/bookmark.svg"
                enabled: playerPage.bookmarks.length > 0
                opacity: enabled ? 1 : 0.45
                Accessible.name: qsTr("Bookmarks")
                ToolTip.text: qsTr("Bookmarks (%1)").arg(playerPage.bookmarks.length)
                ToolTip.visible: hovered
                onClicked: bookmarksDialog.open()
            }

            TrophyButton {
                objectName: "playerProtectionButton"
                implicitHeight: 32
                implicitWidth: 32
                padding: 0
                horizontalPadding: 0
                icon.width: 18
                icon.height: 18
                visible: !!playerPage.infoRec
                icon.source: "qrc:/icons/lock.svg"
                outlined: true
                checked: !!playerPage.infoRec && !!playerPage.infoRec.protectedFromCleanup
                ToolTip.text: qsTr("Protect from auto-cleanup (manual deletion still allowed)")
                ToolTip.visible: hovered
                Accessible.name: qsTr("Protect from auto-cleanup")
                onClicked: window.toggleCaptureProtection(playerPage.filePath)
            }

            TrophyButton {
                objectName: "playerShareExportButton"
                implicitHeight: 32
                padding: 0
                horizontalPadding: 12
                icon.width: 18
                icon.height: 18
                text: qsTr("Share MP4")
                icon.source: "qrc:/icons/share.svg"
                enabled: !!playerPage.infoRec
                onClicked: window.openShareExport(playerPage.filePath, playerPage.mutedTracks.concat(playerPage.hiddenVoiceTrack >= 0 ? [playerPage.hiddenVoiceTrack] : []))
            }

            TrophyButton {
                implicitHeight: 32
                implicitWidth: 32
                padding: 0
                horizontalPadding: 0
                Layout.alignment: Qt.AlignVCenter
                text: "ⓘ"
                ToolTip.text: qsTr("File info")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: fileInfoDialog.open()
            }
        }

        VideoPlayer {
            id: player
            Layout.fillWidth: true
            Layout.fillHeight: true
            volumeLevel: playerPage.masterVolume
            masterMuted: playerPage.masterMuted
            playerFullscreen: playerPage.playerFullscreen
            playbackRate: playerPage.playbackRate
            rate: playerPage.playbackRate
            onPositionChanged: {
                playerPage.followPlayhead()
                playerPage.loopCheck()
            }
            onDurationChanged: {
                // The media clock arrives after open(): arm the full
                // range when clip mode was entered before it landed.
                if (playerPage.clipMode && playerPage.clipEndMs < 0)
                    playerPage.initClipRange()
            }
            onPlayRequested: playerPage.playAll()
            onPauseRequested: playerPage.pauseAll()
            onSeekRequested: function(ms) {
                playerPage.seekAll(ms)
            }
            onVolumeRequested: function(v) {
                playerPage.masterVolume = v
            }
            onMuteRequested: playerPage.masterMuted = !playerPage.masterMuted
            onFullscreenRequested: playerPage.playerFullscreen = !playerPage.playerFullscreen
            onRateRequested: function(r) {
                playerPage.setRate(r)
            }
            onSnapshotRequested: playerPage.snapshotCurrent()
            onSnapshotSaved: function(path, ok) {
                if (typeof notificationService === "undefined" || notificationService === null)
                    return
                if (ok)
                    notificationService.showNotification(qsTr("Snapshot saved"), path.substring(path.lastIndexOf("/") + 1), "trophy", 3000, 1)
                else
                    notificationService.showError(qsTr("Could not save the snapshot."))
            }
        }

        // One audio-only pipeline per stream, pinned to its
        // track. Mute is per-pipeline (genuinely silent), so
        // lanes mix freely instead of soloing. Instantiator,
        // not Repeater: MediaPlayer isn't an Item and Repeater
        // refuses non-Item delegates (audio silently never
        // started). The track pins on LoadedMedia, not at
        // creation: setting it with no open source is ignored.
        Instantiator {
            id: audioPipelines
            objectName: "playerAudioPipelines"
            model: playerPage.displayedAudioTracks

            MediaPlayer {
                id: audioPipeline
                required property int modelData
                objectName: "playerAudioPipeline" + modelData
                source: playerPage.filePath !== "" ? recordingLibrary.localFileUrl(playerPage.filePath) : ""
                autoPlay: false
                playbackRate: playerPage.playbackRate
                function pinTrack() {
                    if (audioTracks.length <= audioPipeline.modelData) return;
                    if (activeAudioTrack !== audioPipeline.modelData)
                        activeAudioTrack = audioPipeline.modelData;
                    position = playerPage.playPosition;
                    if (playerPage.playRunning) play();
                }
                onModelDataChanged: audioPipeline.pinTrack()
                onTracksChanged: audioPipeline.pinTrack()
                onMediaStatusChanged: {
                    if (mediaStatus === MediaPlayer.LoadedMedia) {
                        // Track discovery now runs asynchronously: a new
                        // audio pipeline can load after video has started.
                        // Join its current position/state, not time zero.
                        audioPipeline.pinTrack()
                    }
                }
                audioOutput: AudioOutput {
                    volume: playerPage.masterVolume
                    muted: playerPage.masterMuted || playerPage.isMuted(audioPipeline.modelData)
                }
            }
        }

        // Player-range clip results (the notice + library refresh
        // arrive from the Application wiring, chime included).
        Connections {
            target: recordingLibrary
            function onClipSaved(path, game, secs) {
                // Switch to the fresh clip while still showing its
                // source (open() resets the editor, so the clip
                // lands ready to watch). Otherwise the notice
                // covers it.
                if (playerPage.clipSourcePath !== "" && playerPage.filePath === playerPage.clipSourcePath) {
                    var dateStr = Qt.formatDateTime(new Date(), "MMM d, yyyy HH:mm")
                    var title = game !== "" ? qsTr("%1 Clip").arg(game) : path.substring(path.lastIndexOf("/") + 1)
                    playerPage.open(path, title, dateStr)
                }
                playerPage.clipSourcePath = ""
                playerPage.clipError = ""
            }
            function onClipSaveFailed(reason) {
                playerPage.clipSourcePath = ""
                playerPage.clipError = reason
            }
        }

        // Track lanes (hidden in fullscreen so the video
        // takes the space).
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: playerPage.tracksHeight > 0
                ? playerPage.tracksHeight
                : lanesLayout.implicitHeight + Theme.spacingMedium * 2
            radius: Theme.radiusMedium
            color: Theme.cardColor
            visible: !playerPage.playerFullscreen && playerPage.filePath !== ""

            // Top-edge drag handle: pull down to compress
            // the tracks (the video takes the room). Drag
            // math uses scene coordinates — the handle rides
            // the moving edge, so handle-relative deltas
            // would cancel themselves out.
            MouseArea {
                id: tracksDrag
                anchors.top: parent.top
                anchors.topMargin: -Theme.spacingMedium + 2
                anchors.horizontalCenter: parent.horizontalCenter
                width: parent.width * 0.35
                height: Theme.spacingMedium
                hoverEnabled: true
                cursorShape: Qt.SizeVerCursor
                property double pressSceneY: 0
                property double pressHeight: 0
                onPressed: function(mouse) {
                    pressSceneY = tracksDrag.mapToItem(null, mouse.x, mouse.y).y
                    pressHeight = playerPage.tracksHeight > 0
                        ? playerPage.tracksHeight
                        : playerPage.tracksNaturalHeight
                }
                onPositionChanged: function(mouse) {
                    if (!pressed)
                        return
                    var sceneY = tracksDrag.mapToItem(null, mouse.x, mouse.y).y
                    playerPage.setTracksHeight(pressHeight - (sceneY - pressSceneY))
                }
                onDoubleClicked: playerPage.tracksHeight = 0

                Rectangle {
                    anchors.centerIn: parent
                    width: 48
                    height: 4
                    radius: 2
                    color: tracksDrag.containsMouse ? Theme.textSecondaryColor : Theme.borderColor
                }
            }

            ColumnLayout {
                id: lanesLayout

                anchors.fill: parent
                anchors.margins: Theme.spacingMedium
                spacing: Theme.spacingSmall
                // Compressed heights clip the lower lanes
                // (they shrink first; clipping only kicks
                // in below the smallest all-lanes fit).
                clip: true

                // Tracks header: zoom and clip share one row, so
                // editing never pushes a lane out of the card.
                RowLayout {
                    id: zoomRow
                    Layout.fillWidth: true
                    visible: playerPage.filePath !== ""
                    spacing: Theme.spacingSmall

                    Label {
                        text: qsTr("Tracks")
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.Bold
                        font.capitalization: Font.AllUppercase
                    }

                    // Clip mode: range handles on every lane (snip
                    // cuts via the library).
                    TrophyButton {
                        implicitWidth: 32
                        implicitHeight: 28
                        padding: 0
                        horizontalPadding: 0
                        checkable: true
                        checked: playerPage.clipMode
                        outlined: true
                        icon.source: "qrc:/icons/scissors.svg"
                        icon.width: 16
                        icon.height: 16
                        ToolTip.text: qsTr("Clip a range")
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        onClicked: playerPage.toggleClipMode()
                    }

                    TrophyButton {
                        implicitHeight: 28
                        visible: playerPage.clipMode
                        text: qsTr("Set start")
                        enabled: playerPage.playDuration > 0
                        onClicked: playerPage.setClipHandle(0, playerPage.playPosition)
                    }

                    TrophyButton {
                        implicitHeight: 28
                        visible: playerPage.clipMode
                        text: qsTr("Set end")
                        enabled: playerPage.playDuration > 0
                        onClicked: playerPage.setClipHandle(1, playerPage.playPosition)
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: playerPage.clipMode
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        color: Theme.textSecondaryColor
                        font.pixelSize: Theme.fontSizeSmall
                        font.family: "monospace"
                        text: playerPage.clipRangeValid()
                            ? player.formatTime(playerPage.clipStartMs) + " – " + player.formatTime(playerPage.clipEndMs) + " · " + player.formatTime(playerPage.clipEndMs - playerPage.clipStartMs)
                            : qsTr("Drag the handles or use Set start/end")
                    }

                    TrophyButton {
                        implicitHeight: 28
                        visible: playerPage.clipMode
                        checkable: true
                        checked: playerPage.loopClip
                        outlined: true
                        text: qsTr("Loop")
                        ToolTip.text: qsTr("Loop the selected range")
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        onClicked: playerPage.loopClip = !playerPage.loopClip
                    }

                    TrophyButton {
                        implicitHeight: 28
                        visible: playerPage.clipMode
                        text: qsTr("Clear")
                        onClicked: playerPage.clearClipRange()
                    }

                    TrophyButton {
                        implicitHeight: 28
                        visible: playerPage.clipMode
                        text: playerPage.clipSaving ? qsTr("Saving…") : qsTr("Snip")
                        baseColor: Theme.accentColor || "#3498db"
                        enabled: playerPage.clipRangeValid() && !playerPage.clipSaving && !(typeof storageManager !== "undefined" && storageManager !== null && storageManager.maxSize > 0 && storageManager.usedSize > storageManager.maxSize)
                        onClicked: playerPage.snipClip()
                    }

                    BusyIndicator {
                        Layout.preferredWidth: 20
                        Layout.preferredHeight: 20
                        running: visible
                        visible: playerPage.clipSaving
                    }

                    // Spacer while not editing: keeps the zoom
                    // controls right-aligned (in clip mode the
                    // range readout takes the slack instead).
                    Item {
                        Layout.fillWidth: true
                        visible: !playerPage.clipMode
                    }

                    TrophyButton {
                        implicitWidth: 32
                        implicitHeight: 28
                        padding: 0
                        horizontalPadding: 0
                        text: "−"
                        enabled: playerPage.zoomFactor > 1
                        onClicked: playerPage.setZoom(playerPage.zoomFactor / 2)
                    }

                    Label {
                        Layout.preferredWidth: 44
                        horizontalAlignment: Text.AlignHCenter
                        text: playerPage.zoomFactor === 1 ? qsTr("Fit") : qsTr("%1×").arg(playerPage.zoomFactor)
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                    }

                    TrophyButton {
                        implicitWidth: 32
                        implicitHeight: 28
                        padding: 0
                        horizontalPadding: 0
                        text: "+"
                        enabled: playerPage.zoomFactor < 16
                        onClicked: playerPage.setZoom(playerPage.zoomFactor * 2)
                    }
                }

                Label {
                    id: clipErrorLabel
                    Layout.fillWidth: true
                    visible: playerPage.clipError !== ""
                    text: playerPage.clipError
                    color: Theme.errorColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }

                VideoLane {
                    objectName: "playerVideoLane"
                    Layout.fillWidth: true
                    laneHeight: playerPage.laneHeight
                    headerWidth: playerPage.trackHeaderWidth
                    filePath: playerPage.filePath
                    bookmarks: playerPage.bookmarks
                    onBookmarkActivated: function(ms) { playerPage.seekAll(ms) }
                    playPosition: playerPage.playPosition
                    playDuration: playerPage.playDuration
                    viewStartMs: playerPage.viewStartMs
                    viewSpanMs: playerPage.viewSpanMs
                    clipActive: playerPage.clipMode
                    clipStartMs: playerPage.clipStartMs
                    clipEndMs: playerPage.clipEndMs
                    onSeekRequested: function(ms) {
                        playerPage.seekTo(ms)
                    }
                    onClipHandleMoved: function(which, ms) {
                        playerPage.setClipHandle(which, ms)
                    }
                    onViewZoomRequested: function(t, frac, zoomIn) {
                        playerPage.zoomAt(t, frac, zoomIn)
                    }
                    onViewPanRequested: function(startMs) {
                        playerPage.panViewTo(startMs)
                    }
                }

                Repeater {
                    model: playerPage.displayedAudioTracks

                    WaveLane {
                        required property int modelData
                        objectName: "playerAudioLane" + modelData
                        Layout.fillWidth: true
                        laneHeight: playerPage.laneHeight
                        headerWidth: playerPage.trackHeaderWidth
                        filePath: playerPage.filePath
                        bookmarks: playerPage.bookmarks
                        onBookmarkActivated: function(ms) { playerPage.seekAll(ms) }
                        trackIndex: modelData
                        trackLabelOverride: playerPage.audioTrackName(modelData)
                        trackTotal: playerPage.audioLanes
                        laneMuted: playerPage.mutedTracks.indexOf(modelData) !== -1
                        playPosition: playerPage.playPosition
                        playDuration: playerPage.playDuration
                        viewStartMs: playerPage.viewStartMs
                        viewSpanMs: playerPage.viewSpanMs
                        clipActive: playerPage.clipMode
                        clipStartMs: playerPage.clipStartMs
                        clipEndMs: playerPage.clipEndMs
                        onSeekRequested: function(ms) {
                            playerPage.seekTo(ms)
                        }
                        onClipHandleMoved: function(which, ms) {
                            playerPage.setClipHandle(which, ms)
                        }
                        onZoomRequested: function(t, frac, zoomIn) {
                            playerPage.zoomAt(t, frac, zoomIn)
                        }
                        onPanRequested: function(startMs) {
                            playerPage.panViewTo(startMs)
                        }
                        onMuteToggled: function(t) {
                            playerPage.toggleMute(t)
                        }
                    }
                }

                Label {
                    id: noAudioLabel
                    text: playerPage.audioProbePending ? qsTr("Reading audio tracks…") : playerPage.trackCount < 0 ? qsTr("Audio tracks unavailable.") : qsTr("No audio tracks in this file.")
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    visible: playerPage.trackCount <= 0
                }
            }
        }
    }

    // The same positioned hit targets are used on every lane. Only the
    // video lane draws flags; audio lanes continue their yellow stems.
    component BookmarkMarkers: Item {
        id: markerLayer
        property var bookmarks: []
        property double viewStartMs: 0
        property double viewSpanMs: 0
        property bool showFlags: true
        signal bookmarkActivated(double ms)
        z: 5
        Repeater {
            model: markerLayer.bookmarks
            delegate: ToolButton {
                id: bookmarkMarker
                required property var modelData
                readonly property double timelineX: markerLayer.viewSpanMs > 0 ? (modelData.positionMs - markerLayer.viewStartMs) / markerLayer.viewSpanMs * markerLayer.width : 0
                objectName: "timelineBookmark"
                width: 20
                height: markerLayer.height
                x: Math.max(0, Math.min(markerLayer.width - width, timelineX - width / 2))
                visible: markerLayer.viewSpanMs > 0 && modelData.positionMs >= markerLayer.viewStartMs && modelData.positionMs <= markerLayer.viewStartMs + markerLayer.viewSpanMs
                padding: 0
                horizontalPadding: 0
                icon.source: markerLayer.showFlags ? "qrc:/icons/bookmark.svg" : ""
                icon.color: "#FFD54F"
                icon.width: 16; icon.height: 20
                background: Item {
                    Rectangle {
                        x: Math.max(0, Math.min(parent.width - width, bookmarkMarker.timelineX - bookmarkMarker.x - width / 2))
                        width: 4
                        height: parent.height
                        color: "#B3000000"
                        Rectangle { anchors.centerIn: parent; width: 2; height: parent.height; color: "#FFD54F" }
                    }
                    Rectangle {
                        visible: markerLayer.showFlags
                        anchors.centerIn: parent
                        width: 18
                        height: Math.min(22, parent.height)
                        radius: 3
                        color: "#E6202020"
                    }
                }
                ToolTip.text: qsTr("Bookmark · %1 s").arg((modelData.positionMs / 1000).toFixed(1))
                ToolTip.visible: hovered
                Accessible.name: qsTr("Jump to bookmark")
                onClicked: markerLayer.bookmarkActivated(modelData.positionMs)
            }
        }
    }

    component WaveLane: Rectangle {
        id: lane

        property string filePath: ""
        property int trackIndex: 0
        property int trackTotal: 1
        property string trackLabelOverride: ""
        property real headerWidth: 64
        // True mute from the page (this lane's pipeline is
        // silent). Dimmed while muted.
        property bool laneMuted: false
        property double playPosition: 0
        property double playDuration: 0
        // Shared zoom window (subset of the file the lane
        // draws); the canvas repaints only when it moves.
        property double viewStartMs: 0
        property double viewSpanMs: 0
        property var peaks: []
        property var bookmarks: []
        signal bookmarkActivated(double ms)
        // Set on arrival (even empty) so the spinner stops —
        // mirrors VideoLane stripDone. The cache emits [] on
        // failure, so settled-with-empty means no decodable
        // audio (e.g. an empty voice track), not "still busy".
        property bool peaksDone: false
        readonly property bool trackEmpty: peaksDone && peaks.length === 0 && filePath !== ""
        // Track caption (Game/Voice/Mic), resolved once
        // so both header layouts can show it.
        readonly property string headerText: trackLabelOverride || ((typeof waveformCache !== "undefined" && waveformCache !== null) ? waveformCache.trackLabel(trackIndex, trackTotal) : "")
        // Set from the page: 64 while the card fits its
        // content, shrinking when it is compressed.
        property real laneHeight: 64

        signal seekRequested(double ms)
        signal muteToggled(int track)
        // Clip range overlay (page-owned selection): the lane only
        // draws it — drags report back via clipHandleMoved and the
        // page clamps. Never read player.* here (see page note).
        property bool clipActive: false
        property double clipStartMs: -1
        property double clipEndMs: -1
        signal clipHandleMoved(int which, double ms)
        // Wheel zoom (anchored at the cursor) and middle-drag
        // pan: neither moves the playhead.
        signal zoomRequested(double cursorMs, double cursorFrac, bool zoomIn)
        signal panRequested(double startMs)

        // Middle-drag pan anchor (pressed position + view).
        property double panAnchorX: 0
        property double panAnchorView: 0

        function scrub(x) {
            if (viewSpanMs > 0 && waveArea.width > 0) {
                seekRequested(viewStartMs + Math.max(0, Math.min(waveArea.width, x)) / waveArea.width * viewSpanMs)
            }
        }

        // Clip overlay state: shown while the page selection is set
        // and this lane has a drawable window. Handles position from
        // it; drags report file-clock ms back to the page.
        readonly property bool clipShow: lane.clipActive && lane.clipStartMs >= 0 && lane.clipEndMs > lane.clipStartMs && lane.viewSpanMs > 0 && waveArea.width > 0
        function clipMsToX(ms) {
            return (ms - lane.viewStartMs) / lane.viewSpanMs * waveArea.width
        }

        function refreshPeaks() {
            peaks = []
            peaksDone = false
            wave.requestPaint()
            if (filePath !== "" && typeof waveformCache !== "undefined" && waveformCache !== null)
                waveformCache.requestPeaks(filePath, trackIndex)
        }

        height: laneHeight
        // Report the height as the preferred size so the
        // outer ColumnLayout re-lays-out the lanes as the
        // card compresses — layouts track implicit sizes
        // and do not reliably re-polish when only the
        // raw height changes.
        implicitHeight: laneHeight
        radius: Theme.radiusSmall
        color: Theme.surfaceColor

        Component.onCompleted: lane.refreshPeaks()
        onFilePathChanged: lane.refreshPeaks()
        onViewStartMsChanged: wave.requestPaint()
        onViewSpanMsChanged: wave.requestPaint()

        Connections {
            target: waveformCache
            function onPeaksReady(readyPath, readyTrack, readyPeaks) {
                if (readyPath === lane.filePath && readyTrack === lane.trackIndex) {
                    lane.peaks = readyPeaks
                    lane.peaksDone = true
                    wave.requestPaint()
                    zebra.requestPaint()
                }
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingSmall
            anchors.rightMargin: Theme.spacingSmall
            spacing: Theme.spacingSmall

            // Track header: the mute button sits above
            // the name while the lane has room, and
            // moves beside it once compressed — the
            // name never disappears.
            ColumnLayout {
                id: laneHeader
                // Zero width while hidden: a hidden header
                // must never reserve space, or the wave
                // area gets squeezed to a sliver.
                Layout.preferredWidth: visible ? lane.headerWidth : 0
                Layout.alignment: Qt.AlignVCenter
                spacing: 2
                // Mute above the name while the lane has
                // room; the compact header takes over
                // below. The threshold is the stacked
                // layout's own implicit height, so it
                // adapts instead of hardcoding pixels.
                visible: lane.laneHeight >= laneHeader.implicitHeight

                TrophyButton {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: 28
                    implicitHeight: 28
                    // Zero padding: the style's default insets fight
                    // the fixed 28px footprint and push the icon off
                    // center (same for the zoom buttons below).
                    padding: 0
                    horizontalPadding: 0
                    icon.source: lane.laneMuted ? "qrc:/icons/volume-muted.svg" : "qrc:/icons/volume-high.svg"
                    icon.width: 16
                    icon.height: 16
                    onClicked: lane.muteToggled(lane.trackIndex)
                }

                Label {
                    Layout.preferredWidth: lane.headerWidth
                    Layout.alignment: Qt.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                    text: lane.headerText
                    color: Theme.textColor
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }

            RowLayout {
                id: laneHeaderCompact
                // Zero width while hidden (see above).
                Layout.preferredWidth: visible ? lane.headerWidth : 0
                Layout.alignment: Qt.AlignVCenter
                spacing: 2
                // Mute button first, name to its right
                // (left aligned) — the name never disappears.
                visible: lane.laneHeight < laneHeader.implicitHeight

                TrophyButton {
                    implicitWidth: 28
                    implicitHeight: 28
                    // Hug the lane's left edge: cancel the
                    // outer row's left margin exactly (this
                    // also opens the gap to the label).
                    transform: Translate { x: -Theme.spacingSmall }
                    // Zero padding: the style's default insets fight
                    // the fixed 28px footprint and push the icon off
                    // center (same for the zoom buttons below).
                    padding: 0
                    horizontalPadding: 0
                    icon.source: lane.laneMuted ? "qrc:/icons/volume-muted.svg" : "qrc:/icons/volume-high.svg"
                    icon.width: 16
                    icon.height: 16
                    onClicked: lane.muteToggled(lane.trackIndex)
                }

                Label {
                    // Fixed share of the header (28px button + 2px gap): never
                    // fillWidth here — inside a fixed-width
                    // layout that inflates the header and
                    // squeezes the wave area to a sliver.
                    Layout.preferredWidth: lane.headerWidth - 30
                    horizontalAlignment: Text.AlignLeft
                    text: lane.headerText
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    color: Theme.textColor
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                }
            }

            Item {
                id: waveArea

                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                Canvas {
                    id: wave
                    anchors.fill: parent
                    onPaint: {
                        var ctx = getContext("2d")
                        ctx.clearRect(0, 0, width, height)
                        var n = lane.peaks.length
                        var span = lane.viewSpanMs
                        if (n === 0 || width <= 0 || height <= 0 || span <= 0 || lane.playDuration <= 0)
                            return
                        ctx.fillStyle = Theme.textSecondaryColor
                        var mid = height / 2
                        var amp = Math.max(2, mid - 3)
                        for (var x = 0; x < width; x += 1) {
                            var t = lane.viewStartMs + x / width * span
                            var idx = Math.floor(t / lane.playDuration * n)
                            if (idx < 0 || idx >= n)
                                continue
                            var v = lane.peaks[idx] || 0
                            var h = Math.max(1, v * amp)
                            ctx.fillRect(x, mid - h / 2, 1, h)
                        }
                    }
                }

                // Still decoding: spinner until the first peaks
                // arrive (even silence resolves — zeros paint).
                // Gated on the settled flag so an empty/failed
                // track stops spinning (see trackEmpty below).
                BusyIndicator {
                    width: 24
                    height: 24
                    anchors.centerIn: parent
                    running: visible
                    visible: !lane.peaksDone && lane.filePath !== ""
                }

                // No decodable audio on this track (settled with
                // an empty peak list, e.g. an empty voice track):
                // dimmed like the muted state, with diagonal
                // zebra striping so it reads as "no signal" rather
                // than "still loading". Non-visual items only, so
                // scrub/zoom/pan input still reaches the areas
                // beneath; the header caption stays intact.
                Item {
                    anchors.fill: parent
                    visible: lane.trackEmpty

                    Rectangle {
                        anchors.fill: parent
                        color: "#80000000"
                    }

                    Canvas {
                        id: zebra
                        anchors.fill: parent
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            if (width <= 0 || height <= 0)
                                return
                            ctx.strokeStyle = Theme.withOpacity(Theme.textMutedColor, 0.35)
                            ctx.lineWidth = 1
                            var step = 10
                            var d = width + height
                            for (var s = -height; s < d; s += step) {
                                ctx.beginPath()
                                ctx.moveTo(s, 0)
                                ctx.lineTo(s + height, height)
                                ctx.stroke()
                            }
                        }
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                    }

                    Label {
                        anchors.centerIn: parent
                        text: qsTr("No audio")
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                    }
                }

                BookmarkMarkers {
                    anchors.fill: parent
                    bookmarks: lane.bookmarks
                    viewStartMs: lane.viewStartMs
                    viewSpanMs: lane.viewSpanMs
                    showFlags: false
                    onBookmarkActivated: function(ms) { lane.bookmarkActivated(ms) }
                }

                // Shared playhead: positioned overlay, so the
                // waveform never repaints during playback. Hidden
                // while scrolled out of the zoom window.
                Rectangle {
                    width: 2
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: (lane.playPosition - lane.viewStartMs) / lane.viewSpanMs * waveArea.width - 1
                    color: Theme.primaryColor
                    visible: lane.viewSpanMs > 0 && lane.playPosition >= lane.viewStartMs && lane.playPosition <= lane.viewStartMs + lane.viewSpanMs
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                    onPressed: function(mouse) {
                        if (mouse.button === Qt.MiddleButton) {
                            lane.panAnchorX = mouse.x
                            lane.panAnchorView = lane.viewStartMs
                        } else {
                            lane.scrub(mouse.x)
                        }
                    }
                    onPositionChanged: function(mouse) {
                        if ((pressedButtons & Qt.MiddleButton) !== 0) {
                            if (lane.viewSpanMs > 0 && waveArea.width > 0)
                                lane.panRequested(lane.panAnchorView - (mouse.x - lane.panAnchorX) / waveArea.width * lane.viewSpanMs)
                        } else if (pressed) {
                            lane.scrub(mouse.x)
                        }
                    }
                    onWheel: function(wheel) {
                        if (lane.viewSpanMs <= 0 || waveArea.width <= 0)
                            return
                        var frac = Math.max(0, Math.min(1, wheel.x / waveArea.width))
                        lane.zoomRequested(lane.viewStartMs + frac * lane.viewSpanMs, frac, wheel.angleDelta.y > 0)
                    }
                }

                // Clip range shading: dimmed outside the selection,
                // tinted inside. Rectangles take no input, so clicks
                // elsewhere still reach the scrub area beneath; only
                // the 16px handle bars grab the cursor.
                Rectangle {
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: 0
                    width: lane.clipShow ? Math.max(0, lane.clipMsToX(lane.clipStartMs)) : 0
                    color: "#99000000"
                    visible: lane.clipShow
                }

                Rectangle {
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: lane.clipShow ? lane.clipMsToX(lane.clipEndMs) : 0
                    width: lane.clipShow ? Math.max(0, waveArea.width - x) : 0
                    color: "#99000000"
                    visible: lane.clipShow
                }

                Rectangle {
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: lane.clipShow ? lane.clipMsToX(lane.clipStartMs) : 0
                    width: lane.clipShow ? Math.max(0, lane.clipMsToX(lane.clipEndMs) - x) : 0
                    color: Theme.withOpacity(Theme.primaryColor, 0.12)
                    visible: lane.clipShow
                }

                Item {
                    id: waveHandleStart
                    z: 6
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 16
                    x: (lane.clipShow ? lane.clipMsToX(lane.clipStartMs) : 0) - width / 2
                    visible: lane.clipShow

                    Rectangle {
                        anchors.centerIn: parent
                        width: 4
                        height: parent.height
                        radius: 2
                        color: waveHandleStartMouse.pressed || waveHandleStartMouse.containsMouse ? Theme.primaryHoverColor : Theme.primaryColor
                    }

                    MouseArea {
                        id: waveHandleStartMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeHorCursor
                        property double grabDx: 0
                        onPressed: function(mouse) {
                            grabDx = mouse.x
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed || lane.viewSpanMs <= 0 || waveArea.width <= 0)
                                return
                            var px = waveHandleStart.x + mouse.x - grabDx + waveHandleStart.width / 2
                            lane.clipHandleMoved(0, lane.viewStartMs + px / waveArea.width * lane.viewSpanMs)
                        }
                    }
                }

                Item {
                    id: waveHandleEnd
                    z: 6
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 16
                    x: (lane.clipShow ? lane.clipMsToX(lane.clipEndMs) : 0) - width / 2
                    visible: lane.clipShow

                    Rectangle {
                        anchors.centerIn: parent
                        width: 4
                        height: parent.height
                        radius: 2
                        color: waveHandleEndMouse.pressed || waveHandleEndMouse.containsMouse ? Theme.primaryHoverColor : Theme.primaryColor
                    }

                    MouseArea {
                        id: waveHandleEndMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeHorCursor
                        property double grabDx: 0
                        onPressed: function(mouse) {
                            grabDx = mouse.x
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed || lane.viewSpanMs <= 0 || waveArea.width <= 0)
                                return
                            var px = waveHandleEnd.x + mouse.x - grabDx + waveHandleEnd.width / 2
                            lane.clipHandleMoved(1, lane.viewStartMs + px / waveArea.width * lane.viewSpanMs)
                        }
                    }
                }
            }
        }

        // Dimmed while muted: this lane's pipeline is silent.
        Rectangle {
            anchors.fill: parent
            radius: lane.radius
            color: "#80000000"
            visible: lane.laneMuted
        }
    }

    // Filmstrip lane for the video stream (same scrub +
    // playhead treatment as the audio lanes).
    component VideoLane: Rectangle {
        id: vlane

        property string filePath: ""
        property real headerWidth: 64
        property double playPosition: 0
        property double playDuration: 0
        property double viewStartMs: 0
        property double viewSpanMs: 0
        property var thumbs: []
        property var bookmarks: []
        signal bookmarkActivated(double ms)

        signal seekRequested(double ms)

        // Strip density: 32 thumbs keep the lane readable from
        // full-file fit down to deep zoom (each thumb knows its
        // own timestamp, so zooming never refetches). Each grab
        // is one backward seek + a single keyframe decode —
        // never a full decode — so 32 costs ~2x 16 on first
        // open only; the .strip-N.jpg sidecars cache forever
        // and the lane just blits 32 tiny textures.
        readonly property int stripCount: 32
        // Set on arrival (even empty) so the spinner stops.
        property bool stripDone: false
        // Middle-drag pan anchor (pressed position + view).
        property double panAnchorX: 0
        property double panAnchorView: 0
        // Set from the page: 64 while the card fits its
        // content, shrinking when it is compressed.
        property real laneHeight: 64

        // Wheel zoom (anchored at the cursor) and middle-drag
        // pan: neither moves the playhead.
        signal viewZoomRequested(double cursorMs, double cursorFrac, bool zoomIn)
        signal viewPanRequested(double startMs)
        // Clip range overlay (page-owned selection): the lane only
        // draws it — drags report back via clipHandleMoved and the
        // page clamps. Never read player.* here (see page note).
        property bool clipActive: false
        property double clipStartMs: -1
        property double clipEndMs: -1
        signal clipHandleMoved(int which, double ms)

        function thumbStartTime(i) {
            if (playDuration <= 0 || thumbs.length === 0)
                return 0
            return i / thumbs.length * playDuration
        }

        function scrub(x) {
            if (viewSpanMs > 0 && stripArea.width > 0) {
                seekRequested(viewStartMs + Math.max(0, Math.min(stripArea.width, x)) / stripArea.width * viewSpanMs)
            }
        }

        // Clip overlay state: shown while the page selection is set
        // and this lane has a drawable window. Handles position from
        // it; drags report file-clock ms back to the page.
        readonly property bool clipShow: vlane.clipActive && vlane.clipStartMs >= 0 && vlane.clipEndMs > vlane.clipStartMs && vlane.viewSpanMs > 0 && stripArea.width > 0
        function clipMsToX(ms) {
            return (ms - vlane.viewStartMs) / vlane.viewSpanMs * stripArea.width
        }

        function refresh() {
            thumbs = []
            stripDone = false
            if (filePath !== "" && typeof filmstripCache !== "undefined" && filmstripCache !== null)
                filmstripCache.requestThumbs(filePath, stripCount)
        }

        height: laneHeight
        // Report the height as the preferred size so the
        // outer ColumnLayout re-lays-out the lanes as the
        // card compresses — layouts track implicit sizes
        // and do not reliably re-polish when only the
        // raw height changes.
        implicitHeight: laneHeight
        radius: Theme.radiusSmall
        color: Theme.surfaceColor

        Component.onCompleted: vlane.refresh()
        onFilePathChanged: vlane.refresh()

        Connections {
            target: filmstripCache
            function onFilmstripReady(readyPath, readyPaths) {
                if (readyPath === vlane.filePath) {
                    vlane.thumbs = readyPaths
                    vlane.stripDone = true
                }
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingSmall
            anchors.rightMargin: Theme.spacingSmall
            spacing: Theme.spacingSmall

            // Video caption: it fits at any lane
            // height (the strip has no buttons), so
            // it stays put instead of hiding.
            Label {
                Layout.preferredWidth: vlane.headerWidth
                Layout.alignment: Qt.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("Video")
                color: Theme.textColor
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Font.Medium
                elide: Text.ElideRight
            }

            Item {
                id: stripArea

                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                // Thumbs position by timestamp, so the visible
                // window (not the whole file) decides layout —
                // zooming stays sharp with no refetch.
                Repeater {
                    model: vlane.thumbs

                    Image {
                        // Positioned by slice start, not the
                        // grab's center time: each thumb covers
                        // its own [i/count, (i+1)/count] slice,
                        // so the strip starts on the file's first
                        // frame instead of half a slice in (and
                        // the last thumb no longer overflows the
                        // right edge).
                        property double thumbStart: vlane.thumbStartTime(index)
                        property double thumbSpan: vlane.thumbs.length > 0 && vlane.playDuration > 0 ? vlane.playDuration / vlane.thumbs.length : 0
                        // 1px gutters so adjacent (often near-black)
                        // frames read as separate images instead of
                        // one dark void.
                        x: (vlane.viewSpanMs > 0 ? (thumbStart - vlane.viewStartMs) / vlane.viewSpanMs * stripArea.width : 0) + 1
                        width: (vlane.viewSpanMs > 0 ? thumbSpan / vlane.viewSpanMs * stripArea.width : 0) - 2
                        height: stripArea.height
                        source: recordingLibrary.localFileUrl(modelData)
                        fillMode: Image.PreserveAspectCrop
                        visible: vlane.viewSpanMs > 0 && x + width > 0 && x < stripArea.width
                    }
                }

                BusyIndicator {
                    width: 24
                    height: 24
                    anchors.centerIn: parent
                    running: visible
                    visible: !vlane.stripDone && vlane.filePath !== ""
                }

                BookmarkMarkers {
                    anchors.fill: parent
                    bookmarks: vlane.bookmarks
                    viewStartMs: vlane.viewStartMs
                    viewSpanMs: vlane.viewSpanMs
                    onBookmarkActivated: function(ms) { vlane.bookmarkActivated(ms) }
                }

                Rectangle {
                    width: 2
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: (vlane.playPosition - vlane.viewStartMs) / vlane.viewSpanMs * stripArea.width - 1
                    color: Theme.primaryColor
                    visible: vlane.viewSpanMs > 0 && vlane.playPosition >= vlane.viewStartMs && vlane.playPosition <= vlane.viewStartMs + vlane.viewSpanMs
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                    onPressed: function(mouse) {
                        if (mouse.button === Qt.MiddleButton) {
                            vlane.panAnchorX = mouse.x
                            vlane.panAnchorView = vlane.viewStartMs
                        } else {
                            vlane.scrub(mouse.x)
                        }
                    }
                    onPositionChanged: function(mouse) {
                        if ((pressedButtons & Qt.MiddleButton) !== 0) {
                            if (vlane.viewSpanMs > 0 && stripArea.width > 0)
                                vlane.viewPanRequested(vlane.panAnchorView - (mouse.x - vlane.panAnchorX) / stripArea.width * vlane.viewSpanMs)
                        } else if (pressed) {
                            vlane.scrub(mouse.x)
                        }
                    }
                    onWheel: function(wheel) {
                        if (vlane.viewSpanMs <= 0 || stripArea.width <= 0)
                            return
                        var frac = Math.max(0, Math.min(1, wheel.x / stripArea.width))
                        vlane.viewZoomRequested(vlane.viewStartMs + frac * vlane.viewSpanMs, frac, wheel.angleDelta.y > 0)
                    }
                }

                // Clip range shading: dimmed outside the selection,
                // tinted inside. Rectangles take no input, so clicks
                // elsewhere still reach the scrub area beneath; only
                // the 16px handle bars grab the cursor.
                Rectangle {
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: 0
                    width: vlane.clipShow ? Math.max(0, vlane.clipMsToX(vlane.clipStartMs)) : 0
                    color: "#99000000"
                    visible: vlane.clipShow
                }

                Rectangle {
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: vlane.clipShow ? vlane.clipMsToX(vlane.clipEndMs) : 0
                    width: vlane.clipShow ? Math.max(0, stripArea.width - x) : 0
                    color: "#99000000"
                    visible: vlane.clipShow
                }

                Rectangle {
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    x: vlane.clipShow ? vlane.clipMsToX(vlane.clipStartMs) : 0
                    width: vlane.clipShow ? Math.max(0, vlane.clipMsToX(vlane.clipEndMs) - x) : 0
                    color: Theme.withOpacity(Theme.primaryColor, 0.12)
                    visible: vlane.clipShow
                }

                Item {
                    id: stripHandleStart
                    z: 6 // Range editing remains usable when a handle overlaps a bookmark.
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 16
                    x: (vlane.clipShow ? vlane.clipMsToX(vlane.clipStartMs) : 0) - width / 2
                    visible: vlane.clipShow

                    Rectangle {
                        anchors.centerIn: parent
                        width: 4
                        height: parent.height
                        radius: 2
                        color: stripHandleStartMouse.pressed || stripHandleStartMouse.containsMouse ? Theme.primaryHoverColor : Theme.primaryColor
                    }

                    MouseArea {
                        id: stripHandleStartMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeHorCursor
                        property double grabDx: 0
                        onPressed: function(mouse) {
                            grabDx = mouse.x
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed || vlane.viewSpanMs <= 0 || stripArea.width <= 0)
                                return
                            var px = stripHandleStart.x + mouse.x - grabDx + stripHandleStart.width / 2
                            vlane.clipHandleMoved(0, vlane.viewStartMs + px / stripArea.width * vlane.viewSpanMs)
                        }
                    }
                }

                Item {
                    id: stripHandleEnd
                    z: 6
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 16
                    x: (vlane.clipShow ? vlane.clipMsToX(vlane.clipEndMs) : 0) - width / 2
                    visible: vlane.clipShow

                    Rectangle {
                        anchors.centerIn: parent
                        width: 4
                        height: parent.height
                        radius: 2
                        color: stripHandleEndMouse.pressed || stripHandleEndMouse.containsMouse ? Theme.primaryHoverColor : Theme.primaryColor
                    }

                    MouseArea {
                        id: stripHandleEndMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeHorCursor
                        property double grabDx: 0
                        onPressed: function(mouse) {
                            grabDx = mouse.x
                        }
                        onPositionChanged: function(mouse) {
                            if (!pressed || vlane.viewSpanMs <= 0 || stripArea.width <= 0)
                                return
                            var px = stripHandleEnd.x + mouse.x - grabDx + stripHandleEnd.width / 2
                            vlane.clipHandleMoved(1, vlane.viewStartMs + px / stripArea.width * vlane.viewSpanMs)
                        }
                    }
                }
            }
        }
    }

    // File info dialog (ⓘ next to the header timer). Rows read the
    // library entry so probe NOTIFYs refresh them; every row guards a
    // null entry (unknown file) with a fallback.
    component InfoRow: RowLayout {
        property string rowName: ""
        property string rowValue: ""
        Layout.fillWidth: true
        spacing: Theme.spacingMedium
        Label {
            text: rowName
            color: Theme.textSecondaryColor
            font.pixelSize: Theme.fontSizeSmall
            Layout.preferredWidth: 110
        }
        Label {
            text: rowValue
            color: Theme.textColor
            font.pixelSize: Theme.fontSizeSmall
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
    }

    Dialog {
        id: fileInfoDialog
        modal: true
        anchors.centerIn: parent
        width: 440
        padding: 0

        Overlay.modal: Rectangle {
            color: Theme.withOpacity("#000000", 0.5)
        }

        background: Rectangle {
            color: Theme.cardColor
            radius: Theme.radiusLarge
            border.color: Theme.borderColor
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingSmall

            Label {
                text: qsTr("File Info")
                font.pixelSize: Theme.fontSizeLarge
                font.weight: Font.Bold
                color: Theme.textColor
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacingLarge
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                spacing: 4

                InfoRow {
                    rowName: qsTr("File")
                    rowValue: playerPage.infoRec ? playerPage.infoRec.fileName : playerPage.fileTitle
                }
                InfoRow {
                    rowName: qsTr("Type")
                    rowValue: playerPage.infoRec ? (playerPage.infoRec.isClip ? qsTr("Clip") : qsTr("Recording")) : "–"
                }
                InfoRow {
                    rowName: qsTr("Game")
                    rowValue: playerPage.infoRec ? playerPage.infoRec.gameName : "–"
                }
                InfoRow {
                    rowName: qsTr("Date")
                    rowValue: playerPage.fileDate !== "" ? playerPage.fileDate : "–"
                }
                InfoRow {
                    rowName: qsTr("Duration")
                    rowValue: playerPage.infoRec ? player.formatTime(playerPage.infoRec.duration * 1000) : "–"
                }
                InfoRow {
                    rowName: qsTr("Size")
                    rowValue: playerPage.infoRec ? playerPage.formatInfoSize(playerPage.infoRec.size) : "–"
                }
                InfoRow {
                    rowName: qsTr("Resolution")
                    rowValue: (playerPage.infoRec && playerPage.infoRec.width > 0 && playerPage.infoRec.height > 0) ? (playerPage.infoRec.width + "×" + playerPage.infoRec.height) : "–"
                }
                InfoRow {
                    rowName: qsTr("Frame rate")
                    rowValue: playerPage.infoRec ? playerPage.formatInfoFps(playerPage.infoRec.fps) + (playerPage.infoRec.fps > 0 ? qsTr(" fps") : "") : "–"
                }
                InfoRow {
                    rowName: qsTr("Video")
                    rowValue: playerPage.infoRec ? playerPage.codecDisplayName(playerPage.infoRec.videoCodec) : "–"
                }
                InfoRow {
                    rowName: qsTr("Encoder")
                    rowValue: (playerPage.infoRec && playerPage.infoRec.encoderName !== "") ? playerPage.infoRec.encoderName : qsTr("Unknown")
                }
                InfoRow {
                    rowName: qsTr("Audio")
                    rowValue: (playerPage.infoRec && playerPage.infoRec.audioTrackCount > 0) ? (playerPage.infoRec.audioTrackCount + " × " + playerPage.codecDisplayName(playerPage.infoRec.audioCodec)) : qsTr("None")
                }
                InfoRow {
                    rowName: qsTr("Recorded with")
                    rowValue: (playerPage.infoRec && playerPage.infoRec.trophyVersion !== "") ? ("Trophy " + playerPage.infoRec.trophyVersion) : qsTr("Unknown version")
                }
                InfoRow {
                    rowName: qsTr("Location")
                    rowValue: playerPage.infoRec ? playerPage.infoRec.directory : "–"
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                Layout.bottomMargin: Theme.spacingLarge
                Item { Layout.fillWidth: true }
                TrophyButton {
                    text: qsTr("Close")
                    onClicked: fileInfoDialog.close()
                }
            }
        }
    }
}
