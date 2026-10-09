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

GridView {
    id: root

    property int skip: 0

    property int minCellWidth: 280
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
        var full = (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.recordings : [];
        if (skip > 0 && full.length > skip) {
            full = full.slice(skip);
        }
        // Prepend a sentinel for the in-progress recording so the live
        // card renders as the first grid cell while recording.
        if (typeof recordingManager !== "undefined" && recordingManager && recordingManager.isRecording) {
            return [{ __live: true }].concat(full);
        }
        return full;
    }

    // Empty state
    Label {
        anchors.centerIn: parent
        visible: root.count === 0
        text: qsTr("No recordings found\nRecord some gameplay to see it here!")
        color: Theme.textMutedColor
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.fontSizeMedium
    }

    delegate: Loader {
        id: cellLoader
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
            anchors.fill: parent
        }
    }

    Component {
        id: recordingCardComponent
        RecordingCard {
            anchors.fill: parent
            recording: parent.recData
            cardIndex: parent.recIdx
            onPlay: {
                if (recording) {
                    window.openPlayer(recording.filePath);
                }
            }
            onOpenFolder: {
                if (recording) {
                    window.openFolder(recording.directory);
                }
            }
            onDeleteRequested: {
                if (recording) {
                    window.confirmDelete(recording.filePath, recording.fileName);
                }
            }
            onRenameRequested: {
                if (recording) {
                    window.requestRename(recording.filePath, recording.gameName);
                }
            }
        }
    }

    ScrollBar.vertical: ScrollBar {
        policy: ScrollBar.AsNeeded
    }

}
