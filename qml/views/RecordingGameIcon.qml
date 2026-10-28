/* SPDX-License-Identifier: GPL-3.0-or-later */
import QtQuick

Image {
    id: root
    // Explicit dependency; this default preserves engine/creation-context hosts.
    property var detector: typeof gameDetector !== "undefined" ? gameDetector : null // qmllint disable unqualified
    property string gameName: ""
    property int modelRevision: 0
    source: {
        // Reading the revision keeps model-reset invalidation in this binding.
        const revision = root.modelRevision;
        if (root.gameName === "Manual Recording")
            return "qrc:/icons/manual-recording.svg";
        return root.detector !== null ? root.detector.iconSourceForGame(root.gameName) : "";
    }
    asynchronous: true
    sourceSize.width: width
    sourceSize.height: height
    fillMode: Image.PreserveAspectFit
    visible: status === Image.Ready

    Connections {
        target: root.detector !== null ? root.detector.gamesModel : null
        function onModelReset() { root.modelRevision++; }
    }
}
