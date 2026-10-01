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

    // Section navigation (titles must match main.qml global search mapping)
    property string activeSection: qsTr("Video Recording")
    property bool scrollingProgrammatically: false
    property var sections: [
        { title: qsTr("Video Recording"), icon: "🎥" },
        { title: qsTr("Audio Recording"), icon: "🔊" },
        { title: qsTr("Storage Management"), icon: "💾" },
        { title: qsTr("Game Detection"), icon: "🎮" },
        { title: qsTr("System Tray"), icon: "📌" },
        { title: qsTr("Keyboard Shortcuts"), icon: "⌨️" }
    ]

    function scrollToSection(sectionTitle) {
        var card = null;
        if (sectionTitle === "Video Recording")
            card = videoCard;
        else if (sectionTitle === "Audio Recording")
            card = audioCard;
        else if (sectionTitle === "Storage Management")
            card = storageCard;
        else if (sectionTitle === "Game Detection")
            card = detectionCard;
        else if (sectionTitle === "System Tray")
            card = trayCard;
        else if (sectionTitle === "Keyboard Shortcuts")
            card = shortcutsCard;

        if (!card)
            return;

        activeSection = sectionTitle;
        scrollingProgrammatically = true;
        scrollClearTimer.restart();
        var target = Math.max(0, card.y - Theme.spacingSmall);
        var maxY = Math.max(0, settingsFlick.contentHeight - settingsFlick.height);
        settingsFlick.contentY = Math.min(target, maxY);
    }

    function updateActiveSectionFromScroll() {
        if (scrollingProgrammatically)
            return;

        var y = settingsFlick.contentY + 140;
        var current = "Video Recording";
        if (shortcutsCard && y >= shortcutsCard.y)
            current = "Keyboard Shortcuts";
        else if (trayCard && y >= trayCard.y)
            current = "System Tray";
        else if (detectionCard && y >= detectionCard.y)
            current = "Game Detection";
        else if (storageCard && y >= storageCard.y)
            current = "Storage Management";
        else if (audioCard && y >= audioCard.y)
            current = "Audio Recording";

        activeSection = current;
    }

    function formatSize(bytes) {
        if (!bytes || bytes <= 0)
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

    property real previousStorageLimitGB: settingsManager ? settingsManager.maxStorageSize / (1024 * 1024 * 1024) : 50
    property bool previousAutoCleanupChecked: settingsManager ? settingsManager.autoDeleteOldest : false
    property real pendingStorageLimitGB: 0
    property bool pendingAutoCleanupEnabled: false
    property bool autoCleanupConfirmPending: false
    property string storageConfirmTitle: ""
    property string storageConfirmMessage: ""
    property string storageConfirmConfirmText: qsTr("Confirm")
    property var storageConfirmAction: function() {}

    function openStorageConfirmDialog(title, message, confirmText, callback) {
        storageConfirmTitle = title;
        storageConfirmMessage = message;
        storageConfirmConfirmText = confirmText;
        storageConfirmAction = callback;
        storageLimitConfirmDialog.open();
    }

    function isCurrentUsageAbove(bytes) {
        return storageManager && bytes > 0 && storageManager.usedSize > bytes;
    }

    function applyStorageLimit(valueGB) {
        if (!settingsManager || !storageManager) {
            return;
        }

        var bytes = Math.round(valueGB * 1024 * 1024 * 1024);
        if (isCurrentUsageAbove(bytes)) {
            pendingStorageLimitGB = valueGB;
            var autoCleanupActive = storageManager.autoDeleteOldest;
            var message = autoCleanupActive ?
                qsTr("Your recordings already exceed the selected limit. Old recordings will be deleted until usage fits the new limit.") :
                qsTr("Your recordings already exceed the selected limit. Old recordings can be deleted once to fit the new limit without enabling Auto-Cleanup.");

            openStorageConfirmDialog(
                qsTr("Storage limit is below current usage"),
                message,
                autoCleanupActive ? qsTr("Apply and cleanup") : qsTr("Apply and cleanup once"),
                function() {
                    app.suppressNextStorageNotification = true;
                    settingsManager.maxStorageSize = bytes;
                    storageManager.requestCleanup(true);
                    previousStorageLimitGB = pendingStorageLimitGB;
                }
            );
            return;
        }

        settingsManager.maxStorageSize = bytes;
        previousStorageLimitGB = valueGB;
    }

    function setAutoCleanupEnabled(enabled) {
        if (!settingsManager || !storageManager) {
            return;
        }

        if (enabled && storageManager.maxSize > 0 && storageManager.usedSize > storageManager.maxSize) {
            pendingAutoCleanupEnabled = enabled;
            autoCleanupConfirmPending = true;
            openStorageConfirmDialog(
                qsTr("Enable Auto-Cleanup?"),
                qsTr("Storage is already over the current limit. Enabling Auto-Cleanup will delete the oldest recordings until usage fits the limit."),
                qsTr("Enable Auto-Cleanup"),
                function() {
                    app.suppressNextStorageNotification = true;
                    settingsManager.autoDeleteOldest = true;
                    storageManager.requestCleanup();
                    previousAutoCleanupChecked = true;
                }
            );
            storageAutoCleanupSwitch.checked = previousAutoCleanupChecked;
            return;
        }

        settingsManager.autoDeleteOldest = enabled;
        previousAutoCleanupChecked = enabled;
    }

    Connections {
        target: settingsManager
        function onMaxStorageSizeChanged() {
            storageSlider.value = settingsManager.maxStorageSize / (1024 * 1024 * 1024);
            previousStorageLimitGB = storageSlider.value;
        }
        function onAutoDeleteOldestChanged() {
            storageAutoCleanupSwitch.checked = settingsManager.autoDeleteOldest;
            previousAutoCleanupChecked = storageAutoCleanupSwitch.checked;
        }
    }

    Timer {
        id: scrollClearTimer
        interval: 400
        repeat: false
        onTriggered: scrollingProgrammatically = false
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        anchors.bottomMargin: Theme.spacingMedium
        spacing: Theme.spacingMedium

        // --- HEADER ---
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                text: qsTr("Settings")
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Font.Bold
                color: Theme.textColor
            }

            Label {
                text: qsTr("Tune capture quality, audio, storage and detection.")
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondaryColor
            }
        }

        // --- SECTION PILLS ---
        Flickable {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            contentWidth: pillsRow.implicitWidth
            contentHeight: 36
            flickableDirection: Flickable.HorizontalFlick
            clip: true

            Row {
                id: pillsRow
                height: 36
                spacing: Theme.spacingSmall

                Repeater {
                    model: root.sections

                    Rectangle {
                        property bool isActive: root.activeSection === modelData.title

                        height: 32
                        width: pillContent.implicitWidth + Theme.spacingMedium * 2
                        radius: 16
                        color: isActive ? Theme.withOpacity(Theme.primaryColor, 0.15) : Theme.withOpacity(Theme.textColor, 0.05)
                        border.width: 1
                        border.color: isActive ? Theme.primaryColor : Theme.withOpacity(Theme.textColor, 0.12)

                        Row {
                            id: pillContent
                            anchors.centerIn: parent
                            spacing: 6

                            Label {
                                text: modelData.icon
                                font.pixelSize: Theme.fontSizeSmall
                                anchors.verticalCenter: parent.verticalCenter
                            }

                            Label {
                                text: modelData.title
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: isActive ? Font.Medium : Font.Normal
                                color: isActive ? Theme.primaryColor : Theme.textSecondaryColor
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.scrollToSection(modelData.title)
                        }
                    }
                }
            }
        }

        // --- CONTENT ---
        Flickable {
            id: settingsFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: contentCol.implicitHeight + Theme.spacingLarge
            clip: true
            onContentYChanged: root.updateActiveSectionFromScroll()

            Behavior on contentY {
                NumberAnimation {
                    duration: 250
                    easing.type: Easing.OutCubic
                }
            }

            ScrollBar.vertical: ScrollBar {
                id: settingsVBar
                active: hovered || pressed
                width: 8

                contentItem: Rectangle {
                    implicitWidth: 4
                    implicitHeight: 40
                    radius: 16
                    color: settingsVBar.pressed ? Theme.highlightedColor : (settingsVBar.hovered ? Theme.highlightedColor : Theme.secondaryColor)
                    opacity: settingsVBar.active ? 1.0 : 0.2
                    Behavior on opacity { NumberAnimation { duration: 200 } }
                }

                background: Rectangle {
                    color: "transparent"
                }
            }

            ColumnLayout {
                id: contentCol
                width: Math.min(parent.width - 12, 800)
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spacingLarge

                // ================= VIDEO =================
                SectionCard {
                    id: videoCard
                    sectionTitle: qsTr("Video Recording")
                    iconText: "🎥"
                    description: qsTr("Encoder, quality and output format. Changes apply to the next recording.")

                    WarnBanner {
                        message: qsTr("Recording in progress. To change these settings, stop recording.")
                        visible: recordingManager && recordingManager.isRecording
                    }

                    SettingRow {
                        label: qsTr("Video Encoder")
                        help: qsTr("Auto picks the best hardware encoder (NVENC, VA-API, or AV1). AV1 gives the best quality but needs modern hardware.")

                        TrophyComboBox {
                            id: encoderBox

                            property var values: ["auto", "nvenc_h264", "nvenc_hevc", "nvenc_av1", "vaapi_h264", "vaapi_hevc", "vaapi_av1", "x264"]

                            Layout.preferredWidth: 300
                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
                            model: ["Auto (Recommended)", "NVENC H.264", "NVENC HEVC", "NVENC AV1", "VA-API H.264", "VA-API HEVC", "VA-API AV1", "x264 Software"]

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
                                    if (savedIdx !== -1 && currentIndex !== savedIdx) {
                                        currentIndex = savedIdx;
                                    }
                                }
                            }

                            delegate: ItemDelegate {
                                id: encDelegate

                                readonly property bool isAvailable: app.isEncoderAvailable(encoderBox.values[index])
                                enabled: isAvailable

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

                    SettingStack {
                        label: qsTr("Video Bitrate")
                        help: qsTr("Controls video quality. 10,000–15,000 kbps is great for 1080p. Use 20,000+ kbps for 1440p/4K.")

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
                                    if (!pressed && settingsManager)
                                        settingsManager.videoBitrate = value;
                                }
                                onPressedChanged: {
                                    if (!pressed && settingsManager)
                                        settingsManager.videoBitrate = value;
                                }
                            }

                            ValuePill {
                                pillText: bitrateSlider.value.toFixed(0) + " kbps"
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Frame Rate")
                        help: qsTr("60 FPS is the standard for gameplay. Higher rates look smoother but increase file size.")

                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            Layout.preferredWidth: fpsSegments.implicitWidth + 8
                            Layout.preferredHeight: 36
                            radius: 18
                            color: Theme.withOpacity(Theme.textColor, 0.06)
                            border.width: 1
                            border.color: Theme.withOpacity(Theme.textColor, 0.12)
                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5

                            RowLayout {
                                id: fpsSegments
                                anchors.centerIn: parent
                                spacing: 2

                                Repeater {
                                    model: [30, 60, 120, 144]

                                    Rectangle {
                                        property bool isChecked: settingsManager ? settingsManager.fps === modelData : false

                                        Layout.preferredWidth: 72
                                        Layout.preferredHeight: 28
                                        radius: 14
                                        color: isChecked ? Theme.primaryColor : "transparent"

                                        Label {
                                            anchors.centerIn: parent
                                            text: modelData
                                            font.pixelSize: Theme.fontSizeSmall
                                            font.weight: isChecked ? Font.Medium : Font.Normal
                                            color: isChecked ? "white" : Theme.textSecondaryColor
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            enabled: !(recordingManager && recordingManager.isRecording)
                                            onClicked: {
                                                if (settingsManager)
                                                    settingsManager.fps = modelData;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Resolution")
                        help: qsTr("Native captures full resolution. Downscale to 720p if a demanding game needs the headroom.")

                        TrophyComboBox {
                            id: resolutionBox
                            property var values: ["native", "720x480", "1280x720", "1920x1080", "2560x1440", "3840x2160"]

                            Layout.preferredWidth: 300
                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
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

                    SettingRow {
                        label: qsTr("Encoder Preset")
                        help: qsTr("'Fast' balances quality and GPU load while gaming.")

                        TrophyComboBox {
                            id: presetBox
                            property var values: ["ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow"]

                            Layout.preferredWidth: 300
                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
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

                    SettingStack {
                        label: qsTr("Container Format")
                        help: qsTr("How the video file is stored on disk.")

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            OptionCard {
                                title: qsTr("MKV (Recommended)")
                                description: qsTr("Crash-resistant. Your recording survives power cuts and app crashes.")
                                selected: settingsManager ? settingsManager.containerFormat === "mkv" : true
                                cardEnabled: !(recordingManager && recordingManager.isRecording)
                                onChosen: if (settingsManager) settingsManager.containerFormat = "mkv"
                            }

                            OptionCard {
                                title: qsTr("MP4")
                                description: qsTr("Widely compatible but fragile — an unexpected stop can corrupt the whole file.")
                                selected: settingsManager ? settingsManager.containerFormat === "mp4" : false
                                danger: true
                                cardEnabled: !(recordingManager && recordingManager.isRecording)
                                onChosen: if (settingsManager) settingsManager.containerFormat = "mp4"
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Background Clips")
                        help: (globalShortcuts && globalShortcuts.boundShortcuts["save-clip"]) ? qsTr("Quietly record while you game, then press %1 to keep the last moments as a clip.").arg(globalShortcuts.boundShortcuts["save-clip"]) : qsTr("Quietly record while you game, then press your clip key to keep the last moments as a clip.")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.instantReplayEnabled : true
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.instantReplayEnabled = checked;
                            }
                        }
                    }

                    SettingStack {
                        label: qsTr("Clip Buffer Duration")
                        help: qsTr("How many seconds of recent video are kept for instant clipping.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium

                            TrophySlider {
                                id: clipBufferSlider
                                enabled: !(recordingManager && recordingManager.isRecording)
                                opacity: enabled ? 1 : 0.5
                                Layout.fillWidth: true
                                from: 5
                                to: 120
                                stepSize: 5
                                value: settingsManager ? settingsManager.clipBufferSeconds : 30
                                onMoved: {
                                    if (!pressed && settingsManager)
                                        settingsManager.clipBufferSeconds = value;
                                }
                                onPressedChanged: {
                                    if (!pressed && settingsManager)
                                        settingsManager.clipBufferSeconds = value;
                                }
                            }

                            ValuePill {
                                pillText: clipBufferSlider.value.toFixed(0) + " s"
                            }
                        }
                    }

                    SettingStack {
                        label: qsTr("Clip Cache Folder")
                        help: qsTr("Where background recording chunks live before you save a clip. Empty means /tmp (RAM, cleared on reboot); pick a folder to keep them on disk instead.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            TrophyTextField {
                                id: replayCachePathField

                                Layout.fillWidth: true
                                text: settingsManager ? settingsManager.replayCachePath : ""
                                placeholderText: qsTr("Default (/tmp, RAM-backed)")
                                readOnly: true
                            }

                            TrophyButton {
                                text: qsTr("Browse")
                                onClicked: replayFolderDialog.open()
                            }

                            TrophyButton {
                                text: qsTr("Default")
                                visible: replayCachePathField.text !== ""
                                onClicked: {
                                    if (settingsManager)
                                        settingsManager.replayCachePath = "";
                                }
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Zero-Copy Capture (DMA-BUF)")
                        help: qsTr("Hardware-accelerated capture with much lower CPU usage. Turn off if you see green screens or crashes.")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.useDmaBuf : true
                            enabled: !(recordingManager && recordingManager.isRecording)
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.useDmaBuf = checked;
                            }
                        }
                    }

                    SettingStack {
                        label: qsTr("Wayland Permissions")
                        help: qsTr("Wayland remembers your screen-sharing choice. Reset here to pick a different monitor or window next time.")

                        TrophyButton {
                            Layout.alignment: Qt.AlignLeft
                            text: qsTr("Reset Screen Capture Permissions")
                            enabled: !(recordingManager && recordingManager.isRecording)
                            onClicked: {
                                settingsManager.clearWaylandToken();
                                notificationService.showNotification(qsTr("Wayland Reset"), qsTr("Permissions cleared. You will be prompted to select a source next time you record."));
                            }
                        }
                    }
                }

                // ================= AUDIO =================
                SectionCard {
                    id: audioCard
                    sectionTitle: qsTr("Audio Recording")
                    iconText: "🔊"
                    description: qsTr("What gets heard in your recordings, and how tracks are stored.")

                    WarnBanner {
                        message: qsTr("Recording in progress. To change these settings, stop recording.")
                        visible: recordingManager && recordingManager.isRecording
                    }

                    SettingStack {
                        label: qsTr("Audio Mode")

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            OptionCard {
                                title: qsTr("Game Mode")
                                description: qsTr("Focused capture of game window audio and voice apps.")
                                selected: settingsManager ? settingsManager.audioMode === "game" : true
                                cardEnabled: !(recordingManager && recordingManager.isRecording)
                                onChosen: if (settingsManager) settingsManager.audioMode = "game"
                            }

                            OptionCard {
                                title: qsTr("Desktop Mode")
                                description: qsTr("Captures everything you hear on your system.")
                                selected: settingsManager ? settingsManager.audioMode === "desktop" : false
                                cardEnabled: !(recordingManager && recordingManager.isRecording)
                                onChosen: if (settingsManager) settingsManager.audioMode = "desktop"
                            }
                        }
                    }

                    SettingStack {
                        label: qsTr("Audio Track Configuration")

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            OptionCard {
                                title: qsTr("Single Track (Mixed)")
                                description: {
                                    let mode = settingsManager ? settingsManager.audioMode : "game";
                                    if (mode === "desktop")
                                        return qsTr("System audio and microphone mixed together into one track.");
                                    else
                                        return qsTr("Game, voice chat and microphone all mixed together into one track.");
                                }
                                selected: settingsManager ? !settingsManager.separateAudioTracks : true
                                cardEnabled: !(recordingManager && recordingManager.isRecording)
                                onChosen: if (settingsManager) settingsManager.separateAudioTracks = false
                            }

                            OptionCard {
                                title: qsTr("Separate Tracks")
                                description: {
                                    let mode = settingsManager ? settingsManager.audioMode : "game";
                                    if (mode === "desktop")
                                        return qsTr("System audio and microphone on separate tracks. Switch audio tracks in your media player to hear both.");
                                    else
                                        return qsTr("Game, voice chat and microphone on separate tracks. Switch audio tracks in your media player to hear all audio.");
                                }
                                selected: settingsManager ? settingsManager.separateAudioTracks : false
                                cardEnabled: !(recordingManager && recordingManager.isRecording)
                                onChosen: if (settingsManager) settingsManager.separateAudioTracks = true
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Microphone")
                        help: qsTr("Your voice input device.")

                        TrophyComboBox {
                            id: microphoneBox
                            Layout.preferredWidth: 300
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

                    SettingRow {
                        label: qsTr("System Audio Device")
                        help: qsTr("Where your sound comes from.")
                        visible: settingsManager ? settingsManager.audioMode === "desktop" : false

                        TrophyComboBox {
                            id: gameAudioBox
                            Layout.preferredWidth: 300
                            enabled: !(recordingManager && recordingManager.isRecording)
                            property var devices: audioDeviceEnumerator ? audioDeviceEnumerator.outputDevices : []

                            model: {
                                let names = ["Default Output"];
                                for (let d of devices) { if (d.id !== "default") names.push(d.name); }
                                names;
                            }

                            Binding {
                                target: gameAudioBox
                                property: "currentIndex"
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

                    SettingStack {
                        label: qsTr("Voice Chat App")
                        help: qsTr("Select from running audio applications.")
                        visible: settingsManager ? settingsManager.audioMode === "game" : true

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            TrophyComboBox {
                                id: voiceChatAppBox
                                Layout.fillWidth: true
                                enabled: !(recordingManager && recordingManager.isRecording)
                                property var apps: audioDeviceEnumerator ? audioDeviceEnumerator.audioApps : []

                                model: {
                                    let names = ["None"];
                                    for (let a of apps) {
                                        names.push(a.name);
                                    }
                                    names;
                                }

                                Binding {
                                    target: voiceChatAppBox
                                    property: "currentIndex"
                                    value: (function() {
                                        if (!settingsManager || !audioDeviceEnumerator) return 0;
                                        let apps = audioDeviceEnumerator.audioApps;
                                        let saved = settingsManager.voiceChatApp;
                                        if (!saved || saved === "") return 0;
                                        for (let i = 0; i < apps.length; i++) {
                                            if (apps[i].binary === saved) return i + 1;
                                        }
                                        return 0;
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

                                Connections {
                                    target: audioDeviceEnumerator
                                    function onAudioAppsChanged() {
                                        voiceChatAppBox.apps = audioDeviceEnumerator.audioApps;
                                    }
                                }
                            }

                            TrophyButton {
                                text: qsTr("Refresh")
                                enabled: audioDeviceEnumerator && !(recordingManager && recordingManager.isRecording)
                                onClicked: if (audioDeviceEnumerator) audioDeviceEnumerator.refreshAudioApps()
                            }
                        }
                    }
                }

                // ================= STORAGE =================
                SectionCard {
                    id: storageCard
                    sectionTitle: qsTr("Storage Management")
                    iconText: "💾"
                    description: qsTr("Where recordings live and what happens when space runs out.")

                    WarnBanner {
                        message: qsTr("Recording in progress. These settings will not take effect until the next recording.")
                        visible: recordingManager && recordingManager.isRecording
                    }

                    // Live usage overview (display only)
                    Rectangle {
                        Layout.fillWidth: true
                        radius: Theme.radiusSmall
                        color: Theme.withOpacity(Theme.textColor, 0.04)
                        border.width: 1
                        border.color: Theme.withOpacity(Theme.textColor, 0.1)
                        Layout.preferredHeight: usageCol.implicitHeight + Theme.spacingMedium * 2

                        ColumnLayout {
                            id: usageCol
                            anchors.fill: parent
                            anchors.margins: Theme.spacingMedium
                            spacing: Theme.spacingSmall

                            RowLayout {
                                Layout.fillWidth: true

                                Label {
                                    text: qsTr("Library usage")
                                    font.pixelSize: Theme.fontSizeSmall
                                    font.weight: Font.Medium
                                    color: Theme.textSecondaryColor
                                }

                                Item { Layout.fillWidth: true }

                                Label {
                                    text: storageManager ? (formatSize(storageManager.usedSize) + (storageManager.maxSize > 0 ? " / " + formatSize(storageManager.maxSize) : " / " + qsTr("Unlimited"))) : ""
                                    font.pixelSize: Theme.fontSizeSmall
                                    font.weight: Font.Medium
                                    color: Theme.textColor
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 8
                                radius: 4
                                color: Theme.withOpacity(Theme.textColor, 0.08)
                                clip: true
                                visible: storageManager ? storageManager.maxSize > 0 : false

                                Rectangle {
                                    height: parent.height
                                    radius: 4
                                    width: parent.width * ((storageManager && storageManager.maxSize > 0) ? Math.min(1, storageManager.usedSize / storageManager.maxSize) : 0)
                                    color: (storageManager && storageManager.maxSize > 0 && storageManager.usedSize > storageManager.maxSize) ? Theme.errorColor : Theme.primaryColor
                                    Behavior on width { NumberAnimation { duration: 400 } }
                                }
                            }
                        }
                    }

                    SettingStack {
                        label: qsTr("Recordings Folder")
                        help: qsTr("Where your recordings are saved.")

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

                    SettingStack {
                        label: qsTr("Storage Limit")
                        help: qsTr("Trophy deletes the oldest recordings past this limit when Auto-Cleanup is on, otherwise recording stops at the limit.")

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingMedium

                            TrophySlider {
                                id: storageSlider

                                Layout.fillWidth: true
                                from: 0
                                to: 500
                                stepSize: 0.1
                                value: settingsManager ? settingsManager.maxStorageSize / (1024 * 1024 * 1024) : 50
                                onPressedChanged: {
                                    if (!pressed && settingsManager)
                                        applyStorageLimit(value);
                                }
                            }

                            ValuePill {
                                pillText: storageSlider.value === 0 ? qsTr("No Limit") : storageSlider.value < 1 ? storageSlider.value.toFixed(1) + " GB" : Math.round(storageSlider.value) + " GB"
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Auto-Cleanup")
                        help: qsTr("Automatically delete the oldest recordings when you exceed the storage limit.")
                        rowEnabled: storageSlider.value > 0

                        TrophySwitch {
                            id: storageAutoCleanupSwitch
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.autoDeleteOldest : false
                            onToggled: {
                                if (!settingsManager)
                                    return;

                                if (autoCleanupConfirmPending) {
                                    autoCleanupConfirmPending = false;
                                    return;
                                }

                                setAutoCleanupEnabled(checked);
                            }
                        }
                    }
                }

                // ================= DETECTION =================
                SectionCard {
                    id: detectionCard
                    sectionTitle: qsTr("Game Detection")
                    iconText: "🎮"
                    description: qsTr("How Trophy notices your games and when it records them.")

                    SettingRow {
                        label: qsTr("Auto-detect Games")
                        help: qsTr("Trophy will try to detect when you launch a game.")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.autoDetectGames : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.autoDetectGames = checked;
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Window tracking")
                        help: qsTr("See window titles, focus and fullscreen state to name your games. On KDE this installs a read-only helper script (removable in System Settings → KWin Scripts).")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.windowTrackingEnabled : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.windowTrackingEnabled = checked;
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Vulkan detection layer")
                        help: qsTr("Standard Vulkan implicit layer that notices games at startup, including Proton titles. Install or remove it here.")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.vulkanLayerEnabled : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.vulkanLayerEnabled = checked;
                                if (app)
                                    app.setVulkanLayerEnabled(checked);
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("OpenGL detection shim")
                        help: (app && app.glShimAvailable) ? qsTr("Same idea for older OpenGL games. Takes effect after next login via LD_PRELOAD. Leave off for games with strict anti-cheat — some flag any injected library.") : qsTr("Shim library not found in this install.")
                        rowEnabled: app ? app.glShimAvailable : false

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: app ? app.glShimEnrolled : false
                            onToggled: {
                                if (app)
                                    app.setGlShimEnrolled(checked);
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Auto-record")
                        help: qsTr("Start recording the full game session the moment a game is detected. Never miss a moment!")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.autoRecordEnabled : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.autoRecordEnabled = checked;
                            }
                        }
                    }
                }

                // ================= TRAY =================
                SectionCard {
                    id: trayCard
                    sectionTitle: qsTr("System Tray")
                    iconText: "📌"
                    description: qsTr("How Trophy behaves when you close it or log in.")

                    SettingRow {
                        label: qsTr("Minimize to tray on close")
                        help: qsTr("Closing the window keeps Trophy (and any recording) running in the system tray instead of quitting.")
                        rowEnabled: app ? app.trayAvailable : false

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.minimizeToTray : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.minimizeToTray = checked;
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Start minimized")
                        help: qsTr("Launch Trophy directly into the system tray without showing the window.")
                        rowEnabled: app ? app.trayAvailable : false

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.startMinimized : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.startMinimized = checked;
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Start on login")
                        help: qsTr("Launch Trophy automatically when you log in (starts minimized to the tray). Pairs well with Start minimized.")
                        rowEnabled: app ? app.trayAvailable : false

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.startOnLogin : false
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.startOnLogin = checked;
                            }
                        }
                    }
                }

                // ================= SHORTCUTS =================
                SectionCard {
                    id: shortcutsCard
                    sectionTitle: qsTr("Keyboard Shortcuts")
                    iconText: "⌨️"
                    description: qsTr("System-wide keys that work even while you're in-game.")

                    SettingRow {
                        label: qsTr("Enable global shortcuts")
                        help: (globalShortcuts && globalShortcuts.bound) ? qsTr("Turning this off takes effect immediately — bound keys are simply ignored while off.") : qsTr("Recording and clip keys that work from anywhere. The system asks for approval when you turn this on.")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.globalShortcutsEnabled : true
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.globalShortcutsEnabled = checked;
                                if (checked && globalShortcuts)
                                    globalShortcuts.requestShortcuts();
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Record (global)")
                        help: (globalShortcuts && globalShortcuts.bound && globalShortcuts.boundShortcuts["toggle-recording"]) ? qsTr("Press %1 anywhere — even in-game — to start or stop recording.").arg(globalShortcuts.boundShortcuts["toggle-recording"]) : qsTr("System-wide recording key. The system asks for approval on first launch.")

                        ValuePill {
                            pillText: (globalShortcuts && globalShortcuts.boundShortcuts["toggle-recording"]) || qsTr("Not bound")
                        }
                    }

                    SettingRow {
                        label: qsTr("Save clip (global)")
                        help: (globalShortcuts && globalShortcuts.bound && globalShortcuts.boundShortcuts["save-clip"]) ? qsTr("Press %1 anywhere — even in-game — to save the last moments as a clip.").arg(globalShortcuts.boundShortcuts["save-clip"]) : qsTr("System-wide clip key. The system asks for approval on first launch.")

                        ValuePill {
                            pillText: (globalShortcuts && globalShortcuts.boundShortcuts["save-clip"]) || qsTr("Not bound")
                        }
                    }

                    SettingRow {
                        label: qsTr("Change keys")
                        help: qsTr("Opens the system dialog to remap Trophy's global keys.")
                        rowEnabled: globalShortcuts ? globalShortcuts.bound : false

                        TrophyButton {
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("Configure")
                            onClicked: {
                                if (globalShortcuts)
                                    globalShortcuts.reconfigureShortcuts();
                            }
                        }
                    }
                }

                // --- FOOTER ACTIONS ---
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spacingSmall
                    spacing: Theme.spacingMedium

                    Item {
                        Layout.fillWidth: true
                    }

                    TrophyButton {
                        text: qsTr("Restart Setup Wizard")
                        onClicked: {
                            if (settingsManager)
                                settingsManager.setupCompleted = false;
                        }
                    }

                    TrophyButton {
                        text: qsTr("Reset to Defaults")
                        onClicked: resetConfirmDialog.open()
                    }
                }

                Item {
                    Layout.preferredHeight: Theme.spacingMedium
                }
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

    FolderDialog {
        id: replayFolderDialog

        title: qsTr("Select Clip Cache Folder")
        onAccepted: {
            if (settingsManager)
                settingsManager.replayCachePath = selectedFolder.toString().replace("file://", "");
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

    TrophyDialog {
        id: storageLimitConfirmDialog
        title: storageConfirmTitle
        message: storageConfirmMessage
        confirmText: storageConfirmConfirmText
        cancelText: qsTr("Cancel")
        showDontAsk: false
        onAccepted: {
            if (storageConfirmAction)
                storageConfirmAction();
        }
        onRejected: {
            storageSlider.value = previousStorageLimitGB;
            pendingAutoCleanupEnabled = previousAutoCleanupChecked;
            storageAutoCleanupSwitch.checked = previousAutoCleanupChecked;
        }
    }

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: 12
        topLeftRadius: 26
    }

    // ============ REUSABLE CONTROLS (visual only, no logic changes) ============

    component SectionCard: Rectangle {
        property string sectionTitle: ""
        property string iconText: ""
        property string description: ""
        default property alias content: body.data

        Layout.fillWidth: true
        implicitHeight: cardCol.implicitHeight + Theme.spacingMedium * 2
        radius: Theme.radiusMedium
        color: Theme.cardColor
        border.width: 1
        border.color: Theme.borderColor
        clip: true

        ColumnLayout {
            id: cardCol
            anchors.fill: parent
            anchors.margins: Theme.spacingMedium
            spacing: Theme.spacingSmall

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                Label {
                    text: iconText
                    font.pixelSize: Theme.fontSizeLarge
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Label {
                        text: sectionTitle
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.Medium
                        color: Theme.textColor
                    }

                    Label {
                        Layout.fillWidth: true
                        text: description
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.textSecondaryColor
                        wrapMode: Text.WordWrap
                        visible: description.length > 0
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.dividerColor
            }

            ColumnLayout {
                id: body
                Layout.fillWidth: true
                spacing: Theme.spacingMedium
            }
        }
    }

    component WarnBanner: Rectangle {
        property string message: ""

        Layout.fillWidth: true
        Layout.preferredHeight: bannerRow.implicitHeight + Theme.spacingSmall * 2
        radius: Theme.radiusSmall
        color: Theme.withOpacity(Theme.warningColor, 0.1)
        border.width: 1
        border.color: Theme.withOpacity(Theme.warningColor, 0.3)

        RowLayout {
            id: bannerRow
            anchors.fill: parent
            anchors.margins: Theme.spacingSmall
            spacing: Theme.spacingSmall

            Label {
                text: "⚠️"
            }

            Label {
                Layout.fillWidth: true
                text: message
                color: Theme.textColor
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }
    }

    // Label + help on the left, control right-aligned on the right.
    component SettingRow: ColumnLayout {
        property string label: ""
        property string help: ""
        property bool rowEnabled: true
        default property alias control: controlHolder.data

        Layout.fillWidth: true
        spacing: 2
        enabled: rowEnabled
        opacity: rowEnabled ? 1.0 : 0.5

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingMedium

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: label
                    color: Theme.textColor
                    font.pixelSize: Theme.fontSizeNormal
                    font.weight: Font.Medium
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    text: help
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                    visible: help.length > 0
                }
            }

            RowLayout {
                id: controlHolder
                Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                spacing: Theme.spacingSmall
            }
        }
    }

    // Label on top, full-width control below (sliders, paths, option cards).
    component SettingStack: ColumnLayout {
        property string label: ""
        property string help: ""
        default property alias control: stackHolder.data

        Layout.fillWidth: true
        spacing: Theme.spacingTiny

        Label {
            Layout.fillWidth: true
            text: label
            color: Theme.textColor
            font.pixelSize: Theme.fontSizeNormal
            font.weight: Font.Medium
            wrapMode: Text.WordWrap
        }

        ColumnLayout {
            id: stackHolder
            Layout.fillWidth: true
            spacing: Theme.spacingTiny
        }

        Label {
            Layout.fillWidth: true
            text: help
            color: Theme.textMutedColor
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            visible: help.length > 0
        }
    }

    // Selectable card used for container format, audio mode and track config.
    component OptionCard: Rectangle {
        property string title: ""
        property string description: ""
        property bool selected: false
        property bool danger: false
        property bool cardEnabled: true
        signal chosen()

        Layout.fillWidth: true
        implicitHeight: optionRow.implicitHeight + Theme.spacingMedium * 2
        radius: Theme.radiusSmall
        color: !cardEnabled ? "transparent" : selected ? (danger ? Theme.withOpacity(Theme.warningColor, 0.08) : Theme.withOpacity(Theme.primaryColor, 0.08)) : "transparent"
        border.width: selected ? 2 : 1
        border.color: {
            if (!cardEnabled)
                return Theme.withOpacity(Theme.textColor, 0.1);
            if (selected)
                return danger ? Theme.warningColor : Theme.primaryColor;
            return Theme.withOpacity(Theme.textColor, 0.12);
        }
        opacity: cardEnabled ? 1.0 : 0.5

        RowLayout {
            id: optionRow
            anchors.fill: parent
            anchors.margins: Theme.spacingMedium
            spacing: Theme.spacingSmall

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: title
                    color: Theme.textColor
                    font.weight: Font.Medium
                }

                Label {
                    Layout.fillWidth: true
                    text: description
                    color: (selected && danger) ? Theme.warningColor : Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }

            TrophyRadioButton {
                checked: selected
                enabled: cardEnabled
                onClicked: chosen()
            }
        }

        MouseArea {
            anchors.fill: parent
            enabled: cardEnabled
            cursorShape: Qt.PointingHandCursor
            onClicked: chosen()
        }
    }

    component ValuePill: Rectangle {
        property string pillText: ""

        Layout.preferredWidth: Math.max(pillLabel.implicitWidth + 24, 88)
        Layout.preferredHeight: 28
        Layout.alignment: Qt.AlignVCenter
        radius: 14
        color: Theme.withOpacity(Theme.primaryColor, 0.12)
        border.width: 1
        border.color: Theme.withOpacity(Theme.primaryColor, 0.4)

        Label {
            id: pillLabel
            anchors.centerIn: parent
            text: pillText
            color: Theme.textColor
            font.pixelSize: Theme.fontSizeSmall
            font.weight: Font.Medium
        }
    }
}
