/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../controls"
import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

Page {
    id: root

    property string statusMessage: ""

    function flash(message) {
        root.statusMessage = message
        statusTimer.restart()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacingLarge
        spacing: Theme.spacingMedium

        // Header: title, search, add
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingMedium

            Label {
                text: qsTr("Games")
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Font.Bold
                color: Theme.textColor
            }

            Item {
                Layout.fillWidth: true
            }

            TrophySearchBox {
                id: searchField

                Layout.preferredWidth: 220
                placeholderText: qsTr("Search games…")
                text: gameDetector ? gameDetector.gameFilterText : ""
                onSearchTextChanged: function(query) {
                    if (gameDetector)
                        gameDetector.gameFilterText = query
                }
            }

            TrophyButton {
                text: qsTr("Add Game")
                icon.name: "list-add"
                onClicked: addGameDialog.open()
            }

        }

        // Transient action feedback
        Label {
            Layout.fillWidth: true
            text: root.statusMessage
            color: Theme.textMutedColor
            font.pixelSize: Theme.fontSizeSmall
            visible: text !== ""
        }

        Timer {
            id: statusTimer

            interval: 4000
            repeat: false
            onTriggered: root.statusMessage = ""
        }

        // List options: count + default-exclusions toggle
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingMedium

            Label {
                text: qsTr("%n game(s)", "", gamesList.count)
                color: Theme.textMutedColor
                font.pixelSize: Theme.fontSizeSmall
            }

            Item {
                Layout.fillWidth: true
            }

            Label {
                text: qsTr("Show default exclusions")
                color: Theme.textMutedColor
                font.pixelSize: Theme.fontSizeSmall
            }

            TrophySwitch {
                checked: gameDetector ? gameDetector.showDefaultExclusions : false
                onToggled: {
                    if (gameDetector)
                        gameDetector.showDefaultExclusions = checked
                }
            }

        }

        // Unified list: Now playing / Ready to capture / Apps
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.cardColor
            radius: Theme.radiusMedium

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spacingMedium

                Label {
                    text: qsTr("Games will appear here when detected")
                    color: Theme.textMutedColor
                    Layout.alignment: Qt.AlignCenter
                    visible: gamesList.count === 0
                }

                ListView {
                    id: gamesList

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: Theme.spacingSmall
                    visible: count > 0
                    model: gameDetector ? gameDetector.gamesModel : null

                    section.property: "section"
                    section.delegate: Label {
                        width: gamesList.width
                        text: section
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.Bold
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.5
                        topPadding: 0
                        bottomPadding: Theme.spacingTiny
                        leftPadding: Theme.spacingTiny
                    }

                    delegate: GameListItem {
                        width: gamesList.width
                        entryKey: model.key || ""
                        entryDisplay: model.display || ""
                        entryRunning: model.running || false
                        entryListed: model.listed || 0
                        entrySteamAppId: model.steamAppId || 0
                        entryDefault: model.isDefault || false
                        entryHasToken: model.hasToken || false
                        entryTokenKey: model.tokenKey || ""
                        entryIconSource: model.iconSource || ""
                        entryIdentifiers: model.identifiers || []
                        onNotice: function(text) {
                            root.statusMessage = text
                            statusTimer.restart()
                        }
                    }

                }

            }

        }

    }

    // Add game: pick a running window (themed like WindowPickerDialog)
    Dialog {
        id: addGameDialog

        property var allWindows: []
        property string searchQuery: ""
        property var pickedWindow: null
        readonly property var shownWindows: allWindows.filter(function(w) {
            if (searchQuery === "")
                return true
            const q = searchQuery.toLowerCase()
            return ((w.title || "").toLowerCase().indexOf(q) !== -1) || ((w.class || "").toLowerCase().indexOf(q) !== -1)
        })

        function refreshWindows() {
            if (typeof windowService !== "undefined" && windowService)
                allWindows = windowService.getVisibleWindows()
            else
                allWindows = []
        }

        function addPicked() {
            if (pickedWindow === null)
                return
            const key = ((pickedWindow.class || pickedWindow.title) || "").trim()
            if (key === "") {
                root.flash(qsTr("That window has no usable key."))
                return
            }
            const display = displayField.text.trim() || pickedWindow.title || key
            const ok = blacklistRadio.checked ? gameDetector.addCustomGame(key, display, true) : gameDetector.addCustomGame(key, display, false)
            if (ok) {
                addGameDialog.close()
            } else {
                root.flash(qsTr("That game is already listed."))
            }
        }

        modal: true
        width: 520
        height: 600
        anchors.centerIn: Overlay.overlay
        onOpened: {
            pickedWindow = null
            searchQuery = ""
            processSearch.text = ""
            displayField.text = ""
            whitelistRadio.checked = true
            blacklistRadio.checked = false
            refreshWindows()
        }

        background: Rectangle {
            color: Theme.cardColor
            radius: Theme.radiusLarge
            border.color: Theme.withOpacity(Theme.textColor, 0.1)
            border.width: 1
        }

        header: Item {
        }

        contentItem: ColumnLayout {
            spacing: 0

            // Title + search
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                Layout.bottomMargin: Theme.spacingSmall
                spacing: Theme.spacingMedium

                Label {
                    text: qsTr("Add Game")
                    font.pixelSize: Theme.fontSizeLarge
                    font.weight: Font.Bold
                    color: Theme.textColor
                    Layout.fillWidth: true
                }

                TrophySearchBox {
                    id: processSearch

                    Layout.preferredWidth: 200
                    placeholderText: qsTr("Search processes…")
                    onSearchTextChanged: function(query) {
                        addGameDialog.searchQuery = query
                    }
                }

            }

            // Divider
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.dividerColor
            }

            // Process list
            ListView {
                id: processList

                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.spacingSmall
                clip: true
                model: addGameDialog.shownWindows
                spacing: 2

                delegate: ItemDelegate {
                    width: processList.width
                    height: 50
                    padding: 0
                    highlighted: addGameDialog.pickedWindow !== null && modelData === addGameDialog.pickedWindow
                    onClicked: {
                        addGameDialog.pickedWindow = modelData
                        displayField.text = modelData.title || modelData.class || ""
                    }

                    contentItem: RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMedium
                        anchors.rightMargin: Theme.spacingMedium
                        spacing: Theme.spacingSmall

                        Rectangle {
                            width: 36
                            height: 36
                            radius: Theme.radiusSmall
                            color: Theme.surfaceColor
                            Layout.alignment: Qt.AlignVCenter

                            Label {
                                anchors.centerIn: parent
                                text: ((modelData.title || modelData.class) || "?").charAt(0).toUpperCase()
                                font.pixelSize: Theme.fontSizeNormal
                                color: Theme.primaryColor
                            }

                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 2

                            Label {
                                text: modelData.title || qsTr("Untitled Window")
                                color: Theme.textColor
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Label {
                                text: modelData.class || ""
                                color: Theme.textMutedColor
                                font.pixelSize: Theme.fontSizeSmall
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                visible: text !== ""
                            }

                        }

                    }

                    background: Rectangle {
                        color: parent.highlighted ? Theme.withOpacity(Theme.primaryColor, 0.2) : (parent.hovered ? Theme.cardHoverColor : "transparent")
                        radius: Theme.radiusSmall
                    }

                }

                ScrollIndicator.vertical: ScrollIndicator {
                }

            }

            Label {
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                visible: processList.count === 0
                text: (typeof windowService !== "undefined" && windowService) ? qsTr("No running windows found.") : qsTr("Window tracking is off — enable it in Settings to pick a process.")
                color: Theme.textMutedColor
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }

            // Divider
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Theme.dividerColor
            }

            // Display name + Watch/Block + actions
            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                Layout.topMargin: Theme.spacingMedium
                Layout.bottomMargin: Theme.spacingMedium
                spacing: Theme.spacingSmall

                TrophyTextField {
                    id: displayField

                    Layout.fillWidth: true
                    placeholderText: qsTr("Display Name (optional)")
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacingMedium

                    TrophyRadioButton {
                        id: whitelistRadio

                        text: qsTr("Watch")
                        checked: true
                        onClicked: {
                            whitelistRadio.checked = true
                            blacklistRadio.checked = false
                        }
                    }

                    TrophyRadioButton {
                        id: blacklistRadio

                        text: qsTr("Block")
                        checked: false
                        onClicked: {
                            whitelistRadio.checked = false
                            blacklistRadio.checked = true
                        }
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    TrophyButton {
                        text: qsTr("Cancel")
                        onClicked: addGameDialog.close()
                    }

                    TrophyButton {
                        text: qsTr("Add")
                        enabled: addGameDialog.pickedWindow !== null
                        onClicked: addGameDialog.addPicked()
                    }

                }

            }

        }

    }

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: 12
        topLeftRadius: 26
    }

    component TwoPoleSwitch: Rectangle {
        id: duoSwitch

        // false = Prompt (detection decides, portal asks), true = Block.
        property bool blocked: false

        signal promptSelected()
        signal blockSelected()

        width: 148
        height: 30
        radius: 6
        color: Theme.surfaceColor
        border.color: Theme.textMutedColor
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.margins: 2
            spacing: 2

            Repeater {
                model: [
                    { blocked: false, label: qsTr("Prompt") },
                    { blocked: true, label: qsTr("Block") },
                ]

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 4
                    color: {
                        const active = (duoSwitch.blocked === modelData.blocked)
                        if (!active)
                            return "transparent"
                        return modelData.blocked ? Qt.alpha(Theme.errorColor, 0.25)
                                                 : Qt.alpha(Theme.successColor, 0.15)
                    }

                    Label {
                        anchors.centerIn: parent
                        text: modelData.label
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: duoSwitch.blocked === modelData.blocked ? Font.Bold : Font.Normal
                        color: {
                            if (duoSwitch.blocked !== modelData.blocked)
                                return Theme.textMutedColor
                            return modelData.blocked ? Theme.errorColor : Theme.successColor
                        }
                    }

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: {
                            if (modelData.blocked)
                                duoSwitch.blockSelected()
                            else
                                duoSwitch.promptSelected()
                        }
                    }

                }

            }

        }

    }

    component GameListItem: Item {
        id: gameItem

        property string entryKey: ""
        property string entryDisplay: ""
        property bool entryRunning: false
        property int entryListed: 0
        property int entrySteamAppId: 0
        property bool entryDefault: false
        property bool entryHasToken: false
        property string entryTokenKey: ""
        property url entryIconSource: ""
        property var entryIdentifiers: []

        signal notice(string text)

        height: 60

        // Backdrop: plain rows stay flat on the card, the running game
        // gets a green-tinted lift (drawn by the effect below).
        Rectangle {
            id: rowCard

            anchors.fill: parent
            radius: Theme.radiusSmall
            color: gameItem.entryRunning ? Theme.withOpacity(Theme.successColor, 0.1) : "transparent"
            border.color: gameItem.entryRunning ? Theme.successColor : "transparent"
            border.width: gameItem.entryRunning ? 1 : 0
            visible: false
        }

        MultiEffect {
            anchors.fill: rowCard
            source: rowCard
            visible: gameItem.entryRunning
            shadowEnabled: true
            shadowBlur: 0.4
            shadowColor: "#70000000"
            shadowVerticalOffset: 2
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spacingSmall
            anchors.rightMargin: Theme.spacingSmall
            spacing: Theme.spacingMedium

            Rectangle {
                width: 40
                height: 40
                radius: Theme.radiusSmall
                color: Theme.surfaceColor
                Layout.alignment: Qt.AlignVCenter

                Label {
                    anchors.centerIn: parent
                    text: gameItem.entryDisplay.charAt(0).toUpperCase()
                    objectName: "gameIconFallback"
                    visible: gameIcon.status !== Image.Ready
                    font.pixelSize: Theme.fontSizeLarge
                    color: gameItem.entryRunning ? Theme.successColor : Theme.primaryColor
                }

                Image {
                    id: gameIcon
                    objectName: "gameIcon"
                    anchors.fill: parent
                    anchors.margins: 2
                    source: gameItem.entryIconSource
                    sourceSize: Qt.size(40, 40)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    visible: status === Image.Ready
                }

            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                spacing: 3

                Label {
                    text: gameItem.entryDisplay || gameItem.entryKey
                    color: Theme.textColor
                    font.pixelSize: Theme.fontSizeNormal
                    font.weight: gameItem.entryRunning ? Font.Bold : Font.Normal
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Label {
                    objectName: "gameEntrySubtitle"
                    text: {
                        if (gameItem.entryDefault) {
                            const count = gameItem.entryIdentifiers.length
                            return count === 1 ? qsTr("Default exclusion · 1 identifier")
                                               : qsTr("Default exclusion · %1 identifiers").arg(count)
                        }
                        let sub = gameItem.entryKey
                        if (gameItem.entrySteamAppId > 0 && gameItem.entryKey.indexOf("steam_app_") !== 0)
                            sub += " • AppID " + gameItem.entrySteamAppId
                        return sub
                    }
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    visible: gameItem.entryDefault || (text !== "" && text !== gameItem.entryDisplay)
                    ToolTip.visible: gameItem.entryDefault && identifierHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: gameItem.entryIdentifiers.join("\n")

                    HoverHandler {
                        id: identifierHover
                    }
                }

            }

            RowLayout {
                Layout.alignment: Qt.AlignVCenter
                spacing: Theme.spacingSmall

                Rectangle {
                    visible: gameItem.entryRunning
                    color: Theme.successColor
                    radius: 3
                    Layout.preferredWidth: liveLabel.width + 12
                    Layout.preferredHeight: liveLabel.height + 6

                    Label {
                        id: liveLabel

                        anchors.centerIn: parent
                        text: qsTr("LIVE")
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.Bold
                        color: "black"
                    }

                }

                Rectangle {
                    visible: gameItem.entryHasToken
                    color: "transparent"
                    border.color: Theme.successColor
                    border.width: 1
                    radius: 3
                    Layout.preferredWidth: readyLabel.width + 12
                    Layout.preferredHeight: readyLabel.height + 6

                    Label {
                        id: readyLabel

                        anchors.centerIn: parent
                        text: qsTr("Ready")
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.successColor
                    }

                    HoverHandler {
                        id: readyHover
                    }

                    ToolTip.text: qsTr("Restore token saved — captures start silently, no portal prompt")
                    ToolTip.visible: readyHover.hovered
                    ToolTip.delay: 500
                }

                TrophyButton {
                    visible: gameItem.entryHasToken
                    text: qsTr("Withdraw")
                    onClicked: {
                        const tokenKey = entryTokenKey, name = entryDisplay, running = entryRunning
                        settingsManager.removeRestoreToken(tokenKey)
                        if (running)
                            gameItem.notice(qsTr("Silent capture withdrawn for %1 — takes effect the next time it launches.").arg(name))
                        else
                            gameItem.notice(qsTr("Silent capture withdrawn for %1 — back to prompt.").arg(name))
                    }
                }

                TwoPoleSwitch {
                    visible: !gameItem.entryHasToken
                    blocked: gameItem.entryListed === 2
                    onPromptSelected: {
                        const key = entryKey, name = entryDisplay
                        if (gameDetector.unblockGame(key))
                            gameItem.notice(qsTr("%1 will prompt on detection.").arg(name))
                        else
                            gameItem.notice(qsTr("%1 already prompts.").arg(name))
                    }
                    onBlockSelected: {
                        const key = entryKey, name = entryDisplay
                        if (gameDetector.moveToBlacklist(key))
                            gameItem.notice(qsTr("Blocked %1.").arg(name))
                        else
                            gameItem.notice(qsTr("%1 is already blocked.").arg(name))
                    }
                }

                TrophyButton {
                    visible: !gameItem.entryHasToken
                    text: qsTr("×")
                    implicitWidth: 30
                    onClicked: {
                        const key = entryKey, name = entryDisplay
                        if (gameDetector.forgetGame(key))
                            gameItem.notice(qsTr("Removed %1.").arg(name))
                        else
                            gameItem.notice(qsTr("Nothing to remove."))
                    }
                }

            }

        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 52
            height: 1
            color: Theme.textMutedColor
            opacity: 0.15
        }

    }

}
