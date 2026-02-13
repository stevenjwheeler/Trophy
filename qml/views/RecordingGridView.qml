/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../styles"

GridView {
    id: root

    cellWidth: Theme.recordingCardWidth + Theme.spacingMedium
    cellHeight: Theme.recordingCardHeight + Theme.spacingMedium

    clip: true
    boundsBehavior: Flickable.StopAtBounds

    model: (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.recordings : []

    delegate: RecordingCard {
        width: Theme.recordingCardWidth
        height: Theme.recordingCardHeight

        recording: modelData

        onPlay: if (modelData) Qt.openUrlExternally("file://" + modelData.filePath)
        onOpenFolder: if (modelData) window.openFolder(modelData.directory)
        onDeleteRequested: if (modelData) window.confirmDelete(modelData.filePath, modelData.fileName)
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

    ScrollBar.vertical: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
