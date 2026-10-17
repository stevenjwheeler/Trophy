/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
pragma Singleton

QtObject {
    id: theme
    // The app publishes settings on the engine context as well as the window.
    // This also covers Theme imports through local component directories.
    // Standalone components/tests without a settings context keep defaults.
    property var settings: typeof settingsManager !== "undefined" && settingsManager !== null ? settingsManager : null
    readonly property string mode: settings ? settings.themeMode : "dark"
    readonly property string accentName: settings ? settings.accentColor : "rose"
    readonly property bool amoled: mode === "amoled"
    readonly property bool neutral: mode === "charcoal" || amoled
    readonly property color backgroundColor: amoled ? "#000000" : neutral ? "#18181b" : "#1a1a2e"
    readonly property color whiteColor: "#ffffff"
    readonly property color surfaceColor: amoled ? "#000000" : neutral ? "#202024" : "#16213e"
    readonly property color cardColor: amoled ? "#090909" : neutral ? "#27272c" : "#1f2940"
    readonly property color cardHoverColor: amoled ? "#161616" : neutral ? "#323238" : "#283553"
    readonly property var accentOptions: [
        { key: "rose", label: qsTr("Rose"), color: "#e94560" },
        { key: "blue", label: qsTr("Blue"), color: "#397bdc" },
        { key: "purple", label: qsTr("Purple"), color: "#8055cc" },
        { key: "teal", label: qsTr("Teal"), color: "#167e78" },
        { key: "orange", label: qsTr("Orange"), color: "#f97316" },
        { key: "cyan", label: qsTr("Cyan"), color: "#0891b2" },
        { key: "pink", label: qsTr("Pink"), color: "#db2777" }
    ]
    readonly property color accentColor: {
        for (var i = 0; i < accentOptions.length; ++i) {
            if (accentOptions[i].key === accentName)
                return accentOptions[i].color;
        }
        return accentOptions[0].color;
    }
    readonly property color primaryColor: accentColor
    readonly property color primaryHoverColor: accentName === "rose" ? "#ff6b6b" : Qt.lighter(accentColor, 1.25)
    readonly property color secondaryColor: neutral ? "#303036" : "#0f3460"
    readonly property color highlightedColor: accentName === "rose" ? "#1c62b8" : Qt.lighter(accentColor, 1.35)
    readonly property color recordingGridBackgroundColor: withOpacity(cardColor, 0.294)
    readonly property color textColor: "#eaeaea"
    readonly property color textSecondaryColor: "#a0a0a0"
    readonly property color textMutedColor: "#6c6c6c"
    readonly property color borderColor: neutral ? "#38383f" : "#2d3a4f"
    readonly property color dividerColor: neutral ? "#29292e" : "#252a3a"
    readonly property color successColor: "#4ade80"
    readonly property color warningColor: "#fbbf24"
    readonly property color cautionColor: "#fb923c"
    readonly property color errorColor: "#ef4444"
    readonly property color recordingColor: "#ef4444"
    readonly property color recordingPulseColor: "#ff6b6b"
    // Gradients
    readonly property var
    primaryGradient: Gradient {
        GradientStop {
            position: 0
            color: theme.primaryColor
        }

        GradientStop {
            position: 1
            color: theme.primaryHoverColor
        }

    }

    // Typography
    readonly property int fontSizeTiny: 10
    readonly property int fontSizeSmall: 12
    readonly property int fontSizeNormal: 14
    readonly property int fontSizeMedium: 16
    readonly property int fontSizeLarge: 20
    readonly property int fontSizeXLarge: 24
    readonly property int fontSizeTitle: 32
    // Spacing
    readonly property int spacingTiny: 4
    readonly property int spacingSmall: 8
    readonly property int spacingNormal: 12
    readonly property int spacingMedium: 16
    readonly property int spacingLarge: 24
    readonly property int spacingXLarge: 32
    // Border radius
    readonly property int radiusSmall: 4
    readonly property int radiusNormal: 8
    readonly property int radiusMedium: 12
    readonly property int radiusLarge: 16
    readonly property int radiusRound: 9999
    // Shadows
    readonly property color shadowColor: "#40000000"
    // Animation durations
    readonly property int animationFast: 100
    readonly property int animationNormal: 200
    readonly property int animationSlow: 300
    readonly property int animationPulse: 800
    // Navigation rail
    readonly property int navRailWidth: 72
    readonly property int navRailExpandedWidth: 200
    // Card sizes
    readonly property int recordingCardWidth: 340
    readonly property int recordingCardHeight: 248

    // Helper functions
    function withOpacity(color, opacity) {
        return Qt.rgba(color.r, color.g, color.b, opacity);
    }

}
