import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root

    signal windowSelected(var windowId, string title)

    title: qsTr("Select Window to Record")
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
        radius: Theme.radiusMedium
        border.color: Theme.borderColor
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        Label {
            text: qsTr("Choose a window from the list below:")
            color: Theme.textSecondaryColor
            font.pixelSize: Theme.fontSizeNormal
        }

        ListView {
            id: listView

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: (typeof windowService !== "undefined" && windowService) ? windowService.getVisibleWindows() : []
            spacing: 4

            delegate: ItemDelegate {
                width: listView.width
                height: 50
                onClicked: {
                    root.windowSelected(modelData.id, modelData.title);
                    root.close();
                }

                contentItem: RowLayout {
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
                    color: parent.highlighted ? Theme.primaryColor : (parent.hovered ? Theme.cardHoverColor : "transparent")
                    radius: Theme.radiusSmall
                    opacity: parent.highlighted ? 0.2 : 1
                }

            }

            ScrollIndicator.vertical: ScrollIndicator {
            }

        }

        Button {
            Layout.alignment: Qt.AlignRight
            text: qsTr("Cancel")
            onClicked: root.close()
        }

    }

}
