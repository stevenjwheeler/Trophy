/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import "../styles"

Rectangle {
    id: root

    property alias source: mediaPlayer.source
    property alias playing: mediaPlayer.playing

    color: "black"
    radius: Theme.radiusMedium
    clip: true

    MediaPlayer {
        id: mediaPlayer
        videoOutput: videoOutput
        audioOutput: AudioOutput {}

        onErrorOccurred: function(error, errorString) {
            console.error("Video playback error:", errorString)
        }
    }

    VideoOutput {
        id: videoOutput
        anchors.fill: parent
    }

    // Controls overlay
    Rectangle {
        id: controls
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 60
        color: "#CC000000"

        opacity: controlsArea.containsMouse || !mediaPlayer.playing ? 1 : 0

        Behavior on opacity {
            NumberAnimation { duration: Theme.animationNormal }
        }

        MouseArea {
            id: controlsArea
            anchors.fill: parent
            hoverEnabled: true
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.spacingSmall
            spacing: Theme.spacingSmall

            // Play/Pause button
            Button {
                flat: true
                icon.name: mediaPlayer.playing ? "media-playback-pause" : "media-playback-start"
                icon.color: "white"
                onClicked: {
                    if (mediaPlayer.playing) {
                        mediaPlayer.pause()
                    } else {
                        mediaPlayer.play()
                    }
                }
            }

            // Time display
            Label {
                text: formatTime(mediaPlayer.position) + " / " + formatTime(mediaPlayer.duration)
                color: "white"
                font.pixelSize: Theme.fontSizeSmall
                font.family: "monospace"
            }

            // Seek slider
            Slider {
                id: seekSlider
                Layout.fillWidth: true
                from: 0
                to: mediaPlayer.duration
                value: mediaPlayer.position

                onMoved: mediaPlayer.position = value

                background: Rectangle {
                    x: seekSlider.leftPadding
                    y: seekSlider.topPadding + seekSlider.availableHeight / 2 - height / 2
                    width: seekSlider.availableWidth
                    height: 4
                    radius: 2
                    color: "#40FFFFFF"

                    Rectangle {
                        width: seekSlider.visualPosition * parent.width
                        height: parent.height
                        radius: 2
                        color: Theme.primaryColor
                    }
                }

                handle: Rectangle {
                    x: seekSlider.leftPadding + seekSlider.visualPosition * (seekSlider.availableWidth - width)
                    y: seekSlider.topPadding + seekSlider.availableHeight / 2 - height / 2
                    width: 12
                    height: 12
                    radius: 6
                    color: "white"
                    visible: seekSlider.hovered || seekSlider.pressed
                }
            }

            // Volume
            Button {
                flat: true
                icon.name: mediaPlayer.audioOutput.muted ?
                    "audio-volume-muted" : "audio-volume-high"
                icon.color: "white"
                onClicked: mediaPlayer.audioOutput.muted = !mediaPlayer.audioOutput.muted
            }

            Slider {
                Layout.preferredWidth: 80
                from: 0
                to: 1
                value: mediaPlayer.audioOutput.volume
                onMoved: mediaPlayer.audioOutput.volume = value
            }

            // Fullscreen (placeholder)
            Button {
                flat: true
                icon.name: "view-fullscreen"
                icon.color: "white"
                onClicked: {
                    // TODO: Implement fullscreen
                }
            }
        }
    }

    // Click to play/pause
    MouseArea {
        anchors.fill: parent
        anchors.bottomMargin: controls.height
        onClicked: {
            if (mediaPlayer.playing) {
                mediaPlayer.pause()
            } else {
                mediaPlayer.play()
            }
        }
    }

    function play() {
        mediaPlayer.play()
    }

    function pause() {
        mediaPlayer.pause()
    }

    function stop() {
        mediaPlayer.stop()
    }

    function formatTime(ms) {
        var totalSeconds = Math.floor(ms / 1000)
        var hours = Math.floor(totalSeconds / 3600)
        var minutes = Math.floor((totalSeconds % 3600) / 60)
        var seconds = totalSeconds % 60

        if (hours > 0) {
            return hours + ":" +
                   minutes.toString().padStart(2, '0') + ":" +
                   seconds.toString().padStart(2, '0')
        }
        return minutes + ":" + seconds.toString().padStart(2, '0')
    }
}
