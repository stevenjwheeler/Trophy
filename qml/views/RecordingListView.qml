/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GameRecorder

ListView {
    id: root

    spacing: Theme.spacingSmall
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    model: (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.recordings : []

    delegate: RecordingListItem {
        recording: modelData
    }

    footer: Item {
        height: Theme.spacingLarge
    }

    ScrollBar.vertical: ScrollBar {
        policy: ScrollBar.AsNeeded
    }
}

