/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../styles"
import "../controls"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ListView {
    id: root

    property int skip: 0
    property HoverPreview previewController: null

    spacing: Theme.spacingSmall
    clip: true
    boundsBehavior: Flickable.DragAndOvershootBounds
    boundsMovement: Flickable.FollowBoundsBehavior
    model: {
        var full = (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.recordings : [];
        if (skip > 0) {
            full = full.slice(skip);
        }
        if (typeof recordingManager !== "undefined" && recordingManager && recordingManager.isRecording)
            return [{ __live: true }].concat(full);
        return full;
    }

    delegate: RecordingListItem {
        previewController: root.previewController
        previewAllowed: !root.moving && root.visible
            && (typeof window.hoverPreviewsAllowed === "undefined" || window.hoverPreviewsAllowed)
        live: modelData && modelData.__live === true
        objectName: live ? "liveListRow" : "savedListRow"
        recording: live ? null : modelData
        cardIndex: live ? -1 : index + root.skip - ((typeof recordingManager !== "undefined" && recordingManager && recordingManager.isRecording) ? 1 : 0)
        width: root.width - root.leftMargin - root.rightMargin
    }

    Label {
        anchors.centerIn: parent
        visible: root.count === 0 && !(typeof recordingManager !== "undefined" && recordingManager && recordingManager.isRecording)
        text: recordingLibrary && (recordingLibrary.kindFilter !== 0 || recordingLibrary.searchQuery.length > 0)
            ? qsTr("No matching captures\nTry another filter or search.")
            : qsTr("No captures yet\nRecord some gameplay to see it here!")
        color: Theme.textMutedColor
        horizontalAlignment: Text.AlignHCenter
        font.pixelSize: Theme.fontSizeMedium
    }

    footer: Item {
        height: Theme.spacingLarge
    }

    ScrollBar.vertical: TrophyScrollBar {}

}
