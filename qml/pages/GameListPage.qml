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

Page {
    id: root

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        spacing: Theme.spacingMedium

        // Header
        RowLayout {
            Layout.fillWidth: true

            Label {
                text: qsTr("Games")
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Font.Bold
                color: Theme.textColor
            }

            Item {
                Layout.fillWidth: true
            }

            Button {
                text: qsTr("Add Game")
                icon.name: "list-add"
                onClicked: addGameDialog.open()
            }

        }

        // Tabs
        TabBar {
            id: tabBar

            Layout.fillWidth: true

            TabButton {
                text: qsTr("Detected (%1)").arg(0)
            }

            TabButton {
                text: qsTr("Whitelist (%1)").arg(0)
            }

            TabButton {
                text: qsTr("Blacklist (%1)").arg(0)
            }

        }

        // Content
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabBar.currentIndex

            // Detected games
            Rectangle {
                color: Theme.cardColor
                radius: Theme.radiusMedium

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium

                    Label {
                        text: qsTr("Games will appear here when detected")
                        color: Theme.textMutedColor
                        Layout.alignment: Qt.AlignCenter
                        visible: detectedList.count === 0
                    }

                    ListView {
                        id: detectedList

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: Theme.spacingSmall
                        model: [] // TODO: Connect to gameDetector

                        delegate: GameListItem {
                            width: detectedList.width
                            gameName: modelData.name || ""
                            windowClass: modelData.windowClass || ""
                            isWhitelisted: false
                            isBlacklisted: false
                            onAddToWhitelist: gameDetector.addToWhitelist(windowClass, gameName)
                            onAddToBlacklist: gameDetector.addToBlacklist(windowClass)
                        }

                    }

                }

            }

            // Whitelist
            Rectangle {
                color: Theme.cardColor
                radius: Theme.radiusMedium

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium

                    Label {
                        text: qsTr("No games in whitelist")
                        color: Theme.textMutedColor
                        Layout.alignment: Qt.AlignCenter
                        visible: whitelistView.count === 0
                    }

                    ListView {
                        id: whitelistView

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: Theme.spacingSmall
                        model: [] // TODO: Connect to gameWhitelist

                        delegate: GameListItem {
                            width: whitelistView.width
                            gameName: modelData.name || ""
                            windowClass: modelData.windowClass || ""
                            isWhitelisted: true
                            isBlacklisted: false
                            onRemove: gameDetector.removeFromWhitelist(windowClass)
                        }

                    }

                }

            }

            // Blacklist
            Rectangle {
                color: Theme.cardColor
                radius: Theme.radiusMedium

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium

                    Label {
                        text: qsTr("No games in blacklist")
                        color: Theme.textMutedColor
                        Layout.alignment: Qt.AlignCenter
                        visible: blacklistView.count === 0
                    }

                    ListView {
                        id: blacklistView

                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: Theme.spacingSmall
                        model: [] // TODO: Connect to gameBlacklist

                        delegate: GameListItem {
                            width: blacklistView.width
                            gameName: modelData.name || ""
                            windowClass: modelData.windowClass || ""
                            isWhitelisted: false
                            isBlacklisted: true
                            onRemove: gameDetector.removeFromBlacklist(windowClass)
                        }

                    }

                }

            }

        }

    }

    // Add game dialog
    Dialog {
        id: addGameDialog

        title: qsTr("Add Game")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: {
            if (gameNameField.text.trim() !== "") {
                if (whitelistRadio.checked)
                    gameDetector.addToWhitelist(gameNameField.text, displayNameField.text);
                else
                    gameDetector.addToBlacklist(gameNameField.text);
                gameNameField.text = "";
                displayNameField.text = "";
            }
        }

        ColumnLayout {
            spacing: Theme.spacingMedium

            Label {
                text: qsTr("Window Class or Name")
                color: Theme.textColor
            }

            TextField {
                id: gameNameField

                Layout.preferredWidth: 300
                placeholderText: qsTr("e.g., steam_app_12345")
            }

            Label {
                text: qsTr("Display Name (optional)")
                color: Theme.textColor
            }

            TextField {
                id: displayNameField

                Layout.preferredWidth: 300
                placeholderText: qsTr("e.g., My Game")
            }

            RowLayout {
                Layout.fillWidth: true

                RadioButton {
                    id: whitelistRadio

                    text: qsTr("Add to Whitelist")
                    checked: true
                }

                RadioButton {
                    text: qsTr("Add to Blacklist")
                }

            }

        }

    }

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: 12
        topLeftRadius: 26
    }

    component GameListItem: Rectangle {
        id: gameItem

        property string gameName: ""
        property string windowClass: ""
        property bool isWhitelisted: false
        property bool isBlacklisted: false

        signal addToWhitelist()
        signal addToBlacklist()
        signal remove()

        height: 56
        radius: Theme.radiusSmall
        color: mouseArea.containsMouse ? Theme.cardHoverColor : "transparent"

        MouseArea {
            id: mouseArea

            anchors.fill: parent
            hoverEnabled: true
        }

        RowLayout {
            anchors.fill: parent
            anchors.margins: Theme.spacingSmall
            spacing: Theme.spacingMedium

            Rectangle {
                width: 40
                height: 40
                radius: Theme.radiusSmall
                color: Theme.surfaceColor

                Label {
                    anchors.centerIn: parent
                    text: gameItem.gameName.charAt(0).toUpperCase()
                    font.pixelSize: Theme.fontSizeLarge
                    color: Theme.primaryColor
                }

            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: gameItem.gameName || gameItem.windowClass
                    color: Theme.textColor
                    font.pixelSize: Theme.fontSizeNormal
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Label {
                    text: gameItem.windowClass
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    visible: gameItem.gameName !== "" && gameItem.gameName !== gameItem.windowClass
                }

            }

            Row {
                spacing: Theme.spacingSmall
                visible: mouseArea.containsMouse

                Button {
                    visible: !gameItem.isWhitelisted && !gameItem.isBlacklisted
                    text: qsTr("Whitelist")
                    flat: true
                    onClicked: gameItem.addToWhitelist()
                }

                Button {
                    visible: !gameItem.isWhitelisted && !gameItem.isBlacklisted
                    text: qsTr("Blacklist")
                    flat: true
                    onClicked: gameItem.addToBlacklist()
                }

                Button {
                    visible: gameItem.isWhitelisted || gameItem.isBlacklisted
                    text: qsTr("Remove")
                    flat: true
                    onClicked: gameItem.remove()
                }

            }

        }

    }

}
