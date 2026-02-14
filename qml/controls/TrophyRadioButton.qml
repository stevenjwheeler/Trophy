import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RadioButton {
    id: control

    // Internal color guards
    readonly property color _accent: Theme.accentColor || "#3498db"
    readonly property color _text: Theme.textColor || "#ffffff"

    // The text label
    contentItem: Label {
        text: control.text
        font: control.font
        color: control.down ? _accent : _text
        verticalAlignment: Text.AlignVCenter
        leftPadding: control.indicator.width + control.spacing

        Behavior on color {
            ColorAnimation {
                duration: 100
            }

        }

    }

    // The circle indicator
    indicator: Rectangle {
        implicitWidth: 20
        implicitHeight: 20
        x: control.leftPadding
        y: parent.height / 2 - height / 2
        radius: width / 2
        color: "transparent"
        border.color: control.checked ? _accent : Qt.rgba(_text.r, _text.g, _text.b, 0.3)
        border.width: control.checked ? 2 : 1

        // The "Inner Dot" that pops in when checked
        Rectangle {
            id: innerDot

            width: 10
            height: 10
            x: (parent.width - width) / 2
            y: (parent.height - height) / 2
            radius: width / 2
            color: _accent
            scale: control.checked ? 1 : 0
            opacity: control.checked ? 1 : 0

            Behavior on scale {
                NumberAnimation {
                    duration: 200
                    easing.type: Easing.OutBack // Gives it a slight "pop"
                }

            }

            Behavior on opacity {
                NumberAnimation {
                    duration: 150
                }

            }

        }

        // Hover effect glow
        Rectangle {
            anchors.fill: parent
            anchors.margins: -4
            radius: width / 2
            color: Qt.rgba(_accent.r, _accent.g, _accent.b, 0.1)
            visible: control.hovered
            z: -1
        }

        Behavior on border.color {
            ColorAnimation {
                duration: 150
            }

        }

    }

}
