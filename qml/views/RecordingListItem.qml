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

ItemDelegate {
    id: root

    property var recording: null

    function formatDuration(seconds) {
        if (!seconds)
            return "0:00";

        var m = Math.floor(seconds / 60);
        var s = seconds % 60;
        return m + ":" + s.toString().padStart(2, '0');
    }

    function formatDate(date) {
        if (!date)
            return "";

        return Qt.formatDateTime(date, "MMM d, HH:mm");
    }

    width: ListView.view ? ListView.view.width : 0
    height: 70
    onClicked: Qt.openUrlExternally("file://" + root.recording.filePath)

    background: Rectangle {
        color: root.hovered ? Theme.cardHoverColor : Theme.cardColor
        radius: Theme.radiusSmall
    }

    contentItem: RowLayout {
        spacing: Theme.spacingMedium

        // Thumbnail mini
        Rectangle {
            Layout.preferredWidth: 100
            Layout.preferredHeight: 60
            color: "black"
            radius: 4
            clip: true

            Image {
                anchors.fill: parent
                source: root.recording && root.recording.thumbnailPath ? "file://" + root.recording.thumbnailPath : ""
                fillMode: Image.PreserveAspectCrop
                visible: root.recording && root.recording.thumbnailPath !== ""
            }

        }

        ColumnLayout {
            spacing: 2
            Layout.fillWidth: true

            Label {
                text: root.recording ? root.recording.gameName || qsTr("Untitled Recording") : ""
                color: Theme.textColor
                font.weight: Font.Medium
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Label {
                text: root.recording ? root.recording.fileName : ""
                color: Theme.textMutedColor
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

        }

        Label {
            text: root.recording ? formatDuration(root.recording.duration) : ""
            color: Theme.textSecondaryColor
            font.pixelSize: Theme.fontSizeSmall
        }

        Label {
            text: root.recording ? formatDate(root.recording.date) : ""
            color: Theme.textSecondaryColor
            font.pixelSize: Theme.fontSizeSmall
        }

        Row {
            spacing: Theme.spacingSmall
            Layout.rightMargin: Theme.spacingMedium

            RoundButton {
                width: 32
                height: 32
                icon.name: "media-playback-start"
                icon.color: "white"
                onClicked: Qt.openUrlExternally("file://" + root.recording.filePath)

                background: Rectangle {
                    radius: 16
                    color: parent.hovered ? Theme.primaryHoverColor : Theme.primaryColor
                }

            }

            RoundButton {
                width: 32
                height: 32
                icon.name: "folder-open"
                icon.color: "white"
                onClicked: window.openFolder(root.recording.directory)

                background: Rectangle {
                    radius: 16
                    color: parent.hovered ? "#60FFFFFF" : "#40FFFFFF"
                }

            }

            RoundButton {
                width: 32
                height: 32
                icon.name: "edit-delete"
                icon.color: "white"
                onClicked: window.confirmDelete(root.recording.filePath, root.recording.fileName)

                background: Rectangle {
                    radius: 16
                    color: parent.hovered ? Theme.errorColor : "#40FFFFFF"
                }

            }

        }

    }

}
