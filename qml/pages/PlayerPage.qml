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

    // Audio stream count for the open file (0 = none/unknown).
    // Assigned on open; lanes rebuild from it.
    property int trackCount: 0

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
    readonly property double playDuration: player.duration

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
        + (1 + trackCount) * (minLaneHeight + Theme.spacingSmall)
        + (trackCount === 0 ? noAudioLabel.implicitHeight + Theme.spacingSmall : 0)

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
            (interior - zoomRow.implicitHeight) / (1 + trackCount) - Theme.spacingSmall))
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
        + (1 + trackCount) * (fullLaneHeight + Theme.spacingSmall)
        + (trackCount === 0 ? noAudioLabel.implicitHeight + Theme.spacingSmall : 0)

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
        resetView()
        filePath = path || ""
        fileTitle = title || ""
        fileDate = dateStr || ""
        trackCount = (filePath !== "" && typeof waveformCache !== "undefined")
            ? waveformCache.audioTrackCount(filePath) : 0
        // Start with the tracks at their smallest: the
        // video gets the room, and the card expands on
        // drag (or double-click for full height).
        tracksHeight = minTracksHeight
        player.source = filePath !== "" ? "file://" + filePath : ""
        if (filePath !== "")
            playAll()
    }

    function close() {
        stopAll()
        player.source = ""
        filePath = ""
        fileTitle = ""
        fileDate = ""
        trackCount = 0
        mutedTracks = []
        resetView()
        tracksHeight = 0
        playerFullscreen = false
        visible = false
    }

    function seekTo(ms) {
        seekAll(ms)
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

    background: Rectangle {
        color: Theme.backgroundColor
        // Same page chrome as the other pages: the top
        // tucks under the title bar and the bottom-right
        // rounds into the window corner.
        bottomRightRadius: 12
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
        }

        VideoPlayer {
            id: player
            Layout.fillWidth: true
            Layout.fillHeight: true
            volumeLevel: playerPage.masterVolume
            masterMuted: playerPage.masterMuted
            playerFullscreen: playerPage.playerFullscreen
            onPositionChanged: playerPage.followPlayhead()
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
            model: playerPage.trackCount

            MediaPlayer {
                source: playerPage.filePath !== "" ? "file://" + playerPage.filePath : ""
                autoPlay: false
                onMediaStatusChanged: {
                    if (mediaStatus === MediaPlayer.LoadedMedia)
                        activeAudioTrack = index
                }
                audioOutput: AudioOutput {
                    volume: playerPage.masterVolume
                    muted: playerPage.masterMuted || playerPage.isMuted(index)
                }
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

                RowLayout {
                    id: zoomRow
                    Layout.fillWidth: true
                    visible: playerPage.filePath !== ""
                    spacing: Theme.spacingSmall

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Tracks")
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.Bold
                        font.capitalization: Font.AllUppercase
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

                VideoLane {
                    Layout.fillWidth: true
                    laneHeight: playerPage.laneHeight
                    filePath: playerPage.filePath
                    playPosition: playerPage.playPosition
                    playDuration: playerPage.playDuration
                    viewStartMs: playerPage.viewStartMs
                    viewSpanMs: playerPage.viewSpanMs
                    onSeekRequested: function(ms) {
                        playerPage.seekTo(ms)
                    }
                    onViewZoomRequested: function(t, frac, zoomIn) {
                        playerPage.zoomAt(t, frac, zoomIn)
                    }
                    onViewPanRequested: function(startMs) {
                        playerPage.panViewTo(startMs)
                    }
                }

                Repeater {
                    model: playerPage.trackCount

                    WaveLane {
                        Layout.fillWidth: true
                        laneHeight: playerPage.laneHeight
                        filePath: playerPage.filePath
                        trackIndex: index
                        trackTotal: playerPage.trackCount
                        laneMuted: playerPage.mutedTracks.indexOf(index) !== -1
                        playPosition: playerPage.playPosition
                        playDuration: playerPage.playDuration
                        viewStartMs: playerPage.viewStartMs
                        viewSpanMs: playerPage.viewSpanMs
                        onSeekRequested: function(ms) {
                            playerPage.seekTo(ms)
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
                    text: qsTr("No audio tracks in this file.")
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    visible: playerPage.trackCount === 0
                }
            }
        }
    }

    component WaveLane: Rectangle {
        id: lane

        property string filePath: ""
        property int trackIndex: 0
        property int trackTotal: 1
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
        // Track caption (Game/Voice/Mic), resolved once
        // so both header layouts can show it.
        readonly property string headerText: (typeof waveformCache !== "undefined" && waveformCache !== null) ? waveformCache.trackLabel(trackIndex, trackTotal) : ""
        // Set from the page: 64 while the card fits its
        // content, shrinking when it is compressed.
        property real laneHeight: 64

        signal seekRequested(double ms)
        signal muteToggled(int track)
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

        function refreshPeaks() {
            peaks = []
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
                    wave.requestPaint()
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
                Layout.preferredWidth: visible ? 64 : 0
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
                    Layout.preferredWidth: 64
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
                Layout.preferredWidth: visible ? 64 : 0
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
                    // Fixed share of the 64px header (28
                    // button + 2 spacing + 34 label): never
                    // fillWidth here — inside a fixed-width
                    // layout that inflates the header and
                    // squeezes the wave area to a sliver.
                    Layout.preferredWidth: 34
                    horizontalAlignment: Text.AlignLeft
                    text: lane.headerText
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
                BusyIndicator {
                    width: 24
                    height: 24
                    anchors.centerIn: parent
                    running: visible
                    visible: lane.peaks.length === 0 && lane.filePath !== ""
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
        property double playPosition: 0
        property double playDuration: 0
        property double viewStartMs: 0
        property double viewSpanMs: 0
        property var thumbs: []

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
                Layout.preferredWidth: 64
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
                        source: "file://" + modelData
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
            }
        }
    }
}
