pragma ComponentBehavior: Bound
import "../styles"
import QtQuick
import QtQuick.Controls

ComboBox {
    id: control

    // Internal helper to prevent the "r of undefined" crash
    readonly property color _accent: Theme.accentColor || "#3498db"
    readonly property color _text: Theme.textColor || "#ffffff"

    implicitWidth: 200
    implicitHeight: 32
    hoverEnabled: true

    contentItem: Label {
        leftPadding: 10
        rightPadding: control.indicator.width + control.spacing
        text: control.displayText
        font: control.font
        color: control._text
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Label {
        x: control.width - width - 10
        y: (control.height - height) / 2
        text: "▼"
        font.pixelSize: Theme.fontSizeSmall
        color: (control.hovered || control.popup.visible) ? control._accent : Theme.textMutedColor
        rotation: control.popup.visible ? 180 : 0

        Behavior on color {
            ColorAnimation {
                duration: 150
            }

        }

        Behavior on rotation {
            NumberAnimation {
                duration: 200
            }

        }

    }

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 32
        color: Theme.cardColor
        radius: Theme.radiusSmall
        border.width: control.activeFocus ? 2 : 1
        border.color: {
            if (control.activeFocus)
                return control._accent;

            if (control.hovered)
                return Qt.rgba(control._accent.r, control._accent.g, control._accent.b, 0.5);

            return Qt.rgba(control._text.r, control._text.g, control._text.b, 0.15);
        }

        Behavior on border.color {
            ColorAnimation {
                duration: 150
            }

        }

    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight, 240) + 4
        padding: 2

        enter: Transition {
            NumberAnimation {
                property: "opacity"
                from: 0
                to: 1
                duration: 150
            }

            NumberAnimation {
                property: "y"
                from: control.height - 5
                to: control.height + 4
                duration: 150
                easing.type: Easing.OutCubic
            }

        }

        exit: Transition {
            NumberAnimation {
                property: "opacity"
                from: 1
                to: 0
                duration: 100
            }

        }

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex

            ScrollBar.vertical: TrophyScrollBar {}

        }

        background: Rectangle {
            color: Theme.cardColor
            radius: Theme.radiusSmall
            border.width: 1
            border.color: Qt.rgba(control._text.r, control._text.g, control._text.b, 0.2)
        }

    }

    delegate: ItemDelegate {
        id: itemDelegate
        required property var modelData

        width: control.width - 4
        height: 32
        hoverEnabled: true

        contentItem: Label {
            text: itemDelegate.modelData
            color: (itemDelegate.highlighted || itemDelegate.hovered) ? control._accent : control._text
            font: control.font
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            // Using direct Qt.rgba to bypass the Theme function calls during hover
            color: itemDelegate.hovered ? Qt.rgba(control._accent.r, control._accent.g, control._accent.b, 0.1) : itemDelegate.highlighted ? Qt.rgba(control._accent.r, control._accent.g, control._accent.b, 0.05) : "transparent"
            radius: Theme.radiusSmall - 2
        }

    }

}
