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
    z: 1000 // Ensure it's on top of everything

    property int currentStep: 0
    property int totalSteps: 5

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

                    // Progress indicator
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

            // Content
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                StackLayout {
                    id: stepStack
                    anchors.fill: parent
                    anchors.margins: Theme.spacingXLarge
                    currentIndex: root.currentStep

                    // Step 1: Welcome
                    ScrollView {
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded
                        clip: true
                        
                        ColumnLayout {
                            id: step1
                            width: parent.width
                            height: Math.max(implicitHeight, parent.height)
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
                            text: qsTr("Trophy is designed to stay out of your way and capture your best gaming moments automatically. Let's take a moment to configure your setup.")
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

                    // Step 2: How it works
                    ScrollView {
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded
                        clip: true
                        
                        ColumnLayout {
                            width: parent.width
                            height: Math.max(implicitHeight, parent.height)
                            spacing: Theme.spacingLarge

                        Label {
                            text: qsTr("How Trophy Works")
                            font.pixelSize: Theme.fontSizeXLarge
                            font.weight: Font.Bold
                            color: Theme.textColor
                            Layout.alignment: Qt.AlignHCenter
                        }

                        GridLayout {
                            columns: 2
                            rowSpacing: Theme.spacingLarge
                            columnSpacing: Theme.spacingLarge
                            Layout.fillWidth: true

                            // Item 1
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 100
                                color: Theme.withOpacity(Theme.cardColor, 0.5)
                                radius: Theme.radiusMedium
                                border.width: 1
                                border.color: Theme.borderColor

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingMedium
                                    spacing: Theme.spacingMedium
                                    
                                    Label { text: "🎮"; font.pixelSize: 32 }
                                    ColumnLayout {
                                        spacing: 2
                                        Label { text: qsTr("Auto-Detection"); font.weight: Font.Bold; color: Theme.textColor }
                                        Label { 
                                            text: qsTr("Trophy senses when a game starts and gets ready to record."); 
                                            font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; 
                                            wrapMode: Text.WordWrap; Layout.fillWidth: true 
                                        }
                                    }
                                }
                            }

                            // Item 2
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 100
                                color: Theme.withOpacity(Theme.cardColor, 0.5)
                                radius: Theme.radiusMedium
                                border.width: 1
                                border.color: Theme.borderColor

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingMedium
                                    spacing: Theme.spacingMedium
                                    
                                    Label { text: "⏺️"; font.pixelSize: 32 }
                                    ColumnLayout {
                                        spacing: 2
                                        Label { text: qsTr("Background Recording"); font.weight: Font.Bold; color: Theme.textColor }
                                        Label { 
                                            text: qsTr("Records in the background so you never miss a play."); 
                                            font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; 
                                            wrapMode: Text.WordWrap; Layout.fillWidth: true 
                                        }
                                    }
                                }
                            }

                            // Item 3
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 100
                                color: Theme.withOpacity(Theme.cardColor, 0.5)
                                radius: Theme.radiusMedium
                                border.width: 1
                                border.color: Theme.borderColor

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingMedium
                                    spacing: Theme.spacingMedium
                                    
                                    Label { text: "⚡"; font.pixelSize: 32 }
                                    ColumnLayout {
                                        spacing: 2
                                        Label { text: qsTr("Low Impact"); font.weight: Font.Bold; color: Theme.textColor }
                                        Label { 
                                            text: qsTr("Uses hardware encoders (NVENC/VA-API) for zero lag."); 
                                            font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; 
                                            wrapMode: Text.WordWrap; Layout.fillWidth: true 
                                        }
                                    }
                                }
                            }

                            // Item 4
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 100
                                color: Theme.withOpacity(Theme.cardColor, 0.5)
                                radius: Theme.radiusMedium
                                border.width: 1
                                border.color: Theme.borderColor

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacingMedium
                                    spacing: Theme.spacingMedium
                                    
                                    Label { text: "🎞️"; font.pixelSize: 32 }
                                    ColumnLayout {
                                        spacing: 2
                                        Label { text: qsTr("Instant Replay"); font.weight: Font.Bold; color: Theme.textColor }
                                        Label { 
                                            text: qsTr("Save the last few minutes of gameplay with a hotkey."); 
                                            font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; 
                                            wrapMode: Text.WordWrap; Layout.fillWidth: true 
                                        }
                                    }
                                }
                            }
                        }
                        
                        Item { Layout.fillHeight: true }
                        }
                    }

                    // Step 3: Storage
                    ScrollView {
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded
                        clip: true
                        
                        ColumnLayout {
                            width: parent.width
                            height: Math.max(implicitHeight, parent.height)
                            spacing: Theme.spacingLarge

                        Label {
                            text: qsTr("Storage Settings")
                            font.pixelSize: Theme.fontSizeXLarge
                            font.weight: Font.Bold
                            color: Theme.textColor
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Label {
                            text: qsTr("Choose where your recordings will be saved and how much space Trophy can use.")
                            font.pixelSize: Theme.fontSizeMedium
                            color: Theme.textSecondaryColor
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium

                            Label { text: qsTr("Recordings Folder"); font.weight: Font.Bold; color: Theme.textColor }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacingSmall
                                TrophyTextField {
                                    id: pathField
                                    Layout.fillWidth: true
                                    text: settingsManager ? settingsManager.recordingsPath : ""
                                    readOnly: true
                                }
                                TrophyButton {
                                    text: qsTr("Browse")
                                    onClicked: folderDialog.open()
                                }
                            }

                            Item { Layout.preferredHeight: Theme.spacingMedium }

                            Label { text: qsTr("Storage Limit"); font.weight: Font.Bold; color: Theme.textColor }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacingMedium
                                TrophySlider {
                                    id: storageSlider
                                    Layout.fillWidth: true
                                    from: 0
                                    to: 500
                                    stepSize: 10
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
                                Label {
                                    text: storageSlider.value === 0 ? qsTr("No Limit") : storageSlider.value.toFixed(0) + " GB"
                                    color: Theme.textColor
                                    font.weight: Font.Medium
                                    Layout.preferredWidth: 60
                                }
                            }
                            Label {
                                text: qsTr("Trophy will automatically delete the oldest recordings when this limit is reached.")
                                font.pixelSize: Theme.fontSizeSmall
                                color: Theme.textMutedColor
                            }

                            Item { Layout.preferredHeight: Theme.spacingSmall }

                            RowLayout {
                                spacing: Theme.spacingMedium
                                enabled: storageSlider.value > 0
                                opacity: enabled ? 1.0 : 0.5
                                TrophySwitch {
                                    checked: settingsManager ? settingsManager.autoDeleteOldest : true
                                    onToggled: if (settingsManager) settingsManager.autoDeleteOldest = checked
                                }
                                ColumnLayout {
                                    spacing: 0
                                    Label { text: qsTr("Auto-Cleanup"); font.weight: Font.Bold; color: Theme.textColor }
                                    Label { text: qsTr("Automatically delete oldest videos when storage is full."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                                }
                            }
                        }

                        Item { Layout.fillHeight: true }
                        }
                    }

                    // Step 4: Audio
                    ScrollView {
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded
                        clip: true
                        
                        ColumnLayout {
                            width: parent.width
                            height: Math.max(implicitHeight, parent.height)
                            spacing: Theme.spacingLarge

                        Label {
                            text: qsTr("Audio Configuration")
                            font.pixelSize: Theme.fontSizeXLarge
                            font.weight: Font.Bold
                            color: Theme.textColor
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Label {
                            text: qsTr("Select your primary audio sources for capturing game sound and your voice.")
                            font.pixelSize: Theme.fontSizeMedium
                            color: Theme.textSecondaryColor
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium

                            Label { text: qsTr("Microphone"); font.weight: Font.Bold; color: Theme.textColor }
                            TrophyComboBox {
                                id: micBox
                                Layout.fillWidth: true
                                property var devices: audioDeviceEnumerator ? audioDeviceEnumerator.inputDevices : []
                                model: {
                                    let names = ["Default Input"];
                                    for (let d of devices) { if (d.id !== "default") names.push(d.name); }
                                    names;
                                }
                                onActivated: {
                                    if (settingsManager && audioDeviceEnumerator && currentIndex < devices.length) {
                                        settingsManager.microphoneDevice = devices[currentIndex].id;
                                    }
                                }
                            }

                            Item { Layout.preferredHeight: Theme.spacingMedium }

                            Label { text: qsTr("Audio Mode"); font.weight: Font.Bold; color: Theme.textColor }
                            RowLayout {
                                spacing: Theme.spacingLarge
                                TrophyRadioButton {
                                    text: qsTr("Game + Voice Apps")
                                    checked: settingsManager ? settingsManager.audioMode === "game" : true
                                    onClicked: if (settingsManager) settingsManager.audioMode = "game"
                                }
                                TrophyRadioButton {
                                    text: qsTr("Entire Desktop")
                                    checked: settingsManager ? settingsManager.audioMode === "desktop" : false
                                    onClicked: if (settingsManager) settingsManager.audioMode = "desktop"
                                }
                            }
                            Label {
                                text: settingsManager && settingsManager.audioMode === "game" ? 
                                      qsTr("Records only the game and selected voice applications.") :
                                      qsTr("Records all audio heard on your system.")
                                font.pixelSize: Theme.fontSizeSmall
                                color: Theme.textMutedColor
                            }

                            Item { Layout.preferredHeight: Theme.spacingMedium }

                            Label { text: qsTr("Track Configuration"); font.weight: Font.Bold; color: Theme.textColor }
                            RowLayout {
                                spacing: Theme.spacingLarge
                                TrophyRadioButton {
                                    text: qsTr("Single Track")
                                    checked: settingsManager ? !settingsManager.separateAudioTracks : true
                                    onClicked: if (settingsManager) settingsManager.separateAudioTracks = false
                                }
                                TrophyRadioButton {
                                    text: qsTr("Separate Tracks")
                                    checked: settingsManager ? settingsManager.separateAudioTracks : false
                                    onClicked: if (settingsManager) settingsManager.separateAudioTracks = true
                                }
                            }
                            Label {
                                text: settingsManager && settingsManager.separateAudioTracks ? 
                                      qsTr("Game and Mic audio are stored on different tracks (best for editing).") :
                                      qsTr("All audio is mixed into a single track (best for simple viewing).")
                                font.pixelSize: Theme.fontSizeSmall
                                color: Theme.textMutedColor
                            }

                            Item { 
                                Layout.preferredHeight: Theme.spacingMedium 
                                visible: settingsManager && settingsManager.audioMode === "game"
                            }

                            Label { 
                                text: qsTr("Voice Chat Application"); 
                                font.weight: Font.Bold; 
                                color: Theme.textColor;
                                visible: settingsManager && settingsManager.audioMode === "game"
                            }
                            TrophyComboBox {
                                id: voiceAppBox
                                Layout.fillWidth: true
                                visible: settingsManager && settingsManager.audioMode === "game"
                                property var apps: audioDeviceEnumerator ? audioDeviceEnumerator.audioApps : []
                                model: {
                                    let names = ["None"];
                                    for (let a of apps) { names.push(a.name); }
                                    names;
                                }
                                onActivated: {
                                    if (settingsManager && audioDeviceEnumerator && currentIndex > 0) {
                                        settingsManager.voiceChatApp = apps[currentIndex - 1].binary;
                                    } else if (settingsManager) {
                                        settingsManager.voiceChatApp = "";
                                    }
                                }
                            }
                        }

                        Item { Layout.fillHeight: true }
                        }
                    }

                    // Step 5: Finish
                    ScrollView {
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded
                        clip: true
                        
                        ColumnLayout {
                            width: parent.width
                            height: Math.max(implicitHeight, parent.height)
                            spacing: Theme.spacingLarge
                        
                        Item { Layout.fillHeight: true }

                        Label {
                            text: "✨"
                            Layout.alignment: Qt.AlignHCenter
                            font.pixelSize: 64
                        }

                        Label {
                            text: qsTr("You're All Set!")
                            Layout.alignment: Qt.AlignHCenter
                            font.pixelSize: Theme.fontSizeTitle
                            font.weight: Font.Bold
                            color: Theme.textColor
                        }

                        Label {
                            text: qsTr("Trophy is now configured and ready to go. You can always change these settings later in the settings menu.")
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 500
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            font.pixelSize: Theme.fontSizeMedium
                            color: Theme.textSecondaryColor
                        }

                        Rectangle {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 400
                            Layout.preferredHeight: 80
                            radius: Theme.radiusNormal
                            color: Theme.withOpacity(Theme.primaryColor, 0.05)
                            border.width: 1
                            border.color: Theme.withOpacity(Theme.primaryColor, 0.2)

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: Theme.spacingMedium
                                spacing: Theme.spacingMedium
                                TrophySwitch {
                                    checked: settingsManager ? settingsManager.autoRecordEnabled : false
                                    onToggled: if (settingsManager) settingsManager.autoRecordEnabled = checked
                                }
                                ColumnLayout {
                                    spacing: 0
                                    Label { text: qsTr("Enable Auto-Record"); font.weight: Font.Bold; color: Theme.textColor }
                                    Label { 
                                        text: qsTr("Automatically start recording when a game is detected."); 
                                        font.pixelSize: Theme.fontSizeSmall; 
                                        color: Theme.textSecondaryColor;
                                        wrapMode: Text.WordWrap;
                                        Layout.fillWidth: true
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 400
                            Layout.preferredHeight: 60
                            radius: Theme.radiusNormal
                            color: Theme.withOpacity(Theme.primaryColor, 0.1)
                            border.width: 1
                            border.color: Theme.withOpacity(Theme.primaryColor, 0.3)

                            RowLayout {
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

                        Item { Layout.fillHeight: true }
                        }
                    }
                }
            }

            // Footer
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 80
                color: "transparent"
                bottomLeftRadius: wizardCard.radius
                bottomRightRadius: wizardCard.radius

                Rectangle {
                    anchors.top: parent.top
                    width: parent.width
                    height: 1
                    color: Theme.borderColor
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingXLarge
                    anchors.rightMargin: Theme.spacingXLarge
                    spacing: Theme.spacingMedium

                    TrophyButton {
                        text: qsTr("Back")
                        visible: root.currentStep > 0
                        onClicked: root.currentStep--
                    }

                    Item { Layout.fillWidth: true }

                    TrophyButton {
                        text: root.currentStep === root.totalSteps - 1 ? qsTr("Get Started") : qsTr("Next")
                        highlighted: true
                        onClicked: {
                            if (root.currentStep < root.totalSteps - 1) {
                                root.currentStep++
                            } else {
                                if (settingsManager)
                                    settingsManager.setupCompleted = true;
                            }
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
            if (path.startsWith("file://")) {
                path = path.substring(7);
            }
            if (settingsManager)
                settingsManager.recordingsPath = path;
        }
    }
}
