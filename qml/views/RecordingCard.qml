/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../styles"

Rectangle {
    id: root

    property var recording: null

    signal play()
    signal openFolder()
    signal deleteRequested()

    radius: Theme.radiusMedium
    color: root.hovered ? Theme.cardHoverColor : Theme.cardColor

    Behavior on color {
        ColorAnimation { duration: Theme.animationFast }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.play()
    }

    readonly property bool hovered: mouseArea.containsMouse || playBtn.hovered || folderBtn.hovered || deleteBtn.hovered

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Thumbnail area
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacingSmall
            radius: Theme.radiusSmall
            color: Theme.surfaceColor
            clip: true

            // Thumbnail image
            Image {
                anchors.fill: parent
                source: recording && recording.thumbnailPath ?
                    "file://" + recording.thumbnailPath : ""
                fillMode: Image.PreserveAspectCrop
                visible: status === Image.Ready
            }

            // Placeholder when no thumbnail
            Label {
                anchors.centerIn: parent
                text: recording ? recording.gameName.charAt(0).toUpperCase() : "?"
                font.pixelSize: 48
                color: Theme.textMutedColor
                visible: !recording || !recording.thumbnailPath
            }

            // Duration badge
            Rectangle {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.spacingSmall
                width: durationLabel.width + Theme.spacingTiny * 2
                height: durationLabel.height + Theme.spacingTiny * 2
                radius: Theme.radiusSmall
                color: "#CC000000"

                Label {
                    id: durationLabel
                    anchors.centerIn: parent
                    text: recording ? formatDuration(recording.duration) : "0:00"
                    color: "white"
                    font.pixelSize: Theme.fontSizeSmall
                    font.family: "monospace"
                }
            }

            // Hover overlay with actions
            Rectangle {
                anchors.fill: parent
                color: "#80000000"
                opacity: root.hovered ? 1 : 0
                radius: Theme.radiusSmall

                Behavior on opacity {
                    NumberAnimation { duration: Theme.animationFast }
                }

                Row {
                    anchors.centerIn: parent
                    spacing: Theme.spacingSmall

                    RoundButton {
                        id: playBtn
                        width: 48
                        height: 48
                        icon.name: "media-playback-start"
                        icon.color: "white"
                        onClicked: root.play()

                        background: Rectangle {
                            radius: 24
                            color: parent.hovered ? Theme.primaryHoverColor : Theme.primaryColor
                        }
                    }

                    RoundButton {
                        id: folderBtn
                        width: 36
                        height: 36
                        icon.name: "folder-open"
                        icon.color: "white"
                        onClicked: root.openFolder()

                        background: Rectangle {
                            radius: 18
                            color: parent.hovered ? "#60FFFFFF" : "#40FFFFFF"
                        }
                    }

                    RoundButton {
                        id: deleteBtn
                        width: 36
                        height: 36
                        icon.name: "edit-delete"
                        icon.color: "white"
                        onClicked: root.deleteRequested()

                        background: Rectangle {
                            radius: 18
                            color: parent.hovered ? Theme.errorColor : "#40FFFFFF"
                        }
                    }
                }
            }
        }

        // Metadata
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingSmall
            Layout.topMargin: 0
            spacing: 2

            Label {
                Layout.fillWidth: true
                text: recording ? recording.gameName : qsTr("Unknown Game")
                color: Theme.textColor
                font.pixelSize: Theme.fontSizeNormal
                font.weight: Font.Medium
                elide: Text.ElideRight
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Label {
                    text: recording ? formatDate(recording.date) : ""
                    color: Theme.textSecondaryColor
                    font.pixelSize: Theme.fontSizeSmall
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: recording ? formatSize(recording.size) : ""
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
        }
    }

    function formatDuration(seconds) {
        if (!seconds) return "0:00"
        var h = Math.floor(seconds / 3600)
        var m = Math.floor((seconds % 3600) / 60)
        var s = seconds % 60

        if (h > 0) {
            return h + ":" + m.toString().padStart(2, '0') + ":" + s.toString().padStart(2, '0')
        }
        return m + ":" + s.toString().padStart(2, '0')
    }

    function formatDate(date) {
        if (!date) return ""
        return Qt.formatDateTime(date, "MMM d, yyyy HH:mm")
    }

    function formatSize(bytes) {
        if (!bytes) return ""
        var gb = bytes / (1024 * 1024 * 1024)
        if (gb >= 1) return gb.toFixed(1) + " GB"
        var mb = bytes / (1024 * 1024)
        return mb.toFixed(0) + " MB"
    }
}
