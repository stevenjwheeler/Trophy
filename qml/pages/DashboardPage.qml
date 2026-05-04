/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../components"
import "../styles"
import "../views"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root

    function formatDuration(seconds) {
        if (!seconds || seconds <= 0)
            return "00:00";

        var m = Math.floor(seconds / 60);
        var s = seconds % 60;
        return `%1:%2`.arg(m).arg(s.toString().padStart(2, '0'));
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

    function scrollToRecording(index) {
        recordingsGrid.positionViewAtIndex(index, GridView.Beginning);
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        spacing: Theme.spacingLarge

        // Left side: Header and Grid
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spacingMedium

            // Header
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                spacing: Theme.spacingMedium

                Label {
                    text: qsTr("Dashboard")
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Font.Bold
                    color: Theme.textColor
                }

                Item { Layout.fillWidth: true }
            }

            // Grid Section
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.spacingSmall

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Theme.radiusMedium
                    color: Theme.recordingGridBackgroundColor

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacingMedium
                        spacing: Theme.spacingSmall

                        // Header
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                            spacing: Theme.spacingMedium

                            Label {
                                text: qsTr("Recent Captures")
                                font.pixelSize: Theme.fontSizeMedium
                                font.weight: Font.Medium
                                color: Theme.textColor
                            }

                            Item { Layout.fillWidth: true }

                            RowLayout {
                                spacing: Theme.spacingSmall
                                visible: window.selectedRecordings && window.selectedRecordings.length > 0

                                TrophyButton {
                                    text: qsTr("Export Selected (%1)").arg(window.selectedRecordings.length)
                                    icon.source: "qrc:/icons/folder.svg"
                                    implicitHeight: 28
                                    onClicked: window.exportSelected()
                                }

                                TrophyButton {
                                    text: qsTr("Delete Selected")
                                    icon.source: "qrc:/icons/trash.svg"
                                    implicitHeight: 28
                                    baseColor: Theme.errorColor
                                    onClicked: window.deleteSelected()
                                }
                                
                                TrophyButton {
                                    text: qsTr("Cancel")
                                    implicitHeight: 28
                                    onClicked: window.clearSelection()
                                }
                            }
                        }

                        RecordingGridView {
                            id: recordingsGrid
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            
                            skip: 0
                            leftMargin: Theme.spacingSmall
                            topMargin: Theme.spacingSmall
                            bottomMargin: Theme.spacingSmall
                            
                            // Add margin on the right so your grid content doesn't get covered by the scrollbar
                            rightMargin: Theme.spacingSmall

                            // Attach the custom scrollbar directly to the GridView
                            ScrollBar.vertical: ScrollBar {
                                id: customVBar
                                active: hovered || pressed
                                width: 8
                                
                                // The handle
                                contentItem: Rectangle {
                                    implicitWidth: 4
                                    implicitHeight: 40 // Minimum height of the thumb
                                    radius: 16
                                    color: customVBar.pressed ? Theme.highlightedColor : (customVBar.hovered ? Theme.highlightedColor : Theme.secondaryColor)

                                    // Optional: fade out when not interacting
                                    opacity: customVBar.active ? 1.0 : 0.2
                                    Behavior on opacity { NumberAnimation { duration: 200 } }
                                
                                }

                                background: Rectangle {
                                    color: "transparent"
                                }
                            }
                        }
                    }
                }
            }
        }

        // Right sidebar
        ColumnLayout {
            Layout.preferredWidth: parent.width * 0.3
            Layout.maximumWidth: 320
            Layout.minimumWidth: 280
            Layout.fillHeight: true
            spacing: Theme.spacingMedium

            // Game status card
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                radius: Theme.radiusMedium
                color: (recordingManager && recordingManager.isRecording) ? "#E6CF3131" : Theme.cardColor
                border.color: (recordingManager && recordingManager.isRecording) ? "#FF5F5F" : "transparent"
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingMedium

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4

                        RowLayout {
                            spacing: 8
                            Rectangle {
                                width: 10; height: 10; radius: 5
                                color: (recordingManager && recordingManager.isRecording) ? "white" : ((gameDetector && gameDetector.currentGameName) ? Theme.successColor : Theme.textMutedColor)
                                SequentialAnimation on opacity {
                                    running: (recordingManager && recordingManager.isRecording)
                                    loops: Animation.Infinite
                                    NumberAnimation { from: 1; to: 0.4; duration: 800 }
                                    NumberAnimation { from: 0.4; to: 1; duration: 800 }
                                }
                            }
                            Label {
                                text: (recordingManager && recordingManager.isRecording) ? qsTr("RECORDING") : ((gameDetector && gameDetector.currentGameName) ? qsTr("READY") : qsTr("IDLE"))
                                font.pixelSize: 10; font.weight: Font.Black
                                color: (recordingManager && recordingManager.isRecording) ? "white" : Theme.textSecondaryColor
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: (recordingManager && recordingManager.isRecording) ? (recordingManager.currentGameName || qsTr("Active Recording")) : ((gameDetector && gameDetector.currentGameName) || qsTr("No game detected"))
                            font.pixelSize: Theme.fontSizeNormal; font.weight: Font.Bold
                            color: (recordingManager && recordingManager.isRecording) ? "white" : Theme.textColor
                            elide: Text.ElideRight
                        }
                    }

                    Button {
                        id: recordBtn
                        Layout.preferredWidth: 44; Layout.preferredHeight: 44
                        icon.source: (recordingManager && recordingManager.isRecording) ? "qrc:/icons/stop.svg" : "qrc:/icons/record.svg"
                        icon.width: 24; icon.height: 24
                        icon.color: (recordingManager && recordingManager.isRecording) ? "white" : (recordBtn.hovered ? "white" : (gameDetector && gameDetector.currentGameName ? Theme.primaryColor : Theme.textMutedColor))
                        flat: true
                        onClicked: (recordingManager && recordingManager.isRecording) ? recordingManager.stopRecording() : window.openWindowPicker()
                        background: Rectangle {
                            radius: 22
                            color: recordBtn.hovered ? ((recordingManager && recordingManager.isRecording) ? "#40FFFFFF" : Theme.primaryColor) : ((recordingManager && recordingManager.isRecording) ? "#20FFFFFF" : "transparent")
                            border.color: ((recordingManager && recordingManager.isRecording) ? "white" : Theme.primaryColor)
                            border.width: 2
                        }
                    }
                }
            }

            // Storage Usage
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 100
                radius: Theme.radiusMedium
                color: Theme.cardColor
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: Theme.spacingMedium; spacing: Theme.spacingSmall
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Storage Usage"); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                        Item { Layout.fillWidth: true }
                        Label { text: qsTr("%n recording(s)", "", (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.count : 0); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                    }
                    Label {
                        text: (typeof storageManager !== "undefined" && storageManager) ? (storageManager.maxSize > 0 ? (formatSize(storageManager.usedSize) + " / " + formatSize(storageManager.maxSize)) : (formatSize(storageManager.usedSize) + " / " + qsTr("Unlimited"))) : "0 B / 0 B"
                        font.pixelSize: 10; color: Theme.textColor; opacity: 0.7
                    }
                    Rectangle {
                        visible: (typeof storageManager !== "undefined" && storageManager && storageManager.maxSize > 0)
                        Layout.fillWidth: true; Layout.preferredHeight: 10; radius: 5; color: Theme.surfaceColor; border.color: Theme.borderColor; border.width: 1; clip: true
                        Rectangle {
                            height: parent.height
                            width: parent.width * ((typeof storageManager !== "undefined" && storageManager && storageManager.maxSize > 0) ? (storageManager.usedSize / storageManager.maxSize) : 0)
                            radius: 5; color: Theme.primaryColor
                            Behavior on width { NumberAnimation { duration: 500 } }
                        }
                    }
                }
            }

            // Recording settings box
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 140
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingSmall

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: qsTr("Recording Settings")
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Font.Medium
                            color: Theme.textSecondaryColor
                        }
                        Item { Layout.fillWidth: true }
                        TrophyButton {
                            text: qsTr("Edit")
                            implicitHeight: 28
                            implicitWidth: 70
                            radius: 14
                            onClicked: window.currentPage = 2
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Theme.spacingMedium
                        rowSpacing: Theme.spacingSmall

                        // Encoder
                        ColumnLayout {
                            spacing: 2
                            Label { text: qsTr("Encoder"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                            Label {
                                text: {
                                    if (settingsManager && settingsManager.encoder === "auto") {
                                         var active = (recordingManager && recordingManager.isRecording && recordingManager.activeEncoderName) ? recordingManager.activeEncoderName : (recordingManager ? recordingManager.detectedEncoderName : "");
                                         return active || (settingsManager ? settingsManager.encoderDisplayName : "");
                                     }
                                    return settingsManager ? settingsManager.encoderDisplayName : "";
                                }
                                font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor; elide: Text.ElideRight
                            }
                        }

                        // Resolution
                        ColumnLayout {
                            spacing: 2
                            Label { text: qsTr("Resolution"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                            Label {
                                text: settingsManager.resolution === "auto" ? qsTr("Auto (%1x%2)").arg(settingsManager.autoResolutionWidth).arg(settingsManager.autoResolutionHeight) : settingsManager.resolution
                                font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor
                            }
                        }

                        // Quality
                        ColumnLayout {
                            spacing: 2
                            Label { text: qsTr("Quality"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                            Label { text: (settingsManager ? settingsManager.videoBitrate : 0) + " kbps"; font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor }
                        }

                        // FPS
                        ColumnLayout {
                            spacing: 2
                            Label { text: qsTr("FPS"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                            Label { text: settingsManager ? settingsManager.fps.toString() : "0"; font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor }
                        }
                    }
                }
            }

            // Recent Activity
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: Theme.radiusMedium
                color: Theme.cardColor
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: Theme.spacingMedium; spacing: Theme.spacingSmall
                    Label { text: qsTr("Recent Activity"); font.pixelSize: Theme.fontSizeSmall; font.weight: Font.Medium; color: Theme.textSecondaryColor }
                    ScrollView {
                        Layout.fillWidth: true; Layout.fillHeight: true
                        ColumnLayout {
                            width: parent.width; spacing: Theme.spacingSmall
                            Repeater {
                                model: ListModel {
                                    id: activityModel
                                    Component.onCompleted: updateActivity()
                                    function updateActivity() {
                                        clear()
                                        if (recordingManager && recordingManager.isRecording) {
                                            append({ type: "recording_started", game: recordingManager.currentGameName || "Unknown Game", time: "Now", size: "", duration: "" })
                                        }
                                        if (recordingLibrary && recordingLibrary.count > 0) {
                                            for (var i = 0; i < Math.min(recordingLibrary.count, 5); i++) {
                                                var clip = recordingLibrary.get(i)
                                                if (clip) append({ type: "recording_completed", game: clip.gameName || "Unknown Game", time: getRelativeTime(clip.date), size: formatSize(clip.size), duration: formatDuration(clip.duration) })
                                            }
                                        }
                                    }
                                    function getRelativeTime(d) {
                                        var diffMins = Math.floor((new Date() - new Date(d)) / 60000)
                                        if (diffMins < 1) return "Just now"
                                        if (diffMins < 60) return diffMins + " mins ago"
                                        return Math.floor(diffMins / 60) + " hours ago"
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true; Layout.preferredHeight: 50; radius: 6; color: Theme.surfaceColor
                                    RowLayout {
                                        anchors.fill: parent; anchors.margins: Theme.spacingSmall
                                        Rectangle { width: 8; height: 8; radius: 4; color: model.type === "recording_completed" ? Theme.successColor : Theme.errorColor }
                                        ColumnLayout {
                                            Layout.fillWidth: true; spacing: 1
                                            Label { text: model.type === "recording_completed" ? qsTr("Completed: %1").arg(model.game) : qsTr("Started: %1").arg(model.game); font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor; elide: Text.ElideRight }
                                            Label { text: model.time; font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Timer {
        interval: 30000 // 30 seconds
        running: true
        repeat: true
        onTriggered: activityModel.updateActivity()
    }

    Connections { target: recordingLibrary; function onCountChanged() { activityModel.updateActivity() } }
    Connections { target: recordingManager; function onIsRecordingChanged() { activityModel.updateActivity() } }

    background: Rectangle { color: Theme.backgroundColor; bottomRightRadius: 12; topLeftRadius: 26 }
}
