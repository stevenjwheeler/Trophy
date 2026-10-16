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
    property int totalSteps: 7 // Welcome, Storage, Audio, Modules, Hardware, Shortcuts, Finish

    // Reset to first page whenever the wizard is (re-)shown
    onVisibleChanged: {
        if (visible) {
            currentStep = 0
            wizardCard.opacity = 0
            wizardCard.scale = 0.96
            wizardCardAnimation.start()
            // Its voice picker has no Refresh button — populate on open.
            if (audioDeviceEnumerator) audioDeviceEnumerator.refreshAudioApps()
        }
    }

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

        // Open animation
        ParallelAnimation {
            id: wizardCardAnimation
            NumberAnimation {
                target: wizardCard
                property: "opacity"
                from: 0
                to: 1
                duration: Theme.animationNormal
                easing.type: Easing.OutCubic
            }
            NumberAnimation {
                target: wizardCard
                property: "scale"
                from: 0.96
                to: 1
                duration: Theme.animationNormal
                easing.type: Easing.OutCubic
            }
        }

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
                            spacing: Theme.spacingMedium
                            Item { Layout.fillHeight: true }
                            Image {
                                source: "qrc:/icons/trophy.svg"
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 96
                                Layout.preferredHeight: 96
                                sourceSize: Qt.size(96, 96)
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
                            spacing: Theme.spacingMedium
                            Item { Layout.fillHeight: true }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 2
                                Label { text: qsTr("Storage Settings"); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeLarge; font.weight: Font.Bold; color: Theme.textColor }
                                Label { text: qsTr("Configure where and how your clips are saved."); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                            }
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: settingsCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                ColumnLayout {
                                    id: settingsCol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: Theme.spacingMedium; anchors.verticalCenter: parent.verticalCenter
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
                                            from: 0; to: storageManager && storageManager.driveCapacityBytes > 0 ? storageManager.driveCapacityBytes / (1024 * 1024 * 1024) : 500; stepSize: 0.1
                                            value: settingsManager ? (settingsManager.maxStorageSize / (1024 * 1024 * 1024)) : 50
                                            onMoved: {
                                                if (!pressed && settingsManager)
                                                    settingsManager.maxStorageSize = Math.round(value * 1024 * 1024 * 1024);
                                            }
                                            onPressedChanged: {
                                                if (!pressed && settingsManager)
                                                    settingsManager.maxStorageSize = Math.round(value * 1024 * 1024 * 1024);
                                            }
                                        }
                                        Label { text: mainStorageSlider.value === 0 ? qsTr("No Limit") : mainStorageSlider.value < 1 ? mainStorageSlider.value.toFixed(1) + " GB" : Math.round(mainStorageSlider.value) + " GB"; color: Theme.textColor; font.weight: Font.Medium; Layout.preferredWidth: 70 }
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
                            spacing: Theme.spacingMedium
                            Item { Layout.fillHeight: true }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 2
                                Label { text: qsTr("Audio Configuration"); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeLarge; font.weight: Font.Bold; color: Theme.textColor }
                                Label { text: qsTr("Select your primary audio sources."); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                            }
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: audioCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                ColumnLayout {
                                    id: audioCol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: Theme.spacingMedium; anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingSmall
                                    Label { text: qsTr("Microphone"); font.weight: Font.Bold; color: Theme.textColor }
                                    TrophyComboBox {
                                        id: micBox; Layout.fillWidth: true
                                        property var devices: audioDeviceEnumerator ? audioDeviceEnumerator.inputDevices : []
                                        model: { let names = ["Default Input"]; for (let d of devices) { if (d.id !== "default") names.push(d.name); } return names; }
                                        Binding {
                                            target: micBox
                                            property: "currentIndex"
                                            value: (function() {
                                                if (!settingsManager || !audioDeviceEnumerator) return 0;
                                                let devs = audioDeviceEnumerator.inputDevices;
                                                let saved = settingsManager.microphoneDevice;
                                                for (let i = 0; i < devs.length; i++) { if (devs[i].id === saved) return i; }
                                                return 0;
                                            })()
                                        }
                                        onActivated: if (settingsManager && currentIndex < devices.length) settingsManager.microphoneDevice = devices[currentIndex].id
                                    }
                                    Label { text: qsTr("Audio Capture Mode"); font.weight: Font.Bold; color: Theme.textColor }
                                    RowLayout {
                                        spacing: Theme.spacingLarge
                                        TrophyRadioButton { text: qsTr("Game + Voice"); checked: settingsManager ? settingsManager.audioMode === "game" : true; enabled: !(recordingManager && recordingManager.isRecording); opacity: enabled ? 1.0 : 0.5; onClicked: if (settingsManager) settingsManager.audioMode = "game" }
                                        TrophyRadioButton { text: qsTr("Entire Desktop"); checked: settingsManager ? settingsManager.audioMode === "desktop" : false; enabled: !(recordingManager && recordingManager.isRecording); opacity: enabled ? 1.0 : 0.5; onClicked: if (settingsManager) settingsManager.audioMode = "desktop" }
                                    }
                                    Label { text: qsTr("Recording Tracks"); font.weight: Font.Bold; color: Theme.textColor; visible: settingsManager && settingsManager.audioMode === "game" }
                                    RowLayout {
                                        visible: settingsManager && settingsManager.audioMode === "game"
                                        spacing: Theme.spacingLarge
                                        TrophyRadioButton { text: qsTr("Single (Mixed)"); checked: settingsManager ? !settingsManager.separateAudioTracks : true; enabled: !(recordingManager && recordingManager.isRecording); opacity: enabled ? 1.0 : 0.5; onClicked: if (settingsManager) settingsManager.separateAudioTracks = false }
                                        TrophyRadioButton { text: qsTr("Separate (Multi)"); checked: settingsManager ? settingsManager.separateAudioTracks : false; enabled: !(recordingManager && recordingManager.isRecording); opacity: enabled ? 1.0 : 0.5; onClicked: if (settingsManager) settingsManager.separateAudioTracks = true }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        visible: settingsManager && settingsManager.audioMode === "game"
                                        spacing: Theme.spacingSmall
                                        Label { text: qsTr("Voice Chat Application"); font.weight: Font.Bold; color: Theme.textColor }
                                        TrophyComboBox {
                                            id: voiceAppBox; Layout.fillWidth: true
                                            property var apps: audioDeviceEnumerator ? audioDeviceEnumerator.audioApps : []
                                            model: {
                                                let names = ["None"];
                                                for (let a of apps) { names.push(a.name); }
                                                let saved = settingsManager ? settingsManager.voiceChatApp : "";
                                                if (saved && saved !== "") {
                                                    let found = false;
                                                    for (let a of apps) {
                                                        if (a.binary && a.binary.toLowerCase() === saved.toLowerCase()) { found = true; break; }
                                                    }
                                                    if (!found) names.push(saved);
                                                }
                                                return names;
                                            }
                                            Binding {
                                                target: voiceAppBox
                                                property: "currentIndex"
                                                value: (function() {
                                                    if (!settingsManager || !audioDeviceEnumerator) return 0;
                                                    let apps = audioDeviceEnumerator.audioApps;
                                                    let saved = settingsManager.voiceChatApp;
                                                    if (!saved || saved === "") return 0;
                                                    for (let i = 0; i < apps.length; i++) {
                                                        if (apps[i].binary && apps[i].binary.toLowerCase() === saved.toLowerCase()) return i + 1;
                                                    }
                                                    return apps.length + 1;
                                                })()
                                            }
                                            onActivated: {
                                                if (settingsManager && audioDeviceEnumerator) {
                                                    if (currentIndex === 0) {
                                                        settingsManager.voiceChatApp = "";
                                                    } else if (currentIndex > 0 && currentIndex <= apps.length) {
                                                        settingsManager.voiceChatApp = apps[currentIndex - 1].binary;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            Item { Layout.fillHeight: true }
                        }
                    }

                    /// Step 4: Optional Modules
                    ScrollView {
                        id: step4Scroll
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        clip: true
                        property bool isKwin: windowMonitor ? windowMonitor.compositorName.toLowerCase().indexOf("kwin") !== -1 : false
                        ColumnLayout {
                            width: step4Scroll.availableWidth
                            height: Math.max(implicitHeight, step4Scroll.availableHeight)
                            spacing: Theme.spacingMedium
                            Item { Layout.fillHeight: true }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 2
                                Label { text: qsTr("Optional Modules"); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeLarge; font.weight: Font.Bold; color: Theme.textColor }
                                Label { text: qsTr("Three small helpers that spot your games — all optional."); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                            }
                            // Module 1: window tracking (KWin script on KDE)
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: modTrackCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                ColumnLayout {
                                    id: modTrackCol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: Theme.spacingMedium; anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingSmall
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacingMedium
                                        Label { text: qsTr("1. Window tracking — %1").arg(windowMonitor ? windowMonitor.compositorName : qsTr("unknown compositor")); font.weight: Font.Bold; color: Theme.textColor; Layout.fillWidth: true; elide: Text.ElideRight }
                                        Label { text: (windowMonitor && step4Scroll.isKwin && windowMonitor.kwinScriptInstalled) ? qsTr("Installed") : ((settingsManager && settingsManager.windowTrackingEnabled) ? qsTr("Installing…") : qsTr("Not installed")); color: Theme.textColor; font.pixelSize: Theme.fontSizeSmall }
                                        TrophySwitch { checked: settingsManager ? settingsManager.windowTrackingEnabled : false; onToggled: if (settingsManager) settingsManager.windowTrackingEnabled = checked }
                                    }
                                    Label { text: qsTr("Sees window titles, focus and fullscreen state to name your games. On KDE this installs a tiny read-only script (removable in KWin Scripts)."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                    Label {
                                        visible: !step4Scroll.isKwin
                                        text: qsTr("Compositor-specific tracking for %1 is coming soon.").arg(windowMonitor ? windowMonitor.compositorName : qsTr("this system"))
                                        font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                    }
                                }
                            }
                            // Module 2: Vulkan detection layer
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: modVulkanCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                ColumnLayout {
                                    id: modVulkanCol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: Theme.spacingMedium; anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingSmall
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacingMedium
                                        Label { text: qsTr("2. Vulkan detection layer"); font.weight: Font.Bold; color: Theme.textColor; Layout.fillWidth: true; elide: Text.ElideRight }
                                        Label { text: app && app.vulkanLayerRegistered ? qsTr("Installed") : qsTr("Not installed"); color: Theme.textColor; font.pixelSize: Theme.fontSizeSmall }
                                        TrophySwitch { checked: settingsManager ? settingsManager.vulkanLayerEnabled : false; onToggled: if (settingsManager && app) { settingsManager.vulkanLayerEnabled = checked; app.setVulkanLayerEnabled(checked) } }
                                    }
                                    Label { text: qsTr("Spots the instant a game starts Vulkan graphics, including Proton titles. Detection only — changes nothing."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                }
                            }
                            // Module 3: OpenGL detection shim (opt-in)
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: modGlCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                ColumnLayout {
                                    id: modGlCol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: Theme.spacingMedium; anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingSmall
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacingMedium
                                        enabled: app && app.glShimAvailable
                                        Label { text: qsTr("3. OpenGL detection shim"); font.weight: Font.Bold; color: Theme.textColor; Layout.fillWidth: true; elide: Text.ElideRight }
                                        Label { text: (app && app.glShimEnrolled) ? qsTr("Installed") : qsTr("Not installed"); color: Theme.textColor; font.pixelSize: Theme.fontSizeSmall }
                                        TrophySwitch { checked: app ? app.glShimEnrolled : false; onToggled: if (app) app.setGlShimEnrolled(checked) }
                                    }
                                    Label { text: qsTr("Session-wide LD_PRELOAD: loads into unrelated programs too, not just OpenGL games. Enabling or disabling takes effect after logging out and back in. Leave off for anti-cheat games; disabling detection does not unload the library. Compatibility is not guaranteed."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; wrapMode: Text.WordWrap; Layout.fillWidth: true; opacity: (app && app.glShimAvailable) ? 1.0 : 0.5 }
                                    Label { visible: app && !app.glShimAvailable; text: qsTr("Shim library not found in this install."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                                }
                            }
                            Item { Layout.fillHeight: true }
                        }
                    }

                    /// Step 5: Hardware & Performance
                    ScrollView {
                        id: stepHwScroll
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        clip: true
                        // Preferred hardware family for explicit codec picks.
                        property string hwFamily: hardwareInfo ? (hardwareInfo.nvencAvailable ? "nvenc" : (hardwareInfo.vaapiAvailable ? "vaapi" : "")) : ""
                        function currentCodec() {
                            if (!settingsManager || settingsManager.encoder === "auto") return "auto";
                            var e = settingsManager.encoder;
                            if (e.indexOf("av1") !== -1) return "av1";
                            if (e.indexOf("hevc") !== -1) return "hevc";
                            return "h264";
                        }
                        function codecAvailable(c) {
                            if (c === "h264" || c === "auto") return true;
                            if (!app || !hardwareInfo) return false;
                            var fams = [];
                            if (hardwareInfo.nvencAvailable) fams.push("nvenc");
                            if (hardwareInfo.vaapiAvailable) fams.push("vaapi");
                            for (var i = 0; i < fams.length; i++) {
                                if (app.isEncoderAvailable(fams[i] + "_" + c)) return true;
                            }
                            return false;
                        }
                        function setCodec(c) {
                            if (!settingsManager) return;
                            if (c === "auto") { settingsManager.encoder = "auto"; return; }
                            if (stepHwScroll.hwFamily === "") { settingsManager.encoder = "x264"; return; }
                            var candidate = stepHwScroll.hwFamily + "_" + c;
                            settingsManager.encoder = (app && !app.isEncoderAvailable(candidate)) ? "auto" : candidate;
                        }
                        ColumnLayout {
                            width: stepHwScroll.availableWidth
                            height: Math.max(implicitHeight, stepHwScroll.availableHeight)
                            spacing: Theme.spacingMedium
                            Item { Layout.fillHeight: true }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 2
                                Label { text: qsTr("Your Hardware"); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeLarge; font.weight: Font.Bold; color: Theme.textColor }
                                Label { text: qsTr("What Trophy found, and what it means for recording performance."); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                            }
                            // GPU hero card
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: gpuHeroCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                RowLayout {
                                    id: gpuHeroCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spacingMedium
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingMedium
                                    Image {
                                        Layout.preferredWidth: 56
                                        Layout.preferredHeight: 56
                                        sourceSize: Qt.size(56, 56)
                                        source: {
                                            if (!hardwareInfo) return "qrc:/icons/gpu.svg";
                                            if (hardwareInfo.hasNvidiaGpu) return "qrc:/icons/gpu-nvidia.svg";
                                            if (hardwareInfo.hasAmdGpu) return "qrc:/icons/gpu-amd.svg";
                                            if (hardwareInfo.hasIntelGpu) return "qrc:/icons/gpu-intel.svg";
                                            return "qrc:/icons/gpu.svg";
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Label {
                                            text: (hardwareInfo && hardwareInfo.gpuDescriptions.length > 0) ? hardwareInfo.gpuDescriptions[0] : qsTr("No GPU detected")
                                            font.pixelSize: Theme.fontSizeMedium
                                            font.weight: Font.Bold
                                            color: Theme.textColor
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Label {
                                            text: {
                                                if (!hardwareInfo || hardwareInfo.gpuDescriptions.length === 0) return qsTr("Software encoding will be used");
                                                var vendor = hardwareInfo.hasNvidiaGpu ? "NVIDIA" : (hardwareInfo.hasAmdGpu ? "AMD" : (hardwareInfo.hasIntelGpu ? "Intel" : ""));
                                                var extra = hardwareInfo.gpuDescriptions.length > 1 ? (" · +" + (hardwareInfo.gpuDescriptions.length - 1)) : "";
                                                return (vendor !== "" ? vendor + " graphics" : qsTr("Graphics")) + extra;
                                            }
                                            font.pixelSize: Theme.fontSizeSmall
                                            color: Theme.textSecondaryColor
                                        }
                                    }
                                }
                            }
                            // Encoder cards — only detected hardware is shown,
                            // stacked (a row with fillWidth children makes
                            // layouts ignore preferredWidth). Neither
                            // detected: a software card explains x264.
                            Rectangle {
                                visible: hardwareInfo && hardwareInfo.nvencAvailable
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: nvencCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.successColor
                                ColumnLayout {
                                    id: nvencCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spacingMedium
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2
                                    RowLayout {
                                        spacing: Theme.spacingSmall
                                        Label { text: "✓"; color: Theme.successColor; font.weight: Font.Bold }
                                        Label { text: qsTr("NVENC"); font.weight: Font.Bold; color: Theme.textColor }
                                    }
                                    Label { text: qsTr("NVIDIA dedicated chip — barely touches game FPS."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                }
                            }
                            Rectangle {
                                visible: hardwareInfo && hardwareInfo.vaapiAvailable
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: vaapiCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.successColor
                                ColumnLayout {
                                    id: vaapiCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spacingMedium
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2
                                    RowLayout {
                                        spacing: Theme.spacingSmall
                                        Label { text: "✓"; color: Theme.successColor; font.weight: Font.Bold }
                                        Label { text: qsTr("VA-API"); font.weight: Font.Bold; color: Theme.textColor }
                                    }
                                    Label { text: qsTr("AMD/Intel media engine — small FPS cost."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                }
                            }
                            Rectangle {
                                visible: hardwareInfo && !hardwareInfo.nvencAvailable && !hardwareInfo.vaapiAvailable
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: swCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                ColumnLayout {
                                    id: swCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spacingMedium
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2
                                    Label { text: qsTr("Software encoding (x264)"); font.weight: Font.Bold; color: Theme.textColor }
                                    Label { text: qsTr("No hardware encoder found — works everywhere, but costs CPU and FPS."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                }
                            }
                            // Your choice: codec + zero-copy (neutral card)
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: choiceCol.height + (Theme.spacingMedium * 2)
                                radius: Theme.radiusMedium
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                border.color: Theme.borderColor
                                ColumnLayout {
                                    id: choiceCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spacingMedium
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingSmall
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacingMedium
                                        Label { text: qsTr("Codec"); font.weight: Font.Bold; color: Theme.textColor; Layout.fillWidth: true }
                                        Label {
                                            text: settingsManager ? (settingsManager.encoder === "auto" ? qsTr("Auto — %1").arg(hardwareInfo ? hardwareInfo.recommendedEncoderDisplayName : "x264") : settingsManager.encoderDisplayName) : ""
                                            font.pixelSize: Theme.fontSizeSmall
                                            color: Theme.textSecondaryColor
                                        }
                                    }
                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: 2
                                        columnSpacing: Theme.spacingMedium
                                        rowSpacing: Theme.spacingSmall
                                        TrophyRadioButton { text: qsTr("Automatic (recommended)"); checked: stepHwScroll.currentCodec() === "auto"; enabled: !(recordingManager && recordingManager.isRecording); opacity: enabled ? 1.0 : 0.5; onClicked: stepHwScroll.setCodec("auto") }
                                        TrophyRadioButton { text: qsTr("H.264 — widest support"); checked: stepHwScroll.currentCodec() === "h264"; enabled: !(recordingManager && recordingManager.isRecording); opacity: enabled ? 1.0 : 0.5; onClicked: stepHwScroll.setCodec("h264") }
                                        TrophyRadioButton { text: qsTr("HEVC — smaller files"); checked: stepHwScroll.currentCodec() === "hevc"; enabled: stepHwScroll.codecAvailable("hevc") && !(recordingManager && recordingManager.isRecording); opacity: enabled ? 1.0 : 0.5; onClicked: stepHwScroll.setCodec("hevc") }
                                        TrophyRadioButton { text: qsTr("AV1 — best quality"); checked: stepHwScroll.currentCodec() === "av1"; enabled: stepHwScroll.codecAvailable("av1") && !(recordingManager && recordingManager.isRecording); opacity: enabled ? 1.0 : 0.5; onClicked: stepHwScroll.setCodec("av1") }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacingMedium
                                        TrophySwitch {
                                            checked: settingsManager ? settingsManager.useDmaBuf : true
                                            enabled: (hardwareInfo ? hardwareInfo.dmaBufAvailable : false) && !(recordingManager && recordingManager.isRecording)
                                            opacity: enabled ? 1.0 : 0.5
                                            onToggled: if (settingsManager) settingsManager.useDmaBuf = checked
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Label { text: qsTr("Zero-copy capture"); font.weight: Font.Bold; color: Theme.textColor }
                                            Label { text: (hardwareInfo && hardwareInfo.dmaBufAvailable) ? qsTr("Frames go straight to the encoder.") : qsTr("Not available on this system."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                                        }
                                    }
                                }
                            }
                            Item { Layout.fillHeight: true }
                        }
                    }

                    /// Step 6: Keyboard Shortcuts
                    ScrollView {
                        id: stepKeysScroll
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        clip: true
                        // Bound session vs. actually usable keys: the portal
                        // can report a session whose triggers are empty
                        // (user cleared them). Only hasKeys means ready.
                        property bool portalBound: globalShortcuts && globalShortcuts.bound
                        property string toggleKey: (globalShortcuts && globalShortcuts.boundShortcuts["toggle-recording"]) || ""
                        property string clipKey: (globalShortcuts && globalShortcuts.boundShortcuts["save-clip"]) || ""
                        property bool hasKeys: toggleKey !== "" && clipKey !== ""
                        ColumnLayout {
                            width: stepKeysScroll.availableWidth
                            height: Math.max(implicitHeight, stepKeysScroll.availableHeight)
                            spacing: Theme.spacingMedium
                            Item { Layout.fillHeight: true }
                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 2
                                Label { text: qsTr("Keyboard Shortcuts"); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeLarge; font.weight: Font.Bold; color: Theme.textColor }
                                Label { text: qsTr("Two keys that work even inside your game."); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                            }
                            // User-assigned recording key
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: f9Col.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                RowLayout {
                                    id: f9Col
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spacingMedium
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingMedium
                                    Label { text: stepKeysScroll.toggleKey !== "" ? stepKeysScroll.toggleKey : qsTr("N/A"); font.pixelSize: Theme.fontSizeLarge; font.weight: Font.Bold; color: Theme.primaryColor; Layout.preferredWidth: 64; Layout.minimumWidth: 64; elide: Text.ElideRight }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Label { text: qsTr("Start / stop recording"); font.weight: Font.Bold; color: Theme.textColor }
                                        Label { text: qsTr("Works from anywhere — even in-game."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                                    }
                                }
                            }
                            // User-assigned clip key
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: f10Col.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor
                                RowLayout {
                                    id: f10Col
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spacingMedium
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingMedium
                                    Label { text: stepKeysScroll.clipKey !== "" ? stepKeysScroll.clipKey : qsTr("N/A"); font.pixelSize: Theme.fontSizeLarge; font.weight: Font.Bold; color: Theme.primaryColor; Layout.preferredWidth: 64; Layout.minimumWidth: 64; elide: Text.ElideRight }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Label { text: qsTr("Save a clip"); font.weight: Font.Bold; color: Theme.textColor }
                                        Label { text: qsTr("Saves the last moments instantly."); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                                    }
                                }
                            }
                            // Enable card
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                implicitHeight: keysEnableCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: stepKeysScroll.hasKeys ? Theme.successColor : Theme.borderColor
                                ColumnLayout {
                                    id: keysEnableCol
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: Theme.spacingMedium; anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingSmall
                                    Label {
                                        text: stepKeysScroll.hasKeys ? qsTr("Enabled — remap anytime in Settings → Keyboard Shortcuts.") : (stepKeysScroll.portalBound ? qsTr("Almost there — the system came back with no keys chosen. Pick them in the dialog.") : qsTr("Not enabled yet. Nothing will pop up until you press the button."))
                                        font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                    }
                                    Label {
                                        visible: globalShortcuts && !globalShortcuts.available
                                        text: qsTr("Your system doesn't offer portal shortcuts — bind \"trophy --toggle-record\" and \"trophy --save-clip\" in your compositor's own key settings instead.")
                                        font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; wrapMode: Text.WordWrap; Layout.fillWidth: true
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacingMedium
                                        TrophyButton {
                                            text: stepKeysScroll.hasKeys ? qsTr("Change keys") : (stepKeysScroll.portalBound ? qsTr("Choose keys") : qsTr("Enable global shortcuts"))
                                            highlighted: !stepKeysScroll.hasKeys
                                            onClicked: {
                                                if (settingsManager) settingsManager.globalShortcutsEnabled = true;
                                                if (globalShortcuts) globalShortcuts.ensureSetup();
                                            }
                                        }
                                    }
                                    Label {
                                        visible: !stepKeysScroll.hasKeys
                                        text: qsTr("Skip this and Trophy will ask on next launch instead.")
                                        font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor
                                    }
                                }
                            }
                            Item { Layout.fillHeight: true }
                        }
                    }

                    /// Step 7: Finish
                    ScrollView {
                        id: step5Scroll
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        clip: true
                        ColumnLayout {
                            width: step5Scroll.availableWidth
                            height: Math.max(implicitHeight, step5Scroll.availableHeight)
                            spacing: Theme.spacingMedium
                            Item { Layout.fillHeight: true }

                            ColumnLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 2
                                Label { text: "✨"; Layout.alignment: Qt.AlignHCenter; font.pixelSize: 48 }
                                Label { text: qsTr("You're All Set!"); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeTitle; font.weight: Font.Bold; color: Theme.textColor }
                                Label { text: qsTr("Trophy is now configured and ready to go."); Layout.alignment: Qt.AlignHCenter; font.pixelSize: Theme.fontSizeMedium; color: Theme.textSecondaryColor }
                            }

                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 500
                                // Allow height to be determined by children + margins
                                implicitHeight: finishCol.height + (Theme.spacingMedium * 2)
                                color: Theme.withOpacity(Theme.textColor, 0.05)
                                radius: Theme.radiusMedium
                                border.color: Theme.borderColor

                                ColumnLayout {
                                    id: finishCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spacingMedium
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: Theme.spacingMedium // Gap between toggle boxes and tip box

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

                                    // Box 2: Start on login
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacingMedium
                                        enabled: app ? app.trayAvailable : true
                                        opacity: enabled ? 1.0 : 0.5
                                        TrophySwitch {
                                            checked: settingsManager ? settingsManager.startOnLogin : false
                                            onToggled: if (settingsManager) settingsManager.startOnLogin = checked
                                        }
                                        ColumnLayout {
                                            spacing: 0
                                            Layout.fillWidth: true
                                            Label { text: qsTr("Start on login"); font.weight: Font.Bold; color: Theme.textColor }
                                            Label {
                                                text: qsTr("Launch automatically when you log in (starts minimized to the tray).");
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
                                                text: (globalShortcuts && globalShortcuts.boundShortcuts["toggle-recording"]) ? qsTr("Tip: Press %1 while in-game to start or stop a manual recording.").arg(globalShortcuts.boundShortcuts["toggle-recording"]) : qsTr("Tip: Press your recording key while in-game to start or stop a manual recording.")
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
        currentFolder: settingsManager ? recordingLibrary.localFileUrl(settingsManager.recordingsPath) : ""
        onAccepted: {
            var path = selectedFolder.toLocalFile();
            if (settingsManager) settingsManager.recordingsPath = path;
        }
    }
}
