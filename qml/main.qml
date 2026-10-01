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

    // --- QML-DEFINED PROPERTIES ---
    property int currentPage: 0
    property bool skipDeleteConfirmation: false
    property var selectedRecordings: []
    property int lastSelectedIndex: -1

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

    function exportSelected() {
        if (selectedRecordings.length > 0) {
            exportDialog.open();
        }
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
        if (window.storageManager && window.recordingLibrary) {
            for (var i = 0; i < selectedRecordings.length; i++) {
                window.storageManager.deleteRecording(selectedRecordings[i]);
            }
            window.recordingLibrary.refresh();
        }
        clearSelection();
    }

    function openWindowPicker() {
        console.log("openWindowPicker called. app.isWayland:", window.app.isWayland);
        if (window.app.isWayland) {
            // On Wayland, the system portal handles window/screen selection.
            window.recordingManager.startRecording();
        } else {
            console.log("Opening windowPicker dialog.");
            windowPicker.open();
        }
    }

    function openFolder(path) {
        if (!path) return;

        var url = path;
        if (!url.startsWith("file://")) {
            if (!url.startsWith("/"))
                url = "/" + url;
            url = `file://${url}`;
        }
        Qt.openUrlExternally(url);
    }

    function confirmDelete(filePath, fileName) {
        if (window.skipDeleteConfirmation) {
            if (window.storageManager) {
                window.storageManager.deleteRecording(filePath);
                if (window.recordingLibrary)
                    window.recordingLibrary.refresh();
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
                        "index": i
                    });
                }
            }
        }

        // Search settings
        var settings = [
            { name: "Video Recording", section: "Video Recording", keywords: ["encoder", "av1", "nvenc", "vaapi", "h264", "hevc", "quality", "bitrate", "fps", "resolution", "mkv", "mp4"] },
            { name: "Audio Recording", section: "Audio Recording", keywords: ["audio", "sound", "microphone", "mic", "voice", "tracks", "desktop", "game"] },
            { name: "Storage Management", section: "Storage Management", keywords: ["storage", "path", "folder", "limit", "cleanup", "size", "delete"] },
            { name: "Game Detection", section: "Game Detection", keywords: ["detection", "auto", "record", "launch", "game"] }
        ];

        for (var j = 0; j < settings.length; j++) {
            var setting = settings[j];
            var settingMatch = (setting.name || "").toLowerCase().includes(lowerQuery) ||
                               (setting.keywords || []).some(k => (k || "").toLowerCase().includes(lowerQuery));

            if (settingMatch) {
                globalSearchBox.searchResults.append({
                    "type": "setting",
                    "title": setting.name,
                    "subtitle": "Settings",
                    "section": setting.section,
                    "index": j
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
            dashboardPage.scrollToRecording(result.index);
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
        radius: 12
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.borderColor

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0

            Rectangle {
                id: titleBar
                Layout.fillWidth: true
                Layout.preferredHeight: 60
                color: Theme.surfaceColor
                radius: 12

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

                // Right section: Recording Pill and Window Controls
                RowLayout {
                    anchors.right: parent.right
                    anchors.rightMargin: windowButtons.width + Theme.spacingMedium
                    anchors.verticalCenter: parent.verticalCenter
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
                        symbol: "—"
                        hoverColor: Theme.withOpacity(Theme.textColor, 0.1)
                        onClicked: window.showMinimized()
                    }

                    TrophyWindowButton {
                        symbol: window.visibility === Window.Maximized ? "❐" : "▢"
                        hoverColor: Theme.withOpacity(Theme.textColor, 0.1)
                        onClicked: {
                            if (window.visibility === Window.Maximized)
                                window.showNormal();
                            else
                                window.showMaximized();
                        }
                    }

                    TrophyWindowButton {
                        symbol: "✕"
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
                    currentIndex: window.currentPage
                    onNavigate: function(index) { window.currentPage = index; }
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
                        InfoPage { id: infoPage }
                    }
                }
            }
        }
    }

    MouseArea {
        width: 15
        height: 15
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeFDiagCursor
        onPressed: window.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
    }

    Shortcut {
        sequence: "Ctrl+Q"
        onActivated: Qt.quit()
    }

    Shortcut {
        sequence: "F9"
        onActivated: {
            if (window.recordingManager) {
                if (window.recordingManager.isRecording)
                    window.recordingManager.stopRecording();
                else if (window.gameDetector && window.gameDetector.currentGameName !== "")
                    window.recordingManager.startRecording(window.gameDetector.currentGameName);
            }
        }
    }

    Shortcut {
        sequence: "F10"
        onActivated: {
            if (window.app)
                window.app.requestReplay();
        }
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
            errorDialog.message = error;
            errorDialog.open();
        }
    }

    TrophyDialog {
        id: errorDialog
        title: qsTr("Recording Error")
        confirmText: qsTr("OK")
        cancelText: ""
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
        message: qsTr("Are you sure you want to delete '%1'? This cannot be undone.").arg(fileName)
        confirmText: qsTr("Delete")
        confirmColor: Theme.errorColor
        showDontAsk: true
        onAccepted: {
            if (window.storageManager) {
                window.storageManager.deleteRecording(filePath);
                if (window.recordingLibrary)
                    window.recordingLibrary.refresh();
            }
            if (deleteDialog.dontAskChecked)
                window.skipDeleteConfirmation = true;
        }
    }

    TrophyDialog {
        id: bulkDeleteDialog
        title: qsTr("Delete Selected Recordings")
        message: qsTr("Are you sure you want to delete %1 recordings? This cannot be undone.").arg(window.selectedRecordings.length)
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

        background: Rectangle {
            color: Theme.cardColor || "#212121"
            radius: Theme.radiusLarge
            border.color: Theme.withOpacity(Theme.textColor, 0.1)
            border.width: 1
        }

        contentItem: ColumnLayout {
            spacing: Theme.spacingMedium

            Label {
                text: renameDialog.title
                font.pixelSize: Theme.fontSizeLarge
                font.weight: Font.Bold
                color: Theme.textColor
                Layout.fillWidth: true
            }

            TrophyTextField {
                id: renameField
                Layout.fillWidth: true
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
            }

            Label {
                text: renameDialog.errorMessage
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.errorColor
                wrapMode: Text.WordWrap
                visible: renameDialog.errorMessage !== ""
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
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

    FolderDialog {
        id: exportDialog
        title: qsTr("Choose Export Directory")
        onAccepted: {
            var destDir = exportDialog.selectedFolder.toString();
            if (destDir.startsWith("file://")) {
                destDir = destDir.substring(7); // remove file://
            }
            if (window.storageManager) {
                for (var i = 0; i < window.selectedRecordings.length; i++) {
                    window.storageManager.exportRecording(window.selectedRecordings[i], destDir);
                }
            }
            window.clearSelection();
            window.openFolder(destDir);
        }
    }

    SetupWizard {
        id: setupWizard
        visible: window.settingsManager ? !window.settingsManager.setupCompleted : false
    }
}