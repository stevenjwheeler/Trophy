/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../styles"

ListView {
    id: root

    spacing: Theme.spacingSmall
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    model: (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.recordings : []

    delegate: ItemDelegate {
        width: root.width
        height: 70
        onClicked: Qt.openUrlExternally("file://" + modelData.filePath)

        background: Rectangle {
            color: parent.hovered ? Theme.cardHoverColor : Theme.cardColor
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
                    source: modelData.thumbnailPath ? "file://" + modelData.thumbnailPath : ""
                    fillMode: Image.PreserveAspectCrop
                    visible: modelData.thumbnailPath !== ""
                }
            }

            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true

                Label {
                    text: modelData.gameName || qsTr("Untitled Recording")
                    color: Theme.textColor
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Label {
                    text: modelData.fileName
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }

            Label {
                text: formatDuration(modelData.duration)
                color: Theme.textSecondaryColor
                font.pixelSize: Theme.fontSizeSmall
            }

            Label {
                text: formatDate(modelData.date)
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
                    onClicked: Qt.openUrlExternally("file://" + modelData.filePath)
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
                    onClicked: window.openFolder(modelData.directory)
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
                    onClicked: window.confirmDelete(modelData.filePath, modelData.fileName)
                    background: Rectangle {
                        radius: 16
                        color: parent.hovered ? Theme.errorColor : "#40FFFFFF"
                    }
                }
            }
        }
    }

    function formatDuration(seconds) {
        if (!seconds) return "0:00"
        var m = Math.floor(seconds / 60)
        var s = seconds % 60
        return m + ":" + s.toString().padStart(2, '0')
    }

    function formatDate(date) {
        if (!date) return ""
        return Qt.formatDateTime(date, "MMM d, HH:mm")
    }

    ScrollBar.vertical: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}
