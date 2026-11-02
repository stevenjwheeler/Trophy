/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import "../styles"
import QtQuick
import QtQuick.Controls

// Scrollable themed rich text, used for Markdown release notes.
// The HTML is produced by TrophyBackend::releaseNotesHtml, which has
// already escaped the source and limited links to safe schemes.
Item {
    id: root

    property string html: ""
    // Long notes scroll instead of stretching the dialog off-screen.
    property int maximumHeight: 320

    readonly property real contentHeight: notesText.implicitHeight
    implicitHeight: Math.min(contentHeight, maximumHeight)
    implicitWidth: 200

    signal linkActivated(string link)

    Flickable {
        id: view
        objectName: "releaseNotesFlickable"
        anchors.fill: parent
        contentWidth: width
        contentHeight: root.contentHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

        ScrollBar.vertical: TrophyScrollBar {}

        Text {
            id: notesText
            objectName: "releaseNotesText"
            // Keep a constant gutter so adding the scrollbar cannot
            // reflow the text and toggle the bar on and off.
            width: view.width - 12
            text: root.html
            textFormat: Text.RichText
            color: Theme.textColor
            font.pixelSize: Theme.fontSizeNormal
            wrapMode: Text.WordWrap
            lineHeight: 1.25

            HoverHandler {
                cursorShape: notesText.hoveredLink !== ""
                             ? Qt.PointingHandCursor : Qt.ArrowCursor
            }

            onLinkActivated: (link) => root.linkActivated(link)
        }
    }
}
