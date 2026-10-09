import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Templates as T

T.Button {
    id: control

    // 1. Base color logic: Default to subtle background, or accent if checked
    property color baseColor: control.checked ? (Theme.accentColor || "#3498db") : Theme.withOpacity(Theme.textColor || "#ffffff", 0.05)
    // Outline toggle style (tool look): the fill never goes solid —
    // checked/hover read through the border + tint instead of a full
    // accent fill, so toggles don't mimic action buttons.
    property bool outlined: false
    // 2. ONLY turn text/icon white if the button is Checked or Pressed
    // Removed 'hovered' from here so icons don't flip to white just by touching them
    readonly property bool isBright: checked || pressed
    readonly property color _text: Theme.textColor || "#ffffff"
    property real radius: Theme.radiusSmall

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)
    padding: 8
    horizontalPadding: 16
    font.pixelSize: Theme.fontSizeNormal
    font.weight: Font.Medium

    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: contentRow.implicitHeight

        Row {
            id: contentRow

            anchors.centerIn: parent
            spacing: 6

            Item {
                width: control.icon.source.toString() !== "" ? (control.icon.width || 20) : 0
                height: control.icon.height || 20
                visible: control.icon.source.toString() !== ""
                anchors.verticalCenter: parent.verticalCenter

                Image {
                    id: img

                    anchors.fill: parent
                    source: control.icon.source
                    sourceSize.width: width
                    sourceSize.height: height
                    fillMode: Image.PreserveAspectFit
                    opacity: control.isBright ? 1 : 0.7
                }

                // Color overlay - when isBright, use white; otherwise use text color
                Rectangle {
                    anchors.fill: img
                    color: control.isBright ? "#ffffff" : _text
                    opacity: 0
                    visible: false
                }

            }

            Text {
                text: control.text
                font: control.font
                color: control.isBright ? "#ffffff" : _text
                visible: control.text !== ""
                anchors.verticalCenter: parent.verticalCenter

                Behavior on color {
                    ColorAnimation {
                        duration: 100
                    }

                }

            }

        }

    }

    background: Rectangle {
        implicitWidth: 32
        implicitHeight: 32
        radius: control.radius
        border.width: control.outlined && (control.checked || control.hovered) ? (control.checked ? 1.5 : 1) : 0
        border.color: {
            if (!control.outlined)
                return "transparent";
            let accent = Theme.accentColor || "#3498db";
            if (control.checked)
                return accent;
            if (control.hovered)
                return Qt.rgba(accent.r, accent.g, accent.b, 0.5);
            return Qt.rgba(control._text.r, control._text.g, control._text.b, 0.15);
        }
        color: {
            if (control.outlined) {
                let accentFill = Theme.accentColor || "#3498db";
                if (control.pressed)
                    return Qt.rgba(accentFill.r, accentFill.g, accentFill.b, 0.22);
                if (control.checked)
                    return Qt.rgba(accentFill.r, accentFill.g, accentFill.b, 0.12);
                if (control.hovered)
                    return Theme.withOpacity(Theme.textColor, 0.12);
                return Theme.withOpacity(Theme.textColor, 0.05);
            }
            let isSolid = control.checked || control.baseColor != Theme.withOpacity(Theme.textColor, 0.05);
            if (isSolid) {
                if (control.pressed)
                    return Qt.darker(control.baseColor, 1.4);

                if (control.hovered)
                    return Qt.lighter(control.baseColor, 1.4);

                return control.baseColor;
            } else {
                if (control.pressed)
                    return Theme.withOpacity(Theme.textColor, 0.2);

                if (control.hovered)
                    return Theme.withOpacity(Theme.textColor, 0.12);

                return Theme.withOpacity(Theme.textColor, 0.05);
            }
        }

        Behavior on border.width {
            NumberAnimation {
                duration: 50
            }

        }

        Behavior on color {
            ColorAnimation {
                duration: 120
            }

        }

    }

}
