/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../components"
import "../styles"
import "../views"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root

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

        var m = Math.floor(seconds / 60);
        var s = seconds % 60;
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
        // Index is into the library; offset past the live card when recording.
        var offset = (typeof recordingManager !== "undefined" && recordingManager && recordingManager.isRecording) ? 1 : 0;
        recordingsGrid.positionViewAtIndex(index + offset, GridView.Beginning);
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
                                text: qsTr("Recent Captures")
                                font.pixelSize: Theme.fontSizeMedium
                                font.weight: Font.Medium
                                color: Theme.textColor
                            }

                            Item { Layout.fillWidth: true }

                            RowLayout {
                                spacing: Theme.spacingSmall
                                visible: window.selectedRecordings && window.selectedRecordings.length > 0
                                // True while the background bulk delete runs;
                                // the selection clears on bulkDeleteFinished.
                                property bool bulkDeleting: window.storageManager && window.storageManager.bulkDeleteInProgress

                                TrophyButton {
                                    text: qsTr("Export Selected (%1)").arg(window.selectedRecordings.length)
                                    icon.source: "qrc:/icons/folder.svg"
                                    implicitHeight: 28
                                    enabled: !parent.bulkDeleting
                                    onClicked: window.exportSelected()
                                }

                                TrophyButton {
                                    text: parent.bulkDeleting ? qsTr("Deleting…") : qsTr("Delete Selected")
                                    icon.source: "qrc:/icons/trash.svg"
                                    implicitHeight: 28
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
                                    text: qsTr("Cancel")
                                    implicitHeight: 28
                                    onClicked: window.clearSelection()
                                }
                            }
                        }

                        RecordingGridView {
                            id: recordingsGrid
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            
                            skip: 0
                            leftMargin: Theme.spacingSmall
                            topMargin: Theme.spacingSmall
                            bottomMargin: Theme.spacingSmall
                            
                            // Add margin on the right so your grid content doesn't get covered by the scrollbar
                            rightMargin: Theme.spacingSmall

                            // Attach the custom scrollbar directly to the GridView
                            ScrollBar.vertical: ScrollBar {
                                id: customVBar
                                active: hovered || pressed
                                width: 8
                                
                                // The handle
                                contentItem: Rectangle {
                                    implicitWidth: 4
                                    implicitHeight: 40 // Minimum height of the thumb
                                    radius: 16
                                    color: customVBar.pressed ? Theme.highlightedColor : (customVBar.hovered ? Theme.highlightedColor : Theme.secondaryColor)

                                    // Optional: fade out when not interacting
                                    opacity: customVBar.active ? 1.0 : 0.2
                                    Behavior on opacity { NumberAnimation { duration: 200 } }
                                
                                }

                                background: Rectangle {
                                    color: "transparent"
                                }
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
                        Layout.fillWidth: true
                        spacing: 4

                        RowLayout {
                            spacing: 8
                            Rectangle {
                                width: 10; height: 10; radius: 5
                                color: (recordingManager && recordingManager.isRecording) ? "white" : ((gameDetector && gameDetector.currentGameName) ? Theme.successColor : Theme.textMutedColor)
                                SequentialAnimation on opacity {
                                    running: (recordingManager && recordingManager.isRecording)
                                    loops: Animation.Infinite
                                    NumberAnimation { from: 1; to: 0.4; duration: 800 }
                                    NumberAnimation { from: 0.4; to: 1; duration: 800 }
                                }
                            }
                            Label {
                                text: (recordingManager && recordingManager.isRecording) ? qsTr("RECORDING") : ((gameDetector && gameDetector.currentGameName) ? qsTr("READY") : qsTr("IDLE"))
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

                    Button {
                        id: recordBtn
                        Layout.preferredWidth: 44; Layout.preferredHeight: 44
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

            // Storage Usage
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 100
                radius: Theme.radiusMedium
                color: Theme.cardColor
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: Theme.spacingMedium; spacing: Theme.spacingSmall
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Storage Usage"); font.pixelSize: Theme.fontSizeSmall; color: Theme.textSecondaryColor }
                        Item { Layout.fillWidth: true }
                        Label { text: qsTr("%n recording(s)", "", (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.count : 0); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
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
                        rowSpacing: Theme.spacingSmall

                        // Encoder
                        ColumnLayout {
                            spacing: 2
                            Label { text: qsTr("Encoder"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                            Label {
                                text: {
                                    if (settingsManager && settingsManager.encoder === "auto") {
                                         var active = (recordingManager && recordingManager.isRecording && recordingManager.activeEncoderName) ? recordingManager.activeEncoderName : (recordingManager ? recordingManager.detectedEncoderName : "");
                                         return active || (settingsManager ? settingsManager.encoderDisplayName : "");
                                     }
                                    return settingsManager ? settingsManager.encoderDisplayName : "";
                                }
                                font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor; elide: Text.ElideRight
                            }
                        }

                        // Resolution
                        ColumnLayout {
                            spacing: 2
                            Label { text: qsTr("Resolution"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                            Label {
                                text: settingsManager.resolution === "auto" ? qsTr("Auto (%1x%2)").arg(settingsManager.autoResolutionWidth).arg(settingsManager.autoResolutionHeight) : settingsManager.resolution
                                font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor
                            }
                        }

                        // Audio levels (spans both rows; one icon+bar row
                        // per track)
                        ColumnLayout {
                            Layout.rowSpan: 2
                            Layout.fillHeight: true
                            spacing: 2
                            Label { text: qsTr("Audio"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                            Repeater {
                                id: dashMeters
                                model: 3

                                AudioLevelMeter {
                                    horizontal: true
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 24
                                    label: audioMixer ? audioMixer.trackName(index) : ""
                                    onMutedChanged: if (app) app.setTrackMuted(index, muted)
                                }
                            }
                        }

                        // Quality
                        ColumnLayout {
                            spacing: 2
                            Label { text: qsTr("Quality"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                            Label { text: (settingsManager ? settingsManager.videoBitrate : 0) + " kbps"; font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor }
                        }

                        // FPS
                        ColumnLayout {
                            spacing: 2
                            Label { text: qsTr("FPS"); font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor }
                            Label { text: settingsManager ? settingsManager.fps.toString() : "0"; font.pixelSize: Theme.fontSizeSmall; color: Theme.textColor }
                        }
                    }

                    Timer {
                        interval: 100
                        running: root.visible
                        repeat: true
                        onTriggered: {
                            if (!app) return;
                            const live = (recordingManager && recordingManager.isRecording) || (replayManager && replayManager.active);
                            for (let i = 0; i < 3; i++) {
                                const m = dashMeters.itemAt(i);
                                if (m) {
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
                        clip: true
                        ColumnLayout {
                            width: activityScroll.availableWidth; spacing: 0
                            Repeater {
                                model: ListModel {
                                    id: activityModel
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
                                        if (recordingLibrary && recordingLibrary.count > 0) {
                                            for (var i = 0; i < recordingLibrary.count; i++) {
                                                var clip = recordingLibrary.get(i)
                                                if (!clip) continue
                                                // Clips already show as "Clip saved" rows
                                                // above — listing them again as Completed
                                                // doubles every entry. The container tag
                                                // wins and survives renames; the suffix
                                                // covers older files.
                                                if (clip.isClip || (clip.fileName || "").endsWith("_Replay.mkv")) continue
                                                rows.push({ at: new Date(clip.date), kind: "completed", dot: "green", title: qsTr("Completed: %1").arg(clip.gameName || "Unknown Game"), detail: root.recordingDetail(clip), time: getRelativeTime(clip.date), stamp: clip.date.toLocaleString(Qt.locale(), "MMM d, yyyy HH:mm") })
                                            }
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
        function onCountChanged() { activityModel.updateActivity() }
        function onRecordingRemoved(filePath) {
            root.pushActivityEvent("deleted", "orange", qsTr("Deleted: %1").arg(root.activityGameName(filePath)), qsTr("Removed from library"))
        }
        function onRecordingsRemoved(filePaths) {
            for (var i = 0; i < filePaths.length; i++) {
                root.pushActivityEvent("deleted", "orange", qsTr("Deleted: %1").arg(root.activityGameName(filePaths[i])), qsTr("Removed from library"))
            }
        }
    }
    Connections { target: recordingManager; function onIsRecordingChanged() { activityModel.updateActivity() } }
    Connections {
        target: activityFeed
        function onEventsChanged() { activityModel.updateActivity() }
    }
    Connections {
        target: replayManager
        function onReplaySaved(filePath, seconds) {
            var secs = Math.max(1, Math.round(seconds))
            var span = secs === 1 ? qsTr("Last 1 second") : qsTr("Last %1 seconds").arg(secs)
            var game = (replayManager && replayManager.currentGame) || qsTr("Unknown Game")
            root.pushActivityEvent("clip", "blue", qsTr("Clip saved: %1").arg(game), span, filePath)
        }
    }
    Connections {
        target: storageManager
        function onStorageLimitReached() {
            var auto = settingsManager ? settingsManager.autoDeleteOldest : false
            root.pushActivityEvent("storage", "red", qsTr("Storage Full"), auto ? qsTr("Over limit — oldest files will be auto-deleted") : qsTr("Over limit — enable auto-cleanup or free space manually"))
        }
        function onStorageLimitCannotCleanup() {
            root.pushActivityEvent("storage", "red", qsTr("Recording Stopped"), qsTr("Recording exceeded the storage limit"))
        }
        function onRecordingRenamed(oldPath, newPath) {
            root.pushActivityEvent("renamed", "blue", qsTr("Renamed: %1").arg(root.activityGameName(newPath)), qsTr("%1 → %2").arg(root.activityBaseName(oldPath)).arg(root.activityBaseName(newPath)), newPath)
        }
    }

    background: Rectangle { color: Theme.backgroundColor; bottomRightRadius: 12; topLeftRadius: 26 }
}
