/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../controls"
import "../styles"
import QtMultimedia
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property alias source: mediaPlayer.source
    property alias playing: mediaPlayer.playing
    // Timeline surface for the player screen (track lanes
    // read these and drive seek()).
    property alias position: mediaPlayer.position
    property alias duration: mediaPlayer.duration

    // Transport is page-driven: the player screen runs one
    // audio-only MediaPlayer per track alongside this video
    // output, so play/pause/seek/volume/mute fan out to every
    // pipeline together (a single output can't mix mutes).
    signal playRequested()
    signal pauseRequested()
    signal seekRequested(double position)
    signal volumeRequested(double volume)
    signal muteRequested()
    signal fullscreenRequested()
    // Playback rate cycles here; the page fans the new rate out to
    // every audio pipeline too (lockstep). Snapshot grabs the current
    // video frame to a file and reports back.
    signal rateRequested(double rate)
    signal snapshotRequested()
    signal snapshotSaved(string path, bool ok)

    // Mirrored from the page (master volume/mute live there).
    property real volumeLevel: 1.0
    property bool masterMuted: false
    // Playback rate, bound to the page (which also drives every
    // audio-only pipeline). Sticky on the MediaPlayer: set once,
    // survives play/pause/seek.
    property alias playbackRate: mediaPlayer.playbackRate
    property real rate: 1.0
    readonly property var rateSteps: [0.25, 0.5, 1.0, 1.5, 2.0, 4.0]

    function nextRate() {
        var i = rateSteps.indexOf(rate)
        return rateSteps[(i + 1) % rateSteps.length]
    }
    // Mirrored from the page (the fullscreen state lives
    // there; the window itself is driven by main.qml).
    // Squares the video corners so the frame fills the
    // screen edge-to-edge.
    property bool playerFullscreen: false
    // Controls hover state (auto-hide timer below).
    property bool controlsShown: false

    function play() {
        mediaPlayer.play();
    }

    function seek(ms) {
        if (mediaPlayer.duration > 0)
            mediaPlayer.position = Math.max(0, Math.min(mediaPlayer.duration, ms));
    }

    function pause() {
        mediaPlayer.pause();
    }

    function stop() {
        mediaPlayer.stop();
    }

    function snapshot(filePath) {
        videoOutput.grabToImage(function(result) {
            root.snapshotSaved(filePath, result.saveToFile(filePath))
        })
    }

    function formatTime(ms) {
        var totalSeconds = Math.floor(ms / 1000);
        var hours = Math.floor(totalSeconds / 3600);
        var minutes = Math.floor((totalSeconds % 3600) / 60);
        var seconds = totalSeconds % 60;
        if (hours > 0)
            return hours + ":" + minutes.toString().padStart(2, '0') + ":" + seconds.toString().padStart(2, '0');

        return minutes + ":" + seconds.toString().padStart(2, '0');
    }

    color: "black"
    radius: playerFullscreen ? 0 : Theme.radiusMedium
    clip: true

    MediaPlayer {
        id: mediaPlayer

        // Video only: the player screen runs a separate
        // audio-only pipeline per track (true per-track mute),
        // so this output stays silent.
        autoPlay: false
        videoOutput: videoOutput
        onErrorOccurred: function(error, errorString) {
            console.error("Video playback error:", errorString);
        }

    }

    VideoOutput {
        id: videoOutput

        anchors.fill: parent
    }

    // Hover anywhere over the player reveals the controls
    // (a non-consuming handler, so clicks still reach
    // buttons). They fade again after a quiet moment even
    // while the cursor stays over the video — a bar that
    // never hides blocks the view, especially in fullscreen.
    // Mouse movement re-reveals them (see the click area
    // below).
    HoverHandler {
        id: playerHover
        onHoveredChanged: {
            if (playerHover.hovered)
                root.controlsShown = true
            hideTimer.restart()
        }
    }

    Timer {
        id: hideTimer
        interval: 2500
        repeat: false
        onTriggered: root.controlsShown = false
    }

    // Controls overlay
    Rectangle {
        id: controls

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 60
        color: "#CC000000"
        // Match the video area's rounding so the bar never
        // squares off the bottom corners while visible.
        bottomLeftRadius: playerFullscreen ? 0 : Theme.radiusMedium
        bottomRightRadius: playerFullscreen ? 0 : Theme.radiusMedium
        opacity: root.controlsShown || !mediaPlayer.playing ? 1 : 0

        // The bar stays up while the cursor is over it —
        // its buttons and slider need the cursor there —
        // and fades again once it leaves.
        HoverHandler {
            onHoveredChanged: {
                if (hovered) {
                    root.controlsShown = true
                    hideTimer.stop()
                } else {
                    hideTimer.restart()
                }
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.spacingSmall
            spacing: Theme.spacingSmall

            // Play/Pause button
            TrophyButton {
                implicitWidth: 36
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                icon.source: mediaPlayer.playing ? "qrc:/icons/pause.svg" : "qrc:/icons/play.svg"
                icon.width: 18
                icon.height: 18
                onClicked: {
                    if (mediaPlayer.playing)
                        root.pauseRequested();
                    else
                        root.playRequested();
                }
            }

            // Time display
            Label {
                text: formatTime(mediaPlayer.position) + " / " + formatTime(mediaPlayer.duration)
                color: "white"
                font.pixelSize: Theme.fontSizeSmall
                font.family: "monospace"
            }

            // Seek back 10s
            TrophyButton {
                implicitWidth: 52
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                text: qsTr("−10")
                onClicked: root.seekRequested(Math.max(0, mediaPlayer.position - 10000))
            }

            // Seek forward 10s
            TrophyButton {
                implicitWidth: 52
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                text: qsTr("+10")
                onClicked: {
                    var target = mediaPlayer.position + 10000
                    if (mediaPlayer.duration > 0)
                        target = Math.min(target, mediaPlayer.duration)
                    root.seekRequested(target)
                }
            }

            Item {
                Layout.fillWidth: true
            }

            // Playback speed: cycles 0.25-4x, fanned out to every
            // pipeline by the page.
            TrophyButton {
                implicitWidth: 52
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                text: (root.rate === 1 ? "1×" : root.rate + "×")
                ToolTip.text: qsTr("Playback speed")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: root.rateRequested(root.nextRate())
            }

            // Frame snapshot: grabs the current video frame to a PNG
            // next to the recording.
            TrophyButton {
                implicitWidth: 36
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                icon.source: "qrc:/icons/camera.svg"
                icon.width: 18
                icon.height: 18
                ToolTip.text: qsTr("Save frame as image")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: root.snapshotRequested()
            }

            // Master mute
            TrophyButton {
                implicitWidth: 36
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                icon.source: root.masterMuted ? "qrc:/icons/volume-muted.svg" : "qrc:/icons/volume-high.svg"
                icon.width: 18
                icon.height: 18
                onClicked: root.muteRequested()
            }

            TrophySlider {
                Layout.preferredWidth: 80
                from: 0
                to: 1
                value: root.volumeLevel
                onMoved: root.volumeRequested(value)
            }

            // Fullscreen toggle: the window goes fullscreen
            // and the video fills it (Escape exits).
            TrophyButton {
                implicitWidth: 36
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                icon.source: "qrc:/icons/fullscreen.svg"
                icon.width: 18
                icon.height: 18
                onClicked: root.fullscreenRequested()
            }

        }

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.animationNormal
            }

        }

    }

    // Click to play/pause; movement over the video reveals
    // the controls and restarts their fade (hoverEnabled:
    // bare movement, not just drags, counts).
    MouseArea {
        anchors.fill: parent
        anchors.bottomMargin: controls.height
        hoverEnabled: true
        onClicked: {
            if (mediaPlayer.playing)
                root.pauseRequested();
            else
                root.playRequested();
        }
        onPositionChanged: {
            root.controlsShown = true
            hideTimer.restart()
        }
    }

}
