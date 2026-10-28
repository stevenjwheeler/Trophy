import "../styles"
import QtQuick
import QtQuick.Controls

Slider {
    id: control

    // Internal color guards to prevent the console errors we saw earlier
    readonly property color _accent: Theme.accentColor || "#3498db"
    readonly property color _text: Theme.textColor || "#ffffff"

    implicitHeight: 32

    // The Handle (The knob you slide)
    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height) / 2
        implicitWidth: 16
        implicitHeight: 16
        radius: 8
        color: control.pressed ? control._accent : control._text
        border.color: control._accent
        border.width: control.hovered ? 2 : 0

        Behavior on color {
            ColorAnimation {
                duration: 100
            }

        }

        Behavior on border.width {
            NumberAnimation {
                duration: 100
            }

        }

    }

    // The Track (The line the knob slides on)
    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        implicitWidth: 200
        implicitHeight: 6 // Thicker, modern track
        width: control.availableWidth
        height: implicitHeight
        radius: 3
        color: Qt.rgba(control._text.r, control._text.g, control._text.b, 0.1) // Muted background track

        // The "Progress" part of the track
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            color: control._accent
            radius: 3
        }

    }

}
