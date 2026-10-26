pragma ComponentBehavior: Bound
import "../controls"
import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    // Explicit dependency; this default preserves engine/creation-context hosts.
    property var windowSource: typeof windowService !== "undefined" ? windowService : null // qmllint disable unqualified

    signal windowSelected(var windowId, string title)

    modal: true
    width: 500
    height: 600
    anchors.centerIn: Overlay.overlay
    onOpened: {
        if (root.windowSource)
            listView.model = root.windowSource.getVisibleWindows();

    }

    background: Rectangle {
        color: Theme.cardColor
        radius: Theme.radiusLarge
        border.color: Theme.withOpacity(Theme.textColor, 0.1)
        border.width: 1
    }

    header: Item {
    }

    contentItem: ColumnLayout {
        spacing: 0

        // Title
        Label {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingLarge
            Layout.bottomMargin: Theme.spacingSmall
            text: qsTr("Select Window to Record")
            font.pixelSize: Theme.fontSizeLarge
            font.weight: Font.Bold
            color: Theme.textColor
        }

        // Divider
        Rectangle {
            objectName: "windowPickerTopDivider"
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.dividerColor
        }

        // List
        ListView {
            id: listView

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacingSmall
            clip: true
            model: root.windowSource ? root.windowSource.getVisibleWindows() : []
            spacing: 2

            delegate: ItemDelegate {
                id: windowDelegate
                required property var modelData
                width: listView.width
                height: 50
                padding: 0
                onClicked: {
                    root.windowSelected(windowDelegate.modelData.id, windowDelegate.modelData.title);
                    root.close();
                }

                contentItem: RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: Theme.spacingMedium
                    spacing: Theme.spacingSmall

                    ColumnLayout {
                        spacing: 2

                        Label {
                            text: windowDelegate.modelData.title || qsTr("Untitled Window")
                            color: Theme.textColor
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        Label {
                            text: windowDelegate.modelData.class || ""
                            color: Theme.textMutedColor
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                    }

                }

                background: Rectangle {
                    color: windowDelegate.highlighted ? Theme.withOpacity(Theme.primaryColor, 0.2) : (windowDelegate.hovered ? Theme.cardHoverColor : "transparent")
                    radius: Theme.radiusSmall
                }

            }

            ScrollBar.vertical: TrophyScrollBar {}

        }

        // Divider
        Rectangle {
            objectName: "windowPickerBottomDivider"
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.dividerColor
        }

        // Footer
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spacingLarge
            Layout.topMargin: Theme.spacingMedium
            Layout.bottomMargin: Theme.spacingMedium

            Item {
                Layout.fillWidth: true
            }

            TrophyButton {
                text: qsTr("Cancel")
                onClicked: root.close()
            }

        }

    }

}
