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
    readonly property bool atEnd: mediaPlayer.mediaStatus === MediaPlayer.EndOfMedia

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
    // The page fans the requested playback rate out to
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
        if (typeof videoSurface.clearOutput === "function")
            videoSurface.clearOutput();
    }

    function snapshot(filePath) {
        videoSurface.grabToImage(function(result) {
            root.snapshotSaved(filePath, result.saveToFile(filePath))
        })
    }

    function captureSnapshot(callback) {
        return videoSurface.grabToImage(callback)
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
        videoOutput: videoSurface
        onErrorOccurred: function(error, errorString) {
            console.error("Video playback error:", errorString);
        }

    }

    VideoOutput {
        id: videoSurface
        objectName: "playerVideoOutput"

        anchors.fill: parent
        // Qt 6.9+ clears the output at EOF by default. Keep the final
        // decoded frame instead; older Qt versions lack this policy API.
        Component.onCompleted: {
            if (typeof videoSurface.endOfStreamPolicy !== "undefined")
                videoSurface.endOfStreamPolicy = VideoOutput.KeepLastFrame
        }
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
        bottomLeftRadius: root.playerFullscreen ? 0 : Theme.radiusMedium
        bottomRightRadius: root.playerFullscreen ? 0 : Theme.radiusMedium
        opacity: root.controlsShown || speedPopup.visible || !mediaPlayer.playing ? 1 : 0

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
                text: root.formatTime(mediaPlayer.position) + " / " + root.formatTime(mediaPlayer.duration)
                color: "white"
                font.pixelSize: Theme.fontSizeSmall
                font.family: "monospace"
            }

            // Seek back 10s
            TrophyButton {
                objectName: "playerSeekBackButton"
                implicitWidth: 36
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                icon.source: "qrc:/icons/seek-back-10.svg"
                icon.width: 22
                icon.height: 22
                Accessible.name: qsTr("Back 10 seconds")
                ToolTip.text: qsTr("Back 10 seconds")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: root.seekRequested(Math.max(0, mediaPlayer.position - 10000))
            }

            // Seek forward 10s
            TrophyButton {
                objectName: "playerSeekForwardButton"
                implicitWidth: 36
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                icon.source: "qrc:/icons/seek-forward-10.svg"
                icon.width: 22
                icon.height: 22
                Accessible.name: qsTr("Forward 10 seconds")
                ToolTip.text: qsTr("Forward 10 seconds")
                ToolTip.visible: hovered
                ToolTip.delay: 600
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

            // Playback speed: opens a notched slider popup; the
            // chosen rate is fanned out to every pipeline by
            // the page.
            TrophyButton {
                id: speedButton
                objectName: "playerSpeedButton"
                implicitWidth: 36
                implicitHeight: 36
                padding: 0
                horizontalPadding: 0
                text: (root.rate === 1 ? "1×" : root.rate + "×")
                Accessible.name: qsTr("Playback speed: %1").arg(text)
                contentItem: Item {
                    Image {
                        anchors.centerIn: parent
                        width: 28
                        height: 24
                        source: "qrc:/icons/playback-speed.svg"
                        opacity: speedButton.hovered ? 1 : 0.8
                    }
                    Label {
                        objectName: "playerSpeedValue"
                        anchors.centerIn: parent
                        anchors.verticalCenterOffset: 3
                        text: speedButton.text
                        color: Theme.textColor
                        font.pixelSize: root.rate < 1 ? 7 : 9
                        font.weight: Font.DemiBold
                    }
                }
                ToolTip.text: qsTr("Playback speed")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: speedPopup.opened ? speedPopup.close() : speedPopup.open()

                Popup {
                    id: speedPopup
                    objectName: "playerSpeedPopup"
                    x: (speedButton.width - width) / 2
                    y: -height - Theme.spacingSmall
                    width: 296
                    padding: 24
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    background: Rectangle {
                        color: Theme.cardColor
                        radius: Theme.radiusMedium
                        border.color: Theme.borderColor
                    }

                    contentItem: ColumnLayout {
                        spacing: 8

                        Label {
                            text: qsTr("Playback speed")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Font.Medium
                            Layout.alignment: Qt.AlignLeft
                            Layout.bottomMargin: 4
                        }

                        TrophySlider {
                            id: speedSlider
                            objectName: "playerSpeedSlider"
                            Layout.fillWidth: true
                            from: 0
                            to: root.rateSteps.length - 1
                            stepSize: 1
                            snapMode: Slider.SnapAlways
                            value: {
                                var i = root.rateSteps.indexOf(root.rate)
                                return i >= 0 ? i : 2
                            }
                            onMoved: {
                                var r = root.rateSteps[Math.round(value)]
                                if (r !== root.rate)
                                    root.rateRequested(r)
                            }

                            // Notch dots drawn on the track itself.
                            Repeater {
                                model: root.rateSteps
                                Rectangle {
                                    required property int index
                                    objectName: "playerSpeedNotch" + index
                                    width: 4
                                    height: 4
                                    radius: 2
                                    z: 0
                                    color: Theme.textColor
                                    opacity: 0.8
                                    x: speedSlider.leftPadding
                                       + speedSlider.handle.width / 2
                                       + (root.rateSteps.length > 1
                                          ? (index / (root.rateSteps.length - 1))
                                            * (speedSlider.availableWidth - speedSlider.handle.width)
                                          : 0)
                                       - width / 2
                                    y: speedSlider.topPadding
                                       + (speedSlider.availableHeight - height) / 2
                                }
                            }
                        }

                        // Labels under the matching notches.
                        Item {
                            id: speedLabels
                            Layout.fillWidth: true
                            implicitHeight: 20
                            Repeater {
                                model: root.rateSteps
                                Label {
                                    required property var modelData
                                    required property int index
                                    objectName: "playerSpeedLabel" + index
                                    text: (modelData === 1 ? "1×" : modelData + "×")
                                    color: index === Math.round(speedSlider.value)
                                           ? Theme.accentColor : Theme.textSecondaryColor
                                    font.pixelSize: Theme.fontSizeSmall
                                    font.weight: index === Math.round(speedSlider.value)
                                                 ? Font.DemiBold : Font.Normal
                                    x: speedSlider.leftPadding
                                       + speedSlider.handle.width / 2
                                       + (root.rateSteps.length > 1
                                          ? (index / (root.rateSteps.length - 1))
                                            * (speedSlider.availableWidth - speedSlider.handle.width)
                                          : 0)
                                       - implicitWidth / 2
                                    y: 0
                                }
                            }
                        }
                    }
                }
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
