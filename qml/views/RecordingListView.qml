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

ListView {
    id: root
    // Explicit dependencies; defaults preserve engine/creation-context hosts.
    property var hostWindow: typeof window !== "undefined" ? window : null // qmllint disable unqualified
    property var library: typeof recordingLibrary !== "undefined" ? recordingLibrary : null // qmllint disable unqualified
    property var recorder: typeof recordingManager !== "undefined" ? recordingManager : null // qmllint disable unqualified

    property int skip: 0
    property HoverPreview previewController: null

    spacing: Theme.spacingSmall
    clip: true
    boundsBehavior: Flickable.DragAndOvershootBounds
    boundsMovement: Flickable.FollowBoundsBehavior
    model: {
        var full = root.library ? root.library.recordings : [];
        if (skip > 0) {
            full = full.slice(skip);
        }
        if (root.recorder && root.recorder.isRecording)
            return [{ __live: true }].concat(full);
        return full;
    }

    delegate: RecordingListItem {
        required property var modelData
        required property int index
        hostWindow: root.hostWindow
        library: root.library
        recorder: root.recorder
        previewController: root.previewController
        previewAllowed: !root.moving && root.visible
            && (!root.hostWindow || typeof root.hostWindow.hoverPreviewsAllowed === "undefined" || root.hostWindow.hoverPreviewsAllowed)
        live: modelData && modelData.__live === true
        objectName: live ? "liveListRow" : "savedListRow"
        recording: live ? null : modelData
        cardIndex: live ? -1 : index + root.skip - ((root.recorder && root.recorder.isRecording) ? 1 : 0)
        width: root.width - root.leftMargin - root.rightMargin
    }

    Label {
        anchors.centerIn: parent
        visible: root.count === 0 && !(root.recorder && root.recorder.isRecording)
        text: root.library && (root.library.kindFilter !== 0 || root.library.searchQuery.length > 0)
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
