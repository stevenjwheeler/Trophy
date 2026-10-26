import "../styles"
import QtQuick
import QtQuick.Controls

Switch {
    id: control

    // Internal color guards
    readonly property color _accent: Theme.accentColor || "#3498db"
    readonly property color _text: Theme.textColor || "#ffffff"
    readonly property color _card: Theme.cardColor || "#2a2a2a"

    implicitWidth: 44
    implicitHeight: 22
    padding: 0

    // The Handle (The sliding circle)
    indicator: Rectangle {
        implicitWidth: 44
        implicitHeight: 22
        x: control.leftPadding
        y: parent.height / 2 - height / 2
        radius: 11
        color: control.checked ? control._accent : Qt.rgba(control._text.r, control._text.g, control._text.b, 0.1)
        border.color: control.hovered ? control._accent : "transparent"
        border.width: 1

        // The "Knob"
        Rectangle {
            id: knob

            x: control.checked ? parent.width - width - 3 : 3
            y: (parent.height - height) / 2
            width: 16
            height: 16
            radius: 8
            color: control.checked ? "#ffffff" : control._text
            // Slight size pulse when clicked
            scale: control.pressed ? 0.9 : 1

            // Smooth sliding animation
            Behavior on x {
                NumberAnimation {
                    duration: 200
                    easing.type: Easing.OutCubic
                }

            }

            Behavior on scale {
                NumberAnimation {
                    duration: 100
                }

            }

        }

        Behavior on color {
            ColorAnimation {
                duration: 200
            }

        }

    }

    // Adjusting the label position to match our style
    contentItem: Label {
        text: control.text
        font: control.font
        color: control.checked ? control._text : Theme.textMutedColor
        verticalAlignment: Text.AlignVCenter
        leftPadding: control.indicator.width + control.spacing
    }

}
