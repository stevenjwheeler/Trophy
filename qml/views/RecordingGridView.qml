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
        var cols = Math.floor(width / minCellWidth);
        if (cols <= 0) cols = 1;
        return width / cols;
    }
    cellHeight: Theme.recordingCardHeight + Theme.spacingMedium
    clip: true
    boundsBehavior: Flickable.DragAndOvershootBounds
    boundsMovement: Flickable.FollowBoundsBehavior
    model: {
        var full = (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.recordings : [];
        if (skip > 0 && full.length > skip) {
            return full.slice(skip);
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

    delegate: RecordingCard {
        width: root.cellWidth - Theme.spacingMedium
        height: Theme.recordingCardHeight
        recording: modelData
        onPlay: {
            if (modelData) {
                Qt.openUrlExternally("file://" + modelData.filePath);
            }
        }
        onOpenFolder: {
            if (modelData) {
                window.openFolder(modelData.directory);
            }
        }
        onDeleteRequested: {
            if (modelData) {
                window.confirmDelete(modelData.filePath, modelData.fileName);
            }
        }
    }

    ScrollBar.vertical: ScrollBar {
        policy: ScrollBar.AsNeeded
    }

}
