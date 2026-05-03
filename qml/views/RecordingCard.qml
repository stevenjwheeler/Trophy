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

    property var recording: null
    property bool isAdaptive: true
    readonly property bool hovered: hoverHandler.hovered
    property bool isSelected: window.selectedRecordings && recording && window.selectedRecordings.indexOf(recording.filePath) !== -1
    property bool isSelectionModeActive: window.selectedRecordings && window.selectedRecordings.length > 0
    
    height: 180
    radius: Theme.radiusMedium
    color: Theme.cardColor
    clip: true
    
    // This is the magic for rounded clipping in Qt Quick
    layer.enabled: true

    signal play()
    signal openFolder()
    signal deleteRequested()

    function formatDuration(seconds) {
        if (!seconds || seconds <= 0) return "0:00";
        var h = Math.floor(seconds / 3600);
        var m = Math.floor((seconds % 3600) / 60);
        var s = seconds % 60;
        if (h > 0) return h + ":" + m.toString().padStart(2, '0') + ":" + s.toString().padStart(2, '0');
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

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onClicked: {
            if (root.isSelectionModeActive) {
                if (root.recording) {
                    window.toggleSelection(root.recording.filePath);
                }
            } else {
                root.play();
            }
        }
        z: 0 // Keep it behind everything else
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        z: 1

        // Thumbnail Section
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 100
            color: "black"
            
            Image {
                anchors.fill: parent
                source: root.recording && root.recording.thumbnailPath ? "file://" + root.recording.thumbnailPath : "qrc:/icons/video.svg"
                fillMode: Image.PreserveAspectCrop
                opacity: root.recording && root.recording.thumbnailPath ? 1.0 : 0.3
                
                Behavior on scale {
                    NumberAnimation { duration: 400; easing.type: Easing.OutCubic }
                }
                scale: root.hovered ? 1.05 : 1.0
            }

            // Hover Buttons Overlay
            Rectangle {
                anchors.fill: parent
                color: "#80000000"
                visible: root.hovered && !root.isSelectionModeActive

                Row {
                    anchors.centerIn: parent
                    spacing: Theme.spacingMedium

                    RoundButton {
                        width: 40; height: 40
                        icon.source: "qrc:/icons/play.svg"; icon.color: "white"; icon.width: 24; icon.height: 24
                        onClicked: root.play()
                        background: Rectangle { radius: 20; color: parent.hovered ? Theme.primaryHoverColor : Theme.primaryColor }
                    }

                    RoundButton {
                        width: 40; height: 40
                        icon.source: "qrc:/icons/folder.svg"; icon.color: "white"; icon.width: 24; icon.height: 24
                        onClicked: root.openFolder()
                        background: Rectangle { radius: 20; color: parent.hovered ? "#CCFFFFFF" : "#80FFFFFF" }
                    }

                    RoundButton {
                        width: 40; height: 40
                        icon.source: "qrc:/icons/trash.svg"; icon.color: "white"; icon.width: 24; icon.height: 24
                        onClicked: root.deleteRequested()
                        background: Rectangle { radius: 20; color: parent.hovered ? Theme.errorColor : "#80FFFFFF" }
                    }
                }
            }

            // Duration badge
            Rectangle {
                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: Theme.spacingSmall
                visible: !root.hovered && root.recording
                width: durationLabel.width + 12; height: durationLabel.height + 4
                radius: 4; color: "#CC000000"
                Label {
                    id: durationLabel; anchors.centerIn: parent
                    text: root.recording ? formatDuration(root.recording.duration) : ""
                    color: "white"; font.pixelSize: Theme.fontSizeTiny
                }
            }
        }

        // Info Section
        ColumnLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: Theme.spacingSmall; spacing: 2
            Label {
                text: root.recording ? root.recording.gameName || "Unknown Game" : ""
                font.pixelSize: Theme.fontSizeSmall; font.weight: Font.Bold; color: Theme.textColor; elide: Text.ElideRight; Layout.fillWidth: true
            }
            Label {
                text: root.recording ? formatDuration(root.recording.duration) + " • " + formatSize(root.recording.size) : ""
                font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor
            }
            Item { Layout.fillHeight: true }
            Label {
                text: root.recording ? Qt.formatDateTime(new Date(root.recording.date), "MMM d, yyyy HH:mm") : ""
                font.pixelSize: Theme.fontSizeTiny; color: Theme.textMutedColor; Layout.alignment: Qt.AlignBottom
            }
        }
    }

    // Selection Circle Overlay
    Rectangle {
        id: selectionCircle
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.spacingSmall
        width: 24
        height: 24
        radius: 12
        color: root.isSelected ? Theme.primaryColor : "#80000000"
        border.color: root.isSelected ? Theme.primaryColor : "white"
        border.width: 1.5
        visible: root.hovered || root.isSelectionModeActive
        z: 20

        Item {
            anchors.centerIn: parent
            width: 8
            height: 8
            visible: root.isSelected
            rotation: 45
            anchors.verticalCenterOffset: -2
            
            Rectangle {
                width: 2
                height: 8
                color: "white"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
            }
            Rectangle {
                width: 4
                height: 2
                color: "white"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: {
                if (root.recording) {
                    window.toggleSelection(root.recording.filePath);
                }
            }
            cursorShape: Qt.PointingHandCursor
        }
    }

    // Border overlay (Highest Z to ensure it's on top of the image)
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: root.isSelected ? "#20FFFFFF" : "transparent"
        border.color: root.isSelected ? Theme.primaryColor : (root.hovered ? Theme.primaryColor : Theme.borderColor)
        border.width: root.isSelected ? 2 : (root.hovered ? 2 : 1)
        z: 10
    }
    
    scale: root.isSelected ? 1.02 : 1.0
    Behavior on scale { NumberAnimation { duration: Theme.animationFast } }

    HoverHandler { id: hoverHandler }

    Behavior on color { ColorAnimation { duration: Theme.animationFast } }
}
