import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

TrophyDialog {
    id: wizard
    objectName: "dmaBufTestWizard"
    property var tester: null
    property bool started: false
    readonly property bool running: tester !== null && tester.busy
    readonly property var report: tester !== null ? tester.result : ({})
    title: qsTr("DMA-BUF Tester")
    width: Math.min(600, parent ? parent.width - Theme.spacingLarge * 2 : 600)
    padding: Theme.spacingLarge
    implicitHeight: Math.min(body.implicitHeight + topPadding + bottomPadding,
                             parent ? parent.height - Theme.spacingLarge * 2 : 800)
    closePolicy: Popup.CloseOnEscape
    onOpened: started = running
    onClosed: {
        if (running)
            tester.cancel()
    }

    contentItem: ColumnLayout {
        id: body
        spacing: Theme.spacingMedium

        Label {
            objectName: "dmaBufWizardTitle"
            text: wizard.title
            font.pixelSize: Theme.fontSizeLarge
            font.weight: Font.Bold
            color: Theme.textColor
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }

        ScrollView {
            id: scroll
            objectName: "dmaBufWizardScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 0
            implicitHeight: information.implicitHeight
            contentWidth: availableWidth
            contentHeight: information.implicitHeight
            clip: true
            padding: 0
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: information
                width: scroll.availableWidth
                spacing: Theme.spacingMedium

                ColumnLayout {
                    visible: !wizard.started
                    Layout.fillWidth: true
                    spacing: Theme.spacingMedium

                    Repeater {
                        model: [
                            qsTr("Select an animated window or screen in the sharing dialog."),
                            qsTr("Keep it moving while Trophy tests up to 90 frames (about 3–8 seconds)."),
                            qsTr("Review the delivered buffer layout and encode/decode results.")
                        ]
                        delegate: RowLayout {
                            id: step
                            required property int index
                            required property string modelData
                            Layout.fillWidth: true
                            spacing: Theme.spacingSmall

                            Label {
                                text: String(step.index + 1) + "."
                                font.pixelSize: Theme.fontSizeNormal
                                font.weight: Font.DemiBold
                                color: Theme.accentColor
                                Layout.preferredWidth: 20
                                Layout.alignment: Qt.AlignTop
                            }
                            Label {
                                objectName: "dmaBufWizardStep"
                                text: step.modelData
                                Layout.fillWidth: true
                                font.pixelSize: Theme.fontSizeNormal
                                color: Theme.textSecondaryColor
                                wrapMode: Text.WordWrap
                                lineHeight: 1.2
                            }
                        }
                    }
                }

                Label {
                    objectName: "dmaBufWizardStatus"
                    visible: wizard.started
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: wizard.report.status === "passed" ? Theme.successColor : Theme.textSecondaryColor
                    font.pixelSize: Theme.fontSizeNormal
                    lineHeight: 1.2
                    text: wizard.running ? wizard.tester.stage : (wizard.report.summary || "")
                    textFormat: Text.PlainText
                }
                BusyIndicator {
                    running: wizard.running
                    visible: running
                    Layout.alignment: Qt.AlignHCenter
                }
                Rectangle {
                    visible: wizard.started && !wizard.running && wizard.report.status !== undefined
                    Layout.fillWidth: true
                    implicitHeight: results.implicitHeight + Theme.spacingMedium * 2
                    radius: Theme.radiusSmall
                    color: Theme.withOpacity(Theme.textColor, 0.04)
                    border.color: Theme.borderColor

                    Label {
                        id: results
                        objectName: "dmaBufTestResults"
                        x: Theme.spacingMedium
                        y: Theme.spacingMedium
                        width: parent.width - Theme.spacingMedium * 2
                        wrapMode: Text.WordWrap
                        textFormat: Text.PlainText
                        font.pixelSize: Theme.fontSizeSmall
                        lineHeight: 1.2
                        color: Theme.textSecondaryColor
                        text: qsTr("Result: %1\nEncoder: %2\nDelivered: %3 DMA-BUF / %4 shared-memory frames\nGPU-imported: %5 single-plane / %6 multi-plane\nPaths: %7 direct mapping / %8 EGL + GPU copy / %9 VA-API conversion\nAccepted / decoded: %10 / %11\nCPU fallback / encoder failures: %12 / %13")
                            .arg(wizard.report.status || "").arg(wizard.report.encoder || qsTr("None"))
                            .arg(wizard.report.dmaFrames || 0).arg(wizard.report.shmFrames || 0)
                            .arg(wizard.report.singlePlane || 0).arg(wizard.report.multiPlane || 0)
                            .arg(wizard.report.direct || 0).arg(wizard.report.egl || 0).arg(wizard.report.vpp || 0)
                            .arg(wizard.report.accepted || 0).arg(wizard.report.decoded || 0)
                            .arg(wizard.report.fallback || 0).arg(wizard.report.dropped || 0)
                    }
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    lineHeight: 1.2
                    text: qsTr("Uses the first working H.264 hardware encoder, independently of saved settings. No audio or saved video; no uploads or saved permissions. Stop recording and Background Clips first; a new capture cancels this test.")
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMutedColor
                    font.pixelSize: Theme.fontSizeSmall
                    lineHeight: 1.2
                    text: qsTr("Only the layout delivered by your compositor is tested; an unobserved layout is not a failure. A clean decode is not a visual-quality or performance guarantee. EGL/GPU-copy and conversion paths are not literal zero-copy.")
                }
            }
        }

        RowLayout {
            objectName: "dmaBufWizardActions"
            Layout.fillWidth: true
            spacing: Theme.spacingSmall
            Item { Layout.fillWidth: true }
            TrophyButton {
                text: wizard.running ? qsTr("Cancel") : qsTr("Close")
                onClicked: wizard.close()
            }
            TrophyButton {
                objectName: "startDmaBufTest"
                text: wizard.started ? qsTr("Test Again") : qsTr("Start Test")
                baseColor: Theme.accentColor
                enabled: wizard.tester !== null && wizard.tester.available && !wizard.running
                opacity: enabled ? 1 : 0.5
                onClicked: {
                    wizard.started = true
                    wizard.tester.start()
                }
            }
        }
    }
}
