/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

ItemDelegate {
    id: root

    property var recording: null
    property bool live: false
    property int cardIndex: -1
    property HoverPreview previewController: null
    property HoverPreview requestedController: null
    property bool previewAllowed: true
    readonly property string previewPath: recording ? recording.filePath : ""
    readonly property bool previewEligible: !live && hovered && visible && previewAllowed
        && !isSelectionModeActive && previewPath.length > 0
    readonly property bool previewArmed: currentPreview !== null
    readonly property bool previewFailed: previewController !== null && previewController.failedTarget === previewSurface
    readonly property HoverPreview currentPreview: previewController !== null && previewController.target === previewSurface ? previewController : null
    readonly property bool previewVisible: currentPreview !== null && currentPreview.frameReady

    function resetPreview() {
        if (requestedController && requestedController !== previewController)
            requestedController.release(previewSurface)
        requestedController = previewController
        if (!previewController)
            return
        if (previewEligible)
            previewController.request(previewSurface, recordingLibrary.localFileUrl(previewPath))
        else
            previewController.release(previewSurface)
    }
    onPreviewControllerChanged: resetPreview()
    onPreviewEligibleChanged: resetPreview()
    onPreviewPathChanged: resetPreview()
    onRecordingChanged: resetPreview()

    Component.onDestruction: {
        if (requestedController)
            requestedController.release(previewSurface)
    }
    readonly property bool isSelected: !!(root.recording && window.selectedRecordings && window.selectedRecordings.indexOf(root.recording.filePath) !== -1)
    readonly property bool isSelectionModeActive: !!(window.selectedRecordings && window.selectedRecordings.length > 0)
    readonly property bool showSelection: !root.live && (root.hovered || root.isSelectionModeActive)
    property real selectionReveal: root.showSelection ? 1 : 0
    Behavior on selectionReveal {
        NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
    }
    // Same identity rule as RecordingCard: the container tag wins and
    // survives renames; the filename suffix covers older files and the
    // first paint before the background probe lands.
    readonly property bool isClip: !!(root.recording && (root.recording.isClip || (root.recording.fileName || "").endsWith("_Replay.mkv")))

    function activate(modifiers) {
        if (!root.recording)
            return;
        if (root.isSelectionModeActive || (modifiers & (Qt.ControlModifier | Qt.ShiftModifier))) {
            if (modifiers & Qt.ShiftModifier)
                window.shiftSelectRange(root.cardIndex);
            else {
                window.toggleSelection(root.recording.filePath);
                window.lastSelectedIndex = root.cardIndex;
            }
        } else {
            window.openPlayer(root.recording.filePath);
        }
    }

    function formatDuration(seconds) {
        if (!seconds || seconds <= 0)
            return "0:00";

        var h = Math.floor(seconds / 3600);
        var m = Math.floor((seconds % 3600) / 60);
        var s = seconds % 60;
        if (h > 0)
            return h + ":" + m.toString().padStart(2, '0') + ":" + s.toString().padStart(2, '0');
        return m + ":" + s.toString().padStart(2, '0');
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

    width: ListView.view ? ListView.view.width : 0
    height: 92
    padding: 10
    horizontalPadding: 12
    hoverEnabled: !root.live
    onClicked: root.activate(0)

    MouseArea {
        anchors.fill: parent
        onClicked: mouse => root.activate(mouse.modifiers)
    }

    background: Rectangle {
        color: Theme.cardColor
        radius: Theme.radiusMedium
    }

    contentItem: RowLayout {
        z: 1
        spacing: 0

        // Collapse the gutter completely at rest. Animate one shared
        // value so checkbox opacity and the thumbnail slide stay in sync.
        Item {
            objectName: "listSelectionSlot"
            Layout.preferredWidth: (18 + Theme.spacingNormal) * root.selectionReveal
            Layout.preferredHeight: 18
            Layout.alignment: Qt.AlignVCenter

        Rectangle {
            objectName: "listSelection"
            width: 18
            height: 18
            radius: 5
            color: root.isSelected ? Theme.primaryColor : "transparent"
            border.color: root.isSelected ? Theme.primaryColor : Theme.textSecondaryColor
            border.width: 1.5
            opacity: root.selectionReveal
            visible: root.selectionReveal > 0
            enabled: root.showSelection

            Item {
                anchors.centerIn: parent
                width: 7
                height: 7
                visible: root.isSelected
                rotation: 45
                anchors.verticalCenterOffset: -2
                anchors.horizontalCenterOffset: -1

                Rectangle {
                    width: 2
                    height: 7
                    color: "white"
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                }
                Rectangle {
                    width: 3
                    height: 2
                    color: "white"
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                }
            }

            MouseArea {
                anchors.fill: parent
                onClicked: (mouse) => {
                    if (root.recording) {
                        if (mouse.modifiers & Qt.ShiftModifier) {
                            window.shiftSelectRange(root.cardIndex);
                        } else {
                            window.toggleSelection(root.recording.filePath);
                            window.lastSelectedIndex = root.cardIndex;
                        }
                    }
                }
                cursorShape: Qt.PointingHandCursor
            }
        }
        }

        // Only the small app icon overlays the preview; metadata stays
        // in the details column.
        Rectangle {
            objectName: "listThumbnail"
            Layout.preferredWidth: 128
            Layout.preferredHeight: 72
            Layout.alignment: Qt.AlignVCenter
            Layout.rightMargin: Theme.spacingNormal
            radius: Theme.radiusMedium
            color: root.live ? Theme.surfaceColor : "black"
            clip: true

            // Keep VideoOutput's ancestors visible; mask the live layer,
            // just like grid cards, rather than sampling a hidden subtree.
            Item {
                id: previewSurface
                anchors.fill: parent
                visible: !root.live
                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: sourceMask
                }
                Image {
                    anchors.fill: parent
                    source: root.recording && root.recording.thumbnailPath ? recordingLibrary.localFileUrl(root.recording.thumbnailPath) : "qrc:/icons/video.svg"
                    fillMode: Image.PreserveAspectCrop
                    opacity: root.recording && root.recording.thumbnailPath ? 1.0 : 0.3
                }
            }

            Rectangle {
                id: sourceMask
                anchors.fill: parent
                radius: Theme.radiusMedium
                color: "black"
                visible: false
                layer.enabled: true
            }

            RecordingGameIcon {
                objectName: "listGameIcon"
                gameName: root.live ? recordingManager.currentGameName : (root.recording ? root.recording.gameName : "")
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 6
                width: 24
                height: width
            }

            Rectangle {
                anchors.centerIn: parent
                width: 20
                height: 20
                radius: 10
                color: Theme.primaryColor
                visible: root.live

                SequentialAnimation on opacity {
                    running: root.live && root.visible
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.3; duration: 700; easing.type: Easing.InOutSine }
                    NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutSine }
                }
            }

        }

        // Stable three-line hierarchy: title, type + duration + size, date.
        ColumnLayout {
            objectName: "listDetails"
            spacing: 4
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            Layout.rightMargin: Theme.spacingNormal

            Label {
                objectName: "listTitle"
                text: root.live ? (recordingManager.currentGameName || qsTr("Recording…")) : (root.recording ? (root.isClip ? qsTr("%1 Clip").arg(root.recording.gameName || qsTr("Unknown Game")) : (root.recording.gameName || qsTr("Unknown Game"))) : "")
                color: Theme.textColor
                font.pixelSize: Theme.fontSizeNormal
                font.weight: Font.Bold
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    objectName: "listType"
                    text: root.live ? qsTr("● REC") : (root.isClip ? qsTr("Clip") : qsTr("Recording"))
                    color: root.isClip ? Theme.highlightedColor : Theme.primaryColor
                    font.pixelSize: Theme.fontSizeTiny
                    font.weight: Font.Medium
                }

                Label {
                    objectName: "listMetadata"
                    text: root.live ? "• " + formatDuration(Math.floor(recordingManager.recordingDuration / 1000)) + " • " + formatSize(recordingManager.recordingSize) : (root.recording ? "• " + formatDuration(root.recording.duration) + " • " + formatSize(root.recording.size) : "")
                    color: Theme.textSecondaryColor
                    font.pixelSize: Theme.fontSizeTiny
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            Label {
                text: root.live ? qsTr("Recording in progress") : (root.recording ? Qt.formatDateTime(new Date(root.recording.date), "MMM d, yyyy HH:mm") : "")
                color: Theme.textMutedColor
                font.pixelSize: Theme.fontSizeTiny
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        Row {
            objectName: "listActions"
            // Opacity, rather than visibility, preserves the action slot
            // so titles and metadata never jump when the pointer enters.
            opacity: !root.live && root.hovered && !root.isSelectionModeActive ? 1 : 0
            enabled: !root.live && root.hovered && !root.isSelectionModeActive
            Behavior on opacity { NumberAnimation { duration: Theme.animationFast } }
            spacing: Theme.spacingSmall
            Layout.alignment: Qt.AlignVCenter
            Layout.rightMargin: Theme.spacingSmall

            RoundButton {
                width: 32
                height: 32
                icon.source: "qrc:/icons/play.svg"
                icon.color: "white"
                icon.width: 16
                icon.height: 16
                Accessible.name: qsTr("Play")
                ToolTip.text: qsTr("Play")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: { if (root.recording) window.openPlayer(root.recording.filePath) }

                background: Rectangle {
                    radius: 16
                    color: parent.hovered ? Theme.primaryHoverColor : Theme.primaryColor
                    border.color: parent.hovered ? "#ffffffff" : "#00ffffff"
                    border.width: 1.5
                    scale: parent.hovered ? 1.08 : 1.0
                    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    Behavior on color { ColorAnimation { duration: 150 } }
                }

            }

            RoundButton {
                width: 32
                height: 32
                icon.source: "qrc:/icons/folder.svg"
                icon.color: "white"
                icon.width: 16
                icon.height: 16
                Accessible.name: qsTr("Show in folder")
                ToolTip.text: qsTr("Show in folder")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: { if (root.recording) window.openFolder(root.recording.directory) }

                background: Rectangle {
                    radius: 16
                    color: parent.hovered ? "#e6353f5c" : "#b31a2238"
                    border.color: parent.hovered ? "#b3ffffff" : "#59ffffff"
                    border.width: 1.5
                    scale: parent.hovered ? 1.08 : 1.0
                    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    Behavior on color { ColorAnimation { duration: 150 } }
                }

            }

            RoundButton {
                width: 32
                height: 32
                icon.source: "qrc:/icons/edit.svg"
                icon.color: "white"
                icon.width: 16
                icon.height: 16
                Accessible.name: qsTr("Rename")
                ToolTip.text: qsTr("Rename")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: { if (root.recording) window.requestRename(root.recording.filePath, root.recording.gameName) }

                background: Rectangle {
                    radius: 16
                    color: parent.hovered ? "#e6353f5c" : "#b31a2238"
                    border.color: parent.hovered ? "#b3ffffff" : "#59ffffff"
                    border.width: 1.5
                    scale: parent.hovered ? 1.08 : 1.0
                    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    Behavior on color { ColorAnimation { duration: 150 } }
                }

            }

            RoundButton {
                width: 32
                height: 32
                icon.source: "qrc:/icons/trash.svg"
                icon.color: "white"
                icon.width: 16
                icon.height: 16
                Accessible.name: qsTr("Delete")
                ToolTip.text: qsTr("Delete")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                onClicked: { if (root.recording) window.confirmDelete(root.recording.filePath, root.recording.fileName) }

                background: Rectangle {
                    radius: 16
                    color: parent.hovered ? Theme.errorColor : "#b31a2238"
                    border.color: parent.hovered ? "#ffffffff" : "#59ffffff"
                    border.width: 1.5
                    scale: parent.hovered ? 1.08 : 1.0
                    Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    Behavior on color { ColorAnimation { duration: 150 } }
                }

            }

        }

    }

    // Same border overlay + selected tint + scale as the grid card.
    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusMedium
        color: root.isSelected ? "#20FFFFFF" : "transparent"
        border.color: root.isSelected ? Theme.primaryColor : (root.hovered ? Theme.primaryColor : Theme.borderColor)
        border.width: root.isSelected ? 2 : (root.hovered ? 2 : 1)
        z: 10
    }

}
