/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

pragma ComponentBehavior: Bound
import "../styles"
import "../controls"
import QtQuick
import QtQuick.Controls

GridView {
    id: root
    // Explicit dependencies; defaults preserve engine/creation-context hosts.
    property var hostWindow: typeof window !== "undefined" ? window : null // qmllint disable unqualified
    property var library: typeof recordingLibrary !== "undefined" ? recordingLibrary : null // qmllint disable unqualified
    property var recorder: typeof recordingManager !== "undefined" ? recordingManager : null // qmllint disable unqualified

    property int skip: 0
    property HoverPreview previewController: null

    property int minCellWidth: 340
    cellWidth: {
        var availableWidth = width - leftMargin - rightMargin;
        var cols = Math.floor(availableWidth / minCellWidth);
        if (cols <= 0) cols = 1;
        return availableWidth / cols;
    }
    cellHeight: Theme.recordingCardHeight + Theme.spacingMedium
    clip: false
    boundsBehavior: Flickable.DragAndOvershootBounds
    boundsMovement: Flickable.FollowBoundsBehavior
    model: {
        var full = root.library ? root.library.recordings : [];
        if (skip > 0 && full.length > skip) {
            full = full.slice(skip);
        }
        // Prepend a sentinel for the in-progress recording so the live
        // card renders as the first grid cell while recording.
        if (root.recorder && root.recorder.isRecording) {
            return [{ __live: true }].concat(full);
        }
        return full;
    }

    // Empty state
    Label {
        anchors.centerIn: parent
        visible: root.count === 0
        text: root.library && (root.library.kindFilter !== 0 || root.library.searchQuery.length > 0)
            ? qsTr("No matching captures\nTry another filter or search.")
            : qsTr("No captures yet\nRecord some gameplay to see it here!")
        color: Theme.textMutedColor
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.fontSizeMedium
    }

    delegate: CellLoader {}

    component CellLoader: Loader {
        id: cellLoader
        required property var modelData
        required property int index
        width: root.cellWidth - Theme.spacingMedium
        height: Theme.recordingCardHeight
        // NOTE: modelData/index live on the delegate's context and do NOT
        // resolve inside the Components below — forward them explicitly.
        property var recData: modelData
        property int recIdx: index
        property bool isLive: modelData && modelData.__live === true
        sourceComponent: isLive ? liveCardComponent : recordingCardComponent
    }

    Component {
        id: liveCardComponent
        ActiveRecordingCard {
            recorder: root.recorder
            anchors.fill: parent
        }
    }

    Component {
        id: recordingCardComponent
        RecordingCard {
            readonly property CellLoader cell: parent as CellLoader
            hostWindow: root.hostWindow
            library: root.library
            anchors.fill: parent
            previewController: root.previewController
            previewAllowed: !root.moving && root.visible
                && (!root.hostWindow || typeof root.hostWindow.hoverPreviewsAllowed === "undefined" || root.hostWindow.hoverPreviewsAllowed)
            recording: cell ? cell.recData : null
            // Delegate index includes the live sentinel while recording
            // (scrollToRecording adds the same offset in reverse): subtract
            // it so shift-select ranges index into recordingLibrary.
            cardIndex: !cell || cell.isLive ? -1 : cell.recIdx - ((root.recorder && root.recorder.isRecording) ? 1 : 0)
            onPlay: {
                if (recording) {
                    root.hostWindow.openPlayer(recording.filePath);
                }
            }
            onDeleteRequested: {
                if (recording) {
                    root.hostWindow.confirmDelete(recording.filePath, recording.fileName);
                }
            }
            onShareRequested: {
                if (recording)
                    root.hostWindow.openShareExport(recording.filePath);
            }
            onRenameRequested: {
                if (recording) {
                    root.hostWindow.requestRename(recording.filePath, recording.gameName);
                }
            }
        }
    }

    ScrollBar.vertical: TrophyScrollBar {}

}
