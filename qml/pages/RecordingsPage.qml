/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import TrophyBackend
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.spacingLarge
        anchors.leftMargin: Theme.spacingLarge
        anchors.rightMargin: Theme.spacingLarge
        spacing: Theme.spacingMedium

        // Header
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingMedium

            Label {
                text: qsTr("Recordings")
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Font.Bold
                color: Theme.textColor
                Layout.alignment: Qt.AlignTop
            }

            Item {
                Layout.fillWidth: true
            }

            // Search field
            TrophyTextField {
                id: searchField

                Layout.preferredWidth: 250
                placeholderText: qsTr("Search recordings...")
                onTextChanged: {
                    if (recordingLibrary)
                        recordingLibrary.searchQuery = text;

                }
            }

            // View toggle
            Row {
                spacing: 2

                TrophyButton {
                    width: 36
                    height: 36
                    icon.source: "qrc:/icons/view-grid.svg"
                    checkable: true
                    checked: viewToggle.currentIndex === 0
                    onClicked: viewToggle.currentIndex = 0
                }

                TrophyButton {
                    width: 36
                    height: 36
                    icon.source: "qrc:/icons/view-list.svg"
                    checkable: true
                    checked: viewToggle.currentIndex === 1
                    onClicked: viewToggle.currentIndex = 1
                }

            }

            // Sort dropdown
            TrophyComboBox {
                id: sortCombo

                model: [qsTr("Newest First"), qsTr("Oldest First"), qsTr("Largest First"), qsTr("Name A-Z")]
                onCurrentIndexChanged: {
                    if (recordingLibrary)
                        recordingLibrary.sortOrder = currentIndex;

                }

                // 1. Define the Text (What was missing)
                contentItem: Label {
                    leftPadding: Theme.spacingMedium
                    rightPadding: Theme.spacingMedium
                    text: sortCombo.displayText
                    font: sortCombo.font
                    color: Theme.textColor // Ensure this contrasts with cardColor
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }

                // 2. Your existing background
                background: Rectangle {
                    implicitWidth: 160
                    implicitHeight: 36
                    radius: Theme.radiusNormal
                    color: Theme.cardColor
                    border.color: sortCombo.visualFocus ? Theme.primaryColor : "transparent"
                    border.width: sortCombo.visualFocus ? 2 : 0
                }

            }

        }

        // Toolbar
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Label {
                id: recordingCountLabel

                text: qsTr("%n recording(s)", "", (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.count : 0)
                color: Theme.textSecondaryColor
                font.pixelSize: Theme.fontSizeSmall
            }

            Item {
                Layout.fillWidth: true
            }

            TrophyButton {
                text: qsTr("Open Folder")
                icon.name: "folder-open"
                implicitHeight: 26
                onClicked: {
                    if (settingsManager && settingsManager.recordingsPath)
                        window.openFolder(settingsManager.recordingsPath);

                }
            }

            TrophyButton {
                text: qsTr("Refresh")
                icon.name: "view-refresh"
                implicitHeight: 26
                onClicked: {
                    if (recordingLibrary)
                        recordingLibrary.refresh();

                }
            }

        }

        // Content area
        StackLayout {
            id: viewToggle

            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: 1

            // Grid view
            RecordingGridView {
                id: gridView
            }

            // List view
            RecordingListView {
                id: listView
            }

        }

    }

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: 12
        topLeftRadius: 26
    }

}
