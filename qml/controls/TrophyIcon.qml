import "../styles"
import QtQuick
import QtQuick.Effects

// Bundled monochrome artwork, tinted consistently with the surrounding UI.
Item {
    id: root

    property alias source: image.source
    property color color: Theme.textColor

    implicitWidth: 20
    implicitHeight: 20

    Image {
        id: image
        anchors.fill: parent
        sourceSize: Qt.size(width, height)
        fillMode: Image.PreserveAspectFit
        visible: false
    }

    MultiEffect {
        anchors.fill: parent
        source: image
        colorization: 1.0
        colorizationColor: root.color
    }
}
