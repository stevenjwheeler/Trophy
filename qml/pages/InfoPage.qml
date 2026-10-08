/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import TrophyBackend
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        clip: true

        ColumnLayout {
            id: mainStack

            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width, 800)
            spacing: Theme.spacingLarge

            // Centered Header
            ColumnLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                spacing: Theme.spacingMedium

                Item {
                    Layout.preferredHeight: Theme.spacingLarge
                }

                Image {
                    Layout.alignment: Qt.AlignHCenter
                    source: "qrc:/icons/trophy.svg"
                    Layout.preferredWidth: 96
                    Layout.preferredHeight: 96
                    sourceSize: Qt.size(96, 96)
                    smooth: true
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Trophy")
                    font.pixelSize: 32
                    font.weight: Font.Bold
                    color: Theme.textColor
                    horizontalAlignment: Text.AlignHCenter
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Game recorder and clipping for Linux")
                    font.pixelSize: Theme.fontSizeLarge
                    color: Theme.primaryColor
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignHCenter
                }

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Version %1").arg(AppVersion)
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.textMutedColor
                    horizontalAlignment: Text.AlignHCenter
                }

            }

            // Description
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: descLayout.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    id: descLayout

                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingSmall

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Capture your best gaming moments on Linux with zero hassle. Trophy provides high-performance recording, keybinded clipping, smart game detection, and separate audio tracks for easier editing.")
                        wrapMode: Text.WordWrap
                        color: Theme.textColor
                        font.pixelSize: Theme.fontSizeNormal
                        horizontalAlignment: Text.AlignHCenter
                    }

                }

            }

            // License Section
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: licenseLayout.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    id: licenseLayout

                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingSmall

                    Label {
                        text: qsTr("📜 License")
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.Medium
                        color: Theme.textColor
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("This project is licensed under the GNU General Public License v3.0 or later (GPL-3.0-or-later).")
                        wrapMode: Text.WordWrap
                        color: Theme.textColor
                        font.pixelSize: Theme.fontSizeNormal
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Copyright © 2026")
                        color: Theme.textMutedColor
                        font.pixelSize: Theme.fontSizeSmall
                    }

                }

            }

            // Features Section
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: featuresLayout.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    id: featuresLayout

                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingMedium

                    Label {
                        text: qsTr("✨ Key Features")
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.Medium
                        color: Theme.textColor
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Theme.spacingLarge
                        rowSpacing: Theme.spacingSmall

                        FeatureItem {
                            icon: "🎬"
                            title: qsTr("Hardware Acceleration")
                            description: qsTr("NVIDIA NVENC and AMD/Intel VA-API support")
                        }

                        FeatureItem {
                            icon: "🎵"
                            title: qsTr("Multi-Track Audio")
                            description: qsTr("Separate game, voice chat, and mic tracks")
                        }

                        FeatureItem {
                            icon: "🎮"
                            title: qsTr("Auto Game Detection")
                            description: qsTr("Steam, Proton, and custom game support with automatic detection")
                        }

                        FeatureItem {
                            icon: "💾"
                            title: qsTr("Smart Storage")
                            description: qsTr("Auto-cleanup and crash-resilient formats")
                        }

                    }

                }

            }

            // Acknowledgments Section
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: ackLayout.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    id: ackLayout

                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingMedium

                    Label {
                        text: qsTr("🙏 Acknowledgments")
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.Medium
                        color: Theme.textColor
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Trophy is built on the shoulders of giants. Special thanks to:")
                        wrapMode: Text.WordWrap
                        color: Theme.textColor
                        font.pixelSize: Theme.fontSizeNormal
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingTiny

                        Label {
                            text: qsTr("• The FFmpeg project for multimedia processing")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                        Label {
                            text: qsTr("• The Qt Company for the excellent Qt framework")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                        Label {
                            text: qsTr("• The Wayland and KWin teams for modern Linux compositing")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                        Label {
                            text: qsTr("• The PipeWire team for modern Linux audio")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                        Label {
                            text: qsTr("• The open-source community for inspiration and support")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                    }

                }

            }

            // Keyboard Shortcuts Section
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: shortcutsLayout.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    id: shortcutsLayout

                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingMedium

                    Label {
                        text: qsTr("⌨️ Keyboard Shortcuts")
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.Medium
                        color: Theme.textColor
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Theme.spacingLarge
                        rowSpacing: Theme.spacingSmall

                        Label {
                            text: "Ctrl+Q"
                            font.family: "monospace"
                            font.pixelSize: Theme.fontSizeNormal
                            color: Theme.primaryColor
                            Layout.preferredWidth: 100
                        }

                        Label {
                            text: qsTr("Quit Application")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                    }

                }

            }

            // Logo Section
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: logoLayout.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.cardColor

                ColumnLayout {
                    id: logoLayout

                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingMedium

                    Label {
                        text: qsTr("🏆 What's the logo?")
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.Medium
                        color: Theme.textColor
                    }

                    GridLayout {
                        Layout.fillWidth: true

                        Label {
                            text: qsTr("It's a trophy from the top down, which also happens to look like a recording indicator!")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                    }

                }

            }

            // AI Disclaimer Section
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: disclaimerLayout.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.withOpacity(Theme.primaryColor, 0.1)
                border.width: 1
                border.color: Theme.withOpacity(Theme.primaryColor, 0.3)

                ColumnLayout {
                    id: disclaimerLayout

                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingSmall

                    Label {
                        text: qsTr("🤖 Development Disclosure")
                        font.pixelSize: Theme.fontSizeLarge
                        font.weight: Font.Medium
                        color: Theme.textColor
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("This project has been developed in part with AI assistance (Claude by Anthropic). Any and all generated code has been reviewed and verified for correctness, security, and adherence to best practices. Any and all architecture, design decisions, and implementation details have been validated by the project maintainer.")
                        wrapMode: Text.WordWrap
                        color: Theme.textColor
                        font.pixelSize: Theme.fontSizeSmall
                    }

                }

            }

            // Links
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: Theme.spacingMedium

                Button {
                    flat: true
                    background: null
                    icon.source: "qrc:/icons/github.svg"
                    icon.color: Theme.textColor
                    text: "GitHub"
                    onClicked: Qt.openUrlExternally("https://github.com/stevenjwheeler/Trophy")

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }

                }

            }

            Item {
                Layout.preferredHeight: Theme.spacingLarge
            }

        }

    }

    background: Rectangle {
        color: Theme.backgroundColor
        bottomRightRadius: 12
        topLeftRadius: 26
    }

    // Feature Item Component
    component FeatureItem: ColumnLayout {
        property string icon: ""
        property string title: ""
        property string description: ""

        Layout.fillWidth: true
        spacing: Theme.spacingTiny

        Label {
            text: icon + " " + title
            font.pixelSize: Theme.fontSizeNormal
            font.weight: Font.Medium
            color: Theme.textColor
        }

        Label {
            Layout.fillWidth: true
            text: description
            wrapMode: Text.WordWrap
            color: Theme.textMutedColor
            font.pixelSize: Theme.fontSizeSmall
        }

    }

    // Technology Item Component
    component TechItem: RowLayout {
        property string name: ""
        property string description: ""

        Layout.fillWidth: true
        spacing: Theme.spacingSmall

        Label {
            text: "•"
            color: Theme.primaryColor
            font.pixelSize: Theme.fontSizeMedium
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2

            Label {
                text: name
                font.pixelSize: Theme.fontSizeNormal
                font.weight: Font.Medium
                color: Theme.textColor
            }

            Label {
                Layout.fillWidth: true
                text: description
                wrapMode: Text.WordWrap
                color: Theme.textMutedColor
                font.pixelSize: Theme.fontSizeSmall
            }

        }

    }

}
