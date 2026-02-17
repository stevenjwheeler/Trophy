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
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.spacingLarge

                // --- VIDEO SECTION ---
                SettingsSection {
                    // Removed 'collapsed: false' so it starts closed like the others

                    title: qsTr("Video Recording")

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
                                        text: qsTr("Not available")
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
                        helpText: qsTr("The container format determines how your video is stored. MKV is the most versatile and recommended format.")

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            RowLayout {
                                spacing: Theme.spacingSmall

                                TrophyRadioButton {
                                    enabled: !(recordingManager && recordingManager.isRecording)
                                    opacity: enabled ? 1 : 0.5
                                    text: "MKV (Recommended)"
                                    checked: settingsManager ? settingsManager.containerFormat === "mkv" : false
                                    onClicked: {
                                        if (settingsManager)
                                            settingsManager.containerFormat = "mkv";

                                    }
                                }

                                TrophyRadioButton {
                                    enabled: !(recordingManager && recordingManager.isRecording)
                                    opacity: enabled ? 1 : 0.5
                                    text: "MP4"
                                    checked: settingsManager ? settingsManager.containerFormat === "mp4" : false
                                    onClicked: {
                                        if (settingsManager)
                                            settingsManager.containerFormat = "mp4";

                                    }
                                }

                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: warningLayout.implicitHeight + Theme.spacingSmall * 2
                                radius: Theme.radiusSmall
                                color: Theme.withOpacity(Theme.warningColor, 0.1)
                                border.width: 1
                                border.color: Theme.withOpacity(Theme.warningColor, 0.3)
                                visible: settingsManager ? settingsManager.containerFormat === "mp4" : false

                                RowLayout {
                                    id: warningLayout

                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingSmall

                                    Label {
                                        text: "⚠️"
                                    }

                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("MP4 files can become corrupted if recording stops unexpectedly (crash, power loss). MKV is safer.")
                                        color: Theme.textColor
                                        font.pixelSize: Theme.fontSizeSmall
                                        wrapMode: Text.WordWrap
                                    }

                                }

                            }

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("MKV is crash-resistant and can be converted later if needed.")
                                color: Theme.textMutedColor
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                visible: settingsManager ? settingsManager.containerFormat === "mkv" : false
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
                        label: qsTr("Audio Mode")
                        helpText: settingsManager && settingsManager.audioMode === "game"
                                  ? qsTr("Captures the game window's audio, voice chat app, and microphone.")
                                  : qsTr("Captures all system audio and microphone.")

                        RowLayout {
                            spacing: Theme.spacingSmall

                            TrophyRadioButton {
                                enabled: !(recordingManager && recordingManager.isRecording)
                                opacity: enabled ? 1.0 : 0.5
                                text: qsTr("Game")
                                checked: settingsManager ? settingsManager.audioMode === "game" : true
                                onClicked: if (settingsManager) settingsManager.audioMode = "game"
                            }

                            TrophyRadioButton {
                                enabled: !(recordingManager && recordingManager.isRecording)
                                opacity: enabled ? 1.0 : 0.5
                                text: qsTr("Desktop")
                                checked: settingsManager ? settingsManager.audioMode === "desktop" : false
                                onClicked: if (settingsManager) settingsManager.audioMode = "desktop"
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("Separate Audio Tracks")
                        helpText: qsTr("Records game audio, voice chat, and microphone into separate audio tracks. This makes editing much easier!")
                        visible: settingsManager ? settingsManager.audioMode === "game" : true

                        TrophySwitch {
                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
                            checked: settingsManager ? settingsManager.separateAudioTracks : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.separateAudioTracks = checked;

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
                                    opacity: enabled ? 1 : 0.5
                                    text: modelData + " kbps"
                                    checked: settingsManager ? settingsManager.audioBitrate === modelData : false
                                    onClicked: {
                                        if (settingsManager)
                                            settingsManager.audioBitrate = modelData;

                                    }
                                }

                            }

                        }

                    }

                    SettingItem {
                        label: qsTr("Game Audio Device")
                        helpText: qsTr("Where your game sound comes from.")
                        visible: settingsManager ? settingsManager.audioMode === "desktop" : false

                        TrophyComboBox {
                            id: gameAudioBox
                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
                            Layout.fillWidth: true

                            property var devices: audioDeviceEnumerator ? audioDeviceEnumerator.outputDevices : []

                            model: {
                                let names = ["Default Output"]
                                for (let d of devices) {
                                    if (d.id !== "default") {
                                        names.push(d.name)
                                    }
                                }
                                return names
                            }

                            Binding {
                                target: gameAudioBox
                                property: "currentIndex"
                                value: {
                                    if (!settingsManager || !audioDeviceEnumerator)
                                        return 0
                                    let devices = audioDeviceEnumerator.outputDevices
                                    let saved = settingsManager.gameAudioDevice
                                    for (let i = 0; i < devices.length; i++) {
                                        if (devices[i].id === saved)
                                            return i
                                    }
                                    return 0
                                }
                            }

                            onActivated: {
                                if (!settingsManager || !audioDeviceEnumerator)
                                    return
                                let devices = audioDeviceEnumerator.outputDevices
                                if (currentIndex < devices.length) {
                                    settingsManager.gameAudioDevice = devices[currentIndex].id
                                }
                            }
                        }

                    }

                    SettingItem {
                        label: qsTr("Microphone")
                        helpText: qsTr("Your voice input device.")

                        TrophyComboBox {
                            id: microphoneBox
                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
                            Layout.fillWidth: true

                            property var devices: audioDeviceEnumerator ? audioDeviceEnumerator.inputDevices : []

                            model: {
                                let names = ["Default Input"]
                                for (let d of devices) {
                                    if (d.id !== "default") {
                                        names.push(d.name)
                                    }
                                }
                                return names
                            }

                            Binding {
                                target: microphoneBox
                                property: "currentIndex"
                                value: {
                                    if (!settingsManager || !audioDeviceEnumerator)
                                        return 0
                                    let devices = audioDeviceEnumerator.inputDevices
                                    let saved = settingsManager.microphoneDevice
                                    for (let i = 0; i < devices.length; i++) {
                                        if (devices[i].id === saved)
                                            return i
                                    }
                                    return 0
                                }
                            }

                            onActivated: {
                                if (!settingsManager || !audioDeviceEnumerator)
                                    return
                                let devices = audioDeviceEnumerator.inputDevices
                                if (currentIndex < devices.length) {
                                    settingsManager.microphoneDevice = devices[currentIndex].id
                                }
                            }
                        }

                    }

                    SettingItem {
                        label: qsTr("Voice Chat App")
                        helpText: qsTr("Enter the process name of your voice app (e.g. 'discord') to capture it nicely.")
                        visible: settingsManager ? settingsManager.audioMode === "game" : true

                        TrophyTextField {
                            id: voiceChatAppField

                            Layout.fillWidth: true
                            text: settingsManager ? settingsManager.voiceChatApp : ""
                            placeholderText: qsTr("e.g., discord, teamspeak")
                            onEditingFinished: {
                                if (settingsManager && settingsManager.voiceChatApp !== text) {
                                    settingsManager.voiceChatApp = text;
                                }
                            }
                        }

                    }

                }

                // --- STORAGE SECTION ---
                SettingsSection {
                    title: qsTr("Storage Management")

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

    // --- COMPONENTS ---
    component SettingsSection: Rectangle {
        property string title: ""
        property bool collapsed: true // Default to closed on start
        default property alias content: contentLayout.data

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
                onClicked: collapsed = !collapsed

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
