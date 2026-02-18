/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../controls"
import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Page {
    id: root

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        contentHeight: mainStack.height
        anchors.topMargin: Theme.spacingLarge
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        clip: true

        ColumnLayout {
            id: mainStack

            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width, 800)
            spacing: Theme.spacingLarge

            Label {
                text: qsTr("Settings")
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Font.Bold
                color: Theme.textColor
            }

            ColumnLayout {
                id: settingsList
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.spacingLarge

                property string openSectionTitle: qsTr("")

                // --- VIDEO SECTION ---
                SettingsSection {

                    title: qsTr("Video Recording")
                    collapsed: settingsList.openSectionTitle !== title
                    onToggleRequested: {
                        if (settingsList.openSectionTitle === title) {
                            settingsList.openSectionTitle = ""; // Close if already open
                        } else {
                            settingsList.openSectionTitle = title; // Open this, close others
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: warningContents.implicitHeight + Theme.spacingSmall * 2
                        radius: Theme.radiusSmall
                        color: Theme.withOpacity(Theme.warningColor, 0.1)
                        border.width: 1
                        border.color: Theme.withOpacity(Theme.warningColor, 0.3)
                        visible: recordingManager && recordingManager.isRecording

                        RowLayout {
                            id: warningContents

                            anchors.fill: parent
                            anchors.margins: Theme.spacingSmall

                            Label {
                                text: "⚠️"
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Recording in progress. To change these settings, stop recording.")
                                color: Theme.textColor
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                            }

                        }

                    }

                    SettingItem {
                        label: qsTr("Video Encoder")
                        helpText: qsTr("Choose 'Auto' to let Trophy pick the best hardware encoder for your system (NVIDIA NVENC or AMD/Intel VA-API). Use 'x264' only as a fallback.")

                        TrophyComboBox {
                            id: encoderBox

                            property var values: ["auto", "nvenc_h264", "nvenc_hevc", "vaapi_h264", "vaapi_hevc", "x264"]

                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
                            Layout.fillWidth: true
                            model: ["Auto (Recommended)", "NVENC H.264", "NVENC HEVC", "VA-API H.264", "VA-API HEVC", "x264 Software"]

                            Binding {
                                target: encoderBox
                                property: "currentIndex"
                                value: {
                                    if (!settingsManager)
                                        return 0;

                                    let index = encoderBox.values.indexOf(settingsManager.encoder);
                                    return index !== -1 ? index : 0;
                                }
                            }

                            onCurrentIndexChanged: {
                                if (!settingsManager)
                                    return ;

                                if (app.isEncoderAvailable(values[currentIndex])) {
                                    settingsManager.encoder = values[currentIndex];
                                } else {
                                    // delegateModel already changed currentIndex before onClicked
                                    // could intercept — revert to the last valid selection.
                                    let savedIdx = values.indexOf(settingsManager.encoder);
                                    // Re-evaluate through binding instead of direct assignment
                                }
                            }

                            delegate: ItemDelegate {
                                id: encDelegate

                                readonly property bool isAvailable: app.isEncoderAvailable(encoderBox.values[index])

                                width: encoderBox.width - 4
                                height: 32
                                hoverEnabled: isAvailable
                                onClicked: {
                                    if (!isAvailable)
                                        return ;

                                    encoderBox.currentIndex = index;
                                    encoderBox.popup.close();
                                }

                                contentItem: RowLayout {
                                    spacing: Theme.spacingSmall

                                    Label {
                                        text: modelData
                                        color: !encDelegate.isAvailable ? Theme.textMutedColor : (encDelegate.highlighted || encDelegate.hovered) ? encoderBox._accent : encoderBox._text
                                        font: encoderBox.font
                                        elide: Text.ElideRight
                                        verticalAlignment: Text.AlignVCenter
                                        Layout.fillWidth: true
                                    }

                                    Label {
                                        text: qsTr("Not found")
                                        color: Theme.textMutedColor
                                        font.pixelSize: Theme.fontSizeSmall
                                        visible: !encDelegate.isAvailable
                                    }

                                }

                                background: Rectangle {
                                    color: encDelegate.isAvailable && encDelegate.hovered ? Qt.rgba(encoderBox._accent.r, encoderBox._accent.g, encoderBox._accent.b, 0.1) : encDelegate.highlighted ? Qt.rgba(encoderBox._accent.r, encoderBox._accent.g, encoderBox._accent.b, 0.05) : "transparent"
                                    radius: Theme.radiusSmall - 2
                                }

                            }

                        }

                    }

                    SettingItem {
                        label: qsTr("Video Bitrate")
                        helpText: qsTr("Controls video quality. 10,000-15,000 kbps is great for 1080p. Use 20,000+ kbps for 1440p/4K recording.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium

                            TrophySlider {
                                id: bitrateSlider

                                enabled: !(recordingManager && recordingManager.isRecording)
                                opacity: enabled ? 1 : 0.5
                                Layout.fillWidth: true
                                from: 2000
                                to: 50000
                                stepSize: 1000
                                value: settingsManager ? settingsManager.videoBitrate : 15000
                                onMoved: {
                                    if (settingsManager)
                                        settingsManager.videoBitrate = value;

                                }
                            }

                            Label {
                                text: bitrateSlider.value.toFixed(0) + " kbps"
                                color: Theme.textColor
                                font.weight: Font.Medium
                                Layout.preferredWidth: 80
                            }

                        }

                    }

                    SettingItem {
                        label: qsTr("Frame Rate")
                        helpText: qsTr("Higher FPS looks smoother but increases file size. 60 FPS is the standard for gameplay recordings.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            Repeater {
                                model: [30, 60, 120, 144]

                                TrophyRadioButton {
                                    enabled: !(recordingManager && recordingManager.isRecording)
                                    opacity: enabled ? 1 : 0.5
                                    text: modelData + " FPS"
                                    checked: settingsManager ? settingsManager.fps === modelData : false
                                    onClicked: {
                                        if (settingsManager)
                                            settingsManager.fps = modelData;

                                    }
                                }

                            }

                        }

                    }

                    SettingItem {
                        label: qsTr("Resolution")
                        helpText: qsTr("Native captures your games full resolution. Downscaling (e.g. to 720p) helps performance if your game is demanding.")

                        TrophyComboBox {
                            id: resolutionBox
                            property var values: ["native", "720x480", "1280x720", "1920x1080", "2560x1440", "3840x2160"]

                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
                            Layout.fillWidth: true
                            model: ["Native (Game Resolution)", "480p", "720p", "1080p", "1440p", "4K"]

                            Binding {
                                target: resolutionBox
                                property: "currentIndex"
                                value: {
                                    if (!settingsManager)
                                        return 0;

                                    let index = resolutionBox.values.indexOf(settingsManager.videoResolution);
                                    return index !== -1 ? index : 0;
                                }
                            }

                            onCurrentIndexChanged: {
                                if (settingsManager)
                                    settingsManager.videoResolution = values[currentIndex];

                            }
                        }

                    }

                    SettingItem {
    label: qsTr("Container Format")
    helpText: qsTr("The container format determines how your video is stored.")

    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.spacingSmall

        // --- Option 1: MKV ---
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: mkvLayout.implicitHeight + (Theme.spacingMedium * 2)
            color: "transparent"
            radius: Theme.radiusSmall
            border.width: 1
            border.color: settingsManager && settingsManager.containerFormat === "mkv"
                          ? Theme.highlightedColor 
                          : Theme.withOpacity(Theme.textColor, 0.1)

            RowLayout {
                id: mkvLayout
                anchors.fill: parent
                anchors.margins: Theme.spacingMedium
                spacing: Theme.spacingSmall

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: qsTr("MKV (Recommended)")
                        color: Theme.textColor
                        font.weight: Font.Medium
                    }
                    Label {
                        text: qsTr("Crash-resistant format. Your recording is safe even if the power cuts out or the app crashes.")
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                TrophyRadioButton {
                    enabled: !(recordingManager && recordingManager.isRecording)
                    checked: settingsManager ? settingsManager.containerFormat === "mkv" : true
                    onClicked: if (settingsManager) settingsManager.containerFormat = "mkv"
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: !(recordingManager && recordingManager.isRecording)
                onClicked: if (settingsManager) settingsManager.containerFormat = "mkv"
            }
        }

        // --- Option 2: MP4 ---
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: mp4Layout.implicitHeight + (Theme.spacingMedium * 2)
            color: "transparent"
            radius: Theme.radiusSmall
            border.width: 1
            border.color: settingsManager && settingsManager.containerFormat === "mp4"
                          ? Theme.warningColor // Use warning color since MP4 is risky
                          : Theme.withOpacity(Theme.textColor, 0.1)

            RowLayout {
                id: mp4Layout
                anchors.fill: parent
                anchors.margins: Theme.spacingMedium
                spacing: Theme.spacingSmall

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        text: qsTr("MP4")
                        color: Theme.textColor
                        font.weight: Font.Medium
                    }
                    Label {
                        text: qsTr("Widely compatible but fragile. If recording stops unexpectedly, the entire file may be corrupted.")
                        color: settingsManager && settingsManager.containerFormat === "mp4" 
                               ? Theme.warningColor 
                               : Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                TrophyRadioButton {
                    enabled: !(recordingManager && recordingManager.isRecording)
                    checked: settingsManager ? settingsManager.containerFormat === "mp4" : false
                    onClicked: if (settingsManager) settingsManager.containerFormat = "mp4"
                }
            }

            MouseArea {
                anchors.fill: parent
                enabled: !(recordingManager && recordingManager.isRecording)
                onClicked: if (settingsManager) settingsManager.containerFormat = "mp4"
            }
        }
    }
}

                    SettingItem {
                        label: qsTr("Encoder Preset")
                        helpText: qsTr("Controls how much CPU/GPU power is used. 'Fast' is a good balance for gaming.")

                        TrophyComboBox {
                            id: presetBox
                            property var values: ["ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow"]

                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
                            Layout.fillWidth: true
                            model: ["Ultrafast", "Superfast", "Very Fast", "Faster", "Fast", "Medium", "Slow"]

                            Binding {
                                target: presetBox
                                property: "currentIndex"
                                value: {
                                    if (!settingsManager)
                                        return 0;

                                    let index = presetBox.values.indexOf(settingsManager.encoderPreset);
                                    return index !== -1 ? index : 0;
                                }
                            }

                            onCurrentIndexChanged: {
                                if (settingsManager)
                                    settingsManager.encoderPreset = values[currentIndex];

                            }
                        }

                    }

                }

                // --- AUDIO SECTION ---
                SettingsSection {
                    title: qsTr("Audio Recording")
                    collapsed: settingsList.openSectionTitle !== title
                    onToggleRequested: {
                        if (settingsList.openSectionTitle === title) {
                            settingsList.openSectionTitle = ""; // Close if already open
                        } else {
                            settingsList.openSectionTitle = title; // Open this, close others
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: audioWarningContents.implicitHeight + Theme.spacingSmall * 2
                        radius: Theme.radiusSmall
                        color: Theme.withOpacity(Theme.warningColor, 0.1)
                        border.width: 1
                        border.color: Theme.withOpacity(Theme.warningColor, 0.3)
                        visible: recordingManager && recordingManager.isRecording

                        RowLayout {
                            id: audioWarningContents
                            anchors.fill: parent
                            anchors.margins: Theme.spacingSmall
                            Label { text: "⚠️" }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Recording in progress. To change these settings, stop recording.")
                                color: Theme.textColor
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("Audio Mode")

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            // --- Option 1: Game Mode ---
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: gameModeLayout.implicitHeight + (Theme.spacingMedium * 2)
                                color: "transparent"
                                radius: Theme.radiusSmall
                                border.width: 1
                                border.color: settingsManager && settingsManager.audioMode === "game"
                                              ? Theme.highlightedColor 
                                              : Theme.withOpacity(Theme.textColor, 0.1)

                                RowLayout {
                                    id: gameModeLayout
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingMedium
                                    spacing: Theme.spacingSmall

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            text: qsTr("Game Mode")
                                            color: Theme.textColor
                                            font.weight: Font.Medium
                                        }
                                        Label {
                                            text: qsTr("Focused capture of game window audio and voice apps.")
                                            color: Theme.textMutedColor
                                            font.pixelSize: Theme.fontSizeSmall
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                    }

                                    TrophyRadioButton {
                                        enabled: !(recordingManager && recordingManager.isRecording)
                                        checked: settingsManager ? settingsManager.audioMode === "game" : true
                                        onClicked: if (settingsManager) settingsManager.audioMode = "game"
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !(recordingManager && recordingManager.isRecording)
                                    onClicked: if (settingsManager) settingsManager.audioMode = "game"
                                }
                            }

                            // --- Option 2: Desktop Mode ---
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: desktopModeLayout.implicitHeight + (Theme.spacingMedium * 2)
                                color: "transparent"
                                radius: Theme.radiusSmall
                                border.width: 1
                                border.color: settingsManager && settingsManager.audioMode === "desktop"
                                              ? Theme.highlightedColor 
                                              : Theme.withOpacity(Theme.textColor, 0.1)

                                RowLayout {
                                    id: desktopModeLayout
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingMedium
                                    spacing: Theme.spacingSmall

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            text: qsTr("Desktop Mode")
                                            color: Theme.textColor
                                            font.weight: Font.Medium
                                        }
                                        Label {
                                            text: qsTr("Captures everything you hear on your system.")
                                            color: Theme.textMutedColor
                                            font.pixelSize: Theme.fontSizeSmall
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                    }

                                    TrophyRadioButton {
                                        enabled: !(recordingManager && recordingManager.isRecording)
                                        checked: settingsManager ? settingsManager.audioMode === "desktop" : false
                                        onClicked: if (settingsManager) settingsManager.audioMode = "desktop"
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    enabled: !(recordingManager && recordingManager.isRecording)
                                    onClicked: if (settingsManager) settingsManager.audioMode = "desktop"
                                }
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("Audio Track Configuration")

                        ColumnLayout {
                            Layout.fillWidth: true

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: singleTrackLayout.implicitHeight + (Theme.spacingMedium * 2)
                                color: "transparent"
                                radius: Theme.radiusSmall
                                border.width: 1
                                border.color: settingsManager && !settingsManager.separateAudioTracks
                                              ? Theme.highlightedColor
                                              : Theme.withOpacity(Theme.textColor, 0.1)

                                RowLayout {
                                    id: singleTrackLayout
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingMedium
                                    spacing: Theme.spacingSmall

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            text: qsTr("Single Track (Mixed)")
                                            color: Theme.textColor
                                            font.weight: Font.Medium
                                        }
                                        Label {
                                            text: {
                                                let mode = settingsManager ? settingsManager.audioMode : "game"
                                                if (mode === "desktop")
                                                    return qsTr("System audio and microphone mixed together into one track.")
                                                else
                                                    return qsTr("Game, voice chat and microphone all mixed together into one track.")
                                            }
                                            color: Theme.textMutedColor
                                            font.pixelSize: Theme.fontSizeSmall
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                    }

                                    TrophyRadioButton {
                                        enabled: !(recordingManager && recordingManager.isRecording)
                                        checked: settingsManager ? !settingsManager.separateAudioTracks : true
                                        onClicked: if (settingsManager) settingsManager.separateAudioTracks = false
                                    }
                                }

                                // Make the whole rectangle clickable
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: if (enabled) settingsManager.separateAudioTracks = false
                                    enabled: !(recordingManager && recordingManager.isRecording)
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: multiTrackLayout.implicitHeight + (Theme.spacingMedium * 2)
                                color: "transparent"
                                radius: Theme.radiusSmall
                                border.width: 1
                                border.color: settingsManager && settingsManager.separateAudioTracks
                                              ? Theme.highlightedColor
                                              : Theme.withOpacity(Theme.textColor, 0.1)

                                RowLayout {
                                    id: multiTrackLayout
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingMedium
                                    spacing: Theme.spacingSmall

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            text: qsTr("Separate Tracks")
                                            color: Theme.textColor
                                            font.weight: Font.Medium
                                        }
                                        Label {
                                            text: {
                                                let mode = settingsManager ? settingsManager.audioMode : "game"
                                                if (mode === "desktop")
                                                    return qsTr("System audio and microphone on separate tracks.\n⚠ In media players, you must switch audio tracks to hear both.")
                                                else
                                                    return qsTr("Game, voice chat and microphone on separate tracks.\n⚠ In media players, you must switch audio tracks to hear all audio.")
                                            }
                                            color: Theme.textMutedColor
                                            font.pixelSize: Theme.fontSizeSmall
                                            wrapMode: Text.WordWrap
                                            Layout.fillWidth: true
                                        }
                                    }

                                    TrophyRadioButton {
                                        enabled: !(recordingManager && recordingManager.isRecording)
                                        checked: settingsManager ? settingsManager.separateAudioTracks : false
                                        onClicked: if (settingsManager) settingsManager.separateAudioTracks = true
                                    }
                                }

                                // Make the whole rectangle clickable
                                MouseArea {
                                    anchors.fill: parent
                                    onClicked: if (enabled) settingsManager.separateAudioTracks = true
                                    enabled: !(recordingManager && recordingManager.isRecording)
                                }
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("Audio Bitrate")
                        helpText: qsTr("192 kbps is CD quality and recommended for most users.")
                        RowLayout {
                            spacing: Theme.spacingSmall
                            Repeater {
                                model: [128, 192, 256, 320]
                                TrophyRadioButton {
                                    enabled: !(recordingManager && recordingManager.isRecording)
                                    text: modelData + " kbps"
                                    checked: settingsManager ? settingsManager.audioBitrate === modelData : false
                                    onClicked: if (settingsManager) settingsManager.audioBitrate = modelData
                                }
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("System Audio Device")
                        helpText: qsTr("Where your sound comes from.")
                        visible: settingsManager ? settingsManager.audioMode === "desktop" : false

                        TrophyComboBox {
                            id: gameAudioBox
                            Layout.fillWidth: true
                            enabled: !(recordingManager && recordingManager.isRecording)
                            property var devices: audioDeviceEnumerator ? audioDeviceEnumerator.outputDevices : []
                            
                            // FIXED: Use a clean expression instead of return block
                            model: {
                                let names = ["Default Output"];
                                for (let d of devices) { if (d.id !== "default") names.push(d.name); }
                                names; // The last statement is the value in QML
                            }

                            Binding {
                                target: gameAudioBox
                                property: "currentIndex"
                                // FIXED: Use a self-invoking function for complex logic
                                value: (function() {
                                    if (!settingsManager || !audioDeviceEnumerator) return 0;
                                    let devs = audioDeviceEnumerator.outputDevices;
                                    let saved = settingsManager.gameAudioDevice;
                                    for (let i = 0; i < devs.length; i++) { if (devs[i].id === saved) return i; }
                                    return 0;
                                })()
                            }

                            onActivated: {
                                if (settingsManager && audioDeviceEnumerator && currentIndex < devices.length) {
                                    settingsManager.gameAudioDevice = devices[currentIndex].id;
                                }
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("Microphone")
                        helpText: qsTr("Your voice input device.")

                        TrophyComboBox {
                            id: microphoneBox
                            Layout.fillWidth: true
                            enabled: !(recordingManager && recordingManager.isRecording)
                            property var devices: audioDeviceEnumerator ? audioDeviceEnumerator.inputDevices : []

                            model: {
                                let names = ["Default Input"];
                                for (let d of devices) { if (d.id !== "default") names.push(d.name); }
                                names;
                            }

                            Binding {
                                target: microphoneBox
                                property: "currentIndex"
                                value: (function() {
                                    if (!settingsManager || !audioDeviceEnumerator) return 0;
                                    let devs = audioDeviceEnumerator.inputDevices;
                                    let saved = settingsManager.microphoneDevice;
                                    for (let i = 0; i < devs.length; i++) { if (devs[i].id === saved) return i; }
                                    return 0;
                                })()
                            }

                            onActivated: {
                                if (settingsManager && audioDeviceEnumerator && currentIndex < devices.length) {
                                    settingsManager.microphoneDevice = devices[currentIndex].id;
                                }
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("Voice Chat App")
                        helpText: qsTr("Process name (e.g. 'discord') for dedicated capture.")
                        visible: settingsManager ? settingsManager.audioMode === "game" : true

                        TrophyTextField {
                            Layout.fillWidth: true
                            text: settingsManager ? settingsManager.voiceChatApp : ""
                            placeholderText: qsTr("e.g., discord, teamspeak")
                            onEditingFinished: if (settingsManager) settingsManager.voiceChatApp = text
                        }
                    }
                }

                // --- STORAGE SECTION ---
                SettingsSection {
                    title: qsTr("Storage Management")
                    collapsed: settingsList.openSectionTitle !== title
                    onToggleRequested: {
                        if (settingsList.openSectionTitle === title) {
                            settingsList.openSectionTitle = ""; // Close if already open
                        } else {
                            settingsList.openSectionTitle = title; // Open this, close others
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: storageWarningContents.implicitHeight + Theme.spacingSmall * 2
                        radius: Theme.radiusSmall
                        color: Theme.withOpacity(Theme.warningColor, 0.1)
                        border.width: 1
                        border.color: Theme.withOpacity(Theme.warningColor, 0.3)
                        visible: recordingManager && recordingManager.isRecording

                        RowLayout {
                            id: storageWarningContents

                            anchors.fill: parent
                            anchors.margins: Theme.spacingSmall

                            Label {
                                text: "⚠️"
                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Recording in progress. These settings will not take effect until the next recording.")
                                color: Theme.textColor
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                            }

                        }

                    }

                    SettingItem {
                        label: qsTr("Recordings Folder")
                        helpText: qsTr("Where your recordings are saved.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            TrophyTextField {
                                id: recordingsPathField

                                Layout.fillWidth: true
                                text: settingsManager ? settingsManager.recordingsPath : ""
                                readOnly: true
                            }

                            TrophyButton {
                                text: qsTr("Browse")
                                onClicked: folderDialog.open()
                            }

                        }

                    }

                    SettingItem {
                        label: qsTr("Storage Limit")
                        helpText: qsTr("Trophy will delete the oldest recordings when you exceed this limit if Auto-Cleanup is enabled, otherwise recording will stop when the limit is reached.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium

                            TrophySlider {
                                id: storageSlider

                                Layout.fillWidth: true
                                from: 10
                                to: 500
                                stepSize: 1
                                value: settingsManager ? settingsManager.maxStorageSize / (1024 * 1024 * 1024) : 50
                                onMoved: {
                                    if (settingsManager)
                                        settingsManager.maxStorageSize = value * 1024 * 1024 * 1024;

                                }
                            }

                            Label {
                                text: storageSlider.value.toFixed(0) + " GB"
                                color: Theme.textColor
                                font.weight: Font.Medium
                                Layout.preferredWidth: 70
                            }

                        }

                    }

                    SettingItem {
                        label: qsTr("Auto-Cleanup")
                        helpText: qsTr("Enable this to automatically delete the oldest recordings when you exceed the storage limit.")

                        TrophySwitch {
                            checked: settingsManager ? settingsManager.autoDeleteOldest : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.autoDeleteOldest = checked;

                            }
                        }

                    }

                }

                // --- GAME DETECTION ---
                SettingsSection {
                    title: qsTr("Game Detection")
                    collapsed: settingsList.openSectionTitle !== title
                    onToggleRequested: {
                        if (settingsList.openSectionTitle === title) {
                            settingsList.openSectionTitle = ""; // Close if already open
                        } else {
                            settingsList.openSectionTitle = title; // Open this, close others
                        }
                    }

                    SettingItem {
                        label: qsTr("Auto-detect Games")
                        helpText: qsTr("Trophy will try to detect when you launch a game.")

                        TrophySwitch {
                            checked: settingsManager ? settingsManager.autoDetectGames : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.autoDetectGames = checked;

                            }
                        }

                    }

                    SettingItem {
                        label: qsTr("Auto-record")
                        helpText: qsTr("Automatically start recording the full game session the moment a game is detected. Never miss a moment!")

                        TrophySwitch {
                            checked: settingsManager ? settingsManager.autoRecordEnabled : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.autoRecordEnabled = checked;

                            }
                        }

                    }

                }

            }

            // --- ACTIONS ---
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingMedium
                Layout.topMargin: Theme.spacingMedium

                Item {
                    Layout.fillWidth: true
                }

                TrophyButton {
                    text: qsTr("Reset to Defaults")
                    onClicked: resetConfirmDialog.open()
                }

            }

            Item {
                Layout.preferredHeight: Theme.spacingLarge
            }

        }

    }

    // --- COMPONENTS ---
    component SettingsSection: Rectangle {
        id: sectionRoot
        property string title: ""
        property bool collapsed: true
        default property alias content: contentLayout.data

        signal toggleRequested()

        Layout.fillWidth: true
        implicitHeight: collapsed ? headerLayout.height + (Theme.spacingMedium * 2) : sectionLayout.implicitHeight + Theme.spacingMedium * 2
        radius: Theme.radiusMedium
        color: Theme.cardColor
        clip: true

        ColumnLayout {
            id: sectionLayout

            anchors.fill: parent
            anchors.margins: Theme.spacingMedium
            spacing: Theme.spacingMedium

            MouseArea {
                id: headerLayout

                Layout.fillWidth: true
                Layout.preferredHeight: headerText.implicitHeight + Theme.spacingSmall
                cursorShape: Qt.PointingHandCursor
                onClicked: sectionRoot.toggleRequested()

                RowLayout {
                    anchors.fill: parent

                    Label {
                        id: headerText

                        text: title
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.Medium
                        color: Theme.textColor
                        Layout.fillWidth: true
                    }

                    Label {
                        text: "▼"
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.textMutedColor
                        rotation: collapsed ? 0 : 180

                        Behavior on rotation {
                            NumberAnimation {
                                duration: 200
                            }

                        }

                    }

                }

            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.dividerColor
                visible: !collapsed
            }

            ColumnLayout {
                id: contentLayout

                Layout.fillWidth: true
                spacing: Theme.spacingLarge
                visible: !collapsed
                opacity: collapsed ? 0 : 1

                Behavior on opacity {
                    NumberAnimation {
                        duration: 150
                    }

                }

            }

        }

        Behavior on implicitHeight {
            NumberAnimation {
                duration: 200
                easing.type: Easing.InOutQuad
            }

        }

    }

    // --- DIALOGS ---
    FolderDialog {
        id: folderDialog

        title: qsTr("Select Recordings Folder")
        onAccepted: {
            if (settingsManager)
                settingsManager.recordingsPath = selectedFolder.toString().replace("file://", "");

        }
    }

    TrophyDialog {
        id: resetConfirmDialog

        title: qsTr("Reset Settings")
        message: qsTr("Return all settings to their factory defaults?")
        confirmText: qsTr("Reset")
        confirmColor: Theme.errorColor
        showDontAsk: false
        onAccepted: settingsManager.resetToDefaults()
    }

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: 12
        topLeftRadius: 26
    }

    component SettingItem: ColumnLayout {
        property string label: ""
        property string helpText: ""
        default property alias content: controlLayout.data

        Layout.fillWidth: true
        spacing: Theme.spacingTiny

        Label {
            text: label
            color: Theme.textColor
            font.pixelSize: Theme.fontSizeNormal
            font.weight: Font.Medium
        }

        ColumnLayout {
            id: controlLayout

            Layout.fillWidth: true
            spacing: Theme.spacingTiny
        }

        Label {
            Layout.fillWidth: true
            text: helpText
            color: Theme.textMutedColor
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            visible: helpText.length > 0
        }

    }

}

