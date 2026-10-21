/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import TrophyBackend
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtQuick.Dialogs

ApplicationWindow {
    id: window

    // --- REQUIRED PROPERTIES (Injected from C++) ---
    required property var app
    required property var recordingManager
    required property var storageManager
    required property var recordingLibrary
    required property var gameDetector
    required property var settingsManager

    Binding {
        target: Theme
        property: "settings"
        value: window.settingsManager
    }

    WindowChrome {
        id: windowChrome
        targetWindow: window
    }

    Binding {
        target: Theme
        property: "squareWindowCorners"
        value: windowChrome.squareCorners || playerPage.playerFullscreen
    }

    // --- QML-DEFINED PROPERTIES ---
    property int currentPage: 0
    readonly property bool hoverPreviewsAllowed: window.active && !playerPage.visible
    property bool skipDeleteConfirmation: false
    property var selectedRecordings: []
    property int lastSelectedIndex: -1
    // Maximization the player fullscreen interrupts, so
    // exiting fullscreen restores it (not a plain window).
    property bool wasMaximizedBeforeFullscreen: false

    function requestAttention(reason) {
        // Popups live inside this window; opening one must never restore or
        // activate the window behind a fullscreen game.
        if (!window.active)
            notificationService.showAttention(reason);
    }

    onActiveChanged: {
        if (window.active)
            notificationService.clearAttention();
    }

    Connections {
        target: notificationService
        function onAttentionRequested() {
            if (window.visibility === Window.Minimized)
                window.showNormal();
            window.app.showMainWindow();
        }
    }

    onVisibilityChanged: {
        // The compositor may leave fullscreen on its own
        // (its own keybinding): keep the player's flag in
        // sync so the header and tracks come back.
        if (window.visibility !== Window.FullScreen && playerPage.playerFullscreen)
            playerPage.playerFullscreen = false
    }

    onCurrentPageChanged: {
        // Other navigation paths (global search results)
        // also close the overlay player.
        if (playerPage.visible)
            playerPage.close()
    }

    function toggleSelection(filePath) {
        var idx = selectedRecordings.indexOf(filePath);
        var temp = selectedRecordings.slice();
        if (idx === -1) {
            temp.push(filePath);
        } else {
            temp.splice(idx, 1);
        }
        selectedRecordings = temp;
    }

    function shiftSelectRange(currentIndex) {
        if (currentIndex < 0)
            return;
        if (lastSelectedIndex === -1 || !recordingLibrary) {
            // First selection in this sequence - just toggle normally
            if (recordingLibrary && currentIndex >= 0 && currentIndex < recordingLibrary.count) {
                var recording = recordingLibrary.get(currentIndex);
                if (recording) {
                    toggleSelection(recording.filePath);
                    lastSelectedIndex = currentIndex;
                }
            }
            return;
        }

        var startIdx = Math.min(lastSelectedIndex, currentIndex);
        var endIdx = Math.max(lastSelectedIndex, currentIndex);
        var temp = selectedRecordings.slice();

        // Select all items in the range
        for (var i = startIdx; i <= endIdx; i++) {
            if (recordingLibrary && i >= 0 && i < recordingLibrary.count) {
                var rec = recordingLibrary.get(i);
                if (rec && temp.indexOf(rec.filePath) === -1) {
                    temp.push(rec.filePath);
                }
            }
        }

        selectedRecordings = temp;
        lastSelectedIndex = currentIndex;
    }

    function clearSelection() {
        selectedRecordings = [];
        lastSelectedIndex = -1;
    }

    function toggleCaptureProtection(path) {
        const rec = recordingLibrary ? recordingLibrary.getRecording(path) : null;
        if (rec && !recordingLibrary.setProtected(path, !rec.protectedFromCleanup))
            showMetadataError();
    }

    function showMetadataError() {
        errorDialog.title = qsTr("Unable to save changes");
        errorDialog.message = qsTr("Could not save protection or bookmarks. Check recording-folder permissions. Existing metadata was left unchanged.");
        errorDialog.open();
    }

    function exportSelected() {
        if (selectedRecordings.length > 0) {
            exportDialog.open();
        }
    }

    function openShareExport(path, excludedAudioOrdinals) {
        if (!path || shareExporter.inspecting)
            return;
        shareDialog.sourcePath = path;
        shareDialog.sourcePaths = [];
        shareDialog.excludedAudioOrdinals = excludedAudioOrdinals || [];
        shareDialog.open();
    }

    function openShareExportBatch(paths) {
        if (!paths || !paths.length || shareExporter.inspecting)
            return;
        if (paths.length === 1) {
            openShareExport(paths[0]);
            return;
        }
        shareDialog.sourcePaths = paths.slice();
        shareDialog.excludedAudioOrdinals = [];
        shareDialog.open();
    }

    function queueDeletion(paths) {
        if (deletionQueue.enqueue(paths))
            window.clearSelection();
    }

    function deleteSelected() {
        if (selectedRecordings.length > 0) {
            if (window.skipDeleteConfirmation) {
                performDeleteSelected();
            } else {
                bulkDeleteDialog.open();
            }
        }
    }

    function performDeleteSelected() {
        // Background bulk delete: unlinks on a worker thread and
        // settles the library with one batch update, so selecting
        // dozens of recordings no longer freezes the UI. The selection
        // clears when bulkDeleteFinished arrives (below), keeping the
        // progress indicator visible until then.
        if (window.storageManager) {
            window.queueDeletion(selectedRecordings);
        }
    }

    function openWindowPicker() {
        console.log("openWindowPicker called. app.isWayland:", window.app.isWayland);
        if (window.app.isWayland) {
            // A detected game records directly (same as the hotkey/tray
            // paths); the picker is only for idle manual capture. The
            // portal reports every KWin source as generic "kwin_wayland"
            // and never names the picked item, so manual captures keep
            // the "Manual Recording" display name instead.
            if (window.gameDetector && window.gameDetector.currentGameName !== "")
                window.recordingManager.startRecording(window.gameDetector.currentGameName);
            else
                // On Wayland, the system portal handles window/screen selection.
                window.recordingManager.startRecording();
        } else {
            console.log("Opening windowPicker dialog.");
            windowPicker.open();
        }
    }

    function openFolder(path) {
        if (!path) return;

        Qt.openUrlExternally(recordingLibrary.localFileUrl(path));
    }

    // In-app player overlay (content area; Back/Escape closes
    // and stops playback).
    function openPlayer(filePath) {
        if (!filePath)
            return;
        var title = filePath;
        var dateStr = "";
        if (window.recordingLibrary) {
            var rec = window.recordingLibrary.getRecording(filePath);
            if (rec) {
                title = rec.gameName || rec.fileName || filePath;
                if (rec.date)
                    dateStr = Qt.formatDateTime(rec.date, "MMM d, yyyy HH:mm");
            }
        }
        playerPage.open(filePath, title, dateStr);
        playerPage.visible = true;
    }

    function confirmDelete(filePath, fileName) {
        if (window.skipDeleteConfirmation) {
            if (window.storageManager) {
                window.queueDeletion([filePath]);
            }
        } else {
            deleteDialog.filePath = filePath;
            deleteDialog.fileName = fileName;
            deleteDialog.open();
        }
    }

    function requestRename(filePath, currentName) {
        renameDialog.filePath = filePath;
        renameDialog.renameField.text = currentName || "";
        renameDialog.errorMessage = "";
        renameDialog.open();
        renameDialog.renameField.forceActiveFocus();
        renameDialog.renameField.selectAll();
    }

    function performRename() {
        var newPath = window.storageManager.renameRecording(renameDialog.filePath, renameDialog.renameField.text);
        if (!newPath) {
            renameDialog.errorMessage = qsTr("Rename failed. Try a different name using letters, numbers, - or _.");
            return;
        }
        var idx = selectedRecordings.indexOf(renameDialog.filePath);
        if (idx !== -1) {
            var temp = selectedRecordings.slice();
            temp[idx] = newPath;
            selectedRecordings = temp;
        }
        if (window.recordingLibrary)
            window.recordingLibrary.refresh();
        renameDialog.close();
    }

    function updateGlobalSearchResults(query) {
        globalSearchBox.searchResults.clear();

        if (query.length === 0) {
            globalSearchBox.dropdown.visible = false;
            return;
        }

        var lowerQuery = query.toLowerCase();

        // Helper for date matching
        var now = new Date();
        var today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
        var yesterday = new Date(today);
        yesterday.setDate(yesterday.getDate() - 1);

        function matchesDate(clipDate, query) {
            var d = new Date(clipDate);
            var dayNames = ["sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"];
            var monthNames = ["january", "february", "march", "april", "may", "june", "july", "august", "september", "october", "november", "december"];

            if (query === "today" && d >= today) return true;
            if (query === "yesterday" && d >= yesterday && d < today) return true;
            if (dayNames[d.getDay()] === query) return true;
            if (monthNames[d.getMonth()].includes(query)) return true;
            
            // Format: 2024-05-03 or 03/05/2024 etc.
            var dateStr = d.toLocaleDateString().toLowerCase();
            var timeStr = d.toLocaleTimeString().toLowerCase();
            var isoStr = d.toISOString().toLowerCase(); // e.g. 2026-05-03T14:10...

            if (dateStr.includes(query) || timeStr.includes(query) || isoStr.includes(query)) return true;

            return false;
        }

        // Search clips
        if (recordingLibrary) {
            for (var i = 0; i < recordingLibrary.count; i++) {
                var clip = recordingLibrary.get(i);
                if (!clip) continue;

                var gameName = (clip.gameName || "").toLowerCase();
                var fileName = (clip.fileName || "").toLowerCase();
                
                var match = gameName.includes(lowerQuery) ||
                            fileName.includes(lowerQuery) ||
                            matchesDate(clip.date, lowerQuery);

                if (match) {
                    globalSearchBox.searchResults.append({
                        "type": "clip",
                        "title": clip.gameName || "Unknown Game",
                        "subtitle": formatSize(clip.size) + " • " + clip.date.toLocaleString(Qt.locale(), "MMM d, yyyy HH:mm"),
                        "targetIndex": i
                    });
                }
            }
        }

        // Search settings (names must match SettingsPage sections)
        var settings = [
            { name: "Customization", section: "Customization", keywords: ["theme", "appearance", "customization", "color", "accent", "default", "dark", "charcoal", "amoled", "black", "rose", "blue", "purple", "teal", "orange", "cyan", "pink"] },
            { name: "Video Recording", section: "Video Recording", keywords: ["video", "encoder", "auto", "av1", "nvenc", "vaapi", "h264", "hevc", "x264", "quality", "bitrate", "fps", "frame", "resolution", "720p", "1080p", "1440p", "4k", "preset", "container", "mkv", "mp4", "zero-copy", "dma-buf"] },
            { name: "Background Clips", section: "Background Clips", keywords: ["background clips", "clip buffer", "clip cache", "instant clips", "replay", "buffer", "cache folder"] },
            { name: "Audio Recording", section: "Audio Recording", keywords: ["audio", "sound", "microphone", "mic", "voice", "chat", "tracks", "single track", "separate tracks", "desktop mode", "game mode", "system audio", "device", "input", "output"] },
            { name: "Storage Management", section: "Storage Management", keywords: ["storage", "path", "folder", "recordings folder", "limit", "cleanup", "auto-cleanup", "size", "delete", "usage", "library", "unlimited"] },
            { name: "Notifications", section: "Notifications", keywords: ["notification", "notices", "alerts", "popup", "clips", "notify", "recording notices", "clip notices", "storage notices", "sound", "chime", "ping", "test", "update", "updates", "automatic checks"] },
            { name: "Game Detection", section: "Game Detection", keywords: ["detection", "detect", "auto-detect", "auto-record", "auto", "record", "launch", "game"] },
            { name: "Detection Helpers", section: "Detection Helpers", keywords: ["helpers", "integration", "status", "enrolled", "game window helper", "window tracking", "vulkan", "opengl", "shim", "repair"] },
            { name: "System Tray", section: "System Tray", keywords: ["tray", "system tray", "minimize", "close", "start minimized", "login", "autostart", "background"] },
            { name: "Keyboard Shortcuts", section: "Keyboard Shortcuts", keywords: ["shortcut", "shortcuts", "global", "hotkey", "keys", "keybind", "record key", "clip key", "save clip", "configure", "bind", "remap"] }
        ];

        for (var j = 0; j < settings.length; j++) {
            var setting = settings[j];
            var settingMatch = (setting.name || "").toLowerCase().includes(lowerQuery) ||
                               (setting.keywords || []).some(k => { const kw = (k || "").toLowerCase(); return kw.includes(lowerQuery) || lowerQuery.includes(kw); });

            if (settingMatch) {
                globalSearchBox.searchResults.append({
                    "type": "setting",
                    "title": setting.name,
                    "subtitle": "Settings",
                    "section": setting.section,
                    "targetIndex": j
                });
            }
        }

        globalSearchBox.dropdown.visible = globalSearchBox.searchResults.count > 0;
    }

    function handleSearchResult() {
        if (globalSearchBox.searchResults.count > 0) {
            var firstResult = globalSearchBox.searchResults.get(0);
            navigateToResult(firstResult);
        }
        globalSearchBox.text = "";
    }

    function navigateToResult(result) {
        if (!result) return;
        if (result.type === "clip") {
            window.currentPage = 0; // Dashboard
            dashboardPage.scrollToRecording(result.targetIndex);
        } else if (result.type === "setting") {
            window.currentPage = 2; // Settings
            settingsPage.scrollToSection(result.section);
        }
        globalSearchBox.dropdown.visible = false;
        globalSearchBox.text = "";
    }

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

    readonly property int defaultWindowWidth: 1200
    readonly property int defaultWindowHeight: 800
    readonly property int minimumWindowWidth: 900
    readonly property int minimumWindowHeight: 650

    flags: Qt.FramelessWindowHint | Qt.Window | Qt.WindowMinMaxButtonsHint
    width: defaultWindowWidth
    height: defaultWindowHeight
    minimumWidth: minimumWindowWidth
    minimumHeight: minimumWindowHeight
    visible: true
    title: qsTr("Trophy - Game recording and clipping for Linux")
    palette.window: "transparent"

    onClosing: function(close) {
        // Close-to-tray keeps Trophy (and any recording) running in the
        // background; otherwise confirm when recording, else just quit.
        if (window.app && window.app.minimizeToTray()) {
            close.accepted = false;
        } else if (window.recordingManager && window.recordingManager.isRecording) {
            closeConfirmDialog.open();
            close.accepted = false;
        }
    }

    // Called from C++ (tray "Quit"): confirm when recording, else quit.
    function requestQuit() {
        if (window.recordingManager && window.recordingManager.isRecording) {
            closeConfirmDialog.open();
        } else {
            Qt.quit();
        }
    }

    Rectangle {
        id: windowBody
        anchors.fill: parent
        // Square and borderless in player fullscreen so
        // the video runs edge-to-edge.
        radius: Theme.windowRadius
        color: Theme.surfaceColor
        border.width: playerPage.playerFullscreen ? 0 : 1
        border.color: Theme.borderColor

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: playerPage.playerFullscreen ? 0 : 1
            spacing: 0

            Rectangle {
                id: titleBar
                Layout.fillWidth: true
                Layout.preferredHeight: 60
                // Hidden in player fullscreen (the video
                // fills the window).
                visible: !playerPage.playerFullscreen
                color: Theme.surfaceColor
                radius: Theme.windowRadius

                MouseArea {
                    anchors.fill: parent
                    onPressed: window.startSystemMove()
                }

                RowLayout {
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spacingMedium

                    ColumnLayout {
                        spacing: 2
                        Layout.alignment: Qt.AlignVCenter
                        Layout.leftMargin: 2
                        Layout.topMargin: 4

                        Image {
                            source: "qrc:/icons/trophy.svg"
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 24
                            sourceSize: Qt.size(24, 24)
                            Layout.alignment: Qt.AlignHCenter
                        }

                        Label {
                            text: "Trophy"
                            font.pixelSize: 10
                            font.weight: Font.Bold
                            color: Theme.textColor
                            Layout.alignment: Qt.AlignHCenter
                        }
                    }
                }

                TrophySearchBox {
                    id: globalSearchBox
                    width: 350
                    anchors.centerIn: parent
                    placeholderText: qsTr("Search clips, settings...")
                    onSearchTextChanged: function(query) { updateGlobalSearchResults(query); }
                    onSearchAccepted: handleSearchResult()
                    onResultSelected: function(result) { navigateToResult(result); }
                }

                // Keep the recording pill in the space below the window controls.
                RowLayout {
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingSmall
                    anchors.top: windowButtons.bottom
                    anchors.bottom: parent.bottom
                    spacing: Theme.spacingMedium

                    Rectangle {
                        id: recordingPill
                        Layout.preferredHeight: 22
                        Layout.preferredWidth: pillRow.implicitWidth + 20
                        radius: 11
                        color: Theme.recordingColor
                        visible: window.recordingManager && window.recordingManager.isRecording
                        border.color: Theme.errorColor
                        border.width: 1

                        RowLayout {
                            id: pillRow
                            anchors.centerIn: parent
                            spacing: 10

                            Rectangle {
                                Layout.preferredWidth: 8
                                Layout.preferredHeight: 8
                                radius: 4
                                color: Theme.textColor

                                SequentialAnimation on opacity {
                                    loops: Animation.Infinite
                                    NumberAnimation { from: 1; to: 0.3; duration: Theme.animationPulse }
                                    NumberAnimation { from: 0.3; to: 1; duration: Theme.animationPulse }
                                }
                            }

                            Label {
                                text: window.recordingManager ? window.recordingManager.currentGameName : ""
                                color: "white"
                                font.pixelSize: 11
                                font.weight: Font.Bold
                                visible: text !== ""
                            }

                            Rectangle {
                                Layout.preferredWidth: 1
                                Layout.preferredHeight: 12
                                color: "white"
                                opacity: 0.3
                                visible: window.recordingManager && window.recordingManager.currentGameName !== ""
                            }

                            Label {
                                text: {
                                    var durMs = window.recordingManager ? window.recordingManager.recordingDuration : 0;
                                    var dur = Math.floor(durMs / 1000);
                                    var m = Math.floor(dur / 60);
                                    var s = dur % 60;
                                    return `%1:%2`.arg(m).arg(s.toString().padStart(2, '0'));
                                }
                                color: "white"
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }

                            Button {
                                id: stopButton
                                Layout.preferredWidth: 18
                                Layout.preferredHeight: 18
                                flat: true
                                icon.source: "qrc:/icons/stop.svg"
                                icon.width: 12
                                icon.height: 12
                                icon.color: "white"
                                onClicked: window.recordingManager.stopRecording()
                            }
                        }
                    }
                }

                Row {
                    id: windowButtons
                    anchors.right: parent.right
                    anchors.top: parent.top

                    TrophyWindowButton {
                        iconSource: "qrc:/icons/window-bug.svg"
                        hoverColor: Theme.withOpacity(Theme.textColor, 0.1)
                        ToolTip.text: qsTr("Report a bug")
                        ToolTip.visible: hovered
                        onClicked: window.app.reportBug()
                    }

                    TrophyWindowButton {
                        iconSource: "qrc:/icons/window-minimize.svg"
                        hoverColor: Theme.withOpacity(Theme.textColor, 0.1)
                        onClicked: window.showMinimized()
                    }

                    TrophyWindowButton {
                        iconSource: window.visibility === Window.Maximized ? "qrc:/icons/window-restore.svg" : "qrc:/icons/window-maximize.svg"
                        hoverColor: Theme.withOpacity(Theme.textColor, 0.1)
                        onClicked: {
                            if (window.visibility === Window.Maximized)
                                window.showNormal();
                            else
                                window.showMaximized();
                        }
                    }

                    TrophyWindowButton {
                        iconSource: "qrc:/icons/window-close.svg"
                        hoverColor: Theme.errorColor
                        hoverIconColor: "white"
                        isCloseButton: true
                        onClicked: window.close()
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                NavigationRail {
                    id: navRail
                    Layout.fillHeight: true
                    // Hidden in player fullscreen (the video
                    // fills the window).
                    visible: !playerPage.playerFullscreen
                    currentIndex: window.currentPage
                    onNavigate: function(index) {
                        // The player is an overlay on the
                        // content area, not a page: navigating
                        // anywhere closes it (and stops
                        // playback) instead of leaving it
                        // stacked on top of the destination.
                        if (playerPage.visible)
                            playerPage.close()
                        window.currentPage = index
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    StackLayout {
                        id: pageStack
                        anchors.fill: parent
                        currentIndex: window.currentPage

                        DashboardPage { id: dashboardPage }
                        GameListPage { id: gameListPage }
                        SettingsPage { id: settingsPage }
                        InfoPage {
                            id: infoPage
                            onReportBugRequested: window.app.reportBug()
                        }
                    }

                    // In-app player: overlays the content area only,
                    // so the title bar and nav rail stay interactive.
                    PlayerPage {
                        id: playerPage
                        anchors.fill: parent
                        visible: false
                        // Fullscreen player: the page owns the
                        // flag; the window answers it (a child
                        // file can't reach the window id). The
                        // pre-fullscreen maximization is restored
                        // on exit, like any standalone player.
                        onPlayerFullscreenChanged: {
                            if (playerFullscreen) {
                                wasMaximizedBeforeFullscreen =
                                    (window.visibility === Window.Maximized)
                                window.showFullScreen()
                            } else {
                                window.showNormal()
                                if (wasMaximizedBeforeFullscreen)
                                    window.showMaximized()
                            }
                        }
                        onFullscreenRequested: playerFullscreen = !playerFullscreen
                    }

                    // Pure black removes the usual page/shell contrast. Draw
                    // top/left divider above its contents. Clip the other edges
                    // outside the page so they don't double the window border.
                    // No input handlers or chrome in fullscreen playback.
                    Item {
                        anchors.fill: parent
                        z: 1
                        visible: Theme.amoled && !playerPage.playerFullscreen
                        clip: true

                        Rectangle {
                            width: parent.width + border.width
                            height: parent.height + border.width
                            color: "transparent"
                            topLeftRadius: 26
                            border.width: 1
                            border.color: Theme.borderColor
                        }
                    }
                }
            }
        }
    }

    // Non-overlapping edge strips and larger corner targets sit above the
    // content so title-bar dragging cannot swallow a resize press.
    Repeater {
        model: [Qt.LeftEdge, Qt.RightEdge, Qt.TopEdge, Qt.BottomEdge,
                Qt.TopEdge | Qt.LeftEdge, Qt.TopEdge | Qt.RightEdge,
                Qt.BottomEdge | Qt.LeftEdge, Qt.BottomEdge | Qt.RightEdge]

        delegate: MouseArea {
            required property int modelData
            readonly property bool leftEdge: (modelData & Qt.LeftEdge) !== 0
            readonly property bool rightEdge: (modelData & Qt.RightEdge) !== 0
            readonly property bool topEdge: (modelData & Qt.TopEdge) !== 0
            readonly property bool bottomEdge: (modelData & Qt.BottomEdge) !== 0
            readonly property bool corner: (leftEdge || rightEdge) && (topEdge || bottomEdge)
            readonly property int cornerSize: 15
            readonly property int edgeSize: 6

            x: leftEdge ? 0 : rightEdge ? parent.width - width : cornerSize
            y: topEdge ? 0 : bottomEdge ? parent.height - height : cornerSize
            width: corner ? cornerSize : (leftEdge || rightEdge) ? edgeSize : parent.width - 2 * cornerSize
            height: corner ? cornerSize : (topEdge || bottomEdge) ? edgeSize : parent.height - 2 * cornerSize
            cursorShape: corner ? (leftEdge === topEdge ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor)
                                : (leftEdge || rightEdge) ? Qt.SizeHorCursor : Qt.SizeVerCursor
            visible: window.visibility === Window.Windowed && !playerPage.playerFullscreen
            acceptedButtons: Qt.LeftButton
            onPressed: window.startSystemResize(modelData)
        }
    }

    Shortcut {
        sequence: "Ctrl+Q"
        onActivated: window.close()
    }

    TrophyDialog {
        id: closeConfirmDialog
        title: qsTr("Recording in Progress")
        message: qsTr("A recording is currently in progress. Would you like to stop recording and exit Trophy?")
        confirmText: qsTr("Stop and Exit")
        confirmColor: Theme.errorColor
        onAccepted: {
            window.recordingManager.stopRecording();
            Qt.quit();
        }
    }

    Connections {
        target: window.recordingManager
        function onRecordingError(error) {
            errorDialog.title = qsTr("Recording Error");
            errorDialog.message = error;
            errorDialog.open();
            window.requestAttention(qsTr("Recording error: %1\nOpen Trophy when you're ready.").arg(error));
        }
    }

    Connections {
        target: deletionQueue
        function onFailed(message) {
            errorDialog.title = qsTr("Deletion unavailable");
            errorDialog.message = message;
            errorDialog.open();
            window.requestAttention(qsTr("Capture deletion needs attention. Open Trophy to review."));
        }
    }

    ShareExportDialog {
        id: shareDialog
        exporter: shareExporter
        destination: window.settingsManager.shareExportFolder
        onDestinationChanged: window.settingsManager.shareExportFolder = destination
        includeWatermark: window.settingsManager.shareExportWatermark
        onIncludeWatermarkChanged: window.settingsManager.shareExportWatermark = includeWatermark
    }

    ToastStack {
        exporter: shareExporter
        queue: deletionQueue
        parent: Overlay.overlay
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.spacingLarge
        width: Math.min(400, window.width - 40)
        maxToastHeight: window.height - 80
        z: 100
    }

    TrophyDialog {
        id: errorDialog
        title: qsTr("Recording Error")
        confirmText: qsTr("OK")
        cancelText: ""
        onClosed: {
            if (!trackingRepairDialog.visible)
                notificationService.clearAttention();
        }
    }

    WindowPickerDialog {
        id: windowPicker
        onWindowSelected: function(windowId, title) {
            window.recordingManager.startRecording(title, windowId);
        }
    }

    TrophyDialog {
        id: deleteDialog
        property string filePath: ""
        property string fileName: ""
        title: qsTr("Delete Recording")
        message: qsTr("Delete '%1'? You will have 10 seconds to Undo in the app before permanent deletion.").arg(fileName)
        confirmText: qsTr("Delete")
        confirmColor: Theme.errorColor
        showDontAsk: true
        onAccepted: {
            if (window.storageManager) {
                window.queueDeletion([filePath]);
            }
            if (deleteDialog.dontAskChecked)
                window.skipDeleteConfirmation = true;
        }
    }

    TrophyDialog {
        id: bulkDeleteDialog
        title: qsTr("Delete Selected Recordings")
        message: qsTr("Delete %1 captures? You will have 10 seconds to Undo in the app before permanent deletion.").arg(window.selectedRecordings.length)
        confirmText: qsTr("Delete All")
        confirmColor: Theme.errorColor
        showDontAsk: true
        onAccepted: {
            performDeleteSelected();
            if (bulkDeleteDialog.dontAskChecked)
                window.skipDeleteConfirmation = true;
        }
    }

    Dialog {
        id: renameDialog
        property string filePath: ""
        property string errorMessage: ""
        property alias renameField: renameField
        title: qsTr("Rename Recording")
        modal: true
        anchors.centerIn: parent
        width: 400
        standardButtons: Dialog.NoButton
        // The title renders in the content like TrophyDialog; suppress the
        // default header so it is not shown twice.
        header: Item { height: 0 }

        // Dimmed backdrop overlay (matches TrophyDialog)
        Overlay.modal: Rectangle {
            color: Theme.withOpacity("#000000", 0.5)
            Behavior on opacity {
                NumberAnimation { duration: Theme.animationFast }
            }
        }

        background: Rectangle {
            color: Theme.cardColor || "#212121"
            radius: Theme.radiusLarge
            border.color: Theme.withOpacity(Theme.textColor, 0.1)
            border.width: 1
        }

        // Open/close animation (matches TrophyDialog)
        enter: Transition {
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: Theme.animationNormal
                easing.type: Easing.OutCubic
            }
            NumberAnimation {
                property: "scale"
                from: 0.92
                to: 1
                duration: Theme.animationNormal
                easing.type: Easing.OutCubic
            }
        }

        exit: Transition {
            NumberAnimation {
                property: "opacity"
                from: 1
                to: 0
                duration: Theme.animationFast
                easing.type: Easing.InCubic
            }
            NumberAnimation {
                property: "scale"
                from: 1
                to: 0.95
                duration: Theme.animationFast
                easing.type: Easing.InCubic
            }
        }

        contentItem: ColumnLayout {
            spacing: 0

            Label {
                text: renameDialog.title
                font.pixelSize: Theme.fontSizeLarge
                font.weight: Font.Bold
                color: Theme.textColor
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                Layout.bottomMargin: 0
            }

            TrophyTextField {
                id: renameField
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                Layout.topMargin: Theme.spacingMedium
                Layout.bottomMargin: 0
                placeholderText: qsTr("Recording name")
                maximumLength: 80
                onAccepted: {
                    if (text.trim().length > 0)
                        window.performRename();
                }
                onTextChanged: {
                    if (renameDialog.errorMessage !== "")
                        renameDialog.errorMessage = "";
                }
            }

            Label {
                text: qsTr("The recording date is kept automatically.")
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textMutedColor
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                Layout.topMargin: Theme.spacingMedium
                Layout.bottomMargin: 0
            }

            Label {
                text: renameDialog.errorMessage
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.errorColor
                wrapMode: Text.WordWrap
                visible: renameDialog.errorMessage !== ""
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                Layout.topMargin: visible ? Theme.spacingSmall : 0
                Layout.bottomMargin: 0
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                Layout.topMargin: Theme.spacingMedium
                spacing: 0
                Item { Layout.fillWidth: true }
                RowLayout {
                    spacing: Theme.spacingMedium
                    TrophyButton {
                        text: qsTr("Cancel")
                        onClicked: renameDialog.reject()
                    }
                    TrophyButton {
                        text: qsTr("Rename")
                        baseColor: Theme.accentColor || "#3498db"
                        enabled: renameField.text.trim().length > 0
                        onClicked: window.performRename()
                    }
                }
            }
        }
    }

    OriginalCopyDialog {
        id: exportDialog
        exporter: shareExporter
        sourcePaths: window.selectedRecordings
        onQueued: window.clearSelection()
        onCopyFailed: function(message) {
            errorDialog.title = qsTr("Unable to copy captures");
            errorDialog.message = message;
            errorDialog.open();
        }
    }

    // One warning per failure, re-armed once tracking works again. First-run
    // setup handles its own check; allow initialization/repair time to finish.
    QtObject {
        id: trackingStartupCheck
        property bool prompted: false
        function check() {
            if (!windowMonitor)
                return;
            if (windowMonitor.trackingReady) {
                prompted = false;
                return;
            }
            if (prompted || windowMonitor.trackingChecking || !window.settingsManager ||
                !window.settingsManager.setupCompleted)
                return;
            prompted = true;
            trackingRepairDialog.open();
            window.requestAttention(qsTr("The Game Window Helper needs repair. Open Trophy when you're ready."));
        }
        Component.onCompleted: Qt.callLater(check)
    }

    Connections {
        target: windowMonitor
        function onWindowsChanged() {
            trackingStartupCheck.check();
            if (windowMonitor.trackingReady)
                trackingRepairDialog.close();
        }
    }

    TrophyDialog {
        id: trackingRepairDialog
        title: qsTr("Game Window Helper Needs Attention")
        message: qsTr("The Game Window Helper is not responding. Trophy needs window identity, focus and fullscreen information to identify games and restore capture to the correct window. Repair will reinstall or restart the compositor integration. Your recordings are still available.")
                 + (windowMonitor && windowMonitor.compositorName === "gnome" ? "\n\n" + windowMonitor.gnomeHelperStatus : "")
        confirmText: qsTr("Repair")
        cancelText: qsTr("Later")
        onAccepted: if (windowMonitor) windowMonitor.repairTracking()
        onClosed: {
            if (!errorDialog.visible)
                notificationService.clearAttention();
        }
    }

    SetupWizard {
        id: setupWizard
        visible: window.settingsManager ? !window.settingsManager.setupCompleted : false
    }
}
