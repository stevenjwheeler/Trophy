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

Item {
    id: root

    property string label: ""
    property real level: 0.0  // 0.0 to 1.0
    property bool muted: false

    implicitWidth: 60
    implicitHeight: 120

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingTiny

        // Level meter bar - fills remaining space above label
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 12
            Layout.fillHeight: true
            radius: Theme.radiusSmall
            color: Theme.cardColor
            border.width: 1
            border.color: Theme.borderColor

            // Level fill
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 2

                height: (parent.height - 4) * Math.min(root.level, 1.0)
                radius: Theme.radiusSmall - 1

                gradient: Gradient {
                    GradientStop {
                        position: 0.0
                        color: root.muted ? Theme.errorColor :
                               (root.level > 0.9 ? Theme.errorColor :
                                root.level > 0.7 ? Theme.warningColor :
                                Theme.successColor)
                    }
                    GradientStop {
                        position: 1.0
                        color: root.muted ? Theme.errorColor : Theme.successColor
                    }
                }

                opacity: root.muted ? 0.5 : 1.0

                Behavior on height {
                    NumberAnimation { duration: 50 }
                }
            }

            // Peak indicator lines
            Repeater {
                model: 10

                Rectangle {
                    y: parent.height - (parent.height * (index + 1) / 10) - 1
                    width: parent.width
                    height: 1
                    color: Theme.withOpacity(Theme.borderColor, 0.5)
                }
            }
        }

        // Icon label at bottom - clickable to mute
        AbstractButton {
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: iconLabel.implicitWidth + 4
            implicitHeight: iconLabel.implicitHeight + 4

            onClicked: root.muted = !root.muted

            contentItem: Label {
                id: iconLabel
                text: getIcon()
                color: root.muted ? Theme.errorColor : Theme.textColor
                font.pixelSize: Theme.fontSizeNormal
                horizontalAlignment: Text.AlignHCenter
                opacity: parent.hovered ? 1.0 : 0.8

                Behavior on color {
                    ColorAnimation { duration: 150 }
                }
            }

            background: Rectangle {
                radius: Theme.radiusSmall
                color: parent.hovered ? Theme.withOpacity(Theme.textColor, 0.1) : "transparent"
            }
        }
    }

    function getIcon() {
        var labelLower = label.toLowerCase()
        if (labelLower.includes("game")) return "🎮"
        if (labelLower.includes("comm") || labelLower.includes("voice")) return "🎧"
        if (labelLower.includes("mic")) return "🎤"
        return "🔊"
    }
}
