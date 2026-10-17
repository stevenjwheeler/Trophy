/* SPDX-License-Identifier: GPL-3.0-or-later */
import QtQuick

Image {
    id: root
    property string gameName: ""
    property int modelRevision: 0
    source: {
        root.modelRevision;
        if (root.gameName === "Manual Recording")
            return "qrc:/icons/manual-recording.svg";
        return typeof gameDetector !== "undefined" && gameDetector !== null
                ? gameDetector.iconSourceForGame(root.gameName) : "";
    }
    asynchronous: true
    sourceSize.width: width
    sourceSize.height: height
    fillMode: Image.PreserveAspectFit
    visible: status === Image.Ready

    Connections {
        target: typeof gameDetector !== "undefined" && gameDetector !== null
                ? gameDetector.gamesModel : null
        function onModelReset() { root.modelRevision++; }
    }
}
