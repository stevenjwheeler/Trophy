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

ListView {
    id: root

    property int skip: 0

    spacing: Theme.spacingSmall
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    model: {
        var full = (typeof recordingLibrary !== "undefined" && recordingLibrary) ? recordingLibrary.recordings : [];
        if (skip > 0 && full.length > skip) {
            return full.slice(skip);
        }
        return full;
    }

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
