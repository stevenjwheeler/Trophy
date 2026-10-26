pragma ComponentBehavior: Bound
import "../styles"
import QtQuick
import QtQuick.Controls

Row {
    id: picker

    required property string selectedKey
    signal colorSelected(string key)
    spacing: Theme.spacingSmall

    Repeater {
        model: Theme.accentOptions

        Button {
            id: swatch
            required property var modelData
            objectName: "accentSwatch_" + modelData.key
            width: 36
            height: 36
            padding: 0
            hoverEnabled: true
            focusPolicy: Qt.StrongFocus
            // Selection comes only from saved settings, never a local toggle.
            readonly property bool selected: picker.selectedKey === modelData.key
            Accessible.name: modelData.label
            Accessible.description: selected ? qsTr("Selected accent color") : qsTr("Use this accent color")
            ToolTip.text: modelData.label
            ToolTip.visible: hovered || activeFocus
            ToolTip.delay: 400
            onClicked: picker.colorSelected(modelData.key)

            background: Rectangle {
                radius: width / 2
                color: "transparent"
                border.width: swatch.selected || swatch.activeFocus ? 2 : 1
                border.color: swatch.selected || swatch.activeFocus ? Theme.textColor
                             : swatch.hovered ? Theme.textSecondaryColor : "transparent"

                Rectangle {
                    anchors.centerIn: parent
                    width: 26
                    height: 26
                    radius: 13
                    color: swatch.modelData.color
                    opacity: swatch.down ? 0.7 : 1
                }
            }

            contentItem: Text {
                text: swatch.selected ? "✓" : ""
                color: Theme.whiteColor
                font.pixelSize: 16
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
        }
    }
}
