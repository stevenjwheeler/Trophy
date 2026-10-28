/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../styles"
import "../controls"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string label: ""
    property real level: 0 // 0.0 to 1.0
    property bool muted: false
    // Dashboard card uses the horizontal icon+bar rows; default is the
    // vertical bar. Only one layout is visible at a time.
    property bool horizontal: false
    // Polling/render-state updates are not user mute commands.
    signal muteToggled(bool muted)

    function toggleMute() {
        muted = !muted
        muteToggled(muted)
    }

    function getIcon() {
        var labelLower = label.toLowerCase();
        if (labelLower.includes("game"))
            return "qrc:/icons/gamepad.svg";

        if (labelLower.includes("comm") || labelLower.includes("voice"))
            return "qrc:/icons/headphones.svg";

        if (labelLower.includes("mic"))
            return "qrc:/icons/microphone.svg";

        return "qrc:/icons/volume-high.svg";
    }

    implicitWidth: 60
    implicitHeight: 120

    ColumnLayout {
        visible: !root.horizontal
        anchors.fill: parent
        spacing: Theme.spacingTiny

        // Level meter bar - fills remaining space above label
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 12
            Layout.fillHeight: true
            radius: Theme.radiusSmall
            color: root.muted ? Theme.withOpacity(Theme.errorColor, 0.15) : Theme.cardColor
            border.width: 1
            border.color: root.muted ? Theme.errorColor : Theme.borderColor

            // Level fill
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 2
                height: (parent.height - 4) * Math.min(root.level, 1)
                radius: Theme.radiusSmall - 1
                opacity: root.muted ? 0.5 : 1

                gradient: Gradient {
                    GradientStop {
                        position: 0
                        color: root.muted ? Theme.errorColor : (root.level > 0.9 ? Theme.errorColor : root.level > 0.7 ? Theme.warningColor : Theme.successColor)
                    }

                    GradientStop {
                        position: 1
                        color: root.muted ? Theme.errorColor : Theme.successColor
                    }

                }

                Behavior on height {
                    NumberAnimation {
                        duration: 50
                    }

                }

            }

            // Peak indicator lines
            Repeater {
                model: 10

                Rectangle {
                    required property int index
                    y: parent.height - (parent.height * (index + 1) / 10) - 1
                    width: parent.width
                    height: 1
                    color: Theme.withOpacity(Theme.borderColor, 0.5)
                }

            }

        }

        // Icon label at bottom - clickable to mute
        AbstractButton {
            id: verticalMuteButton
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 24
            implicitHeight: 24
            onClicked: root.toggleMute()

            Accessible.name: root.label
            Accessible.checkable: true
            Accessible.checked: root.muted
            ToolTip.visible: hovered
            ToolTip.text: root.muted ? qsTr("Unmute %1").arg(root.label) : qsTr("Mute %1").arg(root.label)

            contentItem: Item {
                TrophyIcon {
                    anchors.centerIn: parent
                    width: 14
                    height: 14
                    source: root.muted ? "qrc:/icons/volume-muted.svg" : root.getIcon()
                    color: root.muted ? Theme.errorColor : Theme.textSecondaryColor

                    Behavior on color {
                        ColorAnimation {
                            duration: 150
                        }
                    }
                }
            }

            background: Rectangle {
                radius: Theme.radiusSmall
                color: verticalMuteButton.hovered ? Theme.withOpacity(Theme.textColor, 0.1) : "transparent"
            }

        }

    }

    // Horizontal icon+bar row for tight cards (dashboard). Same mute
    // behavior and gradient as the vertical bar.
    RowLayout {
        visible: root.horizontal
        anchors.fill: parent
        spacing: Theme.spacingSmall

        AbstractButton {
            id: horizontalMuteButton
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: 24
            implicitHeight: 24
            onClicked: root.toggleMute()

            Accessible.name: root.label
            Accessible.checkable: true
            Accessible.checked: root.muted
            ToolTip.visible: hovered
            ToolTip.text: root.muted ? qsTr("Unmute %1").arg(root.label) : qsTr("Mute %1").arg(root.label)

            contentItem: Item {
                TrophyIcon {
                    anchors.centerIn: parent
                    width: 14
                    height: 14
                    source: root.muted ? "qrc:/icons/volume-muted.svg" : root.getIcon()
                    color: root.muted ? Theme.errorColor : Theme.textSecondaryColor

                    Behavior on color {
                        ColorAnimation {
                            duration: 150
                        }
                    }
                }
            }

            background: Rectangle {
                radius: Theme.radiusSmall
                color: horizontalMuteButton.hovered ? Theme.withOpacity(Theme.textColor, 0.1) : "transparent"
            }

        }

        Rectangle {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: 10
            radius: height / 2
            color: root.muted ? Theme.withOpacity(Theme.errorColor, 0.15) : Theme.cardColor
            border.width: 1
            border.color: root.muted ? Theme.errorColor : Theme.borderColor

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: 2
                width: (parent.width - 4) * Math.min(root.level, 1)
                radius: height / 2
                opacity: root.muted ? 0.5 : 1

                gradient: Gradient {
                    GradientStop {
                        position: 0
                        color: root.muted ? Theme.errorColor : (root.level > 0.9 ? Theme.errorColor : root.level > 0.7 ? Theme.warningColor : Theme.successColor)
                    }

                    GradientStop {
                        position: 1
                        color: root.muted ? Theme.errorColor : Theme.successColor
                    }

                }

                Behavior on width {
                    NumberAnimation {
                        duration: 50
                    }

                }

            }

        }

    }

}
