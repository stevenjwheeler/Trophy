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
    property int exportMode: 0      // 0 = copy (remux), 1 = convert to H.264
    property int exportHeight: 0    // convert only: 0/720/1080
    // Copy keeps the source codec, so it is universally shareable only for
    // H.264. An H.264 recording therefore has nothing to choose: copy is
    // lossless and needs no re-encode, and converting would produce the same
    // format far slower. Anything else defaults to conversion, which is the
    // mode whose output actually plays everywhere.
    readonly property bool allShareable: {
        if (!tracksLoaded) return false;
        const infos = batch ? captureInfos : (currentInfo ? [currentInfo] : []);
        if (!infos.length) return false;
        for (const info of infos)
            if (!info || info.shareable !== true) return false;
        return true;
    }
    readonly property bool anyUncopyable: {
        const infos = batch ? captureInfos : (currentInfo ? [currentInfo] : []);
        for (const info of infos)
            if (info && info.copyable === false) return true;
        return false;
    }
    // The mode actually used: forced to convert when MP4 cannot carry the
    // source codec at all, otherwise whatever is selected. An H.264 recording
    // defaults to copy because conversion buys it nothing, but the choice
    // stays available for resizing.
    readonly property int effectiveMode: anyUncopyable ? 1 : exportMode
    readonly property bool converting: effectiveMode === 1
    // Both choices are always shown; the unavailable one is disabled and says
    // why, which is more informative than hiding it.
    readonly property bool modeVisible: tracksLoaded
    // Copy keeps the recording's codec; conversion always lands on H.264.
    readonly property string producedVideo: converting
        ? qsTr("H.264")
        : (codecLabel !== "" ? codecLabel : qsTr("the source codec"))
    readonly property string producedResolution: converting && exportHeight > 0
        ? exportHeight + "p" : qsTr("original resolution")
    readonly property string producesText: qsTr("MP4 · %1 video · one AAC stereo track · %2")
        .arg(root.producedVideo).arg(root.producedResolution)
    // Optional per-ordinal volumes from the player (0..1). A zero stays out
    // of selectedAudio, exactly like flipping a track off used to.
    property var sourceAudioVolumes: ({})
    property var selectedGains: ({})
    property var captureGains: []
    // Legacy single-file exclusion list (still accepted from older callers).
    property var excludedAudioOrdinals: []
    property bool tracksLoaded: false
    // Audio collapses to one line by default; the checkboxes are behind it.
    property bool audioExpanded: false
    property string folderError: ""
    // The copy keeps the source codec; warn when that is not H.264.
    readonly property string codecLabel: root.currentInfo.videoCodecLabel || root.currentInfo.videoCodec || ""
    readonly property bool codecWarning: {
        const info = root.currentInfo;
        return !!info && info.shareable === false && (info.videoCodec || "") !== "";
    }
    readonly property bool estimatedNames: currentInfo.tracks
        ? currentInfo.tracks.some(track => track.inferred) : false
    readonly property bool working: preparingBatch || (exporter && exporter.inspecting)
    // Cost model: measured on this machine once it has exported something,
    // otherwise the engine's starting constants.
    readonly property var rates: {
        const fallback = root.currentInfo || {};
        const s = (typeof settingsManager !== "undefined" && settingsManager)
            ? settingsManager : null;
        const av1 = (fallback.videoCodec || "") === "av1";
        return {
            "copy": s ? s.exportCopyRate : (fallback.estimateCopyBase || 0.0017),
            "track": s ? s.exportCopyTrackRate : (fallback.estimateCopyPerTrack || 0.0100),
            "convert": av1
                ? (s ? s.exportConvertAv1Rate : (fallback.estimateConvert || 0.165))
                : (s ? s.exportConvertRate : (fallback.estimateConvert || 0.100))
        };
    }
    function audioCountFor(index) {
        if (!batch)
            return selectedAudio.length;
        return index === currentCapture ? selectedAudio.length
                                        : (captureAudio[index] || []).length;
    }
    // Rough export cost from the engine's measured rates. Guidance, not a
    // promise: conversion assumes a hardware encoder and is far slower on a
    // software-only host.
    readonly property double estimatedSeconds: {
        const infos = batch ? captureInfos : (currentInfo && currentInfo.durationSeconds ? [currentInfo] : []);
        let total = 0;
        for (let i = 0; i < infos.length; ++i) {
            const info = infos[i] || {};
            const duration = info.durationSeconds || 0;
            if (!duration)
                return 0;
            if (effectiveMode === 1)
                total += duration * root.rates.convert;
            else
                total += duration * (root.rates.copy +
                    Math.max(0, audioCountFor(i) - 1) * root.rates.track);
        }
        return total;
    }
    function formatEstimate(seconds) {
        if (!(seconds > 0))
            return "";
        if (seconds < 90)
            return qsTr("about %1 seconds").arg(Math.max(5, Math.round(seconds / 5) * 5));
        return qsTr("about %1 minutes").arg(Math.max(1, Math.round(seconds / 60)));
    }
    readonly property string estimateText: {
        if (!tracksLoaded || preparingBatch || working)
            return "";
        const text = formatEstimate(estimatedSeconds);
        if (!text)
            return "";
        return effectiveMode === 1
            ? qsTr("Estimated time to complete export: %1 (hardware encoder; longer without one)").arg(text)
            : qsTr("Estimated time to complete export: %1").arg(text);
    }
    // Raw gain for the slider/label, independent of whether the track is
    // currently selected. This keeps a drag from being reset while the
    // selection list and the gain map are updated in sequence.
    function gainValue(index) {
        const gain = selectedGains[index];
        return gain === undefined ? (selectedAudio.indexOf(index) !== -1 ? 1 : 0) : gain;
    }
    // Selection-aware gain used by the summary and the export.
    function trackGain(index) {
        return selectedAudio.indexOf(index) === -1 ? 0 : gainValue(index);
    }
    readonly property string audioSummary: {
        if (!tracksLoaded)
            return qsTr("Reading tracks…");
        const tracks = currentInfo.tracks || [];
        const names = tracks.filter(track => trackGain(track.index) > 0.001).map(track => {
            const gain = trackGain(track.index);
            return track.label + (track.inferred ? "*" : "")
                + (gain < 0.995 ? " " + Math.round(gain * 100) + "%" : "");
        });
        return names.length ? names.join(" + ") : qsTr("No audio");
    }
    // The recording behind this export, for the header. Resolved from the
    // library so every caller gets the name, date and length without passing
    // them in; falls back to the file name when the capture is not tracked.
    readonly property var recording: {
        const path = batch ? (sourcePaths.length ? sourcePaths[0] : "") : sourcePath;
        if (!path || typeof recordingLibrary === "undefined" || !recordingLibrary)
            return null;
        const all = recordingLibrary.allRecordings || [];
        for (let i = 0; i < all.length; ++i)
            if (all[i] && all[i].filePath === path) return all[i];
        return null;
    }
    // "Lethal_Company_2026-11-02_16-21-50.mkv" -> "Lethal Company", for
    // captures the library does not know about.
    function displayNameFromPath(path) {
        let stem = path.split("/").pop().replace(/\.(mkv|mp4|mov)$/i, "");
        stem = stem.replace(/_\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}$/, "");
        stem = stem.replace(/_Replay$/, "");
        return stem.replace(/_/g, " ");
    }
    readonly property string recordingName: {
        if (recording && recording.gameName)
            return recording.gameName;
        return displayNameFromPath(batch ? (sourcePaths.length ? sourcePaths[0] : "") : sourcePath);
    }
    function formatLength(seconds) {
        const total = Math.max(0, Math.round(seconds));
        const h = Math.floor(total / 3600);
        const m = Math.floor((total % 3600) / 60);
        const s = total % 60;
        const pad = n => (n < 10 ? "0" : "") + n;
        return h > 0 ? h + ":" + pad(m) + ":" + pad(s) : m + ":" + pad(s);
    }
    readonly property string recordingDetail: {
        if (batch) {
            let total = 0;
            for (const info of (captureInfos || []))
                total += (info && info.durationSeconds) || 0;
            return total > 0
                ? qsTr("%1 captures · %2 total").arg(sourcePaths.length).arg(formatLength(total))
                : qsTr("%1 captures").arg(sourcePaths.length);
        }
        const parts = [];
        if (recording && recording.date)
            parts.push(Qt.formatDateTime(recording.date, qsTr("d MMM yyyy, HH:mm")));
        if (recording && recording.duration > 0)
            parts.push(formatLength(recording.duration));
        if (!parts.length)
            parts.push(sourcePath.split("/").pop());
        return parts.join(" · ");
    }
    title: batch ? qsTr("Exporting %1 captures").arg(sourcePaths.length)
                 : qsTr("Exporting %1").arg(recordingName)
    width: Math.min(560, Math.max(280, parent ? parent.width - 40 : 560))
    padding: Theme.spacingMedium
    // Never taller than the window: the card area scrolls instead.
    implicitHeight: Math.min(exportContent.implicitHeight + topPadding + bottomPadding,
                             Math.max(320, (parent ? parent.height : 800) - Theme.spacingLarge))
    closePolicy: working ? Popup.NoAutoClose : Popup.CloseOnEscape

    function defaultGainFor(track, ordinal) {
        const key = track.ordinal !== undefined ? track.ordinal : ordinal;
        // The player's per-lane volumes win for a single capture; batch
        // exports from the library keep the engine's default selection.
        if (!batch && sourceAudioVolumes && sourceAudioVolumes[key] !== undefined) {
            const volume = Number(sourceAudioVolumes[key]);
            return isFinite(volume) ? Math.max(0, Math.min(1, volume)) : 1;
        }
        if (excludedAudioOrdinals.indexOf(key) !== -1)
            return 0;
        if (track.defaultExcluded)
            return 0;
        return 1;
    }
    function defaultGains(info) {
        const gains = ({});
        (info.tracks || []).forEach((track, ordinal) => {
            gains[track.index] = defaultGainFor(track, ordinal);
        });
        return gains;
    }
    function includedIndices(info, gains) {
        return (info.tracks || [])
            .filter(track => (gains[track.index] === undefined ? 0 : gains[track.index]) > 0.001)
            .map(track => track.index);
    }
    function setTrackGain(index, value) {
        const gain = Math.max(0, Math.min(1, value));
        const gains = Object.assign({}, selectedGains);
        gains[index] = gain;
        selectedGains = gains;
        const tracks = selectedAudio.slice();
        const at = tracks.indexOf(index);
        if (gain > 0.001 && at < 0) tracks.push(index);
        if (gain <= 0.001 && at >= 0) tracks.splice(at, 1);
        selectedAudio = tracks;
    }

    // Called once every selected capture has been probed.
    function applyModeDefault() {
        const infos = batch ? captureInfos : (exporter.info ? [exporter.info] : []);
        let shareable = infos.length > 0;
        for (const info of infos)
            if (!info || info.shareable !== true) shareable = false;
        exportMode = shareable ? 0 : 1;
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
        let gains = captureGains.slice();
        gains[currentCapture] = Object.assign({}, selectedGains);
        captureGains = gains;
        currentCapture = index;
        selectedAudio = (captureAudio[index] || []).slice();
        selectedGains = Object.assign({}, captureGains[index] || {});
    }

    function exportCaptures() {
        // Video is copied and the selected tracks are mixed into one stereo
        // track: chat apps cannot switch audio tracks. Per-track gains travel
        // with the selection so the player's volume mix carries into the file.
        const height = effectiveMode === 1 ? exportHeight : 0;
        const gains = selectedAudio.map(index => trackGain(index));
        if (!batch)
            return exporter.start(destination, selectedAudio, effectiveMode, height, gains);
        chooseCapture(currentCapture);
        const captures = sourcePaths.map((path, index) => ({
            sourcePath: path,
            audioStreams: captureAudio[index],
            audioGains: (captureAudio[index] || []).map(stream => {
                const trackGains = captureGains[index] || {};
                return trackGains[stream] === undefined ? 1 : trackGains[stream];
            })
        }));
        return exporter.startBatch(destination, captures, effectiveMode, height);
    }

    onAboutToShow: {
        selectedAudio = [];
        tracksLoaded = false;
        audioExpanded = false;
        folderError = "";
        preparationError = "";
        captureInfos = [];
        captureAudio = [];
        captureGains = [];
        selectedGains = ({});
        currentCapture = 0;
        scanIndex = 0;
        ++scanTicket;
        preparingBatch = batch;
        exportMode = 0;
        exportHeight = 0;
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
                let gains = root.captureGains.slice();
                const captureGains = root.defaultGains(info);
                infos.push(info);
                selections.push(root.includedIndices(info, captureGains));
                gains.push(captureGains);
                root.captureInfos = infos;
                root.captureAudio = selections;
                root.captureGains = gains;
                ++root.scanIndex;
                if (root.scanIndex < root.sourcePaths.length) {
                    const ticket = root.scanTicket;
                    Qt.callLater(() => root.inspectNext(ticket));
                } else {
                    root.selectedAudio = root.captureAudio[0].slice();
                    root.selectedGains = Object.assign({}, root.captureGains[0] || {});
                    root.preparingBatch = false;
                    root.tracksLoaded = true;
                    root.applyModeDefault();
                }
                return;
            }
            if (!root.tracksLoaded && root.exporter.sourcePath === root.sourcePath
                    && !root.exporter.inspecting && root.exporter.info.tracks) {
                root.selectedGains = root.defaultGains(root.exporter.info);
                root.selectedAudio = root.includedIndices(root.exporter.info, root.selectedGains);
                root.tracksLoaded = true;
                root.applyModeDefault();
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
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingMedium
            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 72
                Layout.preferredHeight: 40
                radius: Theme.radiusSmall
                color: Theme.cardColor
                border.width: 1
                border.color: Theme.borderColor
                clip: true
                Image {
                    objectName: "shareRecordingThumbnail"
                    anchors.fill: parent
                    anchors.margins: 1
                    sourceSize.width: 144
                    sourceSize.height: 80
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    source: {
                        const r = root.recording;
                        if (r && r.thumbnailPath && typeof recordingLibrary !== "undefined" && recordingLibrary)
                            return recordingLibrary.localFileUrl(r.thumbnailPath);
                        return "qrc:/icons/video.svg";
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Label {
                    objectName: "shareTitle"
                    Layout.fillWidth: true
                    text: root.title
                    textFormat: Text.PlainText
                    color: Theme.textColor
                    font.pixelSize: Theme.fontSizeLarge
                    font.bold: true
                    elide: Text.ElideRight
                }
                Label {
                    objectName: "shareSubtitle"
                    Layout.fillWidth: true
                    text: root.recordingDetail
                    textFormat: Text.PlainText
                    color: Theme.textSecondaryColor
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: root.batch
            text: qsTr("One MP4 per capture in the same folder. Review audio for each capture below. Originals stay unchanged.")
            color: Theme.textSecondaryColor
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        ScrollView {
            id: formScroll
            objectName: "shareExportOptions"
            Layout.fillWidth: true
            visible: root.tracksLoaded
            Layout.preferredHeight: exportForm.implicitHeight
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            enabled: !root.working
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical: TrophyScrollBar {}
          ColumnLayout {
            id: exportForm
            width: formScroll.availableWidth
            spacing: Theme.spacingMedium

            // What to do
            RowLayout {
                objectName: "shareModeRow"
                Layout.fillWidth: true
                spacing: Theme.spacingSmall
                ModeOption {
                    objectName: "shareCopyMode"
                    title: qsTr("Copy")
                    subtitle: root.anyUncopyable
                        ? qsTr("This recording's codec cannot be copied into MP4.")
                        : qsTr("Lossless, merges audio tracks without re-encoding video.")
                    selected: !root.converting
                    disabled: root.anyUncopyable
                    onClicked: root.exportMode = 0
                }
                ModeOption {
                    objectName: "shareConvertMode"
                    title: qsTr("Convert")
                    subtitle: root.allShareable
                        ? qsTr("This recording is already H.264, conversion is not needed — but it can still be used to resize the video.")
                        : qsTr("Merges audio tracks and re-encodes video to H.264 for the widest compatibility")
                    selected: root.converting
                    onClicked: root.exportMode = 1
                }
            }
            Rule { objectName: "shareDivider" }

            // Resolution: same card as the rows below it, with the picker as
            // plain text and a chevron rather than a boxed control.
            Rectangle {
                id: resolutionCard
                objectName: "shareResolutionCard"
                Layout.fillWidth: true
                visible: root.converting
                implicitHeight: Math.max(44, Theme.spacingSmall * 2 + 28)
                radius: Theme.radiusSmall
                color: Theme.cardColor
                border.width: 1
                border.color: Theme.borderColor
                clip: true

                Label {
                    id: resolutionLabel
                    objectName: "shareHeightRow"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spacingMedium
                    text: qsTr("Resolution")
                    color: Theme.textColor
                }

                // Same shape as the audio summary: value, then a chevron,
                // ending on the same inset. The combo sits invisibly on top so
                // the popup and selection behave normally.
                Item {
                    id: heightPicker
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.spacingMedium
                    anchors.verticalCenter: parent.verticalCenter
                    width: implicitWidth
                    implicitWidth: heightValue.implicitWidth + Theme.spacingSmall
                        + heightArrow.implicitWidth
                    height: parent.height

                    Label {
                        id: heightValue
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: heightBox.displayText
                        color: Theme.textSecondaryColor
                    }
                    Label {
                        id: heightArrow
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: "›"
                        // Points right when closed, like the audio row;
                        // turns down while the menu is open.
                        rotation: heightBox.popup.visible ? 90 : 0
                        color: Theme.textSecondaryColor
                        Behavior on rotation {
                            NumberAnimation { duration: Theme.animationFast }
                        }
                    }
                    TrophyComboBox {
                        id: heightBox
                        objectName: "shareHeightBox"
                        anchors.fill: parent
                        background: Item {}
                        contentItem: Item {}
                        indicator: Item {}
                        // The control is only as wide as its value, so the
                        // popup gets its own width and hangs leftward from the
                        // value instead of off the dialog's edge.
                        popup.width: 170
                        popup.x: heightPicker.width - popup.width
                        model: [qsTr("Original"), "1080p", "720p"]
                        currentIndex: [0, 1080, 720].indexOf(root.exportHeight) >= 0
                            ? [0, 1080, 720].indexOf(root.exportHeight) : 0
                        onActivated: root.exportHeight = [0, 1080, 720][currentIndex]
                    }
                }
            }

            // Which audio goes in: a thin card that shows a summary line until
            // opened, so the panel stays two choices plus a line or two.
            Rectangle {
                objectName: "shareAudioCard"
                Layout.fillWidth: true
                implicitHeight: Math.max(44, audioCardColumn.implicitHeight + Theme.spacingSmall * 2)
                radius: Theme.radiusSmall
                color: Theme.cardColor
                border.width: 1
                border.color: Theme.borderColor
                ColumnLayout {
                    id: audioCardColumn
                    // Centred in the box, so the closed card reads the same as
                    // the destination card next to it.
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: Theme.spacingMedium
                    spacing: Theme.spacingSmall
            Rectangle {
                objectName: "shareAudioToggle"
                Layout.fillWidth: true
                implicitHeight: audioHeader.implicitHeight + Theme.spacingTiny * 2
                radius: Theme.radiusSmall
                color: audioHover.hovered ? Theme.cardHoverColor : "transparent"
                HoverHandler { id: audioHover }
                TapHandler { onTapped: root.audioExpanded = !root.audioExpanded }
                RowLayout {
                    id: audioHeader
                    // No extra horizontal inset: the card padding alone keeps
                    // this aligned with the rows below it.
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: 0
                    anchors.rightMargin: 0
                    spacing: Theme.spacingSmall
                    Label {
                        text: qsTr("Audio")
                        color: Theme.textColor
                        Layout.fillWidth: false
                    }
                    Label {
                        Layout.fillWidth: true
                        text: root.audioSummary
                        color: Theme.textSecondaryColor
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignRight
                    }
                    Label {
                        text: "›"
                        color: Theme.textSecondaryColor
                        rotation: root.audioExpanded ? 90 : 0
                        Behavior on rotation { NumberAnimation { duration: Theme.animationFast } }
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.audioExpanded
                spacing: 0
                TrophyComboBox {
                    objectName: "shareCapturePicker"
                    Layout.fillWidth: true
                    visible: root.batch
                    model: root.sourcePaths.map((path, index) => (index + 1) + ". " + path.split("/").pop())
                    currentIndex: root.currentCapture
                    onActivated: index => root.chooseCapture(index)
                }
                Repeater {
                    model: root.currentInfo.tracks || []
                    delegate: RowLayout {
                        id: trackRow
                        required property var modelData
                        objectName: "shareAudioTrack" + modelData.index
                        // Kept as a property so callers can read the label the
                        // same way they did when this row had a switch.
                        property string text: modelData.label
                            + (modelData.inferred ? qsTr(" (estimated)") : "")
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.spacingTiny
                        Layout.bottomMargin: Theme.spacingTiny
                        spacing: Theme.spacingSmall
                        Label {
                            Layout.fillWidth: true
                            text: trackRow.text
                            textFormat: Text.PlainText
                            color: Theme.textColor
                            opacity: root.gainValue(trackRow.modelData.index) > 0.001 ? 1 : 0.55
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                        }
                        // A gain bar per source: zero removes it from the
                        // mix, exactly like the player's lane volume.
                        TrophySlider {
                            objectName: "shareAudioSlider" + trackRow.modelData.index
                            Layout.preferredWidth: 120
                            Layout.alignment: Qt.AlignVCenter
                            from: 0
                            to: 1
                            stepSize: 0.01
                            snapMode: Slider.SnapAlways
                            value: root.gainValue(trackRow.modelData.index)
                            onMoved: root.setTrackGain(trackRow.modelData.index, value)
                        }
                        Label {
                            objectName: "shareAudioVolume" + trackRow.modelData.index
                            Layout.preferredWidth: 42
                            horizontalAlignment: Text.AlignRight
                            text: root.gainValue(trackRow.modelData.index) <= 0.001
                                ? qsTr("Muted")
                                : qsTr("%1%").arg(Math.round(root.gainValue(trackRow.modelData.index) * 100))
                            color: root.gainValue(trackRow.modelData.index) <= 0.001
                                ? Theme.errorColor : Theme.textSecondaryColor
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spacingSmall
                    text: root.selectedAudio.length
                        ? qsTr("Mixed into one stereo track; system audio may already include voice.")
                            + (root.estimatedNames ? "\n" + qsTr("* This capture has no verified source labels. Names are estimated from track order, not audio content.") : "")
                        : qsTr("This copy will have no audio.")
                    color: Theme.textSecondaryColor
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
                }
            }

            // Where it goes: label and path, with the folder action filling the
            // right end of the card, top to bottom.
            Rectangle {
                id: saveToCard
                objectName: "shareSaveToCard"
                Layout.fillWidth: true
                implicitHeight: Math.max(44, saveToRow.implicitHeight + Theme.spacingSmall * 2)
                radius: Theme.radiusSmall
                color: Theme.cardColor
                border.width: 1
                border.color: Theme.borderColor
                clip: true

                RowLayout {
                    id: saveToRow
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: changeButton.width + Theme.spacingSmall * 2
                    spacing: Theme.spacingSmall
                    Label {
                        text: qsTr("Save to")
                        color: Theme.textColor
                    }
                    Label {
                        Layout.fillWidth: true
                        objectName: "shareDestinationPath"
                        text: root.destination || qsTr("Select an export folder")
                        textFormat: Text.PlainText
                        color: Theme.textSecondaryColor
                        elide: Text.ElideMiddle
                        ToolTip.text: root.destination
                        ToolTip.visible: destinationHover.hovered && root.destination !== ""
                        HoverHandler { id: destinationHover }
                    }
                }

                // Flush segment: the card's own clip keeps the outer corners
                // round while the action runs edge to edge.
                Rectangle {
                    id: changeButton
                    objectName: "shareChangeFolder"
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: 1
                    // Same width as the resolution segment above it.
                    width: changeLabel.implicitWidth + Theme.spacingMedium * 2
                    radius: Math.max(0, saveToCard.radius - 1)
                    // Opaque, pre-blended against the card: the square-corner
                    // patch paints the same colour on top, and a translucent
                    // fill would composite twice and read brighter there.
                    readonly property color fill: Qt.tint(
                        saveToCard.color,
                        Qt.rgba(Theme.textColor.r, Theme.textColor.g,
                                Theme.textColor.b, changeHover.hovered ? 0.10 : 0.05))
                    color: fill
                    HoverHandler { id: changeHover }
                    TapHandler { onTapped: destinationPicker.open() }
                    // Square off the corners that meet the path, so only the
                    // card's own corners stay round.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: parent.radius
                        color: changeButton.fill
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 1
                        color: Theme.dividerColor
                    }
                    Label {
                        id: changeLabel
                        anchors.centerIn: parent
                        text: root.destination ? qsTr("Change folder…") : qsTr("Choose folder…")
                        color: Theme.textColor
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: root.folderError !== ""
                text: root.folderError
                color: Theme.errorColor
                wrapMode: Text.WordWrap
            }

            // What you get, and how long it takes
            Rectangle {
                objectName: "shareExportDetails"
                Layout.fillWidth: true
                implicitHeight: detailsColumn.implicitHeight + Theme.spacingSmall * 2
                radius: Theme.radiusSmall
                color: Theme.surfaceColor
                border.width: 1
                border.color: Theme.borderColor
                ColumnLayout {
                    id: detailsColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spacingMedium
                    anchors.rightMargin: Theme.spacingMedium
                    spacing: Theme.spacingTiny
                    Label {
                        objectName: "shareProduces"
                        Layout.fillWidth: true
                        text: root.producesText
                        color: Theme.textColor
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        objectName: "shareEstimate"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.estimateText
                        color: Theme.textColor
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        objectName: "shareCodecNote"
                        Layout.fillWidth: true
                        visible: !root.converting && root.codecWarning
                        text: qsTr("This recording is %1. Some sites and players may not accept that codec — choose Convert for the widest compatibility.").arg(root.codecLabel)
                        color: Theme.warningColor
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                }
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

    // One square, accent-outlined choice, matching Settings' option buttons.
    component ModeOption: Rectangle {
        id: option
        property string title: ""
        property string subtitle: ""
        property bool selected: false
        // Shown but unavailable, with the subtitle explaining why.
        property bool disabled: false
        signal clicked()

        Layout.fillWidth: true
        // Big enough to be the primary control on the panel.
        Layout.preferredHeight: Math.max(96, optionText.implicitHeight + Theme.spacingNormal * 2)
        // Both cards take the height of the taller one, so the row reads as a
        // pair rather than two differently sized boxes.
        Layout.fillHeight: true
        // Same corner radius as the app's normal buttons.
        radius: Theme.radiusSmall
        enabled: !option.disabled
        opacity: option.disabled ? 0.55 : 1.0
        color: option.selected && !option.disabled
            ? Theme.withOpacity(Theme.accentColor, 0.14) : Theme.cardColor
        border.width: 1
        border.color: option.disabled ? Theme.borderColor
            : option.selected ? Theme.accentColor
            : optionHover.hovered ? Theme.textMutedColor : Theme.borderColor

        ColumnLayout {
            id: optionText
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: Theme.spacingNormal
            spacing: Theme.spacingTiny
            Label {
                text: option.title
                color: Theme.textColor
                font.pixelSize: Theme.fontSizeMedium
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: option.subtitle
                color: Theme.textSecondaryColor
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }
        HoverHandler { id: optionHover }
        TapHandler { onTapped: option.clicked() }
    }

    // Flat separation between groups; the panel is a single flow, not a stack
    // of collapsible cards.
    component Rule: Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: Theme.dividerColor
    }
}
