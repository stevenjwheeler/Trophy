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
    width: Math.min(560, parent ? parent.width - 32 : 560)
    implicitHeight: body.implicitHeight + topPadding + bottomPadding
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
            text: wizard.title
            font.pixelSize: Theme.fontSizeLarge
            font.bold: true
            color: Theme.textColor
            Layout.fillWidth: true
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondaryColor
            text: !wizard.started
                ? qsTr("1. Select an animated window or screen in the sharing dialog.\n2. Keep it moving while Trophy tests up to 90 frames (about 3–8 seconds).\n3. Review the delivered buffer layout and encode/decode results.")
                : (wizard.running ? wizard.tester.stage : (wizard.report.summary || ""))
            textFormat: Text.PlainText
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMutedColor
            font.pixelSize: Theme.fontSizeSmall
            text: qsTr("Uses the first working H.264 hardware encoder, independently of saved settings. No audio or saved video; no uploads or saved permissions. Stop recording and Background Clips first; a new capture cancels this test.")
        }
        BusyIndicator {
            running: wizard.running
            visible: running
            Layout.alignment: Qt.AlignHCenter
        }
        Label {
            objectName: "dmaBufTestResults"
            visible: wizard.started && !wizard.running && wizard.report.status !== undefined
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: wizard.report.status === "passed" ? Theme.successColor : Theme.textSecondaryColor
            text: qsTr("Result: %1\nEncoder: %2\nDelivered: %3 DMA-BUF / %4 shared-memory frames\nGPU-imported: %5 single-plane / %6 multi-plane\nPaths: %7 direct mapping / %8 EGL + GPU copy / %9 VA-API conversion\nAccepted / decoded: %10 / %11\nCPU fallback / encoder failures: %12 / %13")
                .arg(wizard.report.status || "").arg(wizard.report.encoder || qsTr("None"))
                .arg(wizard.report.dmaFrames || 0).arg(wizard.report.shmFrames || 0)
                .arg(wizard.report.singlePlane || 0).arg(wizard.report.multiPlane || 0)
                .arg(wizard.report.direct || 0).arg(wizard.report.egl || 0).arg(wizard.report.vpp || 0)
                .arg(wizard.report.accepted || 0).arg(wizard.report.decoded || 0)
                .arg(wizard.report.fallback || 0).arg(wizard.report.dropped || 0)
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textMutedColor
            font.pixelSize: Theme.fontSizeSmall
            text: qsTr("Only the layout delivered by your compositor is tested; an unobserved layout is not a failure. A clean decode is not a visual-quality or performance guarantee. EGL/GPU-copy and conversion paths are not literal zero-copy.")
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            TrophyButton {
                text: wizard.running ? qsTr("Cancel") : qsTr("Close")
                onClicked: wizard.close()
            }
            TrophyButton {
                objectName: "startDmaBufTest"
                text: wizard.started ? qsTr("Test Again") : qsTr("Start Test")
                enabled: wizard.tester !== null && wizard.tester.available && !wizard.running
                onClicked: {
                    wizard.started = true
                    wizard.tester.start()
                }
            }
        }
    }
}
