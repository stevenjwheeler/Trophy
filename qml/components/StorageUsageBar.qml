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

    property real used: 0 // bytes
    property real max: 0 // bytes
    property real percent: max > 0 ? (used / max) * 100 : 0

    function formatSize(bytes) {
        var kb = 1024;
        var mb = kb * 1024;
        var gb = mb * 1024;
        var tb = gb * 1024;
        if (bytes >= tb)
            return (bytes / tb).toFixed(2) + " TB";
        else if (bytes >= gb)
            return (bytes / gb).toFixed(2) + " GB";
        else if (bytes >= mb)
            return (bytes / mb).toFixed(1) + " MB";
        else if (bytes >= kb)
            return (bytes / kb).toFixed(1) + " KB";
        return bytes + " B";
    }

    implicitHeight: 50

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spacingTiny

        // Header row
        RowLayout {
            Layout.fillWidth: true

            Label {
                text: qsTr("Storage")
                color: Theme.textSecondaryColor
                font.pixelSize: Theme.fontSizeSmall
            }

            Item {
                Layout.fillWidth: true
            }

            Label {
                text: root.max > 0 ? formatSize(root.used) + " / " + formatSize(root.max) : formatSize(root.used) + " / " + qsTr("Unlimited")
                color: Theme.textColor
                font.pixelSize: Theme.fontSizeSmall
            }

        }

        // Progress bar
        Rectangle {
            visible: root.max > 0
            Layout.fillWidth: true
            Layout.preferredHeight: 8
            radius: 4
            color: Theme.surfaceColor
            border.width: 1
            border.color: Theme.borderColor

            Rectangle {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.margins: 1
                width: Math.max(0, (parent.width - 2) * Math.min(root.percent / 100, 1))
                radius: 3
                color: {
                    if (root.percent > 90)
                        return Theme.errorColor;

                    if (root.percent > 75)
                        return Theme.warningColor;

                    return Theme.primaryColor;
                }

                Behavior on width {
                    NumberAnimation {
                        duration: Theme.animationNormal
                    }

                }

                Behavior on color {
                    ColorAnimation {
                        duration: Theme.animationNormal
                    }

                }

            }

        }

        // Percentage label
        Label {
            visible: root.max > 0
            Layout.alignment: Qt.AlignRight
            text: root.percent.toFixed(1) + "% " + qsTr("used")
            color: {
                if (root.percent > 90)
                    return Theme.errorColor;

                if (root.percent > 75)
                    return Theme.warningColor;

                return Theme.textMutedColor;
            }
            font.pixelSize: Theme.fontSizeSmall
        }

    }

}
