/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
pragma Singleton

QtObject {
    // Color palette - Dark theme
    readonly property color backgroundColor: "#1a1a2e"
    readonly property color whiteColor: "#ffffff"
    readonly property color surfaceColor: "#16213e"
    readonly property color cardColor: "#1f2940"
    readonly property color cardHoverColor: "#283553"
    readonly property color primaryColor: "#e94560"
    readonly property color primaryHoverColor: "#ff6b6b"
    readonly property color secondaryColor: "#0f3460"
    readonly property color highlightedColor: '#1c62b8'
    readonly property color recordingGridBackgroundColor: '#4b1f2940'
    readonly property color textColor: "#eaeaea"
    readonly property color textSecondaryColor: "#a0a0a0"
    readonly property color textMutedColor: "#6c6c6c"
    readonly property color borderColor: "#2d3a4f"
    readonly property color dividerColor: "#252a3a"
    readonly property color successColor: "#4ade80"
    readonly property color warningColor: "#fbbf24"
    readonly property color errorColor: "#ef4444"
    readonly property color recordingColor: "#ef4444"
    readonly property color recordingPulseColor: "#ff6b6b"
    // Gradients
    readonly property var
    primaryGradient: Gradient {
        GradientStop {
            position: 0
            color: "#e94560"
        }

        GradientStop {
            position: 1
            color: "#ff6b6b"
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
    readonly property int recordingCardWidth: 280
    readonly property int recordingCardHeight: 200

    // Helper functions
    function withOpacity(color, opacity) {
        return Qt.rgba(color.r, color.g, color.b, opacity);
    }

}
