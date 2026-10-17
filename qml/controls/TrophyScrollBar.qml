import "../styles"
import QtQuick
import QtQuick.Controls

ScrollBar {
    id: control

    policy: ScrollBar.AsNeeded
    hoverEnabled: true
    padding: 2
    minimumSize: 0.05
    implicitWidth: 8
    implicitHeight: 8

    contentItem: Rectangle {
        implicitWidth: 4
        implicitHeight: 4
        radius: Math.min(width, height) / 2
        color: control.hovered || control.pressed ? Theme.highlightedColor : Theme.secondaryColor
        opacity: control.active || control.hovered || control.pressed ? 1.0 : 0.25
        Behavior on opacity { NumberAnimation { duration: 200 } }
    }

    background: Item {}
}
