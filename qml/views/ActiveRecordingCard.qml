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

Rectangle {
    id: root

    radius: Theme.radiusMedium
    color: Theme.cardColor

    function formatDuration(ms) {
        if (!ms || ms <= 0)
            return "0:00";

        var totalSecs = Math.floor(ms / 1000);
        var h = Math.floor(totalSecs / 3600);
        var m = Math.floor((totalSecs % 3600) / 60);
        var s = totalSecs % 60;
        if (h > 0)
            return h + ":" + m.toString().padStart(2, '0') + ":" + s.toString().padStart(2, '0');

        return m + ":" + s.toString().padStart(2, '0');
    }

    function formatSize(bytes) {
        if (!bytes || bytes <= 0)
            return "0 MB";

        var gb = bytes / (1024 * 1024 * 1024);
        if (gb >= 1)
            return gb.toFixed(1) + " GB";

        var mb = bytes / (1024 * 1024);
        return mb.toFixed(0) + " MB";
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Thumbnail section
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacingSmall
            radius: Theme.radiusSmall
            color: Theme.surfaceColor

            // Pulsing record dot
            Rectangle {
                anchors.centerIn: parent
                width: 40
                height: 40
                radius: 20
                color: Theme.errorColor

                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    running: true

                    NumberAnimation {
                        to: 0.3
                        duration: 700
                        easing.type: Easing.InOutSine
                    }

                    NumberAnimation {
                        to: 1.0
                        duration: 700
                        easing.type: Easing.InOutSine
                    }

                }

            }

            // Duration badge (bottom-right)
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
                    text: recordingManager ? formatDuration(recordingManager.recordingDuration) : "0:00"
                    color: "white"
                    font.pixelSize: Theme.fontSizeSmall
                    font.family: "monospace"
                }

            }

            // REC badge (top-left)
            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: Theme.spacingSmall
                width: recLabel.width + Theme.spacingTiny * 2
                height: recLabel.height + Theme.spacingTiny * 2
                radius: Theme.radiusSmall
                color: Theme.errorColor

                Label {
                    id: recLabel

                    anchors.centerIn: parent
                    text: qsTr("REC")
                    color: "white"
                    font.pixelSize: Theme.fontSizeSmall - 1
                    font.weight: Font.Bold
                }

                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    running: true

                    NumberAnimation {
                        to: 0.4
                        duration: 700
                        easing.type: Easing.InOutSine
                    }

                    NumberAnimation {
                        to: 1.0
                        duration: 700
                        easing.type: Easing.InOutSine
                    }

                }

            }

        }

        // Metadata section
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingSmall
            Layout.topMargin: 0
            spacing: 2

            Label {
                Layout.fillWidth: true
                text: recordingManager ? recordingManager.currentGameName : qsTr("Recording...")
                color: Theme.textColor
                font.pixelSize: Theme.fontSizeNormal
                font.weight: Font.Medium
                elide: Text.ElideRight
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Label {
                    text: qsTr("In Progress")
                    color: Theme.errorColor
                    font.pixelSize: Theme.fontSizeSmall
                }

                Item {
                    Layout.fillWidth: true
                }

                Label {
                    text: recordingManager ? formatSize(recordingManager.recordingSize) : ""
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                }

            }

        }

    }

}
