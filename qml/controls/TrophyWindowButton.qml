import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T

T.Button {
    id: control

    property string symbol: ""
    property color hoverColor: "transparent"
    property color hoverIconColor: Theme.textColor
    property bool isCloseButton: false

    width: 46
    height: 32

    contentItem: Label {
        text: control.symbol
        font.pixelSize: 14
        font.family: "Inter, Segoe UI, sans-serif"
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        // Switches to white if it's the close button being hovered
        color: (control.hovered && control.isCloseButton) ? control.hoverIconColor : Theme.textSecondaryColor

        Behavior on color {
            ColorAnimation {
                duration: 150
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
