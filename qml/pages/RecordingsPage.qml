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
import "../views"

Page {
    id: root

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: 12
        topLeftRadius: 26
    }
    
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

            Item { Layout.fillWidth: true }

            // Search field
            TextField {
                id: searchField
                Layout.preferredWidth: 250
                placeholderText: qsTr("Search recordings...")
                onTextChanged: if (recordingLibrary) recordingLibrary.searchQuery = text

                background: Rectangle {
                    radius: Theme.radiusNormal
                    color: Theme.cardColor
                    border.width: searchField.activeFocus ? 2 : 0
                    border.color: Theme.primaryColor
                }
            }

            // View toggle
            Row {
                spacing: 2

                Button {
                    width: 36
                    height: 36
                    icon.source: "qrc:/icons/view-grid.svg"
                    flat: true
                    checkable: true
                    checked: viewToggle.currentIndex === 0
                    onClicked: viewToggle.currentIndex = 0
                }

                Button {
                    width: 36
                    height: 36
                    icon.source: "qrc:/icons/view-list.svg"
                    flat: true
                    checkable: true
                    checked: viewToggle.currentIndex === 1
                    onClicked: viewToggle.currentIndex = 1
                }
            }

            // Sort dropdown
            ComboBox {
                id: sortCombo
                model: [
                    qsTr("Newest First"),
                    qsTr("Oldest First"),
                    qsTr("Largest First"),
                    qsTr("Name A-Z")
                ]
                onCurrentIndexChanged: if (recordingLibrary) recordingLibrary.sortOrder = currentIndex

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

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Open Folder")
                flat: true
                icon.name: "folder-open"
                onClicked: {
                    if (settingsManager && settingsManager.recordingsPath) {
                        window.openFolder(settingsManager.recordingsPath)
                    }
                }
            }

            Button {
                text: qsTr("Refresh")
                flat: true
                icon.name: "view-refresh"
                onClicked: if (recordingLibrary) recordingLibrary.refresh()
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

}
