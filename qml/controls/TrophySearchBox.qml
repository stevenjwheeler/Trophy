import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property alias text: textField.text
    property alias placeholderText: textField.placeholderText
    property alias searchResults: resultsModel
    property alias dropdown: dropdown
    signal searchTextChanged(string text)
    signal searchAccepted()

    ListModel { id: resultsModel }

    implicitWidth: 300
    implicitHeight: 40

    signal resultSelected(var result)

    function formatDuration(seconds) {
        if (!seconds || seconds <= 0)
            return "00:00";

        var m = Math.floor(seconds / 60);
        var s = seconds % 60;
        return `%1:%2`.arg(m).arg(s.toString().padStart(2, '0'));
    }

    function formatSize(bytes) {
        if (!bytes)
            return "0 B";

        var kb = 1024;
        var mb = kb * 1024;
        var gb = mb * 1024;
        if (bytes >= gb)
            return (bytes / gb).toFixed(1) + " GB";

        if (bytes >= mb)
            return (bytes / mb).toFixed(0) + " MB";

        if (bytes >= kb)
            return (bytes / kb).toFixed(0) + " KB";

        return bytes + " B";
    }

    TrophyTextField {
        id: textField

        anchors.fill: parent
        onTextChanged: {
            root.searchTextChanged(textField.text)
            resultsList.currentIndex = 0
        }
        onAccepted: {
            if (dropdown.visible && resultsList.currentIndex !== -1) {
                var res = resultsModel.get(resultsList.currentIndex);
                root.resultSelected({
                    "type": res.type,
                    "title": res.title,
                    "subtitle": res.subtitle,
                    "index": res.index,
                    "section": res.section || ""
                });
            } else {
                root.searchAccepted();
            }
        }

        Keys.onPressed: function(event) {
            if (dropdown.visible) {
                if (event.key === Qt.Key_Down) {
                    resultsList.currentIndex = Math.min(resultsList.currentIndex + 1, resultsModel.count - 1);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Up) {
                    resultsList.currentIndex = Math.max(resultsList.currentIndex - 1, 0);
                    event.accepted = true;
                } else if (event.key === Qt.Key_Escape) {
                    dropdown.visible = false;
                    event.accepted = true;
                }
            }
        }
    }

    // Search icon
    Image {
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        source: "qrc:/icons/search.svg"
        
        width: 16
        height: 16
        opacity: 0.6
    }

    // Dropdown results
    Popup {
        id: dropdown

        width: root.width
        height: Math.min(resultsList.contentHeight + 21, 300)
        y: root.height + 5
        padding: 0

        background: Rectangle {
            radius: Theme.radiusNormal
            color: Theme.cardColor
            border.color: Theme.borderColor
            border.width: 1
        }

        ListView {
            id: resultsList

            anchors.fill: parent
            anchors.margins: 10
            model: resultsModel
            clip: true
            boundsBehavior: Flickable.DragAndOvershootBounds
            boundsMovement: Flickable.FollowBoundsBehavior

            delegate: Item {
                width: resultsList.width
                height: 50

                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: (mouseArea.containsMouse || resultsList.currentIndex === index) ? Theme.surfaceColor : "transparent"
                    border.color: resultsList.currentIndex === index ? Theme.primaryColor : "transparent"
                    border.width: 1

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.spacingSmall
                        spacing: Theme.spacingSmall

                        // Icon
                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: model.type === "clip" ? Theme.primaryColor : Theme.secondaryColor
                            Layout.alignment: Qt.AlignVCenter
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 2

                            Label {
                                text: model.title
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Font.Medium
                                color: Theme.textColor
                                elide: Text.ElideRight
                            }

                            Label {
                                text: model.subtitle
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.textSecondaryColor
                                elide: Text.ElideRight
                            }

                        }

                    }

                }

                MouseArea {
                    id: mouseArea
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        root.resultSelected({
                            "type": model.type,
                            "title": model.title,
                            "subtitle": model.subtitle,
                            "index": model.index,
                            "section": model.section || ""
                        });
                    }
                }

            }

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
                width: 6
            }

        }

    }


}