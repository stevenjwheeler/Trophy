import "../styles"
import QtQuick
import QtQuick.Controls

TextField {
    id: control

    // Internal color guards for performance and safety
    readonly property color _accent: Theme.accentColor || "#3498db"
    readonly property color _text: Theme.textColor || "#ffffff"
    readonly property color _card: Theme.cardColor || "#2a2a2a"

    implicitWidth: 200
    implicitHeight: 32 // Consistent with your Buttons and ComboBoxes
    leftPadding: 10
    rightPadding: 10
    verticalAlignment: TextInput.AlignVCenter
    color: _text
    selectionColor: _accent
    selectedTextColor: "#ffffff"
    font.pixelSize: Theme.fontSizeNormal
    placeholderTextColor: Theme.textMutedColor

    background: Rectangle {
        radius: Theme.radiusNormal
        color: control._card
        // Permanent border that glows on focus
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

}
