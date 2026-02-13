/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import "../styles"

Page {
    id: root

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: 12
        topLeftRadius: 26
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: Theme.spacingLarge

            // Header
            Label {
                text: qsTr("Settings")
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Font.Bold
                color: Theme.textColor
            }

            // Settings Content
            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.spacingLarge

                // Video Section
                SettingsSection {
                    title: qsTr("Video Recording")

                    SettingItem {
                        label: qsTr("Video Encoder")
                        helpText: qsTr("Choose 'Auto' to let Trophy pick the best hardware encoder for your system (NVIDIA NVENC or AMD/Intel VA-API). Use 'x264' only as a fallback.")

                        ComboBox {
                            Layout.fillWidth: true
                            model: ["Auto (Recommended)", "NVENC H.264", "NVENC HEVC", "VAAPI H.264", "VAAPI HEVC", "x264 Software"]
                            property var values: ["auto", "nvenc_h264", "nvenc_hevc", "vaapi_h264", "vaapi_hevc", "x264"]
                            currentIndex: settingsManager ? values.indexOf(settingsManager.encoder) : 0
                            onCurrentIndexChanged: if (settingsManager) settingsManager.encoder = values[currentIndex]
                        }
                    }

                    SettingItem {
                        label: qsTr("Video Bitrate")
                        helpText: qsTr("Controls video quality. 10,000-15,000 kbps is great for 1080p. Use 20,000+ kbps for 1440p/4K recording.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium

                            Slider {
                                id: bitrateSlider
                                Layout.fillWidth: true
                                from: 2000
                                to: 50000
                                stepSize: 1000
                                value: settingsManager ? settingsManager.videoBitrate : 15000
                                onMoved: if (settingsManager) settingsManager.videoBitrate = value
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

                                RadioButton {
                                    text: modelData + " FPS"
                                    checked: settingsManager ? settingsManager.fps === modelData : false
                                    onClicked: if (settingsManager) settingsManager.fps = modelData
                                }
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("Resolution")
                        helpText: qsTr("Native captures your full screen. Downscaling (e.g. to 1080p) helps performance if your game is demanding.")

                        ComboBox {
                            Layout.fillWidth: true
                            model: ["Native (Game Resolution)", "480p", "720p", "1080p", "1440p", "4K"]
                            property var values: ["native", "720x480", "1280x720", "1920x1080", "2560x1440", "3840x2160"]
                            currentIndex: settingsManager ? values.indexOf(settingsManager.videoResolution) : 0
                            onCurrentIndexChanged: if (settingsManager) settingsManager.videoResolution = values[currentIndex]
                        }
                    }

                    SettingItem {
                        label: qsTr("Container Format")
                        helpText: ""

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            RowLayout {
                                spacing: Theme.spacingSmall

                                RadioButton {
                                    id: mkvRadio
                                    text: "MKV (Recommended)"
                                    checked: settingsManager ? settingsManager.containerFormat === "mkv" : false
                                    onClicked: if (settingsManager) settingsManager.containerFormat = "mkv"
                                }

                                RadioButton {
                                    id: mp4Radio
                                    text: "MP4"
                                    checked: settingsManager ? settingsManager.containerFormat === "mp4" : false
                                    onClicked: if (settingsManager) settingsManager.containerFormat = "mp4"
                                }
                            }

                            // Warning for MP4
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
                                    spacing: Theme.spacingSmall

                                    Label {
                                        text: "⚠️"
                                        font.pixelSize: Theme.fontSizeMedium
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

                            // Info for MKV
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("✓ MKV is crash-resistant and can be converted to MP4 later instantly.")
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

                        ComboBox {
                            Layout.fillWidth: true
                            model: ["Ultrafast", "Superfast", "Very Fast", "Faster", "Fast", "Medium", "Slow"]
                            property var values: ["ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow"]
                            currentIndex: settingsManager ? values.indexOf(settingsManager.encoderPreset) : 0
                            onCurrentIndexChanged: if (settingsManager) settingsManager.encoderPreset = values[currentIndex]
                        }
                    }
                }

                // Audio Section
                SettingsSection {
                    title: qsTr("Audio Recording")

                    SettingItem {
                        label: qsTr("Separate Audio Tracks")
                        helpText: qsTr("Records game audio, voice chat, and microphone into separate tracks. This makes editing much easier!")

                        Switch {
                            checked: settingsManager ? settingsManager.separateAudioTracks : false
                            onToggled: if (settingsManager) settingsManager.separateAudioTracks = checked
                        }
                    }

                    SettingItem {
                        label: qsTr("Audio Bitrate")
                        helpText: qsTr("192 kbps is CD quality and recommended for most users.")

                        RowLayout {
                            spacing: Theme.spacingSmall

                            Repeater {
                                model: [128, 192, 256, 320]

                                RadioButton {
                                    text: modelData + " kbps"
                                    checked: settingsManager ? settingsManager.audioBitrate === modelData : false
                                    onClicked: if (settingsManager) settingsManager.audioBitrate = modelData
                                }
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("Game Audio Device")
                        helpText: qsTr("Where your game sound comes from.")

                        ComboBox {
                            Layout.fillWidth: true
                            model: ["Default Output"]  // TODO: Populate from AudioDeviceEnumerator
                            currentIndex: 0
                        }
                    }

                    SettingItem {
                        label: qsTr("Microphone")
                        helpText: qsTr("Your voice input device.")

                        ComboBox {
                            Layout.fillWidth: true
                            model: ["Default Input"]  // TODO: Populate from AudioDeviceEnumerator
                            currentIndex: 0
                        }
                    }

                    SettingItem {
                        label: qsTr("Voice Chat App")
                        helpText: qsTr("Enter the process name of your voice app (e.g. 'discord') to capture it nicely.")

                        TextField {
                            Layout.fillWidth: true
                            text: settingsManager ? settingsManager.voiceChatApp : ""
                            onTextChanged: if (settingsManager) settingsManager.voiceChatApp = text
                            placeholderText: qsTr("e.g., discord, teamspeak")
                        }
                    }
                }

                // Storage Section
                SettingsSection {
                    title: qsTr("Storage Management")

                    SettingItem {
                        label: qsTr("Recordings Folder")
                        helpText: qsTr("Where your victory moments are saved.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            TextField {
                                Layout.fillWidth: true
                                text: settingsManager ? settingsManager.recordingsPath : ""
                                readOnly: true
                            }
                            Button {
                                text: qsTr("Browse")
                                onClicked: folderDialog.open()
                            }
                        }
                    }

                    SettingItem {
                        label: qsTr("Storage Limit")
                        helpText: qsTr("Trophy will delete the oldest recordings when you exceed this limit.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium

                            Slider {
                                id: storageSlider
                                Layout.fillWidth: true
                                from: 10
                                to: 500
                                stepSize: 10
                                value: settingsManager ? settingsManager.maxStorageSize / (1024 * 1024 * 1024) : 50
                                onMoved: if (settingsManager) settingsManager.maxStorageSize = value * 1024 * 1024 * 1024
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
                        helpText: qsTr("Enable this to keep your drive from filling up.")

                        Switch {
                            checked: settingsManager ? settingsManager.autoDeleteOldest : false
                            onToggled: if (settingsManager) settingsManager.autoDeleteOldest = checked
                        }
                    }
                }

                // Game Detection Section
                SettingsSection {
                    title: qsTr("Game Detection")

                    SettingItem {
                        label: qsTr("Auto-detect Games")
                        helpText: qsTr("Trophy will try to detect when you launch a game to show the correct overlay/status.")

                        Switch {
                            checked: settingsManager ? settingsManager.autoDetectGames : false
                            onToggled: if (settingsManager) settingsManager.autoDetectGames = checked
                        }
                    }

                    SettingItem {
                        label: qsTr("Auto-record")
                        helpText: qsTr("Automatically start recording the moment a game is detected. Never miss a moment!")

                        Switch {
                            checked: settingsManager ? settingsManager.autoRecordEnabled : false
                            onToggled: if (settingsManager) settingsManager.autoRecordEnabled = checked
                        }
                    }
                }
            }

            // Actions
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingMedium
                Layout.topMargin: Theme.spacingMedium

                Item { Layout.fillWidth: true }

                Button {
                    text: qsTr("Reset to Defaults")
                    flat: true
                    onClicked: resetConfirmDialog.open()
                }

                Button {
                    text: qsTr("Save Settings")
                    highlighted: true
                    onClicked: settingsManager.save()
                }
            }

            Item { Layout.preferredHeight: Theme.spacingLarge }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Select Recordings Folder")
        currentFolder: settingsManager ? "file://" + settingsManager.recordingsPath : ""
        onAccepted: {
            if (settingsManager) settingsManager.recordingsPath = selectedFolder.toString().replace("file://", "")
        }
    }

    Dialog {
        id: resetConfirmDialog
        title: qsTr("Reset Settings")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Yes | Dialog.No

        Label {
            text: qsTr("Are you sure you want to reset all settings to defaults?")
        }

        onAccepted: settingsManager.resetToDefaults()
    }

    // Section component
    component SettingsSection: Rectangle {
        property string title: ""
        default property alias content: contentLayout.data

        Layout.fillWidth: true
        implicitHeight: sectionLayout.implicitHeight + Theme.spacingMedium * 2
        radius: Theme.radiusMedium
        color: Theme.cardColor

        ColumnLayout {
            id: sectionLayout
            anchors.fill: parent
            anchors.margins: Theme.spacingMedium
            spacing: Theme.spacingMedium

            Label {
                text: title
                font.pixelSize: Theme.fontSizeLarge
                font.weight: Font.Medium
                color: Theme.textColor
            }

            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.dividerColor
            }

            ColumnLayout {
                id: contentLayout
                Layout.fillWidth: true
                spacing: Theme.spacingLarge
            }
        }
    }

    // Setting item component
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
