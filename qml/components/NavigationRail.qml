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

Rectangle {
    id: root

    property int currentIndex: 0

    signal navigate(int index)

    width: Theme.navRailWidth
    color: Theme.surfaceColor
    radius: 11

    // Square off all corners except bottom-left
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 11
        color: parent.color
    }

    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 11
        color: parent.color
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.spacingLarge
        anchors.bottomMargin: Theme.spacingTiny
        anchors.leftMargin: Theme.spacingTiny
        anchors.rightMargin: Theme.spacingTiny
        spacing: Theme.spacingSmall

        // Navigation buttons
        NavigationButton {
            Layout.fillWidth: true
            iconText: "📊"
            text: qsTr("Dashboard")
            checked: root.currentIndex === 0
            onClicked: root.navigate(0)
        }

        NavigationButton {
            Layout.fillWidth: true
            iconText: "🎬"
            text: qsTr("Recordings")
            checked: root.currentIndex === 1
            onClicked: root.navigate(1)
        }

        NavigationButton {
            Layout.fillWidth: true
            iconText: "🎮"
            text: qsTr("Games")
            checked: root.currentIndex === 2
            onClicked: root.navigate(2)
        }

        // Spacer
        Item {
            Layout.fillHeight: true
        }

        // Settings at bottom
        NavigationButton {
            Layout.fillWidth: true
            iconText: "⚙️"
            text: qsTr("Settings")
            checked: root.currentIndex === 3
            onClicked: root.navigate(3)
        }

        NavigationButton {
            Layout.fillWidth: true
            iconText: "ℹ️"
            text: qsTr("Info")
            checked: root.currentIndex === 4
            onClicked: root.navigate(4)
        }

    }

    component NavigationButton: AbstractButton {
        id: navButton

        property string iconText: ""

        implicitHeight: 56
        leftPadding: Theme.spacingMedium
        rightPadding: Theme.spacingMedium

        contentItem: ColumnLayout {
            spacing: 4
            anchors.fill: parent
            anchors.topMargin: 8
            anchors.bottomMargin: 8

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: navButton.iconText
                font.pixelSize: 20
                opacity: navButton.checked ? 1 : 0.7
            }

            Label {
                Layout.alignment: Qt.AlignHCenter
                horizontalAlignment: Text.AlignHCenter
                text: navButton.text
                font.pixelSize: 10
                font.weight: navButton.checked ? Font.Medium : Font.Normal
                color: navButton.checked ? Theme.primaryColor : Theme.textSecondaryColor
            }

        }

        background: Rectangle {
            radius: Theme.radiusNormal
            color: {
                if (navButton.checked)
                    return Theme.withOpacity(Theme.primaryColor, 0.15);
                else if (navButton.hovered)
                    return Theme.withOpacity(Theme.textColor, 0.05);
                return "transparent";
            }

            Behavior on color {
                ColorAnimation {
                    duration: Theme.animationFast
                }

            }

        }

    }

}
