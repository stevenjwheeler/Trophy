/* SPDX-License-Identifier: GPL-3.0-or-later */
import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root
    objectName: "gameLibrariesTable"
    property var detector: null
    signal addLocationRequested(string launcherId)
    readonly property int statusWidth: 130
    readonly property int gamesWidth: 84
    readonly property int locationsWidth: 88
    spacing: Theme.spacingSmall

    TrophyButton {
        objectName: "rescanGameLibrariesButton"
        text: root.detector && root.detector.librariesScanning ? qsTr("Scanning…") : qsTr("Rescan now")
        icon.source: "qrc:/icons/refresh.svg"
        icon.width: 16
        icon.height: 16
        enabled: root.detector && !root.detector.librariesScanning
        onClicked: root.detector.rescanGameLibraries()
    }

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: tableContent.implicitHeight
        radius: Theme.radiusSmall
        color: "transparent"
        border.color: Theme.borderColor

        ColumnLayout {
            id: tableContent
            width: parent.width
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.preferredHeight: 36
                spacing: 12
                Label { Layout.fillWidth: true; text: qsTr("Launcher"); color: Theme.textMutedColor; font.pixelSize: Theme.fontSizeSmall }
                Label { objectName: "libraryStatusColumnHeader"; Layout.preferredWidth: root.statusWidth; text: qsTr("Status"); color: Theme.textMutedColor; font.pixelSize: Theme.fontSizeSmall }
                Label { Layout.preferredWidth: root.gamesWidth; text: qsTr("Games"); color: Theme.textMutedColor; font.pixelSize: Theme.fontSizeSmall }
                Label { Layout.preferredWidth: root.locationsWidth; text: qsTr("Locations"); color: Theme.textMutedColor; font.pixelSize: Theme.fontSizeSmall }
            }

            Repeater {
                // Keep row/popup objects alive when a scan updates their data.
                model: root.detector ? root.detector.gameLibraries.length : 0
                delegate: Rectangle {
                    id: launcherRow
                    required property int index
                    readonly property var library: root.detector.gameLibraries[index]
                    readonly property var games: library.games || []
                    readonly property bool hasErrors: (library.locations || []).some(function(location) {
                        return location.status !== "Detected"
                    })
                    Layout.fillWidth: true
                    implicitHeight: 52
                    color: index % 2 === 0 ? Theme.withOpacity(Theme.textColor, 0.025) : "transparent"

                    Rectangle { width: parent.width; height: 1; color: Theme.borderColor }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 12
                        Image {
                            objectName: launcherRow.library.id + "LibraryIcon"
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 24
                            sourceSize: Qt.size(48, 48)
                            fillMode: Image.PreserveAspectFit
                            source: launcherRow.library.id === "steam"
                                ? "qrc:/icons/default-apps/steam.svg"
                                : "qrc:/icons/launchers/" + launcherRow.library.id
                                    + (launcherRow.library.id === "legendary" ? ".ico" : ".svg")
                        }
                        Label {
                            Layout.fillWidth: true
                            text: launcherRow.library.name
                            color: Theme.textColor
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        RowLayout {
                            Layout.preferredWidth: root.statusWidth
                            Layout.minimumWidth: root.statusWidth
                            Layout.maximumWidth: root.statusWidth
                            spacing: 8
                            Rectangle {
                                objectName: launcherRow.library.id + "LibraryStatusIndicator"
                                width: 8
                                height: 8
                                radius: 4
                                color: launcherRow.hasErrors ? Theme.warningColor
                                    : launcherRow.library.detected ? Theme.successColor : "transparent"
                                border.width: launcherRow.library.detected || launcherRow.hasErrors ? 0 : 1
                                border.color: Theme.textMutedColor
                            }
                            Label {
                                objectName: launcherRow.library.id + "LibraryStatus"
                                Layout.fillWidth: true
                                text: launcherRow.hasErrors ? qsTr("Check location")
                                    : launcherRow.library.detected ? qsTr("Detected") : qsTr("Not detected")
                                color: launcherRow.hasErrors ? Theme.warningColor : Theme.textSecondaryColor
                                font.pixelSize: Theme.fontSizeSmall
                                elide: Text.ElideRight
                            }
                        }
                        TrophyButton {
                            id: gamesButton
                            objectName: launcherRow.library.id + "DetectedGamesDropdown"
                            Layout.preferredWidth: root.gamesWidth
                            implicitHeight: 32
                            padding: 0
                            horizontalPadding: 0
                            outlined: true
                            checked: gamesPopup.opened
                            Accessible.name: qsTr("%1: %2 detected games").arg(launcherRow.library.name).arg(launcherRow.library.count)
                            ToolTip.text: qsTr("Show detected games")
                            ToolTip.visible: hovered
                            onClicked: gamesPopup.opened ? gamesPopup.close() : gamesPopup.open()
                            contentItem: RowLayout {
                                spacing: 6
                                Label { Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; text: launcherRow.library.count; color: Theme.textColor }
                                Label {
                                    Layout.preferredWidth: 24
                                    text: "⌄"
                                    horizontalAlignment: Text.AlignHCenter
                                    color: Theme.textSecondaryColor
                                    font.pixelSize: 20
                                    rotation: gamesPopup.opened ? 180 : 0
                                }
                            }
                            Popup {
                                id: gamesPopup
                                objectName: launcherRow.library.id + "DetectedGamesPopup"
                                x: gamesButton.width - width
                                y: gamesButton.height + Theme.spacingSmall
                                width: Math.min(360, root.width)
                                padding: 16
                                focus: true
                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                                background: Rectangle { color: Theme.cardColor; radius: Theme.radiusMedium; border.color: Theme.borderColor }
                                contentItem: ColumnLayout {
                                    spacing: 12
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("%1 games").arg(launcherRow.library.name)
                                        color: Theme.textColor
                                        font.weight: Font.Medium
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: launcherRow.games.length === 0
                                        text: qsTr("No usable games detected")
                                        wrapMode: Text.WordWrap
                                        color: Theme.textSecondaryColor
                                    }
                                    ListView {
                                        objectName: launcherRow.library.id + "DetectedGamesList"
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.min(contentHeight, 260)
                                        visible: launcherRow.games.length > 0
                                        clip: true
                                        model: gamesPopup.visible ? launcherRow.games : []
                                        ScrollBar.vertical: TrophyScrollBar {}
                                        delegate: Item {
                                            required property var modelData
                                            width: ListView.view.width
                                            implicitHeight: gameDetails.implicitHeight + 16
                                            ColumnLayout {
                                                id: gameDetails
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.rightMargin: 12
                                                anchors.verticalCenter: parent.verticalCenter
                                                spacing: 2
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: modelData.name
                                                    color: Theme.textColor
                                                    wrapMode: Text.WordWrap
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: modelData.directory || modelData.executable || ""
                                                    visible: text.length > 0
                                                    color: Theme.textMutedColor
                                                    font.pixelSize: Theme.fontSizeSmall
                                                    elide: Text.ElideMiddle
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        TrophyButton {
                            id: locationsButton
                            objectName: launcherRow.library.id + "LibraryLocationsButton"
                            Layout.preferredWidth: root.locationsWidth
                            implicitHeight: 32
                            horizontalPadding: 8
                            text: qsTr("Manage")
                            outlined: true
                            checked: locationsPopup.opened
                            Accessible.name: qsTr("Manage %1 library locations").arg(launcherRow.library.name)
                            onClicked: locationsPopup.opened ? locationsPopup.close() : locationsPopup.open()
                            Popup {
                                id: locationsPopup
                                objectName: launcherRow.library.id + "LibraryLocationsPopup"
                                x: locationsButton.width - width
                                y: locationsButton.height + Theme.spacingSmall
                                width: Math.min(440, root.width)
                                height: Math.min(locationContent.implicitHeight + 32, 460)
                                padding: 16
                                focus: true
                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                                background: Rectangle { color: Theme.cardColor; radius: Theme.radiusMedium; border.color: Theme.borderColor }
                                contentItem: ScrollView {
                                    id: locationScroll
                                    clip: true
                                    contentWidth: availableWidth
                                    ScrollBar.vertical: TrophyScrollBar {}
                                    ColumnLayout {
                                        id: locationContent
                                        width: locationScroll.availableWidth
                                        spacing: 12
                                        Label { text: qsTr("%1 locations").arg(launcherRow.library.name); color: Theme.textColor; font.weight: Font.Medium }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label { Layout.fillWidth: true; text: qsTr("Use automatic locations"); color: Theme.textSecondaryColor }
                                            TrophySwitch {
                                                objectName: launcherRow.library.id + "LibraryAutomaticSwitch"
                                                checked: launcherRow.library.automatic
                                                onToggled: root.detector.setLibraryAutomatic(launcherRow.library.id, checked)
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: launcherRow.library.id === "steam" ? qsTr("Select the Steam directory containing steamapps.")
                                                : launcherRow.library.id === "faugus" ? qsTr("Select the Faugus data directory containing games.json.")
                                                : launcherRow.library.id === "heroic" ? qsTr("Select the heroic config directory containing legendaryConfig, gog_store or nile_config.")
                                                : launcherRow.library.id === "lutris" ? qsTr("Select the Lutris data directory containing pga.db and the games subdirectory with its YAML files.")
                                                : qsTr("Select the launcher metadata directory containing installed.json.")
                                            wrapMode: Text.WordWrap
                                            color: Theme.textSecondaryColor
                                            font.pixelSize: Theme.fontSizeSmall
                                        }
                                        Repeater {
                                            model: launcherRow.library.locations || []
                                            delegate: Label {
                                                required property var modelData
                                                Layout.fillWidth: true
                                                text: modelData.path + "\n" + modelData.status + " · " + qsTr("%1 usable entries").arg(modelData.count)
                                                wrapMode: Text.WrapAnywhere
                                                color: modelData.status === "Detected" ? Theme.textSecondaryColor : Theme.warningColor
                                                font.pixelSize: Theme.fontSizeSmall
                                            }
                                        }
                                        Repeater {
                                            model: launcherRow.library.paths || []
                                            delegate: RowLayout {
                                                id: customLocation
                                                required property var modelData
                                                Layout.fillWidth: true
                                                Label { Layout.fillWidth: true; text: customLocation.modelData; wrapMode: Text.WrapAnywhere; color: Theme.textSecondaryColor }
                                                TrophyButton {
                                                    text: qsTr("Remove")
                                                    onClicked: root.detector.removeLibraryLocation(launcherRow.library.id, customLocation.modelData)
                                                }
                                            }
                                        }
                                        TrophyButton {
                                            text: qsTr("Add location…")
                                            enabled: (launcherRow.library.paths || []).length < 16
                                            onClicked: {
                                                locationsPopup.close()
                                                root.addLocationRequested(launcherRow.library.id)
                                            }
                                        }
                                        TrophyButton {
                                            text: qsTr("Reset to automatic")
                                            onClicked: root.detector.resetLibraryLocations(launcherRow.library.id)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
