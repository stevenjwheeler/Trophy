/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

pragma ComponentBehavior: Bound
import "../controls"
import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root
    // Explicit dependency; this default preserves engine/creation-context hosts.
    property var detector: typeof gameDetector !== "undefined" ? gameDetector : null // qmllint disable unqualified
    property var preferences: typeof settingsManager !== "undefined" ? settingsManager : null // qmllint disable unqualified
    readonly property int statusColumnWidth: 150
    readonly property int actionsColumnWidth: 186
    readonly property var gamesModel: root.detector ? root.detector.gamesModel : null
    signal policyNoticeRequested(string key, string heading, string detail)
    // Run policy changes from the page context: sorting may destroy the pressed
    // row synchronously, but the explanation still needs to be delivered.
    function promptGame(key, blockKey, name) {
        if (!detector)
            return
        detector.unblockGame(key)
        if (blockKey && blockKey !== key)
            detector.unblockGame(blockKey)
        policyNoticeRequested("game-policy:" + key, qsTr("Prompt · %1").arg(name),
            qsTr("Launch the game, then request capture to approve it. This does not start recording."))
    }
    function denyGame(key, blockKey, name) {
        if (!detector)
            return
        detector.moveToBlacklist(blockKey || key)
        policyNoticeRequested("game-policy:" + key, qsTr("Deny · %1").arg(name),
            qsTr("Automatic capture is denied. The game and launcher files are unchanged."))
    }
    function withdrawApproval(key, tokenKey, name) {
        if (!preferences)
            return
        preferences.removeRestoreToken(tokenKey)
        policyNoticeRequested("game-policy:" + key, qsTr("Approval revoked · %1").arg(name),
            qsTr("Saved capture approval was removed. A future capture may ask for approval again."))
    }
    function sectionCollapsed(section) {
        return gamesModel && gamesModel.collapsedSections !== undefined
            && gamesModel.collapsedSections.indexOf(section) >= 0
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
                text: root.detector ? root.detector.gameFilterText : ""
                onSearchTextChanged: function(query) {
                    if (root.detector)
                        root.detector.gameFilterText = query
                }
            }

            TrophyButton {
                text: qsTr("Add Game")
                icon.name: "list-add"
                onClicked: addGameDialog.open()
            }

        }

        RowLayout {
            visible: root.detector && root.detector.instances && root.detector.instances.length > 1
            Layout.fillWidth: true
            Label {
                text: qsTr("Capture target")
                color: Theme.textSecondaryColor
            }
            TrophyComboBox {
                Layout.fillWidth: true
                model: root.detector && root.detector.instances ? root.detector.instances : []
                textRole: "label"
                currentIndex: {
                    if (!root.detector || !root.detector.instances)
                        return -1
                    for (let i = 0; i < root.detector.instances.length; ++i)
                        if (root.detector.instances[i].selected)
                            return i
                    return -1
                }
                onActivated: function(index) {
                    if (root.detector)
                        root.detector.selectInstance(root.detector.instances[index].id)
                }
            }
        }

        // List options: count + default-exclusions toggle
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingMedium

            Label {
                text: qsTr("%n game(s)", "", root.gamesModel && root.gamesModel.matchingGameCount !== undefined
                    ? root.gamesModel.matchingGameCount : gamesList.count)
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
                checked: root.detector ? root.detector.showDefaultExclusions : false
                onToggled: {
                    if (root.detector)
                        root.detector.showDefaultExclusions = checked
                }
            }

        }

        // Installed launcher libraries, custom games and optional exclusions.
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.cardColor
            radius: Theme.radiusSmall
            border.width: 1
            border.color: Theme.borderColor

            ColumnLayout {
                id: gamesContent
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 12
                    Layout.rightMargin: 12 + gamesScrollBar.width + Theme.spacingSmall
                    Layout.preferredHeight: 36
                    spacing: 12
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Game")
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    Label {
                        objectName: "gamesStatusColumnHeader"
                        Layout.preferredWidth: root.statusColumnWidth
                        text: qsTr("Status")
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    Label {
                        objectName: "gamesActionsColumnHeader"
                        Layout.preferredWidth: root.actionsColumnWidth
                        text: qsTr("Actions")
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color: Theme.borderColor
                }

                Label {
                    text: searchField.text ? qsTr("No games match your search")
                        : qsTr("Add a running game or configure launcher libraries in Settings")
                    color: Theme.textMutedColor
                    Layout.alignment: Qt.AlignCenter
                    Layout.fillHeight: true
                    verticalAlignment: Text.AlignVCenter
                    visible: gamesList.count === 0
                }

                ListView {
                    id: gamesList
                    objectName: "gamesList"

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 0
                    visible: count > 0
                    model: root.detector ? root.detector.gamesModel : null
                    ScrollBar.vertical: TrophyScrollBar { id: gamesScrollBar }

                    section.property: "section"
                    section.delegate: ItemDelegate {
                        id: groupHeader
                        required property string section
                        objectName: "gamesSection_" + section
                        readonly property bool collapsed: root.sectionCollapsed(section)
                        width: gamesList.width - gamesScrollBar.width - Theme.spacingSmall
                        height: 36
                        leftPadding: 12
                        rightPadding: 12
                        Accessible.name: section
                        Accessible.description: collapsed ? qsTr("Collapsed") : qsTr("Expanded")
                        onClicked: {
                            if (root.gamesModel && typeof root.gamesModel.toggleSection === "function")
                                root.gamesModel.toggleSection(section)
                        }
                        background: Rectangle {
                            color: groupHeader.hovered ? Theme.withOpacity(Theme.textColor, 0.05) : Theme.surfaceColor
                        }
                        contentItem: RowLayout {
                            spacing: 8
                            Image {
                                readonly property string launcher: groupHeader.section.toLowerCase()
                                visible: ["steam", "heroic", "legendary", "nile", "lutris", "faugus"].indexOf(launcher) >= 0
                                Layout.preferredWidth: 20
                                Layout.preferredHeight: 20
                                sourceSize: Qt.size(40, 40)
                                fillMode: Image.PreserveAspectFit
                                source: !visible ? "" : launcher === "steam" ? "qrc:/icons/default-apps/steam.svg"
                                    : "qrc:/icons/launchers/" + launcher + (launcher === "legendary" ? ".ico" : ".svg")
                            }
                            Label {
                                objectName: "gamesSectionHeading"
                                text: {
                                    const counts = root.gamesModel ? root.gamesModel.sectionCounts : null
                                    return counts !== undefined && counts !== null
                                        ? qsTr("%1 (%2)").arg(groupHeader.section).arg(counts[groupHeader.section] || 0)
                                        : groupHeader.section
                                }
                                color: Theme.textMutedColor
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Font.Bold
                                font.capitalization: Font.AllUppercase
                                font.letterSpacing: 0.5
                                Layout.fillWidth: true
                            }
                            Label {
                                text: groupHeader.collapsed ? "›" : "⌄"
                                color: Theme.textMutedColor
                                font.pixelSize: Theme.fontSizeLarge
                            }
                        }
                    }

                    delegate: GameListItem {
                        required property var model
                        required property int index
                        readonly property bool collapsed: root.sectionCollapsed(model.section || "")
                        height: collapsed ? 0 : 52
                        visible: !collapsed
                        rowIndex: index
                        width: gamesList.width - gamesScrollBar.width - Theme.spacingSmall
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
                        entryLibrary: model.library || ""
                        entryBlockKey: model.blockKey || ""
                        entrySupportTool: model.supportTool || false
                    }

                }

            }

            Rectangle {
                anchors.left: gamesContent.left
                anchors.right: gamesContent.right
                anchors.bottom: gamesContent.bottom
                height: Theme.spacingLarge * 2
                opacity: Math.max(0, Math.min(1, (gamesList.originY + gamesList.contentHeight - gamesList.height - gamesList.contentY) / height))
                visible: gamesList.visible && opacity > 0
                gradient: Gradient {
                    GradientStop { position: 0; color: Qt.alpha(Theme.cardColor, 0) }
                    GradientStop { position: 1; color: Theme.cardColor }
                }
            }

        }

    }

    // Add game: use the cached /proc census, independent of window systems.
    Dialog {
        id: addGameDialog
        objectName: "addGameDialog"

        property var allProcesses: []
        property string searchQuery: ""
        property var pickedProcess: null
        property string errorMessage: ""
        property bool showAllProcesses: false
        readonly property var shownProcesses: allProcesses.filter(function(w) {
            if (!showAllProcesses && w.likelyApplication !== true)
                return false
            if (searchQuery === "")
                return true
            const q = searchQuery.toLowerCase()
            return ((w.title || "").toLowerCase().indexOf(q) !== -1) || ((w.path || "").toLowerCase().indexOf(q) !== -1) || String(w.pid).indexOf(q) !== -1
        })
        onShownProcessesChanged: {
            if (pickedProcess !== null && shownProcesses.indexOf(pickedProcess) === -1)
                pickedProcess = null
        }

        function refreshProcesses() {
            if (typeof gameDetector !== "undefined" && gameDetector && typeof gameDetector.runningProcesses === "function")
                allProcesses = gameDetector.runningProcesses()
            else
                allProcesses = []
        }

        function addPicked() {
            if (pickedProcess === null)
                return
            const display = displayField.text.trim()
            const ok = gameDetector.addProcessGame(pickedProcess.pid, pickedProcess.identity, display || pickedProcess.title, blacklistRadio.checked)
            if (ok) {
                addGameDialog.close()
            } else {
                errorMessage = qsTr("That process exited, changed, or is already listed. Refresh and try again.")
                refreshProcesses()
                pickedProcess = null
            }
        }

        modal: true
        width: 520
        height: 600
        anchors.centerIn: Overlay.overlay
        onOpened: {
            errorMessage = ""
            pickedProcess = null
            searchQuery = ""
            showAllProcesses = false
            processSearch.text = ""
            displayField.text = ""
            whitelistRadio.checked = true
            blacklistRadio.checked = false
            refreshProcesses()
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

            Label {
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                text: addGameDialog.errorMessage
                visible: text !== ""
                color: Theme.errorColor
                wrapMode: Text.WordWrap
            }

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

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                Layout.bottomMargin: Theme.spacingSmall
                spacing: Theme.spacingSmall

                Label {
                    Layout.fillWidth: true
                    text: addGameDialog.showAllProcesses ? qsTr("All processes") : qsTr("Likely applications")
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                }
                Label {
                    text: qsTr("Show all processes")
                    color: Theme.textColor
                    font.pixelSize: Theme.fontSizeSmall
                }
                TrophySwitch {
                    objectName: "showAllProcessesSwitch"
                    checked: addGameDialog.showAllProcesses
                    Accessible.name: qsTr("Show all processes")
                    onToggled: addGameDialog.showAllProcesses = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Suggestions are not a window list. Show all processes to find unrecognized apps and games.")
                    ToolTip.delay: 600
                }
            }

            // Divider
            Rectangle {
                objectName: "gamePickerTopDivider"
                Layout.fillWidth: true
                implicitHeight: 1
                color: Theme.dividerColor
            }

            // Process list
            ListView {
                id: processList

                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.spacingSmall
                clip: true
                model: addGameDialog.shownProcesses
                spacing: 2

                delegate: ItemDelegate {
                    id: processDelegate
                    required property var modelData
                    width: processList.width
                    height: 50
                    padding: 0
                    highlighted: addGameDialog.pickedProcess !== null && modelData === addGameDialog.pickedProcess
                    onClicked: {
                        addGameDialog.pickedProcess = modelData
                        displayField.text = modelData.title || ""
                    }

                    contentItem: RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMedium
                        anchors.rightMargin: Theme.spacingMedium
                        spacing: Theme.spacingSmall

                        Rectangle {
                            objectName: "pickerIconTile"
                            Layout.preferredWidth: 36
                            Layout.preferredHeight: 36
                            radius: Theme.radiusSmall
                            color: Theme.surfaceColor
                            Layout.alignment: Qt.AlignVCenter

                            Label {
                                anchors.centerIn: parent
                                objectName: "pickerIconFallback"
                                visible: !pickerIcon.hasArtwork
                                text: (modelData.title || "?").charAt(0).toUpperCase()
                                font.pixelSize: Theme.fontSizeNormal
                                color: Theme.primaryColor
                            }

                            Image {
                                id: pickerIcon
                                readonly property bool hasArtwork: status === Image.Ready
                                    && implicitWidth > 1 && implicitHeight > 1
                                objectName: "pickerIcon"
                                anchors.fill: parent
                                anchors.margins: 2
                                source: modelData.iconSource || ""
                                sourceSize: Qt.size(36, 36)
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                                visible: hasArtwork
                            }

                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 2

                            Label {
                                text: modelData.title || qsTr("Unknown process")
                                color: Theme.textColor
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }

                            Label {
                                text: qsTr("PID %1 · %2").arg(modelData.pid).arg(modelData.path || "")
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

                ScrollBar.vertical: TrophyScrollBar {}

            }

            Label {
                Layout.fillWidth: true
                Layout.margins: Theme.spacingLarge
                visible: processList.count === 0
                text: addGameDialog.showAllProcesses
                    ? qsTr("No matching processes found. Start the game and refresh.")
                    : qsTr("No likely applications found. Try Show all processes, or start the game and refresh.")
                color: Theme.textMutedColor
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }

            // Divider
            Rectangle {
                objectName: "gamePickerBottomDivider"
                Layout.fillWidth: true
                implicitHeight: 1
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
                        objectName: "watchProcessRadio"

                        text: qsTr("Watch")
                        checked: true
                        onClicked: {
                            whitelistRadio.checked = true
                            blacklistRadio.checked = false
                        }
                    }

                    TrophyRadioButton {
                        id: blacklistRadio
                        objectName: "blockProcessRadio"

                        text: qsTr("Deny")
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
                        text: qsTr("Refresh")
                        onClicked: {
                            addGameDialog.pickedProcess = null
                            addGameDialog.refreshProcesses()
                        }
                    }

                    TrophyButton {
                        text: qsTr("Cancel")
                        onClicked: addGameDialog.close()
                    }

                    TrophyButton {
                        text: qsTr("Add")
                        enabled: addGameDialog.pickedProcess !== null
                        onClicked: addGameDialog.addPicked()
                    }

                }

            }

        }

    }

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: Theme.windowRadius
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
                    { blocked: true, label: qsTr("Deny") },
                ]

                Rectangle {
                    id: pole
                    required property var modelData
                    objectName: modelData.blocked ? "blockPole" : "promptPole"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 4
                    color: {
                        const active = (duoSwitch.blocked === pole.modelData.blocked)
                        if (!active)
                            return "transparent"
                        return pole.modelData.blocked ? Qt.alpha(Theme.errorColor, 0.25)
                                                 : Qt.alpha(Theme.warningColor, 0.15)
                    }

                    Label {
                        anchors.centerIn: parent
                        text: pole.modelData.label
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: duoSwitch.blocked === pole.modelData.blocked ? Font.Bold : Font.Normal
                        color: {
                            if (duoSwitch.blocked !== pole.modelData.blocked)
                                return Theme.textMutedColor
                            return pole.modelData.blocked ? Theme.errorColor : Theme.warningColor
                        }
                    }

                    HoverHandler {
                        id: poleHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    ToolTip.visible: poleHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: pole.modelData.blocked
                        ? qsTr("Prevent automatic capture of this game. Launcher files are unchanged.")
                        : qsTr("Allow Trophy to request capture approval. This does not approve or start recording.")
                    TapHandler {
                        onTapped: {
                            if (pole.modelData.blocked)
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
        property string entryLibrary: ""
        property string entryBlockKey: ""
        property bool entrySupportTool: false
        property int rowIndex: 0
        readonly property string identifierDetails: {
            if (entryDefault)
                return entryIdentifiers.join("\n")
            return entryKey + (entrySteamAppId > 0 ? "\nSteam App ID: " + entrySteamAppId : "")
        }
        readonly property string statusText: {
            if (entrySupportTool)
                return qsTr("Excluded")
            if (entryListed === 2)
                return qsTr("Denied")
            if (!entryHasToken)
                return entryRunning ? qsTr("Waiting for approval") : qsTr("Waiting for launch")
            return entryRunning ? qsTr("Active") : qsTr("Ready to capture")
        }
        readonly property color statusColor: entrySupportTool ? Theme.textMutedColor
            : entryListed === 2 ? Theme.errorColor
            : entryHasToken ? Theme.successColor : Theme.warningColor

        height: 52
        Rectangle {
            anchors.fill: parent
            color: gameItem.entryRunning ? Theme.withOpacity(gameItem.statusColor, 0.06)
                : gameItem.rowIndex % 2 === 0 ? Theme.withOpacity(Theme.textColor, 0.025) : "transparent"
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 12

            Rectangle {
                objectName: "gameIconTile"
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                radius: Theme.radiusSmall
                color: Theme.surfaceColor
                Layout.alignment: Qt.AlignVCenter

                Label {
                    anchors.centerIn: parent
                    text: gameItem.entryDisplay.charAt(0).toUpperCase()
                    objectName: "gameIconFallback"
                    visible: !gameIcon.hasArtwork
                    font.pixelSize: Theme.fontSizeLarge
                    color: gameItem.entryRunning ? gameItem.statusColor : Theme.primaryColor
                }

                Image {
                    id: gameIcon
                    readonly property bool hasArtwork: status === Image.Ready
                        && implicitWidth > 1 && implicitHeight > 1
                    objectName: "gameIcon"
                    anchors.fill: parent
                    anchors.margins: 2
                    source: gameItem.entryIconSource
                    sourceSize: Qt.size(40, 40)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    visible: hasArtwork
                }

            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.alignment: Qt.AlignVCenter
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                spacing: 3

                Label {
                    objectName: "gameEntryName"
                    text: gameItem.entryDisplay || gameItem.entryKey
                    color: Theme.textColor
                    font.pixelSize: Theme.fontSizeNormal
                    font.weight: gameItem.entryRunning ? Font.Bold : Font.Medium
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    HoverHandler { id: nameHover }
                    ToolTip.visible: nameHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: (gameItem.entryDisplay || gameItem.entryKey) + "\n" + gameItem.identifierDetails
                        + (gameItem.entryLibrary ? "\n" + gameItem.entryLibrary : "")
                }

                Label {
                    objectName: "gameEntrySubtitle"
                    text: {
                        if (gameItem.entrySupportTool)
                            return gameItem.entrySteamAppId > 0
                                ? qsTr("Steam compatibility / support tool") : qsTr("Game installer or support helper")
                        if (gameItem.entryDefault) {
                            const count = gameItem.entryIdentifiers.length
                            return count === 1 ? qsTr("Default exclusion · 1 identifier")
                                               : qsTr("Default exclusion · %1 identifiers").arg(count)
                        }
                        return ""
                    }
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    visible: gameItem.entryDefault
                    ToolTip.visible: gameItem.entryDefault && identifierHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: gameItem.entryIdentifiers.join("\n")

                    HoverHandler {
                        id: identifierHover
                    }
                }

            }

            RowLayout {
                objectName: "gameEntryStatus"
                Layout.preferredWidth: root.statusColumnWidth
                Layout.minimumWidth: root.statusColumnWidth
                Layout.maximumWidth: root.statusColumnWidth
                Layout.alignment: Qt.AlignVCenter
                spacing: 8
                Rectangle {
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    radius: 4
                    color: gameItem.entryHasToken || gameItem.entryRunning || gameItem.entryListed === 2
                        ? gameItem.statusColor : "transparent"
                    border.width: color.a === 0 ? 1 : 0
                    border.color: gameItem.statusColor
                }
                Label {
                    objectName: "gameEntryStatusText"
                    Layout.fillWidth: true
                    text: gameItem.statusText
                    font.pixelSize: Theme.fontSizeSmall
                    color: gameItem.statusColor
                    elide: Text.ElideRight
                    HoverHandler { id: statusHover }
                    ToolTip.text: gameItem.entrySupportTool ? qsTr("Support tools are excluded from game detection")
                        : gameItem.entryListed === 2 ? qsTr("Automatic capture denied")
                        : gameItem.entryHasToken ? qsTr("Capture approval saved; the desktop portal may still require authorization")
                        : qsTr("No saved capture approval; the desktop portal will ask for authorization")
                    ToolTip.visible: statusHover.hovered
                    ToolTip.delay: 500
                }
            }

            RowLayout {
                objectName: "gameEntryActions"
                Layout.preferredWidth: root.actionsColumnWidth
                Layout.minimumWidth: root.actionsColumnWidth
                Layout.maximumWidth: root.actionsColumnWidth
                Layout.alignment: Qt.AlignVCenter
                spacing: 8
                TrophyButton {
                    objectName: "withdrawGameApprovalButton"
                    visible: gameItem.entryHasToken && !gameItem.entryRunning && !gameItem.entrySupportTool
                    Layout.fillWidth: true
                    implicitHeight: 32
                    text: qsTr("Revoke")
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Forget saved capture approval. Future capture may ask for approval again.")
                    onClicked: {
                        root.withdrawApproval(gameItem.entryKey, gameItem.entryTokenKey,
                            gameItem.entryDisplay || gameItem.entryKey)
                    }
                }

                TwoPoleSwitch {
                    visible: !gameItem.entryHasToken && !gameItem.entrySupportTool && !gameItem.entryRunning
                    Layout.preferredWidth: gameItem.entryLibrary ? root.actionsColumnWidth : 148
                    Layout.minimumWidth: Layout.preferredWidth
                    Layout.maximumWidth: Layout.preferredWidth
                    blocked: gameItem.entryListed === 2
                    onPromptSelected: {
                        root.promptGame(gameItem.entryKey, gameItem.entryBlockKey,
                            gameItem.entryDisplay || gameItem.entryKey)
                    }
                    onBlockSelected: {
                        root.denyGame(gameItem.entryKey, gameItem.entryBlockKey,
                            gameItem.entryDisplay || gameItem.entryKey)
                    }
                }

                TrophyButton {
                    objectName: "forgetGameButton"
                    visible: !gameItem.entryHasToken && !gameItem.entryLibrary && !gameItem.entrySupportTool && !gameItem.entryRunning
                    text: qsTr("×")
                    implicitWidth: 30
                    Layout.preferredWidth: 30
                    Layout.minimumWidth: 30
                    Layout.maximumWidth: 30
                    implicitHeight: 32
                    Accessible.name: qsTr("Forget %1").arg(gameItem.entryDisplay || gameItem.entryKey)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Remove this game rule")
                    onClicked: {
                        gameDetector.forgetGame(gameItem.entryKey)
                    }
                }

            }

        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.borderColor
        }

    }

}
