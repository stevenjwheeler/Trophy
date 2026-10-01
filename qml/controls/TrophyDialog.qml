import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Templates as T

T.Dialog {
    id: control

    property string message: ""
    property bool showDontAsk: false
    property alias dontAskChecked: dontAskCheckbox.checked
    property string confirmText: qsTr("Confirm")
    property string cancelText: qsTr("Cancel")
    property color confirmColor: Theme.accentColor || "#3498db"
    readonly property color _text: Theme.textColor || "#ffffff"
    readonly property color _textMuted: Theme.textMutedColor || "#aaaaaa"
    readonly property color _accent: Theme.accentColor || "#3498db"

    modal: true
    anchors.centerIn: parent
    width: 400
    implicitHeight: mainLayout.implicitHeight

    // Dimmed backdrop overlay
    Overlay.modal: Rectangle {
        color: Theme.withOpacity("#000000", 0.5)
        Behavior on opacity {
            NumberAnimation { duration: Theme.animationFast }
        }
    }

    background: Rectangle {
        color: Theme.cardColor || "#212121"
        radius: Theme.radiusLarge
        border.color: Theme.withOpacity(_text, 0.1)
        border.width: 1
    }

    // Open/close animation
    enter: Transition {
        NumberAnimation {
            property: "opacity"
            from: 0
            to: 1
            duration: Theme.animationNormal
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            property: "scale"
            from: 0.92
            to: 1
            duration: Theme.animationNormal
            easing.type: Easing.OutCubic
        }
    }

    exit: Transition {
        NumberAnimation {
            property: "opacity"
            from: 1
            to: 0
            duration: Theme.animationFast
            easing.type: Easing.InCubic
        }
        NumberAnimation {
            property: "scale"
            from: 1
            to: 0.95
            duration: Theme.animationFast
            easing.type: Easing.InCubic
        }
    }

    contentItem: ColumnLayout {
        id: mainLayout

        spacing: 0

        // Title
        Label {
            text: control.title
            font.pixelSize: Theme.fontSizeLarge
            font.weight: Font.Bold
            Layout.fillWidth: true
            Layout.margins: Theme.spacingLarge
            Layout.bottomMargin: 0
            color: _text
        }

        // Message
        Label {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingLarge
            Layout.topMargin: Theme.spacingMedium
            text: control.message
            wrapMode: Text.WordWrap
            color: _textMuted
            lineHeight: 1.2
            font.pixelSize: Theme.fontSizeNormal
        }

        // Action Row
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingLarge
            Layout.topMargin: Theme.spacingMedium
            spacing: 0

            CheckBox {
                id: dontAskCheckbox

                text: qsTr("Don't ask again")
                visible: control.showDontAsk
                // Fix vertical alignment: explicitly set height and remove default padding
                padding: 0
                spacing: 8

                indicator: Rectangle {
                    implicitWidth: 20
                    implicitHeight: 20
                    // Ensures the box is vertically centered relative to the text
                    y: (dontAskCheckbox.availableHeight - height) / 2
                    radius: 6
                    // SAFE COLORS: Use the local _accent property
                    color: dontAskCheckbox.checked ? _accent : "transparent"
                    border.color: dontAskCheckbox.checked ? _accent : Theme.withOpacity(_text, 0.2)

                    Text {
                        text: "✓"
                        anchors.centerIn: parent
                        color: "white"
                        font.bold: true
                        visible: dontAskCheckbox.checked
                    }

                    Behavior on color {
                        ColorAnimation {
                            duration: 100
                        }
                    }

                }

                contentItem: Text {
                    text: dontAskCheckbox.text
                    font.pixelSize: Theme.fontSizeSmall
                    color: _textMuted
                    // Add padding to make room for the indicator
                    leftPadding: dontAskCheckbox.indicator.width + dontAskCheckbox.spacing
                    verticalAlignment: Text.AlignVCenter
                    // Ensure the text takes up the same height as the indicator for centering
                    height: dontAskCheckbox.indicator.height
                }

            }

            Item {
                Layout.fillWidth: true
            }

            RowLayout {
                spacing: Theme.spacingMedium

                TrophyButton {
                    text: control.cancelText
                    visible: control.cancelText.length > 0
                    onClicked: control.reject()
                }

                TrophyButton {
                    text: control.confirmText
                    baseColor: control.confirmColor
                    onClicked: control.accept()
                }

            }

        }

    }

}
