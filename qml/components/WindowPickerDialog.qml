import "../controls"
import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root

    signal windowSelected(var windowId, string title)

    modal: true
    width: 500
    height: 600
    anchors.centerIn: Overlay.overlay
    onOpened: {
        if (typeof windowService !== "undefined" && windowService)
            listView.model = windowService.getVisibleWindows();

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
            Layout.fillWidth: true
            height: 1
            color: Theme.dividerColor
        }

        // List
        ListView {
            id: listView

            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: Theme.spacingSmall
            clip: true
            model: (typeof windowService !== "undefined" && windowService) ? windowService.getVisibleWindows() : []
            spacing: 2

            delegate: ItemDelegate {
                width: listView.width
                height: 50
                padding: 0
                onClicked: {
                    root.windowSelected(modelData.id, modelData.title);
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
                            text: modelData.title || qsTr("Untitled Window")
                            color: Theme.textColor
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                        Label {
                            text: modelData.class || ""
                            color: Theme.textMutedColor
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }

                    }

                }

                background: Rectangle {
                    color: parent.highlighted ? Theme.withOpacity(Theme.primaryColor, 0.2) : (parent.hovered ? Theme.cardHoverColor : "transparent")
                    radius: Theme.radiusSmall
                }

            }

            ScrollBar.vertical: TrophyScrollBar {}

        }

        // Divider
        Rectangle {
            Layout.fillWidth: true
            height: 1
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
