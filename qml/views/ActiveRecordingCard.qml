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
    clip: true

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
        if (!bytes || bytes <= 0) return "0 B";
        var kb = 1024;
        var mb = kb * 1024;
        var gb = mb * 1024;
        if (bytes >= gb) return (bytes / gb).toFixed(1) + " GB";
        if (bytes >= mb) return (bytes / mb).toFixed(1) + " MB";
        if (bytes >= kb) return (bytes / kb).toFixed(0) + " KB";
        return bytes + " B";
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- THUMBNAIL SECTION (same geometry as RecordingCard) ---
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 125
            clip: true

            // Full-bleed background with top-rounded corners only —
            // same silhouette as RecordingCard's masked thumbnail.
            Rectangle {
                width: parent.width
                height: parent.height + root.radius
                topLeftRadius: root.radius
                topRightRadius: root.radius
                color: Theme.surfaceColor
            }

            // Pulsing record dot (the "live" placeholder for a thumbnail)
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

            // REC badge (top-left), styled like the duration badge
            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: Theme.spacingSmall
                width: recLabel.width + 12
                height: recLabel.height + 4
                radius: 4
                color: Theme.errorColor

                Label {
                    id: recLabel

                    anchors.centerIn: parent
                    text: qsTr("REC")
                    color: "white"
                    font.pixelSize: Theme.fontSizeTiny
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

            // Duration badge (bottom-right) — same metrics as RecordingCard
            Rectangle {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Theme.spacingSmall
                width: durationLabel.width + 12
                height: durationLabel.height + 4
                radius: 4
                color: "#CC000000"

                Label {
                    id: durationLabel

                    anchors.centerIn: parent
                    text: recordingManager ? formatDuration(recordingManager.recordingDuration) : "0:00"
                    color: "white"
                    font.pixelSize: Theme.fontSizeTiny
                }
            }
        }

        // --- INFO SECTION (same structure as RecordingCard) ---
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacingSmall
            spacing: 2

            Label {
                Layout.fillWidth: true
                text: recordingManager ? (recordingManager.currentGameName || qsTr("Recording...")) : qsTr("Recording...")
                color: Theme.textColor
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Font.Bold
                elide: Text.ElideRight
            }

            Label {
                text: recordingManager ? (formatDuration(recordingManager.recordingDuration) + " • " + formatSize(recordingManager.recordingSize)) : ""
                color: Theme.textSecondaryColor
                font.pixelSize: Theme.fontSizeTiny
            }

            Item {
                Layout.fillHeight: true
            }

            Label {
                text: qsTr("In Progress")
                color: Theme.errorColor
                font.pixelSize: Theme.fontSizeTiny
                Layout.alignment: Qt.AlignBottom
            }
        }
    }

    // --- BORDER OVERLAY (same as RecordingCard default state) ---
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: "transparent"
        border.color: Theme.borderColor
        border.width: 1
    }
}
