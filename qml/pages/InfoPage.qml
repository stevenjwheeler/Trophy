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

    // Update-check state (fed by updateChecker.checkFinished).
    // updateNewer tints the status; the notice action carries
    // the releases URL, so the page only needs the verdict.
    property string updateStatus: ""
    property bool updateChecking: false
    property bool updateNewer: false
    // Release page behind the "Version X is available"
    // verdict, which opens it on click.
    property string updateReleaseUrl: ""

    Connections {
        target: updateChecker
        function onCheckFinished(newerAvailable, latestVersion, releaseUrl) {
            updateChecking = false
            updateNewer = newerAvailable
            updateReleaseUrl = newerAvailable ? releaseUrl : ""
            if (newerAvailable) {
                // Names both versions (they differ) and stays up:
                // it's actionable.
                updateDismissTimer.stop()
                updateStatus = qsTr("Version %1 is available — you're on %2.").arg(latestVersion, AppVersion)
            } else {
                // The badge right above already shows the
                // version; this confirmation fades shortly.
                updateStatus = qsTr("You're on the latest version.")
                updateDismissTimer.restart()
            }
        }
    }

    Timer {
        id: updateDismissTimer
        interval: 4000
        repeat: false
        onTriggered: {
            root.updateStatus = ""
            root.updateNewer = false
        }
    }

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
                spacing: Theme.spacingSmall

                Item {
                    Layout.preferredHeight: Theme.spacingLarge * 2
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

                // Version in a little red badge.
                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: versionText.implicitWidth + 18
                    implicitHeight: versionText.implicitHeight + 8
                    radius: Theme.radiusSmall
                    color: Theme.primaryColor

                    Label {
                        id: versionText
                        anchors.centerIn: parent
                        text: AppVersion
                        color: "white"
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Font.Medium
                    }
                }

                // Update verdict from the background check.
                // The manual link below hides while this (or
                // "Checking…") shows, and returns when it clears.
                // A newer-version verdict opens its release page.
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    visible: root.updateChecking || root.updateStatus !== ""
                    text: root.updateChecking ? qsTr("Checking…") : root.updateStatus
                    color: root.updateNewer ? Theme.primaryColor : Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    font.underline: verdictHover.hovered && root.updateNewer
                    horizontalAlignment: Text.AlignHCenter

                    HoverHandler {
                        id: verdictHover
                        cursorShape: root.updateNewer ? Qt.PointingHandCursor : Qt.ArrowCursor
                    }
                    TapHandler {
                        enabled: root.updateNewer && root.updateReleaseUrl !== ""
                        onTapped: Qt.openUrlExternally(root.updateReleaseUrl)
                    }
                }

                // Text button: manual check lives here, the
                // automatic-checks toggle lives in Settings.
                // Hidden while a check runs or its verdict shows,
                // so the feed can't be spammed.
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    visible: !root.updateChecking && root.updateStatus === ""
                    text: qsTr("Check for updates")
                    color: checkUpdatesHover.hovered ? Theme.primaryColor : Theme.textSecondaryColor
                    font.pixelSize: Theme.fontSizeSmall
                    font.underline: checkUpdatesHover.hovered
                    horizontalAlignment: Text.AlignHCenter

                    HoverHandler {
                        id: checkUpdatesHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        enabled: typeof updateChecker !== "undefined"
                        onTapped: {
                            root.updateChecking = true
                            updateChecker.checkNow()
                        }
                    }
                }

            }

            // Breathing room between the header block and the
            // first card.
            Item {
                Layout.preferredHeight: Theme.spacingMedium
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
                        text: qsTr("Capture your best gaming moments on Linux with zero hassle. Trophy provides high-performance recording, one-press clipping, smart game detection, and separate audio tracks for easier editing.")
                        wrapMode: Text.WordWrap
                        color: Theme.textColor
                        font.pixelSize: Theme.fontSizeNormal
                        horizontalAlignment: Text.AlignHCenter
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

                        FeatureItem {
                            icon: "⏪"
                            title: qsTr("Instant Clips")
                            description: qsTr("Background DVR — save recent moments anytime, buffer length adjustable")
                        }

                        FeatureItem {
                            icon: "🔄"
                            title: qsTr("Automatic Updates")
                            description: qsTr("Daily release check with a one-click download page")
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
                            text: qsTr("• The Qt project for the application framework")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                        Label {
                            text: qsTr("• The Wayland compositor teams (KWin, Hyprland, Sway, GNOME) for modern Linux compositing")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                        Label {
                            text: qsTr("• The xdg-desktop-portal contributors for secure screen capture")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                        Label {
                            text: qsTr("• The Khronos Group for the open Vulkan graphics standard")
                            color: Theme.textColor
                            font.pixelSize: Theme.fontSizeNormal
                        }

                        Label {
                            text: qsTr("• LinuxDeploy for AppImage distribution")
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

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacingSmall

                        Label {
                            text: qsTr("📜 License")
                            font.pixelSize: Theme.fontSizeLarge
                            font.weight: Font.Medium
                            color: Theme.textColor
                        }

                        Item { Layout.fillWidth: true }

                        Label {
                            text: qsTr("Copyright © 2026")
                            color: Theme.textMutedColor
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("This project is licensed under the GNU General Public License v3.0 or later (GPL-3.0-or-later).")
                        wrapMode: Text.WordWrap
                        color: Theme.textColor
                        font.pixelSize: Theme.fontSizeNormal
                    }

                }

            }

            // AI Disclaimer Section
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: disclaimerLayout.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: Theme.cardColor

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
                spacing: Theme.spacingLarge

                FooterLink {
                    text: "GitHub"
                    url: "https://github.com/OasisLab-Software/Trophy"
                    iconSource: "qrc:/icons/github.svg"
                }

                FooterLink {
                    text: qsTr("Report an Issue")
                    url: "https://github.com/OasisLab-Software/Trophy/issues/new"
                }

                FooterLink {
                    text: qsTr("Releases")
                    url: "https://github.com/OasisLab-Software/Trophy/releases"
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

    // Footer link: same hover language as the
    // "Check for updates" text button (primary + underline),
    // with an optional leading icon. Hover/click use
    // non-visual handlers: an anchored MouseArea inside a
    // layout child is undefined behavior (engine warning).
    component FooterLink: RowLayout {
        property string text: ""
        property string url: ""
        property string iconSource: ""

        spacing: 4

        Image {
            visible: parent.iconSource !== ""
            source: parent.iconSource
            Layout.preferredWidth: 14
            Layout.preferredHeight: 14
            sourceSize: Qt.size(14, 14)
            Layout.alignment: Qt.AlignVCenter
        }

        Label {
            text: parent.text
            color: footerHover.hovered ? Theme.primaryColor : Theme.textSecondaryColor
            font.pixelSize: Theme.fontSizeSmall
            font.underline: footerHover.hovered
            horizontalAlignment: Text.AlignHCenter
            Layout.alignment: Qt.AlignVCenter
        }

        HoverHandler {
            id: footerHover
            // Cursor lives here, not on the TapHandler:
            // TapHandler only shows its cursor while pressed,
            // HoverHandler shows it on hover.
            cursorShape: Qt.PointingHandCursor
        }

        TapHandler {
            onTapped: Qt.openUrlExternally(parent.url)
        }
    }

}
