/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../components"
import "../controls"
import "../styles"
import "../views"
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

Page {
    id: root
    // Explicit dependencies; defaults preserve engine/creation-context hosts.
    property var recorder: typeof recordingManager !== "undefined" ? recordingManager : null // qmllint disable unqualified
    property var feed: typeof activityFeed !== "undefined" ? activityFeed : null // qmllint disable unqualified
    property var replay: typeof replayManager !== "undefined" ? replayManager : null // qmllint disable unqualified
    property var storage: typeof storageManager !== "undefined" ? storageManager : null // qmllint disable unqualified
    property var preferences: typeof settingsManager !== "undefined" ? settingsManager : null // qmllint disable unqualified

    property bool libraryListView: false
    property var captureStatus: ({})
    // Retain the last live snapshot until the closing animation finishes.
    property var healthStatus: ({})
    property var audioSignalSnapshot: ({})
    onCaptureStatusChanged: {
        if (captureStatus.mode) {
            healthStatus = captureStatus;
            audioSignalSnapshot = captureStatus;
        } else {
            audioSignalSnapshot = ({});
            captureInfoDialog.close();
        }
    }
    onVisibleChanged: { if (!visible) captureInfoDialog.close() }
    function captureInfoValue(key, suffix) {
        const value = root.captureStatus[key];
        return value === undefined ? "–" : String(value) + (suffix || "");
    }
    function captureInfoSize(widthKey, heightKey) {
        return root.captureStatus[widthKey] > 0 && root.captureStatus[heightKey] > 0
            ? root.captureStatus[widthKey] + " × " + root.captureStatus[heightKey] : "–";
    }
    function captureInfoNumber(key, decimals, suffix) {
        const value = root.captureStatus[key];
        return value === undefined || !isFinite(Number(value)) ? "–"
            : Number(value).toFixed(decimals) + (suffix || "");
    }
    function captureInfoBitrate(key) {
        const value = root.captureInfoNumber(key, 0, " kbps");
        return value === "–" ? value : value
            + (root.captureStatus.rateControlMode ? " · " + root.captureStatus.rateControlMode : "");
    }
    function audioSignalForRole(role) {
        const snapshot = root.audioSignalSnapshot.session === root.captureStatus.session
            ? root.audioSignalSnapshot : root.captureStatus;
        return snapshot["audio" + role + "Signal"] || ({});
    }
    function audioDb(data, key) {
        const value = data[key];
        if (value === undefined || value === null || !isFinite(Number(value)) || value < 0)
            return "–";
        return value === 0 ? qsTr("−∞ dBFS")
            : (20 * Math.log(value) / Math.LN10).toFixed(1) + " dBFS";
    }
    function audioMeterFraction(value) {
        return value > 0 ? Math.max(0, Math.min(1, (20 * Math.log(value) / Math.LN10 + 60) / 60)) : 0;
    }
    function audioReceiptText(role, data) {
        if (role === 1 && (root.healthStatus.desktopAudio || root.healthStatus.desktopFallback
                           || root.healthStatus.audio1 === "not-configured"))
            return qsTr("No separate source");
        if (!data.received) return qsTr("No buffers received");
        const age = Number(data.lastBufferAgeMs);
        if (!isFinite(age)) return "–";
        return age < 1000 ? qsTr("%1 ms ago").arg(Math.round(age))
            : qsTr("%1 s ago · no recent buffers").arg((age / 1000).toFixed(1));
    }

    component CaptureInfoRow: Item {
        id: infoRow
        property string rowName: ""
        property string rowValue: ""
        Layout.fillWidth: true
        // The outer layout owns width. Do not propagate the inner layout's
        // width-dependent text hints back into that same layout negotiation.
        implicitHeight: infoContent.implicitHeight + Theme.spacingSmall + 1
        RowLayout {
            id: infoContent
            width: infoRow.width
            Label {
                text: infoRow.rowName
                color: Theme.textSecondaryColor
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Label {
                text: infoRow.rowValue
                color: Theme.textColor
                Layout.maximumWidth: infoRow.width * 0.48
                wrapMode: Text.WrapAnywhere
                horizontalAlignment: Text.AlignRight
            }
        }
        Rectangle {
            y: infoContent.height + Theme.spacingSmall
            width: infoRow.width
            height: 1
            color: Theme.borderColor
        }
    }
    property int dropWarningTicks: 0
    function updateCaptureStatus() {
        const next = (typeof app !== "undefined" && app && typeof app.captureHealth === "function") ? app.captureHealth() : ({});
        if (next.session && next.session === root.captureStatus.session && next.queueDrops > root.captureStatus.queueDrops)
            root.dropWarningTicks++;
        else
            root.dropWarningTicks = 0;
        root.captureStatus = next;
    }
    function audioHealthName(role) {
        return role === 0 ? (root.healthStatus.desktopAudio || root.healthStatus.desktopFallback ? qsTr("System") : qsTr("Game")) : role === 1 ? qsTr("Voice") : qsTr("Mic");
    }
    function deviceDescription(id, isInput) {
        if (!id || id === "default")
            return isInput ? qsTr("System default input") : qsTr("System default output");
        const enumerator = (typeof audioDeviceEnumerator !== "undefined") ? audioDeviceEnumerator : null;
        const list = enumerator ? (isInput ? enumerator.inputDevices : enumerator.outputDevices) : [];
        for (let i = 0; list && i < list.length; ++i) {
            if (list[i].id === id)
                return list[i].name;
        }
        return id;
    }
    // What a role is actually bound to, for "Connected to <name>".
    function audioHealthTarget(role) {
        const status = root.healthStatus;
        if (role === 0) {
            if (status.desktopFallback)
                return qsTr("desktop mix");
            if (status.desktopAudio)
                return root.deviceDescription(settingsManager ? settingsManager.gameAudioDevice : "", false);
            return status.gameName || status.audio0Name || "";
        }
        if (role === 1)
            return status.audio1Name || "";
        const micId = settingsManager ? settingsManager.microphoneDevice : "";
        return (micId ? root.deviceDescription(micId, true) : "") || status.audio2Name || "";
    }
    function audioHealthText(role) {
        const state = root.healthStatus["audio" + role];
        let detail = "";
        if (state === "capturing") {
            const target = root.audioHealthTarget(role);
            detail = target ? qsTr("Connected to %1").arg(target) : qsTr("Connected");
        } else {
            detail = state === "muted" ? qsTr("Muted") : state === "starting" ? qsTr("Connecting…") : state === "not-configured" ? qsTr("Not configured") : state === "included-in-desktop" ? qsTr("Included in desktop mix") : state === "desktop-mix-muted" ? qsTr("Desktop mix is muted") : state === "desktop-mix-unavailable" ? qsTr("Desktop mix unavailable") : qsTr("Unavailable — retrying");
        }
        if (role === 0 && root.healthStatus.desktopFallback)
            detail = qsTr("Desktop fallback") + (state === "muted" ? qsTr(" · Muted") : state === "capturing" ? "" : " · " + detail);
        return detail;
    }
    function audioHealthColor(role) {
        const state = root.healthStatus["audio" + role];
        if (state === "muted" || state === "not-configured" || state === "desktop-mix-muted")
            return Theme.textMutedColor;
        if (role === 0 && root.healthStatus.desktopFallback)
            return Theme.warningColor;
        return state === "capturing" || state === "included-in-desktop" ? Theme.successColor : Theme.warningColor;
    }

    Timer {
        objectName: "captureStatusTimer"
        interval: 1000
        repeat: true
        running: root.visible
        triggeredOnStart: true
        onTriggered: root.updateCaptureStatus()
    }
    Timer {
        interval: 100
        repeat: true
        running: root.visible && captureInfoDialog.visible && captureInfoDialog.section === 1
        triggeredOnStart: true
        onTriggered: {
            if (typeof app === "undefined" || !app || typeof app.captureHealth !== "function") return;
            const next = app.captureHealth();
            // Do not change the Dashboard's one-second drop-warning cadence,
            // or adopt a retired DVR segment's audio into its successor.
            if (next.session && next.session === root.captureStatus.session && next.mode === root.captureStatus.mode)
                root.audioSignalSnapshot = next;
        }
    }

    Dialog {
        id: captureInfoDialog
        objectName: "captureInfoDialog"
        property int section: 0
        onOpened: section = 0
        onSectionChanged: {
            if (captureInfoScroll) {
                const flickable = captureInfoScroll.contentItem as Flickable;
                if (flickable) flickable.contentY = 0;
            }
        }
        modal: true
        anchors.centerIn: parent
        width: Math.min(640, root.width - 32)
        height: Math.min(660, root.height - 32)
        padding: Theme.spacingLarge
        Overlay.modal: Rectangle { color: Theme.withOpacity("#000000", 0.5) }
        background: Rectangle {
            color: Theme.cardColor
            radius: Theme.radiusLarge
            border.color: Theme.borderColor
        }
        header: ColumnLayout {
            spacing: Theme.spacingSmall
            Label {
                text: qsTr("Capture Info")
                padding: Theme.spacingLarge
                color: Theme.textColor
                font.pixelSize: Theme.fontSizeLarge
                font.bold: true
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                Layout.bottomMargin: Theme.spacingSmall
                Repeater {
                    model: [qsTr("Video"), qsTr("Audio"), qsTr("Session")]
                    delegate: TrophyButton {
                        required property int index
                        required property string modelData
                        objectName: "captureInfoTab" + index
                        Layout.fillWidth: true
                        text: modelData
                        checkable: true
                        autoExclusive: true
                        outlined: true
                        checked: captureInfoDialog.section === index
                        onClicked: captureInfoDialog.section = index
                    }
                }
            }
        }
        contentItem: ScrollView {
            id: captureInfoScroll
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: Theme.spacingMedium
                Label {
                    Layout.fillWidth: true
                    text: root.captureStatus.mode === "replay"
                        ? qsTr("DVR · current segment (counters reset at rotation)")
                        : qsTr("Full recording · current session")
                    color: Theme.accentColor
                    wrapMode: Text.WordWrap
                }
                Repeater {
                    model: captureInfoDialog.section === 0 ? [
                        [qsTr("Input resolution"), root.captureInfoSize("inputWidth", "inputHeight")],
                        [qsTr("Output resolution"), root.captureInfoSize("outputWidth", "outputHeight")],
                        [qsTr("Delivered transport"), root.captureStatus.transport === "dma-buf" ? qsTr("DMA-BUF") : root.captureStatus.transport === "shm" ? qsTr("Shared memory (SHM)") : "–"],
                        [qsTr("Transport changes"), root.captureInfoValue("transportChanges")],
                        [qsTr("DMA-BUF → SHM changes"), root.captureInfoValue("shmTransitions")],
                        [qsTr("Encoder / codec"), root.captureInfoValue("encoder")],
                        [qsTr("Target frame rate"), root.captureStatus.targetFps === 0 ? qsTr("Uncapped") : root.captureInfoValue("targetFps", " fps")],
                        [qsTr("Actual capture frame rate"), root.captureInfoNumber("actualCaptureFps", 1, " fps")],
                        [qsTr("Actual encode frame rate"), root.captureInfoNumber("actualEncodeFps", 1, " fps")],
                        [qsTr("Target video bitrate"), root.captureInfoBitrate("bitrateKbps")],
                        [qsTr("Actual output bitrate (A/V payload)"), root.captureInfoBitrate("actualOutputKbps")],
                        [qsTr("Rate measurement interval"), root.captureInfoNumber("rateWindowSeconds", 2, " s")],
                        [qsTr("Requested preset"), root.captureInfoValue("preset")],
                        [qsTr("Target keyframe interval"), root.captureInfoValue("keyframeSeconds", " s")],
                        [qsTr("Session average encoder call"), root.captureInfoNumber("averageEncodeMs", 2, " ms")],
                        [qsTr("Recent average encoder call"), root.captureInfoNumber("recentEncodeAverageMs", 2, " ms")],
                        [qsTr("Recent maximum encoder call"), root.captureInfoNumber("recentEncodeMaximumMs", 2, " ms")],
                        [qsTr("Recent 95th percentile"), root.captureInfoNumber("recentEncodeP95Ms", 2, " ms")],
                        [qsTr("Recent timing samples"), root.captureInfoValue("recentEncodeSamples")],
                        [qsTr("Frames delivered to session"), root.captureInfoValue("deliveredFrames")],
                        [qsTr("DMA-BUF frames delivered"), root.captureInfoValue("dmaBufFrames")],
                        [qsTr("Frames accepted by encoder"), root.captureInfoValue("acceptedFrames")],
                        [qsTr("Video packets produced"), root.captureInfoValue("videoPackets")],
                        [qsTr("Queue depth / capacity"), root.captureInfoValue("queueDepth") + " / " + root.captureInfoValue("queueCapacity")],
                        [qsTr("Peak queue depth"), root.captureInfoValue("peakQueueDepth")],
                        [qsTr("Session queue drops"), root.captureInfoValue("queueDrops")],
                        [qsTr("Encoder input drops"), root.captureInfoValue("encoderDrops")],
                        [qsTr("EGL / CUDA imports"), root.captureInfoValue("eglImportFrames")],
                        [qsTr("VA-API VPP frames"), root.captureInfoValue("vaapiVppFrames")],
                        [qsTr("Hardware-mapped frames"), root.captureInfoValue("hwMappedFrames")],
                        [qsTr("GPU-scaled frames"), root.captureInfoValue("gpuScaledFrames")],
                        [qsTr("CPU-input frames"), root.captureInfoValue("cpuInputFrames")],
                        [qsTr("Written size"), root.captureStatus.bytesWritten === undefined ? "–" : root.formatSize(root.captureStatus.bytesWritten)],
                        [qsTr("Captured video"), root.captureStatus.videoSeconds === undefined ? "–" : Number(root.captureStatus.videoSeconds).toFixed(1) + " s"]
                    ] : []
                    delegate: CaptureInfoRow {
                        required property var modelData
                        rowName: modelData[0]
                        rowValue: modelData[1]
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: captureInfoDialog.section === 0
                    text: qsTr("Rates use the interval shown above. Capture FPS counts session deliveries; encode FPS counts accepted inputs, including delayed output. Output bitrate includes successfully muxed audio/video payload, not container overhead. Recent timing covers the last 5 seconds, up to 1024 completed calls. Encoder-call time includes conversion, GPU import and driver waits; it is not end-to-end latency. Transport changes are observed deliveries, not inferred failure reasons. Drop counters do not include every compositor or PipeWire loss.")
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
                Label {
                    visible: captureInfoDialog.section === 1
                    text: qsTr("Audio")
                    color: Theme.textColor
                    font.bold: true
                    font.pixelSize: Theme.fontSizeMedium
                }
                Label {
                    Layout.fillWidth: true
                    visible: captureInfoDialog.section === 1
                    text: (root.captureStatus.separateAudio ? qsTr("Separate tracks") : qsTr("Mixed track"))
                        + " · " + root.captureInfoValue("audioBitrateKbps", qsTr(" kbps target per encoded stream"))
                    color: Theme.textSecondaryColor
                    wrapMode: Text.WordWrap
                }
                Repeater {
                    model: captureInfoDialog.section === 1 ? (root.captureStatus.audioStreams || []) : []
                    delegate: CaptureInfoRow {
                        required property var modelData
                        rowName: qsTr("Audio stream %1").arg(modelData.stream)
                        rowValue: modelData.codec + " · " + modelData.sampleRate + " Hz · "
                            + qsTr("%1 channels").arg(modelData.channels)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: captureInfoDialog.section === 1 && !(root.captureStatus.audioStreams || []).length
                    text: qsTr("No encoded audio streams")
                    color: Theme.textMutedColor
                }
                Repeater {
                    model: captureInfoDialog.section === 1 ? 3 : 0
                    delegate: ColumnLayout {
                        id: audioPanel
                        required property int index
                        readonly property var signalData: root.audioSignalForRole(index)
                        objectName: "captureAudioSignal" + index
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.spacingSmall
                        spacing: Theme.spacingSmall
                        Label {
                            Layout.fillWidth: true
                            text: root.audioHealthName(audioPanel.index) + ": " + root.audioHealthText(audioPanel.index)
                            color: root.audioHealthColor(audioPanel.index)
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: root.audioHealthTarget(audioPanel.index) || qsTr("No bound source")
                            color: Theme.textSecondaryColor
                            wrapMode: Text.WrapAnywhere
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 6
                            radius: 3
                            color: Theme.borderColor
                            Rectangle {
                                height: parent.height
                                width: parent.width * root.audioMeterFraction(audioPanel.signalData.peak)
                                radius: parent.radius
                                color: audioPanel.signalData.peak >= 1 ? Theme.errorColor : Theme.accentColor
                            }
                        }
                        CaptureInfoRow {
                            rowName: qsTr("Peak (≈1 s)")
                            rowValue: root.audioDb(audioPanel.signalData, "peak")
                        }
                        CaptureInfoRow {
                            rowName: qsTr("Peak hold (≈5 s)")
                            rowValue: root.audioDb(audioPanel.signalData, "peakHold")
                        }
                        CaptureInfoRow {
                            rowName: qsTr("RMS (≈1 s)")
                            rowValue: root.audioDb(audioPanel.signalData, "rms")
                        }
                        Label {
                            Layout.fillWidth: true
                            text: audioPanel.signalData.clippingRisk ? qsTr("Full scale / clipping risk · last 5 s")
                                : audioPanel.signalData.received ? qsTr("No full-scale samples in the last 5 s") : qsTr("Clipping: no measurements")
                            color: audioPanel.signalData.clippingRisk ? Theme.errorColor : Theme.textMutedColor
                            wrapMode: Text.WordWrap
                        }
                        CaptureInfoRow {
                            rowName: qsTr("Full-scale samples (since source bind)")
                            rowValue: audioPanel.signalData.fullScaleSamplesTotal === undefined ? "–"
                                : String(audioPanel.signalData.fullScaleSamplesTotal)
                        }
                        CaptureInfoRow {
                            rowName: qsTr("Mute control")
                            rowValue: audioPanel.signalData.muted === undefined ? "–"
                                : audioPanel.signalData.muted ? qsTr("Muted") : qsTr("Unmuted")
                        }
                        CaptureInfoRow {
                            rowName: qsTr("Last audio buffer")
                            rowValue: root.audioReceiptText(audioPanel.index, audioPanel.signalData)
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: captureInfoDialog.section === 1
                    text: qsTr("Post-mute source PCM, before session startup gating, track mixing and encoding. Peak/RMS use about 1 second; peak hold and clipping risk use about 5 seconds, in 100 ms buckets. 0 dBFS is full scale; digital silence is −∞. RMS averages sample power across channels, not LUFS. These are sample peaks, not true-peak or physical microphone loudness; final mixed-track clipping can differ. History follows each source binding and resets when that source is replaced or restarted.")
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
                Label {
                    visible: captureInfoDialog.section === 2
                    text: qsTr("Session and source")
                    color: Theme.textColor
                    font.bold: true
                    font.pixelSize: Theme.fontSizeMedium
                }
                Repeater {
                    model: captureInfoDialog.section === 2 ? [
                        [qsTr("Game"), root.captureStatus.gameName || "–"],
                        [qsTr("Detected game identity"), root.captureInfoValue("detectedGameKey")],
                        [qsTr("Steam App ID"), root.captureInfoValue("detectedSteamAppId")],
                        [qsTr("Detected game PID (at capture start)"), root.captureInfoValue("detectedGamePid")],
                        [qsTr("Capture backend"), root.captureInfoValue("captureBackend")],
                        [qsTr("Portal stream label"), root.captureStatus.sourceLabel || qsTr("Not reported")],
                        [qsTr("Portal application / ID"), root.captureStatus.sourceAppId || qsTr("Not reported")],
                        [qsTr("Portal source PID"), root.captureInfoValue("sourcePid")],
                        [root.captureStatus.mode === "replay" ? qsTr("Current segment duration") : qsTr("Session duration"), root.captureInfoNumber("sessionSeconds", 1, " s")]
                    ].concat(root.captureStatus.mode === "replay" ? [
                        [qsTr("DVR session duration"), root.captureInfoNumber("dvrSessionSeconds", 1, " s")],
                        [qsTr("Segments retained (including current)"), root.captureInfoValue("retainedSegments")],
                        [qsTr("Segments finalizing"), root.captureInfoValue("retiringSegments")],
                        [qsTr("Retained video (including safety margin)"), root.captureInfoNumber("retainedVideoSeconds", 1, " s")]
                    ] : []) : []
                    delegate: CaptureInfoRow {
                        required property var modelData
                        rowName: modelData[0]
                        rowValue: modelData[1]
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: captureInfoDialog.section === 2 && root.captureStatus.mode === "replay"
                    text: qsTr("DVR buffer: %1 / %2 s (approximate)")
                        .arg(Number(root.captureStatus.bufferSeconds || 0).toFixed(1))
                        .arg(root.captureStatus.bufferTargetSeconds || 0)
                    color: Theme.textSecondaryColor
                    wrapMode: Text.WordWrap
                }
                Label {
                    Layout.fillWidth: true
                    visible: captureInfoDialog.section === 2
                    text: qsTr("Detected identity, Steam ID and game PID are pinned when capture starts; they do not prove which window a manual picker selected. Portal metadata is separate and may name the compositor (such as kwin_wayland) rather than the game. DVR session duration starts when DVR is armed; buffered video is measured separately.")
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
        }
        footer: DialogButtonBox {
            padding: Theme.spacingLarge
            background: Item {}
            TrophyButton {
                objectName: "captureInfoCloseButton"
                text: qsTr("Close")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                onClicked: captureInfoDialog.close()
            }
        }
    }

    HoverPreview {
        id: libraryPreview
        objectName: "libraryHoverPreview"
        parkingItem: root.contentItem
    }

    // The library layout persists in settings so the dashboard reopens
    // in the user's last-used mode (grid default).
    Component.onCompleted: {
        if (typeof settingsManager !== "undefined" && settingsManager)
            libraryListView = settingsManager.libraryListView
    }

    onLibraryListViewChanged: {
        if (typeof settingsManager !== "undefined" && settingsManager
                && settingsManager.libraryListView !== root.libraryListView)
            settingsManager.libraryListView = root.libraryListView
    }

    // Session-persistent activity event feed (clip saves, deletions,
    // storage warnings). Library entries are derived, so transient events
    // live in the activityFeed backend (activity.json next to
    // settings.json) and updateActivity() merges both newest-first.
    function pushActivityEvent(kind, dot, title, detail, path) {
        if (typeof activityFeed === "undefined" || !activityFeed)
            return
        activityFeed.addEvent(kind, dot, title, detail, path || "")
        activityModel.updateActivity()
    }

    function activityBaseName(path) {
        if (!path)
            return qsTr("Unknown file")
        var parts = ("" + path).split("/")
        return parts[parts.length - 1] || qsTr("Unknown file")
    }

    // Friendly game name from a file path, mirroring
    // Recording::parseFileNameForGameName (everything before the
    // timestamp, underscores to spaces; clip _Replay suffix first).
    function activityGameName(path) {
        var base = root.activityBaseName(path).replace(/\.[^.]*$/, "").replace(/_Replay$/, "")
        var ts = base.search(/_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}$/)
        if (ts > 0)
            base = base.substring(0, ts)
        return (base.replace(/_/g, " ") || qsTr("Unknown Game"))
    }

    // Same detail line as a Completed row: size • duration • resolution.
    function recordingDetail(clip) {
        var res = (clip.width > 0 && clip.height > 0) ? " • %1×%2".arg(clip.width).arg(clip.height) : ""
        return formatSize(clip.size) + " • " + formatDuration(clip.duration) + res
    }

    function formatDuration(seconds) {
        if (!seconds || seconds <= 0)
            return "00:00";

        var h = Math.floor(seconds / 3600);
        var m = Math.floor((seconds % 3600) / 60);
        var s = seconds % 60;
        if (h > 0)
            return h + ":" + m.toString().padStart(2, '0') + ":" + s.toString().padStart(2, '0');
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
        // The grid prepends a live cell; the list uses a separate header.
        var offset = (typeof recordingManager !== "undefined" && recordingManager && recordingManager.isRecording) ? 1 : 0;
        if (libraryView.item)
            libraryView.item.positionViewAtIndex(index + offset, ListView.Beginning);
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
                                text: qsTr("Library")
                                font.pixelSize: Theme.fontSizeMedium
                                font.weight: Font.Medium
                                color: Theme.textColor
                            }

                            Item { Layout.fillWidth: true }

                            RowLayout {
                                spacing: 4

                                TrophyButton {
                                    objectName: "libraryGridButton"
                                    implicitWidth: 32
                                    implicitHeight: 28
                                    padding: 0
                                    horizontalPadding: 0
                                    icon.source: "qrc:/icons/view-grid.svg"
                                    icon.width: 18
                                    icon.height: 18
                                    outlined: true
                                    checked: !root.libraryListView
                                    Accessible.name: qsTr("Grid view")
                                    ToolTip.text: qsTr("Grid view")
                                    ToolTip.visible: hovered
                                    onClicked: root.libraryListView = false
                                }

                                TrophyButton {
                                    objectName: "libraryListButton"
                                    implicitWidth: 32
                                    implicitHeight: 28
                                    padding: 0
                                    horizontalPadding: 0
                                    icon.source: "qrc:/icons/view-list.svg"
                                    icon.width: 18
                                    icon.height: 18
                                    outlined: true
                                    checked: root.libraryListView
                                    Accessible.name: qsTr("List view")
                                    ToolTip.text: qsTr("List view")
                                    ToolTip.visible: hovered
                                    onClicked: root.libraryListView = true
                                }

                                Rectangle {
                                    Layout.leftMargin: Theme.spacingSmall
                                    Layout.rightMargin: Theme.spacingSmall
                                    Layout.preferredWidth: 1
                                    Layout.preferredHeight: 18
                                    color: Theme.borderColor
                                }

                                TrophyButton {
                                    id: filterButton
                                    objectName: "libraryFilterButton"
                                    implicitWidth: 32
                                    implicitHeight: 28
                                    padding: 0
                                    horizontalPadding: 0
                                    icon.source: "qrc:/icons/filter.svg"
                                    icon.width: 18
                                    icon.height: 18
                                    outlined: true
                                    checked: filterPopup.opened || (recordingLibrary &&
                                        (recordingLibrary.kindFilter !== 0 || recordingLibrary.sortOrder !== 0 || recordingLibrary.protectedOnly))
                                    Accessible.name: qsTr("Filter and sort captures")
                                    ToolTip.text: qsTr("Filter and sort captures")
                                    ToolTip.visible: hovered
                                    onClicked: filterPopup.opened ? filterPopup.close() : filterPopup.open()

                                    Popup {
                                        id: filterPopup
                                        objectName: "libraryFilterPopup"
                                        x: filterButton.width - width
                                        y: filterButton.height + Theme.spacingNormal
                                        width: 260
                                        padding: 20
                                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                                        enter: Transition {
                                            NumberAnimation {
                                                property: "opacity"
                                                from: 0
                                                to: 1
                                                duration: Theme.animationFast
                                            }
                                        }
                                        exit: Transition {
                                            NumberAnimation {
                                                property: "opacity"
                                                from: 1
                                                to: 0
                                                duration: Theme.animationFast
                                            }
                                        }

                                        function setKindVisible(kind, enabled) {
                                            var current = recordingLibrary.kindFilter
                                            var recordings = current === 0 || current === 1
                                            var clips = current === 0 || current === 2
                                            if (kind === 1)
                                                recordings = enabled
                                            else
                                                clips = enabled
                                            window.clearSelection()
                                            recordingLibrary.kindFilter = recordings
                                                ? (clips ? 0 : 1) : (clips ? 2 : 3)
                                        }

                                        background: Rectangle {
                                            radius: Theme.radiusMedium
                                            gradient: Gradient {
                                                GradientStop { position: 0; color: Qt.lighter(Theme.cardColor, 1.12) }
                                                GradientStop { position: 1; color: Theme.cardColor }
                                            }
                                            border.width: 1
                                            border.color: Qt.lighter(Theme.borderColor, 1.25)
                                            layer.enabled: filterPopup.visible
                                            layer.effect: MultiEffect {
                                                shadowEnabled: true
                                                shadowColor: "#80000000"
                                                shadowBlur: 0.7
                                                shadowVerticalOffset: 6
                                                blurMax: 32
                                            }
                                        }

                                        contentItem: ColumnLayout {
                                            spacing: Theme.spacingSmall

                                            Label {
                                                text: qsTr("Show")
                                                color: Theme.textSecondaryColor
                                                font.pixelSize: Theme.fontSizeSmall
                                                font.weight: Font.Medium
                                                font.capitalization: Font.AllUppercase
                                                font.letterSpacing: 1
                                            }

                                            Repeater {
                                                model: [qsTr("Recordings"), qsTr("Clips")]
                                                delegate: CheckBox {
                                                    id: kindCheck
                                                    required property int index
                                                    required property string modelData
                                                    objectName: index === 0 ? "libraryShowRecordings" : "libraryShowClips"
                                                    Layout.fillWidth: true
                                                    implicitHeight: 28
                                                    text: modelData
                                                    padding: 0
                                                    spacing: Theme.spacingSmall
                                                    checked: recordingLibrary && (recordingLibrary.kindFilter === 0
                                                        || recordingLibrary.kindFilter === index + 1)
                                                    onToggled: filterPopup.setKindVisible(index + 1, checked)

                                                    indicator: Rectangle {
                                                        implicitWidth: 18
                                                        implicitHeight: 18
                                                        y: (kindCheck.height - height) / 2
                                                        radius: 4
                                                        color: kindCheck.checked ? Theme.accentColor : "transparent"
                                                        border.color: kindCheck.checked ? Theme.accentColor : Theme.textMutedColor
                                                        Label {
                                                            anchors.centerIn: parent
                                                            text: "✓"
                                                            color: "white"
                                                            visible: kindCheck.checked
                                                        }
                                                    }

                                                    contentItem: Label {
                                                        text: kindCheck.text
                                                        leftPadding: kindCheck.indicator.width + kindCheck.spacing
                                                        color: Theme.textColor
                                                        font.pixelSize: Theme.fontSizeSmall
                                                        verticalAlignment: Text.AlignVCenter
                                                    }
                                                }
                                            }

                                            CheckBox {
                                                id: protectedCheck
                                                objectName: "libraryProtectedOnly"
                                                Layout.fillWidth: true
                                                implicitHeight: 28
                                                padding: 0
                                                spacing: Theme.spacingSmall
                                                text: qsTr("Protected only")
                                                checked: recordingLibrary ? recordingLibrary.protectedOnly : false
                                                onToggled: { if (recordingLibrary) recordingLibrary.protectedOnly = checked }
                                                indicator: Rectangle {
                                                    implicitWidth: 18; implicitHeight: 18
                                                    y: (protectedCheck.height - height) / 2
                                                    radius: 4
                                                    color: protectedCheck.checked ? Theme.accentColor : "transparent"
                                                    border.color: protectedCheck.checked ? Theme.accentColor : Theme.textMutedColor
                                                    Label { anchors.centerIn: parent; text: "✓"; color: "white"; visible: protectedCheck.checked }
                                                }
                                                contentItem: Label {
                                                    text: protectedCheck.text
                                                    leftPadding: protectedCheck.indicator.width + protectedCheck.spacing
                                                    color: Theme.textColor
                                                    font.pixelSize: Theme.fontSizeSmall
                                                    verticalAlignment: Text.AlignVCenter
                                                }
                                            }

                                            Rectangle {
                                                Layout.fillWidth: true
                                                Layout.topMargin: Theme.spacingSmall
                                                Layout.bottomMargin: Theme.spacingSmall
                                                implicitHeight: 1
                                                color: Theme.borderColor
                                            }

                                            Label {
                                                text: qsTr("Sort by")
                                                color: Theme.textSecondaryColor
                                                font.pixelSize: Theme.fontSizeSmall
                                                font.weight: Font.Medium
                                                font.capitalization: Font.AllUppercase
                                                font.letterSpacing: 1
                                            }

                                            RowLayout {
                                                id: sortControls
                                                Layout.fillWidth: true
                                                spacing: Theme.spacingSmall
                                                // Date, size, length, name: map both directions to
                                                // the existing backend enum without changing its API.
                                                readonly property var ascendingOrders: [1, 4, 6, 3]
                                                readonly property var descendingOrders: [0, 2, 5, 7]
                                                readonly property int order: recordingLibrary ? recordingLibrary.sortOrder : 0
                                                readonly property bool descending: descendingOrders.indexOf(order) >= 0
                                                readonly property int fieldIndex: descending
                                                    ? descendingOrders.indexOf(order) : ascendingOrders.indexOf(order)

                                                TrophyComboBox {
                                                    objectName: "librarySortOrder"
                                                    Layout.fillWidth: true
                                                    model: [qsTr("Date"), qsTr("Size"), qsTr("Length"), qsTr("Name")]
                                                    currentIndex: sortControls.fieldIndex
                                                    Accessible.name: qsTr("Sort captures")
                                                    onActivated: {
                                                        window.clearSelection()
                                                        recordingLibrary.sortOrder = sortControls.descending
                                                            ? sortControls.descendingOrders[currentIndex]
                                                            : sortControls.ascendingOrders[currentIndex]
                                                    }
                                                }

                                                TrophyButton {
                                                    objectName: "librarySortDirection"
                                                    implicitWidth: 32
                                                    implicitHeight: 32
                                                    padding: 0
                                                    horizontalPadding: 0
                                                    text: sortControls.descending ? "↓" : "↑"
                                                    outlined: true
                                                    Accessible.name: sortControls.descending
                                                        ? qsTr("Descending; switch to ascending")
                                                        : qsTr("Ascending; switch to descending")
                                                    ToolTip.text: sortControls.descending
                                                        ? qsTr("Descending; click for ascending")
                                                        : qsTr("Ascending; click for descending")
                                                    ToolTip.visible: hovered
                                                    onClicked: {
                                                        window.clearSelection()
                                                        recordingLibrary.sortOrder = sortControls.descending
                                                            ? sortControls.ascendingOrders[sortControls.fieldIndex]
                                                            : sortControls.descendingOrders[sortControls.fieldIndex]
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                spacing: Theme.spacingSmall
                                visible: window.selectedRecordings && window.selectedRecordings.length > 0
                                // True while the background bulk delete runs;
                                    // expired undo batches are permanently removed.
                                property bool bulkDeleting: window.storageManager && window.storageManager.bulkDeleteInProgress

                                TrophyButton {
                                    icon.source: "qrc:/icons/share.svg"
                                    icon.width: 18
                                    icon.height: 18
                                    outlined: true
                                    implicitWidth: 32
                                    implicitHeight: 28
                                    padding: 0
                                    horizontalPadding: 0
                                    Accessible.name: qsTr("Export Selected (%1)").arg(window.selectedRecordings.length)
                                    ToolTip.text: Accessible.name
                                    ToolTip.visible: hovered
                                    enabled: !parent.bulkDeleting
                                    onClicked: exportActions.open()
                                    Popup {
                                        id: exportActions
                                        y: parent.height + 4
                                        width: 230
                                        padding: Theme.spacingSmall
                                        background: Rectangle { color: Theme.cardColor; radius: Theme.radiusMedium; border.color: Theme.borderColor }
                                        contentItem: ColumnLayout {
                                            TrophyButton {
                                                Layout.fillWidth: true
                                                text: qsTr("Share-ready MP4…")
                                                onClicked: { exportActions.close(); window.openShareExportBatch(window.selectedRecordings) }
                                            }
                                            TrophyButton {
                                                Layout.fillWidth: true
                                                text: qsTr("Copy original files…")
                                                onClicked: { exportActions.close(); window.exportSelected() }
                                            }
                                        }
                                    }
                                }

                                TrophyButton {
                                    icon.source: "qrc:/icons/trash.svg"
                                    icon.width: 18
                                    icon.height: 18
                                    implicitWidth: 32
                                    implicitHeight: 28
                                    padding: 0
                                    horizontalPadding: 0
                                    Accessible.name: parent.bulkDeleting ? qsTr("Deleting…") : qsTr("Delete Selected")
                                    ToolTip.text: Accessible.name
                                    ToolTip.visible: hovered
                                    baseColor: Theme.errorColor
                                    enabled: !parent.bulkDeleting
                                    onClicked: window.deleteSelected()
                                }

                                BusyIndicator {
                                    implicitWidth: 20
                                    implicitHeight: 20
                                    visible: parent.bulkDeleting
                                    running: visible
                                }
                                
                                TrophyButton {
                                    icon.source: "qrc:/icons/window-close.svg"
                                    icon.width: 18
                                    icon.height: 18
                                    outlined: true
                                    implicitWidth: 32
                                    implicitHeight: 28
                                    padding: 0
                                    horizontalPadding: 0
                                    Accessible.name: qsTr("Cancel selection")
                                    ToolTip.text: Accessible.name
                                    ToolTip.visible: hovered
                                    onClicked: window.clearSelection()
                                }
                            }
                        }

                        Loader {
                            id: libraryView
                            objectName: "libraryView"
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            sourceComponent: root.libraryListView ? listComponent : gridComponent

                            Rectangle {
                                id: libraryBottomFade
                                // The grid surface is translucent; fade to its opaque
                                // composite so cards actually disappear at the edge.
                                readonly property color fadeColor: Qt.tint(Theme.backgroundColor, Theme.recordingGridBackgroundColor)
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                anchors.bottom: parent.bottom
                                height: Theme.spacingLarge * 2
                                z: 1
                                opacity: libraryView.item ? Math.max(0, Math.min(1, (libraryView.item.originY + libraryView.item.contentHeight + libraryView.item.bottomMargin - libraryView.item.height - libraryView.item.contentY) / height)) : 0
                                visible: opacity > 0
                                gradient: Gradient {
                                    GradientStop { position: 0; color: Qt.alpha(libraryBottomFade.fadeColor, 0) }
                                    GradientStop { position: 1; color: libraryBottomFade.fadeColor }
                                }
                            }
                        }

                        Component {
                            id: listComponent
                            RecordingListView {
                                previewController: libraryPreview
                                leftMargin: Theme.spacingSmall
                                rightMargin: Theme.spacingSmall
                                topMargin: Theme.spacingSmall
                                bottomMargin: Theme.spacingSmall
                            }
                        }

                        Component {
                            id: gridComponent
                            RecordingGridView {
                                previewController: libraryPreview
                                clip: true
                                leftMargin: Theme.spacingSmall
                                topMargin: Theme.spacingSmall
                                bottomMargin: Theme.spacingSmall
                                rightMargin: Theme.spacingSmall

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
                objectName: "captureStatusCard"
                Layout.fillWidth: true
                Layout.preferredHeight: statusCardContent.implicitHeight
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    id: statusCardContent
                    anchors.fill: parent
                    spacing: 0

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
                        objectName: "statusTextColumn"
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 4

                        RowLayout {
                            spacing: 8
                            Rectangle {
                                width: 10; height: 10; radius: 5
                                Layout.alignment: Qt.AlignVCenter
                                color: (recordingManager && recordingManager.isRecording) ? "white" : ((gameDetector && gameDetector.currentGameName) ? Theme.successColor : Theme.textMutedColor)
                                SequentialAnimation on opacity {
                                    running: (recordingManager && recordingManager.isRecording)
                                    loops: Animation.Infinite
                                    NumberAnimation { from: 1; to: 0.4; duration: 800 }
                                    NumberAnimation { from: 0.4; to: 1; duration: 800 }
                                }
                            }
                            Label {
                                text: (recordingManager && recordingManager.isRecording) ? qsTr("RECORDING")
                                      : (app && app.recordingPhase === "initializing") ? qsTr("STARTING")
                                      : (app && app.recordingPhase === "stopping") ? qsTr("SAVING")
                                      : ((gameDetector && gameDetector.currentGameName) ? qsTr("READY") : qsTr("IDLE"))
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

                    TrophyButton {
                        objectName: "captureInfoButton"
                        text: "ⓘ"
                        flat: true
                        padding: 0
                        horizontalPadding: 0
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        visible: !!root.captureStatus.mode
                        enabled: !!root.captureStatus.mode
                        Accessible.name: qsTr("Capture info")
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Live capture info")
                        ToolTip.delay: 600
                        onClicked: captureInfoDialog.open()
                    }

                    Button {
                        id: recordBtn
                        objectName: "recordButton"
                        enabled: (recordingManager && recordingManager.isRecording)
                                 || (app && app.canStartRecording)
                        Layout.preferredWidth: 44; Layout.preferredHeight: 44
                        Layout.alignment: Qt.AlignVCenter
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

                Item {
                    id: healthReveal
                    objectName: "captureHealthStatus"
                    Layout.fillWidth: true
                    implicitHeight: (healthContent.implicitHeight + Theme.spacingMedium * 2) * reveal
                    property real reveal: root.captureStatus.mode ? 1 : 0
                    visible: reveal > 0
                    enabled: !!root.captureStatus.mode
                    clip: true
                    Behavior on reveal {
                        NumberAnimation { duration: 240; easing.type: Easing.InOutCubic }
                    }

                ColumnLayout {
                    id: healthContent
                    x: Theme.spacingMedium
                    y: Theme.spacingMedium - (1 - healthReveal.reveal) * Theme.spacingMedium
                    width: healthReveal.width - Theme.spacingMedium * 2
                    opacity: healthReveal.reveal
                    spacing: Theme.spacingSmall

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Capture health")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Font.Medium
                        }
                        Rectangle {
                            implicitWidth: videoStatus.implicitWidth + 12
                            implicitHeight: videoStatus.implicitHeight + 6
                            radius: height / 2
                            color: Qt.alpha(root.healthStatus.hasVideo ? Theme.successColor : Theme.warningColor, 0.12)
                            Label {
                                id: videoStatus
                                anchors.centerIn: parent
                                text: root.healthStatus.hasVideo ? qsTr("Video active") : qsTr("Starting…")
                                font.pixelSize: Theme.fontSizeTiny
                                color: root.healthStatus.hasVideo ? Theme.successColor : Theme.warningColor
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !root.healthStatus.hasVideo || root.healthStatus.mode === "replay"
                        text: !root.healthStatus.hasVideo ? qsTr("Waiting for video frames…") : (root.healthStatus.bufferSeconds >= root.healthStatus.bufferTargetSeconds ? qsTr("Ready to clip · ~%1 s buffered").arg(root.healthStatus.bufferTargetSeconds) : qsTr("Replay buffer filling · %1 / %2 s").arg(Math.floor(root.healthStatus.bufferSeconds || 0)).arg(root.healthStatus.bufferTargetSeconds || 0))
                        color: Theme.textSecondaryColor
                        font.pixelSize: Theme.fontSizeTiny
                        wrapMode: Text.WordWrap
                    }
                    Repeater {
                        model: 3
                        RowLayout {
                            required property int index
                            visible: index !== 1 || !(root.healthStatus.desktopAudio || root.healthStatus.desktopFallback)
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall
                            Rectangle {
                                implicitWidth: 5
                                implicitHeight: 5
                                radius: 3
                                color: root.audioHealthColor(parent.index)
                            }
                            Label {
                                Layout.preferredWidth: 42
                                text: root.audioHealthName(parent.index)
                                color: Theme.textSecondaryColor
                                font.pixelSize: Theme.fontSizeTiny
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.audioHealthText(parent.index)
                                color: Theme.textColor
                                font.pixelSize: Theme.fontSizeTiny
                                horizontalAlignment: Text.AlignRight
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: !!root.healthStatus.desktopFallback
                        text: qsTr("Desktop fallback includes other apps.")
                        color: Theme.warningColor
                        font.pixelSize: Theme.fontSizeTiny
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Connection status only · check meters for signal")
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeTiny
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.dropWarningTicks >= 3
                        text: qsTr("Video queue is dropping frames. Try a lower resolution or frame rate.")
                        color: Theme.errorColor
                        font.pixelSize: Theme.fontSizeTiny
                        wrapMode: Text.WordWrap
                    }
                    TrophyButton {
                        objectName: "addRecordingBookmark"
                        Layout.fillWidth: true
                        visible: root.healthStatus.mode === "recording"
                        text: qsTr("Bookmark moment")
                        icon.source: "qrc:/icons/bookmark.svg"
                        enabled: !!root.healthStatus.hasVideo
                        onClicked: { if (app) app.requestBookmark() }
                    }
                }
                }
                }
            }

            // Storage Usage
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: storageContent.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.cardColor
                ColumnLayout {
                    id: storageContent
                    anchors.fill: parent; anchors.margins: Theme.spacingMedium; spacing: Theme.spacingSmall
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Storage Usage"); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor; Layout.alignment: Qt.AlignTop; Layout.topMargin: 1 }
                        Item { Layout.fillWidth: true }
                        ColumnLayout {
                            spacing: 2
                            Layout.alignment: Qt.AlignTop
                            Label { objectName: "storageRecordingCount"; text: qsTr("%n recording(s)", "", (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.totalCount : 0); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor; Layout.alignment: Qt.AlignRight }
                            Label {
                                text: ((typeof storageManager !== "undefined" && storageManager && storageManager.autoDeleteOldest) ? qsTr("Auto-Cleanup: on") : qsTr("Auto-Cleanup: off"))
                                font.pixelSize: Theme.fontSizeTiny
                                color: (typeof storageManager !== "undefined" && storageManager && storageManager.autoDeleteOldest) ? Theme.successColor : Theme.textMutedColor
                                Layout.alignment: Qt.AlignRight
                            }
                        }
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

            // Recording settings box (height follows content; a fixed
            // height either clips the meters or leaves dead slack, and a
            // plain Rectangle takes no height from anchored children)
            Rectangle {
                objectName: "recordingSettingsCard"
                Layout.fillWidth: true
                Layout.preferredHeight: settingsContent.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    id: settingsContent
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
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 1
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
                        columns: 3
                        columnSpacing: Theme.spacingMedium
                        rowSpacing: 0

                        // Keep text rows stable while the independent audio
                        // stack collapses hidden meters without empty slots.
                        Label { text: qsTr("Encoder"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor; Layout.row: 0; Layout.column: 0 }
                        Label { text: qsTr("Resolution"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor; Layout.row: 0; Layout.column: 1 }
                        Label { text: qsTr("Audio"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor; Layout.row: 0; Layout.column: 2 }
                        Label {
                            text: {
                                if (settingsManager && settingsManager.encoder === "auto") {
                                     var active = (recordingManager && recordingManager.isRecording && recordingManager.activeEncoderName) ? recordingManager.activeEncoderName : (recordingManager ? recordingManager.detectedEncoderName : "");
                                     return active || (settingsManager ? settingsManager.encoderDisplayName : "");
                                 }
                                return settingsManager ? settingsManager.encoderDisplayName : "";
                            }
                            font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor; elide: Text.ElideRight
                            Layout.row: 1; Layout.column: 0; Layout.alignment: Qt.AlignVCenter
                        }
                        Label {
                            text: (settingsManager ? settingsManager.resolution : "") === "auto" ? qsTr("Auto (%1x%2)").arg(settingsManager.autoResolutionWidth).arg(settingsManager.autoResolutionHeight) : (settingsManager ? settingsManager.resolution : "")
                            font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor
                            Layout.row: 1; Layout.column: 1; Layout.alignment: Qt.AlignVCenter
                        }
                        Label { text: qsTr("Quality"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor; Layout.row: 2; Layout.column: 0; Layout.minimumHeight: 18; verticalAlignment: Text.AlignVCenter }
                        Label { text: qsTr("FPS"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor; Layout.row: 2; Layout.column: 1 }
                        Label { text: (settingsManager ? settingsManager.videoBitrate : 0) + " kbps"; font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor; Layout.row: 3; Layout.column: 0; Layout.alignment: Qt.AlignVCenter }
                        Label { text: settingsManager ? (settingsManager.fps === 0 ? "∞" : settingsManager.fps.toString()) : "0"; font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor; Layout.row: 3; Layout.column: 1; Layout.alignment: Qt.AlignVCenter }

                        Item {
                            Layout.row: 1; Layout.column: 2; Layout.rowSpan: 3
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            implicitHeight: 3 * 18
                            implicitWidth: audioMeterStack.implicitWidth

                            ColumnLayout {
                                id: audioMeterStack
                                width: parent.width
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 0

                                AudioLevelMeter {
                                    id: meter0
                                    objectName: "primaryAudioMeter"
                                    horizontal: true
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 18
                                    label: root.captureStatus.mode ? (root.captureStatus.desktopAudio || root.captureStatus.desktopFallback ? qsTr("System") : qsTr("Game")) : (settingsManager && settingsManager.audioMode === "desktop" ? qsTr("System") : qsTr("Game"))
                                    onMuteToggled: function(muted) { if (app) app.setTrackMuted(0, muted) }
                                }

                                AudioLevelMeter {
                                    id: meter1
                                    objectName: "voiceAudioMeter"
                                    // Desktop mode mixes; fallback includes Voice.
                                    visible: !(settingsManager && settingsManager.audioMode === "desktop") && !root.captureStatus.desktopFallback && !root.captureStatus.desktopAudio
                                    horizontal: true
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 18
                                    label: audioMixer ? audioMixer.trackName(1) : ""
                                    onMuteToggled: function(muted) { if (app) app.setTrackMuted(1, muted) }
                                }
                                AudioLevelMeter {
                                    id: meter2
                                    objectName: "micAudioMeter"
                                    visible: !(settingsManager && settingsManager.audioMode === "desktop")
                                    horizontal: true
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 18
                                    label: audioMixer ? audioMixer.trackName(2) : ""
                                    onMuteToggled: function(muted) { if (app) app.setTrackMuted(2, muted) }
                                }
                            }
                        }
                    }

                    Timer {
                        interval: 100
                        running: root.visible
                        repeat: true
                        onTriggered: {
                            if (!app) return;
                            const live = (recordingManager && recordingManager.isRecording) || (replayManager && replayManager.active);
                            const meters = [meter0, meter1, meter2];
                            for (let i = 0; i < meters.length; i++) {
                                const m = meters[i];
                                if (m && m.visible) {
                                    m.level = live ? app.trackLevel(i) : 0;
                                    m.muted = app.isTrackMuted(i);
                                }
                            }
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
                        id: activityScroll
                        Layout.fillWidth: true; Layout.fillHeight: true
                        // Long clip filenames must truncate (labels elide),
                        // never widen the content and summon a h-bar.
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical: TrophyScrollBar {}
                        clip: true
                        ColumnLayout {
                            width: activityScroll.availableWidth; spacing: 0
                            Repeater {
                                model: ListModel {
                                    id: activityModel
                                    objectName: "recentActivityModel"
                                    Component.onCompleted: updateActivity()
                                    function updateActivity() {
                                        var rows = []
                                        var now = new Date()
                                        if (recordingManager && recordingManager.isRecording) {
                                            rows.push({ at: now, kind: "started", dot: "red", title: qsTr("Started: %1").arg(recordingManager.currentGameName || "Unknown Game"), detail: qsTr("Recording in progress"), time: "Now", stamp: "" })
                                        }
                                        if (typeof activityFeed !== "undefined" && activityFeed) {
                                            var stored = activityFeed.events
                                            for (var s = 0; s < stored.length; s++) {
                                                var ev = stored[s]
                                                var at = new Date(ev.at)
                                                if (isNaN(at)) continue
                                                // Clip rows enrich from the live library entry
                                                // (same detail as Completed); the stored span
                                                // covers deleted clips whose file is gone.
                                                var evDetail = ev.detail
                                                if (ev.kind === "clip" && ev.path) {
                                                    var rec = recordingLibrary ? recordingLibrary.getRecording(ev.path) : null
                                                    if (rec) evDetail = root.recordingDetail(rec)
                                                }
                                                rows.push({ at: at, kind: ev.kind, dot: ev.dot, title: ev.title, detail: evDetail, time: getRelativeTime(at), stamp: at.toLocaleString(Qt.locale(), "MMM d, yyyy HH:mm") })
                                            }
                                        }
                                        var captures = recordingLibrary ? recordingLibrary.allRecordings : []
                                        for (var i = 0; i < captures.length; i++) {
                                            var clip = captures[i]
                                            if (!clip) continue
                                            // Clips already show as "Clip saved" rows
                                            // above — listing them again as Completed
                                            // doubles every entry. The container tag
                                            // wins and survives renames; the suffix
                                            // covers older files.
                                            if (clip.isClip || (clip.fileName || "").endsWith("_Replay.mkv")) continue
                                            rows.push({ at: new Date(clip.date), kind: "completed", dot: "green", title: qsTr("Completed: %1").arg(clip.gameName || "Unknown Game"), detail: root.recordingDetail(clip), time: getRelativeTime(clip.date), stamp: clip.date.toLocaleString(Qt.locale(), "MMM d, yyyy HH:mm") })
                                        }
                                        rows.sort(function(a, b) { return b.at - a.at })
                                        clear()
                                        for (var r = 0; r < rows.length; r++)
                                            append({ kind: rows[r].kind, dot: rows[r].dot, title: rows[r].title, detail: rows[r].detail, time: rows[r].time, stamp: rows[r].stamp })
                                    }
                                    function getRelativeTime(d) {
                                        var diffMins = Math.floor((new Date() - new Date(d)) / 60000)
                                        if (diffMins < 1) return "Just now"
                                        if (diffMins < 60) return diffMins === 1 ? "1 min ago" : diffMins + " mins ago"
                                        var diffHours = Math.floor(diffMins / 60)
                                        if (diffHours < 24) return diffHours === 1 ? "1 hour ago" : diffHours + " hours ago"
                                        var diffDays = Math.floor(diffHours / 24)
                                        if (diffDays < 7) return diffDays === 1 ? "1 day ago" : diffDays + " days ago"
                                        if (diffDays < 30) {
                                            var weeks = Math.floor(diffDays / 7)
                                            return weeks === 1 ? "1 week ago" : weeks + " weeks ago"
                                        }
                                        if (diffDays < 365) {
                                            var months = Math.floor(diffDays / 30)
                                            return months === 1 ? "1 month ago" : months + " months ago"
                                        }
                                        var years = Math.floor(diffDays / 365)
                                        return years === 1 ? "1 year ago" : years + " years ago"
                                    }
                                }

                                // Timeline row: dot on a connecting rail, text to the right.
                                // Root is the RowLayout itself so its height comes
                                // from the text (anchoring a fill-layout inside a
                                // height-bound Item looped and collapsed the rows).
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.spacingSmall

                                    // Rail with connecting line; first row has no
                                    // segment above the dot, last row none below.
                                    // Segments run edge-to-edge through the dot
                                    // center (13) so rows join into one line.
                                    Item {
                                        Layout.preferredWidth: 14
                                        Layout.fillHeight: true
                                        Layout.minimumHeight: 46

                                        Rectangle {
                                            x: 6
                                            y: 0
                                            width: 2
                                            height: 13
                                            visible: index > 0
                                            color: Theme.withOpacity(Theme.textColor, 0.2)
                                        }

                                        Rectangle {
                                            x: 6
                                            y: 13
                                            width: 2
                                            height: parent.height - 13
                                            visible: index < activityModel.count - 1
                                            color: Theme.withOpacity(Theme.textColor, 0.2)
                                        }

                                        Rectangle {
                                            x: 2
                                            y: 8
                                            width: 10
                                            height: 10
                                            radius: 5
                                            color: model.dot === "green" ? Theme.successColor : model.dot === "blue" ? Theme.highlightedColor : model.dot === "orange" ? Theme.warningColor : Theme.errorColor
                                            border.width: 2
                                            border.color: Theme.cardColor
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: 4
                                        Layout.bottomMargin: 4
                                        spacing: 1

                                        Label { text: model.title; font.pixelSize: Theme.fontSizeSmall; font.weight: Font.Medium; color: Theme.textColor; elide: Text.ElideRight; Layout.fillWidth: true }
                                        Label { text: model.detail; font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor; elide: Text.ElideRight; Layout.fillWidth: true; visible: (model.detail || "") !== "" }
                                        Label { text: model.stamp && model.stamp !== "" ? "%1 • %2".arg(model.time).arg(model.stamp) : model.time; font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor; elide: Text.ElideRight; Layout.fillWidth: true }
                                    }
                                }
                            }

                            // Empty state
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("No activity yet — your recordings will show up here.")
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.textSecondaryColor
                                wrapMode: Text.WordWrap
                                visible: activityModel.count === 0
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

    Connections {
        target: recordingLibrary
        function onRecordingsChanged() { activityModel.updateActivity() }
        function onRecordingRemoved(filePath) {
            root.pushActivityEvent("deleted", "orange", qsTr("Deleted: %1").arg(root.activityGameName(filePath)), qsTr("Removed from library"))
        }
        function onRecordingsRemoved(filePaths) {
            for (var i = 0; i < filePaths.length; i++) {
                root.pushActivityEvent("deleted", "orange", qsTr("Deleted: %1").arg(root.activityGameName(filePaths[i])), qsTr("Removed from library"))
            }
        }
    }
    Connections { target: root.recorder; function onStateChanged() { activityModel.updateActivity() } }
    Connections {
        target: root.feed
        function onEventsChanged() { activityModel.updateActivity() }
    }
    Connections {
        target: root.replay
        function onReplaySaved(filePath, seconds) {
            var secs = Math.max(1, Math.round(seconds))
            var span = secs === 1 ? qsTr("Last 1 second") : qsTr("Last %1 seconds").arg(secs)
            var game = (root.replay && root.replay.currentGame) || qsTr("Unknown Game")
            root.pushActivityEvent("clip", "blue", qsTr("Clip saved: %1").arg(game), span, filePath)
        }
    }
    Connections {
        target: root.storage
        function onStorageLimitReached() {
            var auto = root.preferences ? root.preferences.autoDeleteOldest : false
            root.pushActivityEvent("storage", "red", qsTr("Storage Full"), auto ? qsTr("Over limit — oldest files will be auto-deleted") : qsTr("Over limit — enable auto-cleanup or free space manually"))
        }
        function onStorageLimitCannotCleanup() {
            root.pushActivityEvent("storage", "red", qsTr("Recording Stopped"), qsTr("Recording exceeded the storage limit"))
        }
        function onRecordingRenamed(oldPath, newPath) {
            root.pushActivityEvent("renamed", "blue", qsTr("Renamed: %1").arg(root.activityGameName(newPath)), qsTr("%1 → %2").arg(root.activityBaseName(oldPath)).arg(root.activityBaseName(newPath)), newPath)
        }
    }

    background: Rectangle { color: Theme.backgroundColor; bottomRightRadius: Theme.windowRadius; topLeftRadius: 26 }
}
