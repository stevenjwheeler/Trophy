/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import GameRecorder 1.0

import "styles"
import "components"
import "pages"

ApplicationWindow {
    id: window

    // Removes system borders/titlebar
    flags: Qt.FramelessWindowHint | Qt.Window | Qt.WindowMinMaxButtonsHint

    width: 1200
    height: 800
    minimumWidth: 1200
    minimumHeight: 800
    maximumWidth: 1200
    maximumHeight: 800

    visible: true
    title: qsTr("Trophy - Game recording and clipping for Linux")

    // Make the actual window surface transparent
    color: "transparent"

    property int currentPage: 0

    function openWindowPicker() {
        windowPicker.open()
    }

    // No background - we use a clipped Rectangle as content root instead
    background: Item {}

    // Single clipped Rectangle that IS the window body
    Rectangle {
        id: windowBody
        anchors.fill: parent
        radius: 12
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.borderColor
        clip: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0

            // --- CUSTOM TITLE BAR ---
            Rectangle {
                id: titleBar
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                color: Theme.surfaceColor
                topLeftRadius: 12
                topRightRadius: 12

                MouseArea {
                    anchors.fill: parent
                    onPressed: window.startSystemMove()
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMedium
                    spacing: Theme.spacingSmall

                    Image {
                        source: "qrc:/icons/trophy.svg"
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        sourceSize: Qt.size(18, 18)
                    }

                    Label {
                        text: window.title
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        color: Theme.textSecondaryColor
                    }

                    Item { Layout.fillWidth: true } // Spacer

                    Rectangle {
                        id: recordingPill
                        Layout.preferredHeight: 22
                        Layout.preferredWidth: pillRow.implicitWidth + 20
                        radius: 11
                        color: "#E6CF3131"
                        visible: (typeof recordingManager !== "undefined" && recordingManager) ? recordingManager.isRecording : false
                        border.color: "#FF5F5F"
                        border.width: 1

                        RowLayout {
                            id: pillRow
                            anchors.centerIn: parent
                            spacing: 10

                            // Dot
                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                color: "#FFF"
                                SequentialAnimation on opacity {
                                    loops: Animation.Infinite
                                    NumberAnimation { from: 1.0; to: 0.3; duration: 800 }
                                    NumberAnimation { from: 0.3; to: 1.0; duration: 800 }
                                }
                            }

                            // App Name
                            Label {
                                text: (typeof recordingManager !== "undefined" && recordingManager) ? recordingManager.currentGameName : ""
                                color: "white"
                                font.pixelSize: 11
                                font.weight: Font.Bold
                                visible: text !== ""
                            }

                            // Separator
                            Rectangle {
                                width: 1
                                height: 12
                                color: "white"
                                opacity: 0.3
                                visible: (typeof recordingManager !== "undefined" && recordingManager) ? recordingManager.currentGameName !== "" : false
                            }

                            // Timer
                            Label {
                                text: {
                                    var durMs = (typeof recordingManager !== "undefined" && recordingManager) ? recordingManager.recordingDuration : 0
                                    var dur = Math.floor(durMs / 1000)
                                    var m = Math.floor(dur / 60)
                                    var s = dur % 60
                                    return m + ":" + s.toString().padStart(2, '0')
                                }
                                color: "white"
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }

                            // Small Stop Button
                            Button {
                                Layout.preferredWidth: 18
                                Layout.preferredHeight: 18
                                flat: true
                                padding: 0
                                icon.source: "qrc:/icons/stop.svg"
                                icon.width: 12
                                icon.height: 12
                                icon.color: "white"
                                palette.button: "transparent"
                                onClicked: if (recordingManager) recordingManager.stopRecording()
                                
                                background: Rectangle {
                                    radius: 9
                                    color: parent.hovered ? "#40FFFFFF" : "transparent"
                                }
                            }
                        }
                    }

                    // Window Control Buttons
                    Row {
                        Layout.fillHeight: true

                        Button {
                            width: 46
                            height: 32
                            text: "—"
                            flat: true
                            onClicked: window.showMinimized()
                        }
                        Button {
                            width: 46
                            height: 32
                            text: "✕"
                            flat: true
                            onClicked: window.close()
                        }
                    }
                }
            }

            // --- MAIN CONTENT AREA ---
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                NavigationRail {
                    id: navRail
                    Layout.fillHeight: true
                    currentIndex: window.currentPage
                    onNavigate: function(index) {
                        window.currentPage = index
                    }
                }

                // Main panel
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    StackLayout {
                        id: pageStack
                        anchors.fill: parent
                        currentIndex: window.currentPage

                        DashboardPage { id: dashboardPage }
                        RecordingsPage { id: recordingsPage }
                        GameListPage { id: gameListPage }
                        SettingsPage { id: settingsPage }
                        InfoPage { id: infoPage }
                    }
                }
            }
        }
    }

    // --- OVERLAYS & DIALOGS ---

    // Old RecordingIndicator removed

    // Resize handle for bottom-right corner
    MouseArea {
        width: 15
        height: 15
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeFDiagCursor
        onPressed: window.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
    }

    Shortcut {
        sequence: "Ctrl+Q"
        onActivated: Qt.quit()
    }

    Shortcut {
        sequence: "F9"
        onActivated: {
            if (recordingManager) {
                if (recordingManager.isRecording) {
                    recordingManager.stopRecording()
                } else if (gameDetector && gameDetector.currentGameName !== "") {
                    recordingManager.startRecording(gameDetector.currentGameName)
                }
            }
        }
    }

    onClosing: function(close) {
        if (recordingManager && recordingManager.isRecording) {
            closeConfirmDialog.open()
            close.accepted = false
        }
    }

    Dialog {
        id: closeConfirmDialog
        title: qsTr("Recording in Progress")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Yes | Dialog.No
        Label { text: qsTr("A recording is in progress. Stop recording and exit?") }
        onAccepted: {
            recordingManager.stopRecording()
            Qt.quit()
        }
    }

    Connections {
        target: recordingManager
        function onRecordingError(error) {
            errorDialog.text = error
            errorDialog.open()
        }
    }

    Dialog {
        id: errorDialog
        title: qsTr("Error")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok
        property alias text: errorLabel.text
        Label {
            id: errorLabel
            wrapMode: Text.WordWrap
            width: 300
        }
    }

    WindowPickerDialog {
        id: windowPicker
        onWindowSelected: function(windowId, title) {
            recordingManager.startRecording(title, windowId)
        }
    }

    function openFolder(path) {
        if (!path) return
        var url = path
        if (!url.startsWith("file://")) {
            if (!url.startsWith("/")) url = "/" + url
            url = "file://" + url
        }
        Qt.openUrlExternally(url)
    }

    function confirmDelete(filePath, fileName) {
        deleteDialog.filePath = filePath
        deleteDialog.fileName = fileName
        deleteDialog.open()
    }

    Dialog {
        id: deleteDialog
        modal: true
        width: 400
        height: 220
        anchors.centerIn: parent
        
        background: Rectangle {
            color: Theme.cardColor
            radius: Theme.radiusLarge
            border.color: Theme.borderColor
            border.width: 1
            
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0.0; color: Theme.borderColor }
                GradientStop { position: 1.0; color: Theme.cardColor }
            }
        }

        header: Label {
            text: qsTr("Delete Recording")
            font.pixelSize: Theme.fontSizeLarge
            font.weight: Font.Bold
            padding: Theme.spacingLarge
            color: Theme.textColor
        }

        property string filePath: ""
        property string fileName: ""

        contentItem: ColumnLayout {
            spacing: Theme.spacingMedium
            
            Label {
                text: qsTr("Are you sure you want to delete '%1'? This action cannot be undone.").arg(deleteDialog.fileName)
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                color: Theme.textSecondaryColor
                lineHeight: 1.2
            }
        }

        footer: DialogButtonBox {
            background: Item {}
            alignment: Qt.AlignRight
            
            Button {
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
                flat: true
            }
            
            Button {
                text: qsTr("Delete")
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
                highlighted: true
                palette.button: Theme.errorColor
            }
        }

        onAccepted: {
            if (recordingLibrary && typeof storageManager !== "undefined" && storageManager) {
                storageManager.deleteRecording(filePath)
                recordingLibrary.refresh()
            }
        }
    }

    Component.onCompleted: {
        console.log("Trophy started")
    }
}
