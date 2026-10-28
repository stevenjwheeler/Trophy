import "../styles"
import QtQuick
import QtQuick.Effects
import QtQuick.Templates as T

T.Button {
    id: control

    property string iconSource: ""
    property int iconSize: 16
    property color hoverColor: "transparent"
    property color hoverIconColor: Theme.textColor
    property bool isCloseButton: false

    width: 46
    height: 32

    // All glyphs are white-stroked SVGs on the same 24-grid,
    // recolored here so every button shares one size, stroke
    // weight and theme-aware color. (Font glyphs and color
    // emoji can't promise that: each renders at its own
    // metrics, and emoji ignore text color entirely.)
    contentItem: Item {
        Image {
            id: iconImage
            anchors.centerIn: parent
            width: control.iconSize
            height: control.iconSize
            source: control.iconSource
            sourceSize: Qt.size(control.iconSize, control.iconSize)
            smooth: true
            visible: false
        }

        MultiEffect {
            anchors.centerIn: parent
            width: control.iconSize
            height: control.iconSize
            source: iconImage
            colorization: 1.0
            // Switches to white if it's the close button being hovered
            colorizationColor: (control.hovered && control.isCloseButton) ? control.hoverIconColor : Theme.textSecondaryColor

            Behavior on colorizationColor {
                ColorAnimation {
                    duration: 150
                }

            }

        }

    }

    background: Rectangle {
        // Top-right radius only for the close button to match the window frame
        topRightRadius: control.isCloseButton ? 12 : 0
        color: control.pressed ? Qt.darker(control.hoverColor, 1.2) : (control.hovered ? control.hoverColor : "transparent")

        Behavior on color {
            ColorAnimation {
                duration: 150
            }

        }

    }

}
