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
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Dialogs

Rectangle {
    id: root

    anchors.fill: parent
    color: Theme.withOpacity(Theme.backgroundColor, 0.95)
    z: 1000 

    property int currentStep: 0
    property int totalSteps: 4 // Adjusted to match your current step count

    // Prevent interaction with underlying UI
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: {}
    }

    Rectangle {
        id: wizardCard

        anchors.centerIn: parent
        width: 700
        height: 650
        radius: Theme.radiusLarge
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.borderColor
        clip: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // Header
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                color: Theme.withOpacity(Theme.secondaryColor, 0.3)
                topLeftRadius: wizardCard.radius
                topRightRadius: wizardCard.radius

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingXLarge
                    anchors.rightMargin: Theme.spacingXLarge
                    spacing: Theme.spacingMedium

                    Image {
                        source: "qrc:/icons/trophy.svg"
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        sourceSize: Qt.size(32, 32)
                    }

                    ColumnLayout {
                        spacing: 0
                        Label {
                            text: qsTr("Welcome to Trophy")
                            font.pixelSize: Theme.fontSizeXLarge
                            font.weight: Font.Bold
                            color: Theme.textColor
                        }
                        Label {
                            text: qsTr("Setup your ultimate game recording experience")
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textSecondaryColor
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Row {
                        spacing: Theme.spacingSmall
                        Repeater {
                            model: root.totalSteps
                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                color: index <= root.currentStep ? Theme.primaryColor : Theme.withOpacity(Theme.textColor, 0.1)
                                Behavior on color { ColorAnimation { duration: Theme.animationNormal } }
                            }
                        }
                    }
                }

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.borderColor
                }
            }

            // Content Area
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                StackLayout {
                    id: stepStack
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    currentIndex: root.currentStep

                    // Step 1: Welcome
                    ScrollView {
                        id: step1Scroll
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        clip: true
                        ColumnLayout {
                            width: step1Scroll.availableWidth
                            height: Math.max(implicitHeight, step1Scroll.availableHeight)
                            spacing: Theme.spacingLarge
                            Item { Layout.fillHeight: true }
                            Image {
                                source: "qrc:/icons/trophy.svg"
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 120
                                Layout.preferredHeight: 120
                                sourceSize: Qt.size(120, 120)
                                opacity: 0.8
                            }
                            Label {
                                text: qsTr("Let's Get Started")
                                Layout.alignment: Qt.AlignHCenter
                                font.pixelSize: Theme.fontSizeTitle
                                font.weight: Font.ExtraBold
                                color: Theme.textColor
                            }
                            Label {
                                text: qsTr("Trophy captures your best gaming moments automatically. Let's take a moment to configure your setup.")
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                font.pixelSize: Theme.fontSizeMedium
                                color: Theme.textSecondaryColor
                                lineHeight: 1.4
                            }
                            Item { Layout.fillHeight: true }
                        }
                    }

                    // Step 2: Storage
                    ScrollView {
                        id: step2Scroll
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        clip: true
                        ColumnLayout {
                            width: step2Scroll.availableWidth
                            height: Math.max(implicitHeight, step2Scroll.availableHeight)
                            spacing: Theme.spacingLarge
                            Item { Layout.fillHeight: true }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: Theme.spacingSmall
                                Label { text: qsTr("Storage Settings"); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeLarge; font.weight: Font.Bold; color: Theme.textColor }
                                Label { text: qsTr("Configure where and how your clips are saved."); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                            }
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: settingsCol.height + (Theme.spacingLarge * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                ColumnLayout {
                                    id: settingsCol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: Theme.spacingLarge; anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingMedium
                                    Label { text: qsTr("Recordings Folder"); font.weight: Font.Bold; color: Theme.textColor }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TrophyTextField { id: pathField; Layout.fillWidth: true; text: settingsManager ? settingsManager.recordingsPath : ""; readOnly: true }
                                        TrophyButton { text: qsTr("Browse"); onClicked: folderDialog.open() }
                                    }
                                    Label { text: qsTr("Storage Limit"); font.weight: Font.Bold; color: Theme.textColor }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TrophySlider {
                                            id: mainStorageSlider
                                            Layout.fillWidth: true
                                            from: 0; to: 500; stepSize: 1
                                            value: settingsManager ? (settingsManager.maxStorageSize / (1024 * 1024 * 1024)) : 50
                                            onMoved: {
                                                if (!pressed && settingsManager)
                                                    settingsManager.maxStorageSize = value * 1024 * 1024 * 1024;
                                            }
                                            onPressedChanged: {
                                                if (!pressed && settingsManager)
                                                    settingsManager.maxStorageSize = value * 1024 * 1024 * 1024;
                                            }
                                        }
                                        Label { text: mainStorageSlider.value === 0 ? qsTr("No Limit") : Math.round(mainStorageSlider.value) + " GB"; color: Theme.textColor; font.weight: Font.Medium; Layout.preferredWidth: 70 }
                                    }
                                    RowLayout {
                                        spacing: Theme.spacingMedium
                                        enabled: mainStorageSlider.value > 0
                                        opacity: enabled ? 1.0 : 0.5
                                        TrophySwitch { checked: settingsManager ? settingsManager.autoDeleteOldest : true; onToggled: if (settingsManager) settingsManager.autoDeleteOldest = checked }
                                        ColumnLayout {
                                            spacing: 0
                                            Label { text: qsTr("Auto-Cleanup"); font.weight: Font.Bold; color: Theme.textColor }
                                            Label { text: qsTr("Delete oldest clips when space is low."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                                        }
                                    }
                                }
                            }
                            Item { Layout.fillHeight: true }
                        }
                    }

                    // Step 3: Audio
                    ScrollView {
                        id: step3Scroll
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        clip: true
                        ColumnLayout {
                            width: step3Scroll.availableWidth
                            height: Math.max(implicitHeight, step3Scroll.availableHeight)
                            spacing: Theme.spacingLarge
                            Item { Layout.fillHeight: true }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: Theme.spacingSmall
                                Label { text: qsTr("Audio Configuration"); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeLarge; font.weight: Font.Bold; color: Theme.textColor }
                                Label { text: qsTr("Select your primary audio sources."); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                            }
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: audioCol.height + (Theme.spacingLarge * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                ColumnLayout {
                                    id: audioCol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: Theme.spacingLarge; anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingMedium
                                    Label { text: qsTr("Microphone"); font.weight: Font.Bold; color: Theme.textColor }
                                    TrophyComboBox {
                                        id: micBox; Layout.fillWidth: true
                                        property var devices: audioDeviceEnumerator ? audioDeviceEnumerator.inputDevices : []
                                        model: { let names = ["Default Input"]; for (let d of devices) { if (d.id !== "default") names.push(d.name); } return names; }
                                        onActivated: if (settingsManager && currentIndex < devices.length) settingsManager.microphoneDevice = devices[currentIndex].id
                                    }
                                    Label { text: qsTr("Audio Capture Mode"); font.weight: Font.Bold; color: Theme.textColor }
                                    RowLayout {
                                        spacing: Theme.spacingLarge
                                        TrophyRadioButton { text: qsTr("Game + Voice"); checked: settingsManager ? settingsManager.audioMode === "game" : true; onClicked: if (settingsManager) settingsManager.audioMode = "game" }
                                        TrophyRadioButton { text: qsTr("Entire Desktop"); checked: settingsManager ? settingsManager.audioMode === "desktop" : false; onClicked: if (settingsManager) settingsManager.audioMode = "desktop" }
                                    }
                                    Label { text: qsTr("Recording Tracks"); font.weight: Font.Bold; color: Theme.textColor }
                                    RowLayout {
                                        spacing: Theme.spacingLarge
                                        TrophyRadioButton { text: qsTr("Single (Mixed)"); checked: settingsManager ? !settingsManager.separateAudioTracks : true; onClicked: if (settingsManager) settingsManager.separateAudioTracks = false }
                                        TrophyRadioButton { text: qsTr("Separate (Multi)"); checked: settingsManager ? settingsManager.separateAudioTracks : false; onClicked: if (settingsManager) settingsManager.separateAudioTracks = true }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        visible: settingsManager && settingsManager.audioMode === "game"
                                        spacing: Theme.spacingSmall
                                        Label { text: qsTr("Voice Chat Application"); font.weight: Font.Bold; color: Theme.textColor }
                                        TrophyComboBox {
                                            id: voiceAppBox; Layout.fillWidth: true
                                            property var apps: audioDeviceEnumerator ? audioDeviceEnumerator.audioApps : []
                                            model: { let names = ["None"]; for (let a of apps) { names.push(a.name); } return names; }
                                            onActivated: if (settingsManager) settingsManager.voiceChatApp = (currentIndex > 0) ? apps[currentIndex - 1].binary : ""
                                        }
                                    }
                                }
                            }
                            Item { Layout.fillHeight: true }
                        }
                    }

                    /// Step 4: Finish
                    ScrollView {
                        id: step5Scroll
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        clip: true
                        ColumnLayout {
                            width: step5Scroll.availableWidth
                            height: Math.max(implicitHeight, step5Scroll.availableHeight)
                            spacing: Theme.spacingLarge
                            Item { Layout.fillHeight: true }

                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: Theme.spacingSmall
                                Label { text: "✨"; Layout.alignment: Qt.AlignHCenter; font.pixelSize: 64 }
                                Label { text: qsTr("You're All Set!"); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeTitle; font.weight: Font.Bold; color: Theme.textColor }
                                Label { text: qsTr("Trophy is now configured and ready to go."); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeMedium; color: Theme.textSecondaryColor }
                            }

                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                // Allow height to be determined by children + margins
                                implicitHeight: finishCol.height + (Theme.spacingLarge * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor

                                ColumnLayout {
                                    id: finishCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spacingLarge
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingLarge // Gap between toggle box and tip box

                                    // Box 1: Enable Auto-Record
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacingMedium
                                        TrophySwitch { 
                                            checked: settingsManager ? settingsManager.autoRecordEnabled : false
                                            onToggled: if (settingsManager) settingsManager.autoRecordEnabled = checked 
                                        }
                                        ColumnLayout {
                                            spacing: 0
                                            Layout.fillWidth: true
                                            Label { text: qsTr("Enable Auto-Record"); font.weight: Font.Bold; color: Theme.textColor }
                                            Label { 
                                                text: qsTr("Start recording when a game is detected."); 
                                                font.pixelSize: Theme.fontSizeSmall; 
                                                color: Theme.textSecondaryColor; 
                                                wrapMode: Text.WordWrap; 
                                                Layout.fillWidth: true 
                                            }
                                        }
                                    }

                                    // Box 2: Tip (Now properly spaced below)
                                    Rectangle {
                                        Layout.fillWidth: true
                                        // Set height based on internal content
                                        implicitHeight: tipRow.implicitHeight + (Theme.spacingMedium * 2)
                                        radius: Theme.radiusNormal
                                        color: Theme.withOpacity(Theme.primaryColor, 0.1)
                                        border.color: Theme.withOpacity(Theme.primaryColor, 0.2)

                                        RowLayout {
                                            id: tipRow
                                            anchors.fill: parent
                                            anchors.margins: Theme.spacingMedium
                                            spacing: Theme.spacingMedium
                                            Label { text: "💡"; font.pixelSize: 20 }
                                            Label { 
                                                text: qsTr("Tip: Press F9 while in-game to start or stop a manual recording.")
                                                font.pixelSize: Theme.fontSizeSmall
                                                color: Theme.textColor
                                                wrapMode: Text.WordWrap
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }
                                }
                            }
                            Item { Layout.fillHeight: true }
                        }
                    }
                }
            }

            // Footer
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 80; color: "transparent"
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.borderColor }
                RowLayout {
                    anchors.fill: parent; anchors.leftMargin: Theme.spacingXLarge; anchors.rightMargin: Theme.spacingXLarge; spacing: Theme.spacingMedium
                    TrophyButton { text: qsTr("Back"); visible: root.currentStep > 0; onClicked: root.currentStep-- }
                    Item { Layout.fillWidth: true }
                    TrophyButton {
                        text: root.currentStep === root.totalSteps - 1 ? qsTr("Get Started") : qsTr("Next")
                        highlighted: true
                        onClicked: {
                            if (root.currentStep < root.totalSteps - 1) root.currentStep++
                            else if (settingsManager) settingsManager.setupCompleted = true
                        }
                    }
                }
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Select Recordings Folder")
        currentFolder: settingsManager ? ("file://" + settingsManager.recordingsPath) : ""
        onAccepted: {
            var path = selectedFolder.toString();
            if (path.startsWith("file://")) path = path.substring(7);
            if (settingsManager) settingsManager.recordingsPath = path;
        }
    }
}