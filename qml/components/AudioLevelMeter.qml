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

Item {
    id: root

    property string label: ""
    property real level: 0 // 0.0 to 1.0
    property bool muted: false
    // Dashboard card uses the horizontal icon+bar rows; default is the
    // vertical bar. Only one layout is visible at a time.
    property bool horizontal: false

    function getIcon() {
        var labelLower = label.toLowerCase();
        if (labelLower.includes("game"))
            return "🎮";

        if (labelLower.includes("comm") || labelLower.includes("voice"))
            return "🎧";

        if (labelLower.includes("mic"))
            return "🎤";

        return "🔊";
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

                // Emoji glyphs ignore text color, so muted state swaps the
                // glyph itself (a red recolor would never render).
                text: root.muted ? "🔇" : getIcon()
                color: root.muted ? Theme.errorColor : Theme.textColor
                font.pixelSize: Theme.fontSizeNormal
                horizontalAlignment: Text.AlignHCenter
                opacity: parent.hovered ? 1 : 0.8

                Behavior on color {
                    ColorAnimation {
                        duration: 150
                    }

                }

            }

            background: Rectangle {
                radius: Theme.radiusSmall
                color: parent.hovered ? Theme.withOpacity(Theme.textColor, 0.1) : "transparent"
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
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: iconLabelH.implicitWidth + 4
            implicitHeight: iconLabelH.implicitHeight + 4
            onClicked: root.muted = !root.muted

            contentItem: Label {
                id: iconLabelH

                text: root.muted ? "🔇" : getIcon()
                color: root.muted ? Theme.errorColor : Theme.textColor
                font.pixelSize: Theme.fontSizeNormal
                horizontalAlignment: Text.AlignHCenter
                opacity: parent.hovered ? 1 : 0.8

                Behavior on color {
                    ColorAnimation {
                        duration: 150
                    }

                }

            }

            background: Rectangle {
                radius: Theme.radiusSmall
                color: parent.hovered ? Theme.withOpacity(Theme.textColor, 0.1) : "transparent"
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
