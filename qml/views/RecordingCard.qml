/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

Rectangle {
    id: root

    property var recording: null
    property bool isAdaptive: true
    property int cardIndex: -1
    readonly property bool hovered: hoverHandler.hovered
    property HoverPreview previewController: null
    property HoverPreview requestedController: null
    property bool previewAllowed: true
    readonly property string previewPath: recording ? recording.filePath : ""
    readonly property bool previewEligible: hovered && visible && previewAllowed
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
    property bool isSelected: window.selectedRecordings && recording && window.selectedRecordings.indexOf(recording.filePath) !== -1
    property bool isSelectionModeActive: window.selectedRecordings && window.selectedRecordings.length > 0
    // Clips read as "<game> Clip" with a badge; full recordings show
    // the game name with a recording badge. The container tag wins and
    // survives renames; the filename suffix covers older files and the
    // first paint before the background probe lands.
    readonly property bool isClip: root.recording && (root.recording.isClip || (root.recording.fileName || "").endsWith("_Replay.mkv"))
    
    height: Theme.recordingCardHeight
    radius: Theme.radiusMedium
    color: Theme.cardColor
    clip: true
    
    layer.enabled: false

    signal play()
    signal shareRequested()
    signal renameRequested()
    signal deleteRequested()

    function formatDuration(seconds) {
        if (!seconds || seconds <= 0) return "0:00";
        var h = Math.floor(seconds / 3600);
        var m = Math.floor((seconds % 3600) / 60);
        var s = seconds % 60;
        if (h > 0) return h + ":" + m.toString().padStart(2, '0') + ":" + s.toString().padStart(2, '0');
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

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onClicked: (mouse) => {
            if (root.isSelectionModeActive) {
                if (root.recording) {
                    if (mouse.modifiers & Qt.ShiftModifier) {
                        // Shift+click: range select
                        window.shiftSelectRange(root.cardIndex);
                    } else {
                        // Regular click in selection mode: toggle this item
                        window.toggleSelection(root.recording.filePath);
                        window.lastSelectedIndex = root.cardIndex;
                    }
                }
            } else {
                root.play();
            }
        }
        z: 0 
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        z: 1

        // --- THUMBNAIL SECTION ---
        Item {
            id: thumbnailContainer
            objectName: "gridThumbnail"
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            
            // This keeps the bottom edge razor sharp where it meets the info section
            clip: true

            // Mask both the fallback thumbnail and the first decoded frame
            // together, preserving the card's existing corner treatment.
            Item {
                id: previewSurface
                anchors.fill: parent
                // VideoOutput needs a visible scene-graph subtree to update
                // its texture. Mask the live layer rather than hiding its
                // ancestor and sampling it through a separate effect.
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
                    scale: root.hovered ? 1.05 : 1.0
                    Behavior on scale { NumberAnimation { duration: 400; easing.type: Easing.OutCubic } }
                }
            }

            // 2. The Shape Mask
            Rectangle {
                id: sourceMask
                width: parent.width
                // Push bottom radius out of bounds so the clip catches it
                height: parent.height + root.radius 
                topLeftRadius: root.radius
                topRightRadius: root.radius
                color: "black" 
                visible: false 
                
                // CRITICAL FIX: The mask MUST be a layer for MultiEffect to read its alpha channel
                layer.enabled: true 
            }

            // 4. Hover Buttons Overlay
            Item {
                anchors.fill: parent
                visible: root.hovered && !root.isSelectionModeActive

                Rectangle {
                    width: parent.width
                    height: parent.height + root.radius
                    radius: root.radius
                    color: root.previewVisible ? "#33000000" : "#99000000"
                    Behavior on color { ColorAnimation { duration: 180 } }
                }

                Row {
                    anchors.centerIn: parent
                    spacing: 10

                    RoundButton {
                        width: 42; height: 42
                        icon.source: "qrc:/icons/play.svg"; icon.color: "white"; icon.width: 20; icon.height: 20
                        Accessible.name: qsTr("Play")
                        ToolTip.text: qsTr("Play")
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        onClicked: root.play()
                        background: Rectangle {
                            radius: 21
                            color: parent.hovered ? Theme.primaryHoverColor : Theme.primaryColor
                            border.color: parent.hovered ? "#ffffffff" : "#00ffffff"
                            border.width: 1.5
                            scale: parent.hovered ? 1.1 : 1.0
                            Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                            Behavior on color { ColorAnimation { duration: 150 } }
                        }
                    }

                    RoundButton {
                        objectName: "gridShareExport"
                        width: 42; height: 42
                        icon.source: "qrc:/icons/share.svg"; icon.color: "white"; icon.width: 20; icon.height: 20
                        Accessible.name: qsTr("Export to share")
                        ToolTip.text: qsTr("Export to share")
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        onClicked: root.shareRequested()
                        background: Rectangle {
                            radius: 21
                            color: parent.hovered ? "#e6353f5c" : "#b31a2238"
                            border.color: parent.hovered ? "#b3ffffff" : "#59ffffff"
                            border.width: 1.5
                            scale: parent.hovered ? 1.08 : 1.0
                            Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                            Behavior on color { ColorAnimation { duration: 150 } }
                        }
                    }

                    RoundButton {
                        width: 42; height: 42
                        icon.source: "qrc:/icons/edit.svg"; icon.color: "white"; icon.width: 20; icon.height: 20
                        Accessible.name: qsTr("Rename")
                        ToolTip.text: qsTr("Rename")
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        onClicked: root.renameRequested()
                        background: Rectangle {
                            radius: 21
                            color: parent.hovered ? "#e6353f5c" : "#b31a2238"
                            border.color: parent.hovered ? "#b3ffffff" : "#59ffffff"
                            border.width: 1.5
                            scale: parent.hovered ? 1.08 : 1.0
                            Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                            Behavior on color { ColorAnimation { duration: 150 } }
                        }
                    }

                    RoundButton {
                        width: 42; height: 42
                        icon.source: "qrc:/icons/trash.svg"; icon.color: "white"; icon.width: 20; icon.height: 20
                        Accessible.name: qsTr("Delete")
                        ToolTip.text: qsTr("Delete")
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        onClicked: root.deleteRequested()
                        background: Rectangle {
                            radius: 21
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

            // 5. Duration badge
            Rectangle {
                anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: Theme.spacingSmall
                visible: !root.hovered && root.recording
                width: durationLabel.width + 12; height: durationLabel.height + 4
                radius: 4; color: "#CC000000"
                Label {
                    id: durationLabel; anchors.centerIn: parent
                    text: root.recording ? formatDuration(root.recording.duration) : ""
                    color: "white"; font.pixelSize: Theme.fontSizeTiny
                }
            }
        }

        // --- INFO SECTION ---
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.margins: Theme.spacingSmall; spacing: 2
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Label {
                    objectName: "gridTitle"
                    text: root.recording ? (root.isClip ? qsTr("%1 Clip").arg(root.recording.gameName || qsTr("Unknown Game")) : (root.recording.gameName || qsTr("Unknown Game"))) : ""
                    font.pixelSize: Theme.fontSizeSmall; font.weight: Font.Bold; color: Theme.textColor; elide: Text.ElideRight; Layout.fillWidth: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6

                    Label {
                        objectName: "gridType"
                        text: root.isClip ? qsTr("Clip") : qsTr("Recording")
                        color: root.isClip ? Theme.highlightedColor : Theme.primaryColor
                        font.pixelSize: Theme.fontSizeTiny
                        font.weight: Font.Medium
                    }

                    Label {
                        objectName: "gridMetadata"
                        text: root.recording ? "• " + formatDuration(root.recording.duration) + " • " + formatSize(root.recording.size) : ""
                        font.pixelSize: Theme.fontSizeTiny; color: Theme.textSecondaryColor
                        elide: Text.ElideRight; Layout.fillWidth: true
                    }
                }
                Label {
                    objectName: "gridDate"
                    Layout.fillWidth: true
                    text: root.recording ? Qt.formatDateTime(new Date(root.recording.date), "MMM d, yyyy HH:mm") : ""
                    font.pixelSize: Theme.fontSizeTiny; color: Theme.textMutedColor
                    elide: Text.ElideRight
                }
            }
            RecordingGameIcon {
                objectName: "gridGameIcon"
                gameName: root.recording ? root.recording.gameName : ""
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                Layout.leftMargin: Theme.spacingSmall
                Layout.alignment: Qt.AlignVCenter
            }
        }
    }

    // --- SELECTION CIRCLE OVERLAY ---
    Rectangle {
        id: selectionCircle
        objectName: "gridSelection"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.spacingSmall
        width: 24
        height: 24
        radius: 12
        color: root.isSelected ? Theme.primaryColor : "#80000000"
        border.color: root.isSelected ? Theme.primaryColor : "white"
        border.width: 1.5
        visible: root.hovered || root.isSelectionModeActive
        z: 20

        Item {
            anchors.centerIn: parent
            width: 8
            height: 8
            visible: root.isSelected
            rotation: 45
            anchors.verticalCenterOffset: -2
            anchors.horizontalCenterOffset: -1
            
            Rectangle {
                width: 2
                height: 8
                color: "white"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
            }
            Rectangle {
                width: 4
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
                        // Shift+click: range select
                        window.shiftSelectRange(root.cardIndex);
                    } else {
                        // Regular click: toggle this item
                        window.toggleSelection(root.recording.filePath);
                        window.lastSelectedIndex = root.cardIndex;
                    }
                }
            }
            cursorShape: Qt.PointingHandCursor
        }
    }

    // Protection stays at the top-left; selection keeps the top-right.
    ToolButton {
        id: protectionButton
        objectName: "captureProtectionButton"
        readonly property bool protectionOn: !!root.recording && !!root.recording.protectedFromCleanup
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: Theme.spacingSmall
        width: 24
        height: 24
        padding: 0
        horizontalPadding: 0
        z: 20
        visible: !!root.recording && (root.hovered || protectionOn)
        icon.source: "qrc:/icons/lock.svg"
        icon.width: 16; icon.height: 16
        icon.color: "white"
        Accessible.name: qsTr("Protect from auto-cleanup")
        ToolTip.text: protectionOn ? qsTr("Protected from auto-cleanup — click to unprotect") : qsTr("Protect from auto-cleanup (manual deletion still allowed)")
        ToolTip.visible: hovered
        onClicked: { if (root.recording) window.toggleCaptureProtection(root.recording.filePath) }
        background: Rectangle {
            radius: 12
            color: protectionButton.protectionOn ? Theme.primaryColor : "#80000000"
            border.color: protectionButton.protectionOn ? Theme.primaryColor : "white"
            border.width: 1.5
        }
    }

    // --- BORDER OVERLAY ---
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: root.isSelected ? "#20FFFFFF" : "transparent"
        border.color: root.isSelected ? Theme.primaryColor : (root.hovered ? Theme.primaryColor : Theme.borderColor)
        border.width: root.isSelected ? 2 : (root.hovered ? 2 : 1)
        z: 10
    }
    
    scale: root.isSelected ? 1.03 : (root.hovered ? 1.02 : 1.0)
    Behavior on scale { NumberAnimation { duration: Theme.animationFast } }

    HoverHandler { id: hoverHandler }

    Behavior on color { ColorAnimation { duration: Theme.animationFast } }
}
