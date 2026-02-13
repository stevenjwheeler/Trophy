/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../styles"

Rectangle {
    id: root

    property int duration: 0  // milliseconds
    property string gameName: ""

    implicitWidth: layout.implicitWidth + Theme.spacingMedium * 2
    implicitHeight: layout.implicitHeight + Theme.spacingSmall * 2

    radius: Theme.radiusMedium
    color: Theme.withOpacity(Theme.recordingColor, 0.9)

    // Pulsing animation
    SequentialAnimation on opacity {
        running: root.visible
        loops: Animation.Infinite
        NumberAnimation { to: 0.7; duration: 800; easing.type: Easing.InOutQuad }
        NumberAnimation { to: 1.0; duration: 800; easing.type: Easing.InOutQuad }
    }

    RowLayout {
        id: layout
        anchors.centerIn: parent
        spacing: Theme.spacingSmall

        // Recording dot
        Rectangle {
            width: 10
            height: 10
            radius: 5
            color: "white"

            SequentialAnimation on opacity {
                running: root.visible
                loops: Animation.Infinite
                NumberAnimation { to: 0.3; duration: 500 }
                NumberAnimation { to: 1.0; duration: 500 }
            }
        }

        // Duration
        Label {
            text: formatDuration(root.duration)
            color: "white"
            font.pixelSize: Theme.fontSizeNormal
            font.weight: Font.Medium
            font.family: "monospace"
        }

        // Game name (if available)
        Label {
            visible: root.gameName !== ""
            text: "| " + root.gameName
            color: Theme.withOpacity(Qt.color("white"), 0.8)
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideRight
            Layout.maximumWidth: 150
        }

        // Stop button
        Button {
            display: AbstractButton.IconOnly
            icon.source: "qrc:/icons/stop.svg"
            icon.color: "white"
            flat: true
            onClicked: {
                if (typeof recordingManager !== "undefined" && recordingManager) {
                    recordingManager.stopRecording()
                }
            }
            
            background: Rectangle {
                color: parent.hovered ? "rgba(255,255,255,0.2)" : "transparent"
                radius: 4
            }
        }
    }

    function formatDuration(ms) {
        var totalSeconds = Math.floor(ms / 1000)
        var hours = Math.floor(totalSeconds / 3600)
        var minutes = Math.floor((totalSeconds % 3600) / 60)
        var seconds = totalSeconds % 60

        if (hours > 0) {
            return hours.toString().padStart(2, '0') + ":" +
                   minutes.toString().padStart(2, '0') + ":" +
                   seconds.toString().padStart(2, '0')
        }
        return minutes.toString().padStart(2, '0') + ":" +
               seconds.toString().padStart(2, '0')
    }
}
