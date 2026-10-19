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

    // Refresh live choices on open; the voice picker retains the saved app
    // independently because apps publish audio streams on demand.
    onVisibleChanged: {
        if (visible && audioDeviceEnumerator) audioDeviceEnumerator.refreshAudioApps()
    }

    property var scrollTargetCard: null

    function scrollSectionIntoView(card) {
        var target = Math.max(0, card.y - Theme.spacingSmall);
        var maxY = Math.max(0, settingsFlick.contentHeight - settingsFlick.height);
        settingsFlick.contentY = Math.min(target, maxY);
    }

    Connections {
        target: root.scrollTargetCard
        function onExpansionFinished() {
            var card = root.scrollTargetCard;
            Qt.callLater(function() {
                if (root.scrollTargetCard !== card)
                    return;
                root.scrollSectionIntoView(card);
                root.scrollTargetCard = null;
            });
        }
    }

    function scrollToSection(sectionTitle) {
        var card = null;
        if (sectionTitle === "Video Recording")
            card = videoCard;
        else if (sectionTitle === "Background Clips")
            card = clipsCard;
        else if (sectionTitle === "Audio Recording")
            card = audioCard;
        else if (sectionTitle === "Storage Management")
            card = storageCard;
        else if (sectionTitle === "Notifications")
            card = notificationsCard;
        else if (sectionTitle === "Game Detection")
            card = detectionCard;
        else if (sectionTitle === "Detection Helpers")
            card = detectionHelpersCard;
        else if (sectionTitle === "System Tray")
            card = trayCard;
        else if (sectionTitle === "Keyboard Shortcuts")
            card = shortcutsCard;
        else if (sectionTitle === "Customization")
            card = customizationCard;

        if (!card)
            return;

        scrollTargetCard = card;
        card.expanded = true;
        Qt.callLater(function() {
            root.scrollSectionIntoView(card);
            if (card.expansionProgress === 1 && root.scrollTargetCard === card)
                root.scrollTargetCard = null;
        });
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

    ColumnLayout {
        id: settingsLayout
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        // Let the viewport and its fade reach the page edge, as on Info.
        // The scrolling content already supplies its own bottom padding.
        anchors.bottomMargin: 0
        anchors.rightMargin: 0
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
                text: qsTr("Tune capture quality, audio, storage, detection and appearance.")
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textSecondaryColor
            }
        }

        // --- CONTENT ---
        Flickable {
            id: settingsFlick
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: Theme.spacingMedium
            contentWidth: width
            contentHeight: contentCol.implicitHeight + Theme.spacingLarge
            clip: true

            Behavior on contentY {
                NumberAnimation {
                    duration: 250
                    easing.type: Easing.OutCubic
                }
            }

            ScrollBar.vertical: TrophyScrollBar {}

            ColumnLayout {
                id: contentCol
                width: Math.min(parent.width - 12, 800)
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.spacingLarge

                // ================= CUSTOMIZATION =================
                SectionCard {
                    id: customizationCard
                    sectionTitle: qsTr("Customization")
                    iconSource: "qrc:/icons/paintbrush.svg"
                    description: qsTr("Choose a theme and accent color. Changes apply immediately.")

                    SettingRow {
                        label: qsTr("Theme")
                        help: qsTr("Default keeps the classic navy palette. Charcoal is neutral gray. AMOLED uses a pure-black background.")

                        TrophyComboBox {
                            id: themeModeBox
                            objectName: "themeModeBox"
                            Layout.preferredWidth: 200
                            property var values: ["dark", "charcoal", "amoled"]
                            model: [qsTr("Default"), qsTr("Charcoal"), qsTr("AMOLED")]
                            currentIndex: settingsManager ? Math.max(0, values.indexOf(settingsManager.themeMode)) : 0
                            onActivated: settingsManager.themeMode = values[currentIndex]
                        }
                    }

                    SettingRow {
                        label: qsTr("Accent color")
                        help: qsTr("Used for buttons, switches and selections. Recording and warning colors stay distinct.")

                        AccentColorPicker {
                            selectedKey: settingsManager ? settingsManager.accentColor : "rose"
                            onColorSelected: function(key) {
                                if (settingsManager)
                                    settingsManager.accentColor = key;
                            }
                        }
                    }
                }

                // ================= VIDEO =================
                SectionCard {
                    id: videoCard
                    sectionTitle: qsTr("Video Recording")
                    iconSource: "qrc:/icons/video.svg"
                    description: qsTr("Encoder, quality and output format. Changes apply to the next recording.")

                    WarnBanner {
                        message: qsTr("Recording in progress. To change these settings, stop recording.")
                        visible: recordingManager && recordingManager.isRecording
                    }

                    SettingRow {
                        label: qsTr("Video Encoder")
                        help: qsTr("Auto picks the best hardware encoder (NVENC, VA-API, or x264). AV1 gives the best quality but needs modern hardware.")

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

                                        HoverHandler {
                                            cursorShape: Qt.PointingHandCursor
                                        }
                                        TapHandler {
                                            enabled: !(recordingManager && recordingManager.isRecording)
                                            onTapped: {
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

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Recordings are saved as crash-resilient MKV. Use Export to share in the Library to create a compatible MP4 copy.")
                        color: Theme.textSecondaryColor
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                }

                // ================= BACKGROUND CLIPS =================
                SectionCard {
                    id: clipsCard
                    sectionTitle: qsTr("Background Clips")
                    iconSource: "qrc:/icons/scissors.svg"
                    description: qsTr("Quiet background buffering for instant clips. Changes take effect within a minute.")

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
                                enabled: !(replayManager && replayManager.active)
                                onClicked: replayFolderDialog.open()
                            }

                            TrophyButton {
                                text: qsTr("Default")
                                visible: replayCachePathField.text !== ""
                                enabled: !(replayManager && replayManager.active)
                                onClicked: {
                                    if (settingsManager)
                                        settingsManager.replayCachePath = "";
                                }
                            }
                        }
                    }
                }

                // ================= AUDIO =================
                SectionCard {
                    id: audioCard
                    sectionTitle: qsTr("Audio Recording")
                    iconSource: "qrc:/icons/audio-recording.svg"
                    description: qsTr("What gets heard in your recordings, and how tracks are stored.")

                    WarnBanner {
                        message: qsTr("Recording in progress. Mode and track layout are locked; devices and the voice app apply live.")
                        visible: recordingManager && recordingManager.isRecording
                    }

                    SettingRow {
                        label: qsTr("Capture")
                        help: settingsManager && settingsManager.audioMode === "desktop"
                            ? qsTr("Everything you hear, plus your microphone.")
                            : qsTr("Game audio, with desktop audio as a fallback until the game is available.")

                        RowLayout {
                            objectName: "audioCaptureMode"
                            Layout.preferredWidth: 300
                            spacing: Theme.spacingSmall
                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
                            TrophyButton {
                                Layout.preferredWidth: 146
                                text: qsTr("Game audio")
                                radius: 0
                                outlined: true
                                checked: !settingsManager || settingsManager.audioMode === "game"
                                onClicked: if (settingsManager) settingsManager.audioMode = "game"
                            }
                            TrophyButton {
                                Layout.preferredWidth: 146
                                text: qsTr("Desktop audio")
                                radius: 0
                                outlined: true
                                checked: settingsManager && settingsManager.audioMode === "desktop"
                                onClicked: if (settingsManager) settingsManager.audioMode = "desktop"
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Tracks")
                        help: settingsManager && !settingsManager.separateAudioTracks
                            ? qsTr("Combine game, voice chat and microphone into one track.")
                            : qsTr("Keep game, voice chat and microphone separate for editing.")
                        // Desktop mode always uses a single mixed track.
                        visible: settingsManager ? settingsManager.audioMode === "game" : true

                        RowLayout {
                            objectName: "audioTrackLayout"
                            Layout.preferredWidth: 300
                            spacing: Theme.spacingSmall
                            enabled: !(recordingManager && recordingManager.isRecording)
                            opacity: enabled ? 1 : 0.5
                            TrophyButton {
                                Layout.preferredWidth: 146
                                text: qsTr("Separate tracks")
                                radius: 0
                                outlined: true
                                checked: !settingsManager || settingsManager.separateAudioTracks
                                onClicked: if (settingsManager) settingsManager.separateAudioTracks = true
                            }
                            TrophyButton {
                                Layout.preferredWidth: 146
                                text: qsTr("Mixed track")
                                radius: 0
                                outlined: true
                                checked: settingsManager && !settingsManager.separateAudioTracks
                                onClicked: if (settingsManager) settingsManager.separateAudioTracks = false
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Microphone")
                        help: qsTr("Your voice input device.")

                        TrophyComboBox {
                            id: microphoneBox
                            Layout.preferredWidth: 300
                            // Live-applied to the running pipeline; no lock needed.
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
                            // Live-applied to the running pipeline (desktop mode); no lock needed.
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

                    SettingRow {
                        label: qsTr("Voice Chat App")
                        help: qsTr("Select from running audio applications.")
                        visible: settingsManager ? settingsManager.audioMode === "game" : true

                        RowLayout {
                            spacing: Theme.spacingSmall

                            TrophyButton {
                                text: qsTr("Refresh")
                                enabled: audioDeviceEnumerator
                                onClicked: if (audioDeviceEnumerator) audioDeviceEnumerator.refreshAudioApps()
                            }

                            VoiceAppComboBox {
                                id: voiceChatAppBox
                                Layout.preferredWidth: 300
                                // Live-applied to the running pipeline; no lock needed.
                                apps: audioDeviceEnumerator ? audioDeviceEnumerator.audioApps : []
                                savedApp: settingsManager ? settingsManager.voiceChatApp : ""
                                onAppSelected: function(binary) {
                                    if (settingsManager) settingsManager.voiceChatApp = binary;
                                }
                            }

                        }
                    }

                }

                // ================= STORAGE =================
                SectionCard {
                    id: storageCard
                    sectionTitle: qsTr("Storage Management")
                    iconSource: "qrc:/icons/storage.svg"
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
                                // Upper bound is the drive holding the
                                // recordings (0 = unknown → legacy 500 GB cap).
                                to: storageManager && storageManager.driveCapacityBytes > 0 ? storageManager.driveCapacityBytes / (1024 * 1024 * 1024) : 500
                                stepSize: 0.1
                                value: settingsManager ? settingsManager.maxStorageSize / (1024 * 1024 * 1024) : 50
                                onPressedChanged: {
                                    if (!pressed && settingsManager)
                                        applyStorageLimit(value);
                                }
                            }

                            ValuePill {
                                pillText: storageSlider.value === 0 ? qsTr("No Limit") : storageSlider.value >= 1024 ? (storageSlider.value / 1024).toFixed(1) + " TB" : storageSlider.value < 1 ? storageSlider.value.toFixed(1) + " GB" : Math.round(storageSlider.value) + " GB"
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

                // ================= NOTIFICATIONS =================
                SectionCard {
                    id: notificationsCard
                    sectionTitle: qsTr("Notifications")
                    iconSource: "qrc:/icons/bell.svg"
                    description: qsTr("Which popups Trophy may show. Errors, the capture-authorize prompt and direct keypress feedback always show.")

                    SettingRow {
                        label: qsTr("Recording Notices")
                        help: qsTr("Recording started and recording saved.")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.notifyRecordings : true
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.notifyRecordings = checked;
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Clip Notices")
                        help: qsTr("Clip saved after pressing the clip key.")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.notifyClips : true
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.notifyClips = checked;
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Storage Notices")
                        help: qsTr("Storage-full warnings.")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.notifyStorage : true
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.notifyStorage = checked;
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Completion Sounds")
                        help: qsTr("Short chimes when a clip or a recording finishes saving.")

                        RowLayout {
                            spacing: Theme.spacingSmall

                            TrophyButton {
                                text: qsTr("Clip")
                                onClicked: if (app) app.playClipPing(true)
                            }

                            TrophyButton {
                                text: qsTr("Stop")
                                onClicked: if (app) app.playRecordingStoppedPing(true)
                            }

                            TrophySwitch {
                                Layout.alignment: Qt.AlignVCenter
                                checked: settingsManager ? settingsManager.completionSoundEnabled : true
                                onToggled: {
                                    if (settingsManager)
                                        settingsManager.completionSoundEnabled = checked;
                                }
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Automatic Update Checks")
                        help: qsTr("Check GitHub for a new Trophy release once a day. The Check for updates link on the Info page always runs.")

                        TrophySwitch {
                            Layout.alignment: Qt.AlignVCenter
                            checked: settingsManager ? settingsManager.updateCheckEnabled : true
                            onToggled: {
                                if (settingsManager)
                                    settingsManager.updateCheckEnabled = checked;
                            }
                        }
                    }
                }

                // ================= DETECTION =================
                SectionCard {
                    id: detectionCard
                    sectionTitle: qsTr("Game Detection")
                    iconSource: "qrc:/icons/gamepad.svg"
                    description: qsTr("Choose whether Trophy detects games and records sessions automatically.")

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
                        label: qsTr("Auto-record")
                        help: qsTr("Automatically record the full session when an authorized game is detected.")

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

                SectionCard {
                    id: detectionHelpersCard
                    objectName: "detectionHelpersCard"
                    sectionTitle: qsTr("Detection Helpers")
                    iconSource: "qrc:/icons/puzzle.svg"
                    description: qsTr("Integration status at a glance. Graphics helpers are optional.")
                    readonly property real statusWidth: Math.max(vulkanControls.badgeWidth, glControls.badgeWidth)
                    readonly property real actionWidth: Math.max(vulkanHelperSwitch.implicitWidth, glHelperSwitch.implicitWidth)

                    SettingRow {
                        label: qsTr("Game Window Helper (required)")
                        help: qsTr("Provides window identity, focus and fullscreen state for game detection, the window picker and capture restoration.%1 Screen capture still requires separate authorization.")
                              .arg(windowMonitor && windowMonitor.compositorName === "kwin"
                                   ? qsTr(" On KDE this uses a read-only KWin script.")
                                   : (windowMonitor && windowMonitor.compositorName === "gnome"
                                      ? qsTr(" On GNOME this uses the Trophy Window Helper Shell extension. Logout/login may be required after installation. %1").arg(windowMonitor.gnomeHelperStatus) : ""))

                        HelperControls {
                            id: windowHelperControls
                            objectName: "gameWindowHelperStatus"
                            statusWidth: badgeWidth
                            actionWidth: helperRepair.implicitWidth
                            showAction: windowMonitor && !windowMonitor.trackingReady
                            healthy: windowMonitor && windowMonitor.trackingReady
                            attention: !healthy && !(windowMonitor && windowMonitor.trackingChecking)
                            text: windowMonitor && windowMonitor.trackingReady
                                  ? (windowMonitor.compositorName === "kwin" ? qsTr("Installed · Working") : qsTr("Working"))
                                  : (windowMonitor && windowMonitor.trackingChecking ? qsTr("Checking…")
                                     : (windowMonitor && windowMonitor.compositorName === "kwin"
                                         ? (windowMonitor.kwinScriptInstalled
                                            ? (windowMonitor.kwinScriptEnabled ? qsTr("Installed · Not reporting") : qsTr("Installed · Disabled"))
                                            : qsTr("Not installed"))
                                        : (windowMonitor && windowMonitor.compositorName === "gnome"
                                           ? (windowMonitor.gnomeHelperInstalled ? qsTr("Installed · Not reporting") : qsTr("Not installed"))
                                           : qsTr("Unavailable"))))
                            TrophyButton {
                                id: helperRepair
                                anchors.right: parent.right
                                visible: windowHelperControls.showAction
                                text: qsTr("Repair")
                                enabled: windowMonitor && !windowMonitor.trackingChecking
                                onClicked: if (windowMonitor) windowMonitor.repairTracking()
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("Vulkan Detection")
                        help: qsTr("Notices games starting Vulkan graphics and notifies Trophy to start recording, including Proton titles.")

                        HelperControls {
                            id: vulkanControls
                            statusWidth: detectionHelpersCard.statusWidth
                            actionWidth: detectionHelpersCard.actionWidth
                            healthy: app && app.vulkanLayerRegistered
                            text: healthy ? qsTr("Enrolled") : qsTr("Not enrolled")

                            TrophySwitch {
                                id: vulkanHelperSwitch
                                anchors.right: parent.right
                                checked: settingsManager ? settingsManager.vulkanLayerEnabled : false
                                onToggled: {
                                    if (settingsManager)
                                        settingsManager.vulkanLayerEnabled = checked;
                                    if (app)
                                        app.setVulkanLayerEnabled(checked);
                                }
                            }
                        }
                    }

                    SettingRow {
                        label: qsTr("OpenGL Detection")
                        helpTextFormat: Text.StyledText
                        help: (app && app.glShimAvailable)
                              ? qsTr("Notices games starting OpenGL graphics and notifies Trophy to start recording. Loads into any OpenGL app and %1. Changes require logging out and back in.")
                                .arg(app.glShimEnrolled
                                     ? '<font color="' + Theme.cautionColor + '">' + qsTr("may cause issues with anti-cheat games") + '</font>'
                                     : qsTr("may cause issues with anti-cheat games"))
                              : qsTr("OpenGL detection helper not found in this install.")
                        rowEnabled: app ? app.glShimAvailable : false

                        HelperControls {
                            id: glControls
                            statusWidth: detectionHelpersCard.statusWidth
                            actionWidth: detectionHelpersCard.actionWidth
                            attention: !app || !app.glShimAvailable
                            enrolled: app && app.glShimEnrolled
                            text: attention ? qsTr("Unavailable") : (app.glShimEnrolled ? qsTr("Enrolled") : qsTr("Not enrolled"))

                            TrophySwitch {
                                id: glHelperSwitch
                                anchors.right: parent.right
                                checked: app ? app.glShimEnrolled : false
                                onToggled: {
                                    if (app)
                                        app.setGlShimEnrolled(checked);
                                }
                            }
                        }
                    }

                }

                // ================= TRAY =================
                SectionCard {
                    id: trayCard
                    sectionTitle: qsTr("System Tray")
                    iconSource: "qrc:/icons/tray.svg"
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
                    iconSource: "qrc:/icons/keyboard.svg"
                    description: qsTr("System-wide keys that work even while you're in-game.")

                    SettingRow {
                        label: qsTr("Enable global shortcuts")
                        help: (globalShortcuts && globalShortcuts.bound) ? qsTr("Turning this off takes effect immediately — bound keys are simply ignored while off.") : qsTr("Two keys that work from anywhere: recording toggle and clip/bookmark. The system asks for approval when you turn this on.")

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
                        help: ((globalShortcuts && globalShortcuts.bound && globalShortcuts.boundShortcuts["toggle-recording"]) ? qsTr("Press %1 anywhere — even in-game — to start or stop recording.").arg(globalShortcuts.boundShortcuts["toggle-recording"]) : qsTr("System-wide recording key. The system asks for approval on first launch.")) + " " + qsTr("Or bind trophy --toggle-record in your compositor.")

                        ValuePill {
                            pillText: (globalShortcuts && globalShortcuts.boundShortcuts["toggle-recording"]) || qsTr("Not bound")
                        }
                    }

                    SettingRow {
                        label: qsTr("Clip / bookmark (global)")
                        help: ((globalShortcuts && globalShortcuts.bound && globalShortcuts.boundShortcuts["save-clip"]) ? qsTr("Press %1 to save a replay clip in DVR mode, or add a bookmark during a full recording.").arg(globalShortcuts.boundShortcuts["save-clip"]) : qsTr("One key saves DVR clips or bookmarks full recordings.")) + " " + qsTr("Or bind trophy --save-clip in your compositor.")

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
                            onClicked: { if (globalShortcuts) globalShortcuts.reconfigureShortcuts() }
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

    // Keep the fade outside the Flickable so it stays fixed over scrolling content.
    Rectangle {
        anchors.left: settingsLayout.left
        anchors.right: settingsLayout.right
        anchors.rightMargin: settingsFlick.ScrollBar.vertical.width
        anchors.bottom: settingsLayout.bottom
        height: Theme.spacingLarge * 2
        opacity: Math.max(0, Math.min(1, (settingsFlick.contentHeight - settingsFlick.height - settingsFlick.contentY) / height))
        visible: opacity > 0
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.alpha(Theme.backgroundColor, 0) }
            GradientStop { position: 1; color: Theme.backgroundColor }
        }
    }

    // --- DIALOGS ---
    FolderDialog {
        id: folderDialog

        title: qsTr("Select Recordings Folder")
        onAccepted: {
            if (settingsManager)
                settingsManager.recordingsPath = selectedFolder.toLocalFile();
        }
    }

    FolderDialog {
        id: replayFolderDialog

        title: qsTr("Select Clip Cache Folder")
        onAccepted: {
            if (settingsManager)
                settingsManager.replayCachePath = selectedFolder.toLocalFile();
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
        bottomRightRadius: Theme.windowRadius
        topLeftRadius: 26
    }

    // ============ REUSABLE CONTROLS (visual only, no logic changes) ============

    component SectionCard: SettingsSectionCard {}

    // Optional helpers share compact status/action columns. A status-only
    // helper collapses its action slot entirely rather than leaving a gap.
    // A plain Item honors its implicit width without stretching the label column.
    component HelperControls: Item {
        id: helperControls
        property real statusWidth: 0
        property real actionWidth: 0
        property bool showAction: true
        property alias text: helperBadge.text
        property alias healthy: helperBadge.healthy
        property alias attention: helperBadge.attention
        property alias enrolled: helperBadge.enrolled
        readonly property real badgeWidth: helperBadge.implicitWidth
        default property alias action: actionSlot.data

        Layout.leftMargin: Theme.spacingLarge
        implicitWidth: statusWidth + (showAction ? Theme.spacingMedium + actionWidth : 0)
        implicitHeight: Math.max(helperBadge.implicitHeight, showAction ? actionSlot.childrenRect.height : 0)

        HelperStatusBadge {
            id: helperBadge
            anchors.right: actionSlot.left
            anchors.rightMargin: helperControls.showAction ? Theme.spacingMedium : 0
            anchors.verticalCenter: parent.verticalCenter
        }

        Item {
            id: actionSlot
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            visible: helperControls.showAction
            width: helperControls.showAction ? helperControls.actionWidth : 0
            height: childrenRect.height
        }
    }

    component HelperStatusBadge: Rectangle {
        id: badge
        property string text: ""
        property bool healthy: false
        property bool attention: false
        property bool enrolled: false
        readonly property color statusColor: healthy ? Theme.successColor
                                                   : (enrolled ? Theme.cautionColor : (attention ? Theme.warningColor : Theme.textSecondaryColor))

        implicitWidth: statusRow.implicitWidth + Theme.spacingSmall * 2
        implicitHeight: statusRow.implicitHeight + Theme.spacingTiny * 2
        radius: Theme.radiusSmall
        color: Theme.withOpacity(statusColor, 0.1)
        border.width: 1
        border.color: Theme.withOpacity(statusColor, 0.3)

        Row {
            id: statusRow
            anchors.centerIn: parent
            spacing: Theme.spacingSmall

            Rectangle {
                width: 6
                height: 6
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: badge.statusColor
            }

            Label {
                text: badge.text
                color: badge.statusColor
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Font.Medium
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

            TrophyIcon {
                source: "qrc:/icons/warning.svg"
                color: Theme.warningColor
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
        property int helpTextFormat: Text.AutoText
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
                    textFormat: helpTextFormat
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
