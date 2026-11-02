import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: sectionCard
    property string sectionTitle: ""
    property url iconSource
    property string description: ""
    property color descriptionColor: Theme.textSecondaryColor
    property bool expanded: false
    signal expansionFinished()
    default property alias content: body.data
    readonly property real expansionProgress: revealProgress
    property real revealProgress: expanded ? 1 : 0

    Behavior on revealProgress {
        NumberAnimation {
            duration: Theme.animationNormal
            easing.type: Easing.InOutCubic
            onRunningChanged: {
                if (!running && sectionCard.expanded)
                    sectionCard.expansionFinished();
            }
        }
    }

    Layout.fillWidth: true
    implicitHeight: cardCol.implicitHeight + Theme.spacingMedium * 2
    radius: Theme.radiusMedium
    color: Theme.cardColor
    border.width: 1
    border.color: Theme.borderColor
    clip: true

    MouseArea {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: sectionCard.expanded ? cardCol.y + sectionHeader.height : parent.height
        cursorShape: Qt.PointingHandCursor
        onClicked: sectionCard.expanded = !sectionCard.expanded
    }

    ColumnLayout {
        id: cardCol
        // Never constrain the natural body height to the animated card height:
        // that would feed layout changes back into the animation's target.
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.spacingMedium
        spacing: 0

        RowLayout {
            id: sectionHeader
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            TrophyIcon {
                source: sectionCard.iconSource
                color: Theme.textSecondaryColor
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    text: sectionCard.sectionTitle
                    font.pixelSize: Theme.fontSizeLarge
                    font.weight: Font.Medium
                    color: Theme.textColor
                }
                Label {
                    Layout.fillWidth: true
                    text: sectionCard.description
                    font.pixelSize: Theme.fontSizeSmall
                    color: sectionCard.descriptionColor
                    wrapMode: Text.WordWrap
                    visible: text.length > 0
                }
            }

            Label {
                text: "›"
                font.pixelSize: 26
                color: Theme.textSecondaryColor
                rotation: sectionCard.revealProgress * 90
            }
        }

        Item {
            id: reveal
            Layout.fillWidth: true
            implicitHeight: sectionCard.revealProgress * (body.implicitHeight + Theme.spacingSmall * 2 + 1)
            visible: sectionCard.revealProgress > 0
            enabled: sectionCard.expanded
            opacity: sectionCard.revealProgress
            clip: true

            Rectangle {
                id: divider
                anchors.top: parent.top
                anchors.topMargin: Theme.spacingSmall
                width: parent.width
                height: 1
                color: Theme.dividerColor
            }

            ColumnLayout {
                id: body
                anchors.top: divider.bottom
                anchors.topMargin: Theme.spacingSmall
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: Theme.spacingMedium
            }
        }
    }
}
