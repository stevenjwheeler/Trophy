pragma ComponentBehavior: Bound
import "../controls"
import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

TrophyDialog {
    id: root
    required property var exporter
    property string sourcePath: ""
    property var sourcePaths: []
    readonly property bool batch: sourcePaths.length > 1
    property var captureInfos: []
    property var captureAudio: []
    property int currentCapture: 0
    property int scanIndex: 0
    property int scanTicket: 0
    property bool awaitingProbe: false
    property bool preparingBatch: false
    property string preparationError: ""
    readonly property var currentInfo: batch ? (captureInfos[currentCapture] || {}) : exporter.info
    property string destination: ""
    property var selectedAudio: []
    property var excludedAudioOrdinals: []
    property bool tracksLoaded: false
    property string folderError: ""
    // The copy keeps the source codec; warn when that is not H.264.
    readonly property bool codecWarning: {
        const info = root.currentInfo;
        return !!info && info.shareable === false && (info.videoCodec || "") !== "";
    }
    readonly property bool estimatedNames: currentInfo.tracks
        ? currentInfo.tracks.some(track => track.inferred) : false
    readonly property bool working: preparingBatch || (exporter && exporter.inspecting)
    readonly property string audioSummary: {
        if (!tracksLoaded)
            return qsTr("Reading tracks…");
        const tracks = currentInfo.tracks || [];
        const names = tracks.filter(track => selectedAudio.indexOf(track.index) !== -1)
            .map(track => track.label + (track.inferred ? "*" : ""));
        return names.length ? names.join(" + ") : qsTr("No audio");
    }
    title: qsTr("Export to share")
    width: Math.min(560, Math.max(280, parent ? parent.width - 40 : 560))
    padding: Theme.spacingLarge
    implicitHeight: exportContent.implicitHeight + topPadding + bottomPadding
    closePolicy: working ? Popup.NoAutoClose : Popup.CloseOnEscape

    function defaultAudio(info) {
        return (info.tracks || [])
            .filter((track, ordinal) => !track.defaultExcluded && (batch || excludedAudioOrdinals.indexOf(track.ordinal !== undefined ? track.ordinal : ordinal) === -1))
            .map(track => track.index);
    }

    function inspectNext(ticket) {
        if (ticket !== scanTicket)
            return;
        awaitingProbe = true;
        exporter.inspect(sourcePaths[scanIndex]);
    }

    function chooseCapture(index) {
        let selections = captureAudio.slice();
        selections[currentCapture] = selectedAudio.slice();
        captureAudio = selections;
        currentCapture = index;
        selectedAudio = (captureAudio[index] || []).slice();
    }

    function exportCaptures() {
        // Video is copied and the selected tracks are mixed into one stereo
        // track: chat apps cannot switch audio tracks.
        if (!batch)
            return exporter.start(destination, selectedAudio);
        chooseCapture(currentCapture);
        const captures = sourcePaths.map((path, index) => ({sourcePath: path, audioStreams: captureAudio[index]}));
        return exporter.startBatch(destination, captures);
    }

    onAboutToShow: {
        selectedAudio = [];
        tracksLoaded = false;
        audioSection.expanded = false;
        folderError = "";
        preparationError = "";
        captureInfos = [];
        captureAudio = [];
        currentCapture = 0;
        scanIndex = 0;
        ++scanTicket;
        preparingBatch = batch;
        if (batch)
            inspectNext(scanTicket);
        else
            exporter.inspect(sourcePath);
    }
    onClosed: {
        ++scanTicket;
        awaitingProbe = false;
        preparingBatch = false;
        exporter.release();
    }

    Connections {
        target: root.exporter
        function onChanged() {
            if (root.batch) {
                if (!root.awaitingProbe || root.exporter.inspecting)
                    return;
                root.awaitingProbe = false;
                // QVariant property wrappers can continue referring to the
                // exporter's next probe. Keep an independent per-file snapshot.
                const info = JSON.parse(JSON.stringify(root.exporter.info));
                if (!info.tracks || info.error) {
                    root.preparingBatch = false;
                    root.preparationError = root.sourcePaths[root.scanIndex].split("/").pop() + ": "
                        + (info.error || root.exporter.error || qsTr("Unable to read capture."));
                    return;
                }
                let infos = root.captureInfos.slice();
                let selections = root.captureAudio.slice();
                infos.push(info);
                selections.push(root.defaultAudio(info));
                root.captureInfos = infos;
                root.captureAudio = selections;
                ++root.scanIndex;
                if (root.scanIndex < root.sourcePaths.length) {
                    const ticket = root.scanTicket;
                    Qt.callLater(() => root.inspectNext(ticket));
                } else {
                    root.selectedAudio = root.captureAudio[0].slice();
                    root.preparingBatch = false;
                    root.tracksLoaded = true;
                }
                return;
            }
            if (!root.tracksLoaded && root.exporter.sourcePath === root.sourcePath
                    && !root.exporter.inspecting && root.exporter.info.tracks) {
                root.selectedAudio = root.defaultAudio(root.exporter.info);
                root.tracksLoaded = true;
            }
        }
    }

    FolderDialog {
        id: destinationPicker
        objectName: "shareDestinationPicker"
        title: qsTr("Choose export folder")
        options: typeof app !== "undefined" && app && !app.useNativeDialogs ? FolderDialog.DontUseNativeDialog : 0
        onAccepted: {
            const path = root.exporter.localFolderPath(selectedFolder);
            if (path !== "") {
                root.destination = path;
                root.folderError = "";
            } else {
                root.folderError = qsTr("Choose a local folder for your export.");
            }
        }
    }

    contentItem: ColumnLayout {
        id: exportContent
        spacing: Theme.spacingMedium
        Label { text: root.title; color: Theme.textColor; font.pixelSize: Theme.fontSizeLarge; font.bold: true }
        Label {
            Layout.fillWidth: true
            text: root.batch ? qsTr("%1 captures selected").arg(root.sourcePaths.length) : root.sourcePath.split("/").pop()
            textFormat: Text.PlainText
            color: Theme.textSecondaryColor
            elide: Text.ElideMiddle
        }
        Label {
            Layout.fillWidth: true
            text: root.batch
                ? qsTr("Create one MP4 per capture in the same folder. Review audio for each capture below. Originals stay unchanged.")
                : qsTr("Copy this capture to MP4. Your original stays unchanged.")
            color: Theme.textSecondaryColor
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        ScrollView {
            id: formScroll
            objectName: "shareExportOptions"
            Layout.fillWidth: true
            visible: root.tracksLoaded
            implicitHeight: Math.min(exportForm.implicitHeight, Math.max(160, (root.parent ? root.parent.height : 800) - 300))
            enabled: !root.working
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical: TrophyScrollBar {}
          ColumnLayout {
            id: exportForm
            width: formScroll.availableWidth
            spacing: Theme.spacingMedium
            SettingsSectionCard {
                id: copySection
                objectName: "shareCopySection"
                sectionTitle: qsTr("Copy")
                iconSource: "qrc:/icons/video.svg"
                description: qsTr("Instant MP4 · original quality")
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Video is copied without re-encoding, so this finishes in seconds and loses nothing. The selected tracks are mixed into one stereo track because chat apps cannot switch between tracks.")
                    color: Theme.textSecondaryColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
                Label {
                    objectName: "shareCodecNote"
                    Layout.fillWidth: true
                    visible: root.codecWarning
                    text: qsTr("This recording uses %1 video. The copy keeps that codec, which some sites and players may not accept — record H.264 for the widest compatibility.").arg(currentInfo.videoCodec || "")
                    color: Theme.warningColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }

            SettingsSectionCard {
                id: audioSection
                objectName: "shareAudioSection"
                sectionTitle: qsTr("Audio")
                iconSource: "qrc:/icons/audio-recording.svg"
                description: root.audioSummary
            TrophyComboBox {
                objectName: "shareCapturePicker"
                Layout.fillWidth: true
                visible: root.batch
                model: root.sourcePaths.map((path, index) => (index + 1) + ". " + path.split("/").pop())
                currentIndex: root.currentCapture
                onActivated: index => root.chooseCapture(index)
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Include these sources in the shared video:")
                color: Theme.textSecondaryColor
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            ScrollView {
                id: audioScroll
                Layout.fillWidth: true
                implicitHeight: Math.min(140, audioChoices.implicitHeight)
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ColumnLayout {
                    id: audioChoices
                    width: audioScroll.availableWidth
                    Repeater {
                        model: root.currentInfo.tracks || []
                        delegate: CheckBox {
                            id: trackChoice
                            required property var modelData
                            objectName: "shareAudioTrack" + modelData.index
                            visible: !modelData.defaultExcluded
                            Layout.fillWidth: true
                            text: modelData.label + (modelData.inferred ? qsTr(" (estimated)") : "")
                            padding: Theme.spacingSmall
                            spacing: 8
                            checked: root.selectedAudio.indexOf(modelData.index) !== -1
                            onToggled: {
                                let tracks = root.selectedAudio.slice();
                                const at = tracks.indexOf(modelData.index);
                                if (checked && at < 0) tracks.push(modelData.index);
                                if (!checked && at >= 0) tracks.splice(at, 1);
                                root.selectedAudio = tracks;
                            }
                            indicator: Rectangle {
                                implicitWidth: 18; implicitHeight: 18
                                x: trackChoice.leftPadding
                                y: (trackChoice.height - height) / 2
                                radius: 4
                                color: trackChoice.checked ? Theme.accentColor : "transparent"
                                border.color: Theme.textMutedColor
                                Label { anchors.centerIn: parent; text: "✓"; color: "white"; visible: trackChoice.checked }
                            }
                            contentItem: Label {
                                text: trackChoice.text
                                textFormat: Text.PlainText
                                color: Theme.textColor
                                leftPadding: trackChoice.indicator.width + trackChoice.spacing
                                 verticalAlignment: Text.AlignVCenter
                                 wrapMode: Text.WordWrap
                             }
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                text: root.selectedAudio.length
                    ? qsTr("Sources are combined into one stereo track. System audio may already include voice and microphone.")
                         + (root.estimatedNames ? "\n" + qsTr("* This capture has no verified source labels. Names are estimated from track order, not audio content.") : "")
                    : qsTr("This copy will have no audio.")
                color: Theme.textSecondaryColor
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            }
            Label { text: qsTr("Save to"); color: Theme.textColor; font.bold: true }
            RowLayout {
                Layout.fillWidth: true
                TrophyButton { text: root.destination ? qsTr("Change folder…") : qsTr("Choose folder…"); onClicked: destinationPicker.open() }
                Label { Layout.fillWidth: true; objectName: "shareDestinationPath"; text: root.destination || qsTr("Select an export folder"); textFormat: Text.PlainText; color: Theme.textSecondaryColor; elide: Text.ElideMiddle; ToolTip.text: root.destination; ToolTip.visible: destinationHover.hovered && root.destination !== ""; HoverHandler { id: destinationHover } }
            }
            Label {
                Layout.fillWidth: true
                visible: root.folderError !== ""
                text: root.folderError
                color: Theme.errorColor
                wrapMode: Text.WordWrap
            }
          }
        }
        Label {
            Layout.fillWidth: true
            visible: root.working || !!root.preparationError || !!root.exporter.error
            text: root.preparingBatch ? qsTr("Reading capture %1 of %2…").arg(root.scanIndex + 1).arg(root.sourcePaths.length)
                : root.preparationError || (root.exporter.inspecting ? qsTr("Reading audio tracks…") : root.exporter.error)
            textFormat: Text.PlainText
            maximumLineCount: 3
            elide: Text.ElideRight
            color: root.preparationError || root.exporter.error ? Theme.errorColor : Theme.textColor
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            TrophyButton {
                objectName: "closeShareExport"
                text: qsTr("Close")
                onClicked: root.close()
            }
            TrophyButton {
                objectName: "startShareExport"
                text: root.batch ? qsTr("Export %1 MP4s").arg(root.sourcePaths.length) : qsTr("Export MP4")
                highlighted: true
                enabled: !root.working && root.destination !== "" && root.tracksLoaded && (root.batch || root.exporter.sourcePath === root.sourcePath)
                onClicked: {
                    if (root.exportCaptures())
                        root.close();
                }
            }
        }
    }
}
