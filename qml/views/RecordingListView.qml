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

    spacing: Theme.spacingSmall
    clip: true
    boundsBehavior: Flickable.DragAndOvershootBounds
    boundsMovement: Flickable.FollowBoundsBehavior
    model: {
        var full = (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.recordings : [];
        if (skip > 0) {
            return full.slice(skip);
        }
        return full;
    }

    delegate: RecordingListItem {
        recording: modelData
        cardIndex: index + root.skip
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

    header: Rectangle {
        id: liveHeader
        readonly property bool live: typeof recordingManager !== "undefined" && recordingManager && recordingManager.isRecording
        width: root.width - root.leftMargin - root.rightMargin
        height: live ? 70 : 0
        visible: live
        radius: Theme.radiusSmall
        color: Theme.cardColor
        border.color: Theme.primaryColor

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.spacingMedium
            spacing: Theme.spacingMedium
            Label {
                text: qsTr("● REC")
                color: Theme.primaryColor
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: liveHeader.live ? (recordingManager.currentGameName || qsTr("Recording…")) : ""
                color: Theme.textColor
                elide: Text.ElideRight
            }
            Label {
                text: liveHeader.live ? qsTr("Recording in progress") : ""
                color: Theme.textSecondaryColor
            }
        }
    }

    footer: Item {
        height: Theme.spacingLarge
    }

    ScrollBar.vertical: TrophyScrollBar {}

}
