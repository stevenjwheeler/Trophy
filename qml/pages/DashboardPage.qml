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

    function formatSize(bytes) {
        if (!bytes)
            return "0 B";

        var kb = 1024;
        var mb = kb * 1024;
        var gb = mb * 1024;
        if (bytes >= gb)
            return (bytes / gb).toFixed(1) + " GB";

        if (bytes >= mb)
            return (bytes / mb).toFixed(0) + " MB";

        if (bytes >= kb)
            return (bytes / kb).toFixed(0) + " KB";

        return bytes + " B";
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: Theme.spacingMedium

            // Header with status cards
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingMedium

                // Dashboard title
                Label {
                    text: qsTr("Dashboard")
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Font.Bold
                    color: Theme.textColor
                    Layout.alignment: Qt.AlignTop
                }

                Item {
                    Layout.fillWidth: true
                }

                // Compact game status card with button
                Rectangle {
                    Layout.preferredWidth: 320
                    Layout.preferredHeight: 80
                    radius: Theme.radiusMedium
                    color: (recordingManager && recordingManager.isRecording) ? "#E6CF3131" : Theme.cardColor
                    Layout.alignment: Qt.AlignTop
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
                                    width: 10
                                    height: 10
                                    radius: 5
                                    color: (recordingManager && recordingManager.isRecording) ? "white" : ((gameDetector && gameDetector.currentGameName) ? Theme.successColor : Theme.textMutedColor)

                                    SequentialAnimation on opacity {
                                        running: (recordingManager && recordingManager.isRecording)
                                        loops: Animation.Infinite

                                        NumberAnimation {
                                            from: 1
                                            to: 0.4
                                            duration: 800
                                        }

                                        NumberAnimation {
                                            from: 0.4
                                            to: 1
                                            duration: 800
                                        }

                                    }

                                }

                                Label {
                                    text: {
                                        if (recordingManager && recordingManager.isRecording)
                                            return qsTr("RECORDING");

                                        if (gameDetector && gameDetector.currentGameName)
                                            return qsTr("READY");

                                        return qsTr("IDLE");
                                    }
                                    font.pixelSize: 10
                                    font.weight: Font.Black
                                    color: (recordingManager && recordingManager.isRecording) ? "white" : Theme.textSecondaryColor
                                }

                            }

                            Label {
                                Layout.fillWidth: true
                                text: {
                                    if (recordingManager && recordingManager.isRecording)
                                        return recordingManager.currentGameName || qsTr("Active Recording");

                                    return (gameDetector && gameDetector.currentGameName) || qsTr("No game detected");
                                }
                                font.pixelSize: Theme.fontSizeNormal
                                font.weight: Font.Bold
                                color: (recordingManager && recordingManager.isRecording) ? "white" : Theme.textColor
                                elide: Text.ElideRight
                            }

                        }

                        Button {
                            id: recordBtn

                            Layout.preferredWidth: 44
                            Layout.preferredHeight: 44
                            icon.source: (recordingManager && recordingManager.isRecording) ? "qrc:/icons/stop.svg" : "qrc:/icons/record.svg"
                            icon.width: 24
                            icon.height: 24
                            icon.color: (recordingManager && recordingManager.isRecording) ? "white" : (recordBtn.hovered ? "white" : (gameDetector && gameDetector.currentGameName ? Theme.primaryColor : Theme.textMutedColor))
                            flat: true
                            onClicked: {
                                if (recordingManager && recordingManager.isRecording)
                                    recordingManager.stopRecording();
                                else
                                    window.openWindowPicker();
                            }

                            background: Rectangle {
                                radius: 22
                                color: recordBtn.hovered ? ((recordingManager && recordingManager.isRecording) ? "#40FFFFFF" : Theme.primaryColor) : ((recordingManager && recordingManager.isRecording) ? "#20FFFFFF" : "transparent")
                                border.color: ((recordingManager && recordingManager.isRecording) ? "white" : Theme.primaryColor)
                                border.width: 2
                            }

                        }

                    }

                }

                // Compact encoder info
                Rectangle {
                    Layout.preferredWidth: 200
                    Layout.preferredHeight: 80
                    radius: Theme.radiusMedium
                    color: Theme.cardColor
                    Layout.alignment: Qt.AlignTop

                    GridLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacingSmall
                        columns: 2
                        columnSpacing: Theme.spacingSmall
                        rowSpacing: 2

                        Label {
                            text: qsTr("Encoder:")
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textSecondaryColor
                        }

                        Label {
                            text: {
                                if (settingsManager && settingsManager.encoder === "auto") {
                                    var active = (recordingManager && recordingManager.isRecording && recordingManager.activeEncoderName) ? recordingManager.activeEncoderName : (recordingManager ? recordingManager.detectedEncoderName : "");
                                    return active !== "" ? "Auto (" + active + ")" : "Auto";
                                }
                                return settingsManager ? settingsManager.encoder : "";
                            }
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textColor
                            elide: Text.ElideRight
                        }

                        Label {
                            text: qsTr("Bitrate:")
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textSecondaryColor
                        }

                        Label {
                            text: (settingsManager ? settingsManager.videoBitrate : 0) + " kbps"
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textColor
                        }

                        Label {
                            text: qsTr("FPS:")
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textSecondaryColor
                        }

                        Label {
                            text: settingsManager ? settingsManager.fps.toString() : "0"
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textColor
                        }

                        Label {
                            text: qsTr("Resolution:")
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textSecondaryColor
                        }

                        Label {
                            text: settingsManager ? settingsManager.videoResolution : ""
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textColor
                        }

                    }

                }

                // Compact audio levels
                Rectangle {
                    Layout.preferredWidth: 100
                    Layout.preferredHeight: 80
                    radius: Theme.radiusMedium
                    color: "transparent"
                    Layout.alignment: Qt.AlignTop

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 4
                        spacing: 4
                        Layout.alignment: Qt.AlignVCenter

                        AudioLevelMeter {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 30
                            label: qsTr("Game")
                            level: 0
                        }

                        AudioLevelMeter {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 30
                            label: qsTr("Comms")
                            level: 0
                        }

                        AudioLevelMeter {
                            Layout.fillHeight: true
                            Layout.preferredWidth: 30
                            label: qsTr("Mic")
                            level: 0
                        }

                    }

                }

            }

            // Storage usage (Small bar)
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 60
                radius: Theme.radiusMedium
                color: Theme.cardColor

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: Theme.spacingMedium
                    spacing: Theme.spacingLarge

                    ColumnLayout {
                        spacing: 2
                        Layout.preferredWidth: 120
                        Layout.alignment: Qt.AlignVCenter

                        Label {
                            text: qsTr("Storage Usage")
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Font.Medium
                            color: Theme.textSecondaryColor
                        }

                        Label {
                            text: (typeof storageManager !== "undefined" && storageManager) ? (formatSize(storageManager.usedSize) + " / " + formatSize(storageManager.maxSize)) : "0 B / 0 B"
                            font.pixelSize: 10
                            color: Theme.textColor
                            opacity: 0.7
                        }

                    }

                    // Simple Progress Bar
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 10
                        Layout.alignment: Qt.AlignVCenter
                        radius: 5
                        color: Theme.surfaceColor
                        border.color: Theme.borderColor
                        border.width: 1
                        clip: true

                        Rectangle {
                            height: parent.height
                            width: parent.width * ((typeof storageManager !== "undefined" && storageManager && storageManager.maxSize > 0) ? (storageManager.usedSize / storageManager.maxSize) : 0)
                            radius: 5
                            color: {
                                if (typeof storageManager === "undefined" || !storageManager || storageManager.maxSize <= 0)
                                    return Theme.primaryColor;

                                var p = (storageManager.usedSize / storageManager.maxSize) * 100;
                                if (p > 90)
                                    return Theme.errorColor;

                                if (p > 75)
                                    return Theme.warningColor;

                                return Theme.primaryColor;
                            }

                            Behavior on width {
                                NumberAnimation {
                                    duration: 500
                                }

                            }

                        }

                    }

                }

            }

            // Recent recordings
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 300
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingSmall

                    RowLayout {
                        Layout.fillWidth: true

                        Label {
                            text: qsTr("Recent Recordings")
                            font.pixelSize: Theme.fontSizeMedium
                            font.weight: Font.Medium
                            color: Theme.textColor
                        }

                        Item {
                            Layout.fillWidth: true
                        }

                        TrophyButton {
                            text: qsTr("View All")
                            implicitHeight: 26
                            onClicked: window.currentPage = 1
                        }

                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: Theme.spacingMedium

                        ActiveRecordingCard {
                            Layout.preferredWidth: 240
                            Layout.preferredHeight: Theme.recordingCardHeight
                            Layout.alignment: Qt.AlignTop
                            visible: recordingManager && recordingManager.isRecording
                        }

                        ListView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            orientation: ListView.Horizontal
                            spacing: Theme.spacingMedium
                            clip: true
                            model: (recordingManager && recordingManager.isRecording) ? (recordingLibrary ? recordingLibrary.recordings.slice(0, 3) : []) : (recordingLibrary ? recordingLibrary.recordings.slice(0, 4) : [])

                            Label {
                                anchors.centerIn: parent
                                visible: !recordingLibrary || (recordingLibrary.count === 0 && !(recordingManager && recordingManager.isRecording))
                                text: qsTr("No recordings yet")
                                color: Theme.textMutedColor
                            }

                            delegate: RecordingCard {
                                width: 240
                                height: Theme.recordingCardHeight
                                recording: modelData
                                onPlay: {
                                    if (modelData)
                                        Qt.openUrlExternally("file://" + modelData.filePath);

                                }
                                onOpenFolder: {
                                    if (modelData)
                                        window.openFolder(modelData.directory);

                                }
                                onDeleteRequested: {
                                    if (modelData)
                                        window.confirmDelete(modelData.filePath, modelData.fileName);

                                }
                            }

                        }

                    }

                }

            }

        }

    }

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: 12
        topLeftRadius: 26
    }

}
