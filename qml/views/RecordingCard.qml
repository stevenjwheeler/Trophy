/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Rectangle {
    id: root

    property var recording: null
    readonly property bool hovered: mouseArea.containsMouse || playBtn.hovered || folderBtn.hovered || deleteBtn.hovered

    signal play()
    signal openFolder()
    signal deleteRequested()

    function formatDuration(seconds) {
        if (!seconds)
            return "0:00";

        var h = Math.floor(seconds / 3600);
        var m = Math.floor((seconds % 3600) / 60);
        var s = seconds % 60;
        if (h > 0)
            return h + ":" + m.toString().padStart(2, '0') + ":" + s.toString().padStart(2, '0');

        return m + ":" + s.toString().padStart(2, '0');
    }

    function formatDate(date) {
        if (!date)
            return "";

        return Qt.formatDateTime(date, "MMM d, yyyy HH:mm");
    }

    function formatSize(bytes) {
        if (!bytes)
            return "";

        var gb = bytes / (1024 * 1024 * 1024);
        if (gb >= 1)
            return gb.toFixed(1) + " GB";

        var mb = bytes / (1024 * 1024);
        return mb.toFixed(0) + " MB";
    }

    radius: Theme.radiusMedium
    color: root.hovered ? Theme.cardHoverColor : Theme.cardColor

    MouseArea {
        id: mouseArea

        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.play()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- THUMBNAIL SECTION ---
        Rectangle {
            id: thumbContainer

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacingSmall
            color: Theme.surfaceColor
            
            layer.enabled: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: thumbContainer
            }

            // This wrapper scales both the image together
            Item {
                id: zoomWrapper

                anchors.fill: parent
                clip: true
                scale: root.hovered ? 1.05 : 1 // Subtle zoom

                Image {
                    id: thumbnailImage

                    anchors.fill: parent
                    source: recording && recording.thumbnailPath ? "file://" + recording.thumbnailPath : ""
                    fillMode: Image.PreserveAspectCrop
                }

                Behavior on scale {
                    NumberAnimation {
                        duration: 400
                        easing.type: Easing.OutCubic
                    }

                }

            }

            // Placeholder when no thumbnail is available
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

            // Action Overlay (appears on hover)
            Rectangle {
                id: hoverOverlay

                anchors.fill: parent
                color: "#80000000"
                opacity: root.hovered ? 1 : 0
                radius: Theme.radiusSmall

                Row {
                    anchors.centerIn: parent
                    spacing: Theme.spacingSmall

                    RoundButton {
                        id: playBtn

                        width: 48
                        height: 48
                        icon.source: "qrc:/icons/play.svg"
                        icon.color: "white"
                        icon.width: 32
                        icon.height: 32
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
                        icon.source: "qrc:/icons/folder.svg"
                        icon.color: "white"
                        icon.width: 28
                        icon.height: 28
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
                        icon.source: "qrc:/icons/trash.svg"
                        icon.color: "white"
                        icon.width: 28
                        icon.height: 28
                        onClicked: root.deleteRequested()

                        background: Rectangle {
                            radius: 18
                            color: parent.hovered ? Theme.errorColor : "#40FFFFFF"
                        }

                    }

                }

                Behavior on opacity {
                    NumberAnimation {
                        duration: Theme.animationFast
                    }

                }

            }

        }

        // --- METADATA SECTION ---
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

                Item {
                    Layout.fillWidth: true
                }

                Label {
                    text: recording ? formatSize(recording.size) : ""
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                }

            }

        }

    }

    // Smooth transition for card background
    Behavior on color {
        ColorAnimation {
            duration: Theme.animationFast
        }

    }

}
