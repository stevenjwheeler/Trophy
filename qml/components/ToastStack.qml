import "../controls"
import "../styles"
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One ordered, scrollable stack for ongoing actions and transient results.
// Keyed model updates preserve delegates (and pressed buttons) on progress ticks.
ScrollView {
    id: root
    required property var exporter
    required property var queue
    property real maxToastHeight: 500
    property int successDuration: 5000
    property int errorDuration: 8000
    readonly property int count: notices.count
    implicitHeight: Math.min(stack.implicitHeight, maxToastHeight)
    visible: notices.count > 0
    clip: true
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical: TrophyScrollBar {}

    ListModel { id: notices }

    function indexOf(key) {
        for (let i = 0; i < notices.count; ++i)
            if (notices.get(i).toastKey === key) return i;
        return -1;
    }

    function dismiss(key) {
        const at = indexOf(key);
        if (at !== -1) notices.remove(at);
    }

    function update(key, values) {
        let at = indexOf(key);
        if (at === -1) {
            notices.append({ toastKey: key, kind: "", jobId: "", heading: "", detail: "",
                phase: "working", progress: -1, actionText: "", actionEnabled: true,
                expiresAt: 0 });
            at = notices.count - 1;
            Qt.callLater(function() {
                root.contentItem.contentY = Math.max(0, stack.height - root.availableHeight);
            });
        }
        for (const field in values)
            notices.setProperty(at, field, values[field]);
    }

    function syncExports() {
        const jobs = root.exporter.jobs;
        const keys = [];
        for (const job of jobs) {
            const key = "export:" + job.id;
            keys.push(key);
            update(key, {kind: "export", jobId: job.id,
                heading: job.sourcePath.split("/").pop(),
                detail: job.cancelling ? (job.copyOriginal ? qsTr("Cancelling copy…") : qsTr("Cancelling export…")) : job.active
                    ? (job.copyOriginal ? qsTr("Copying · %1%") : qsTr("Exporting · %1%")).arg(Math.floor(job.progress * 100))
                    : (job.copyOriginal ? qsTr("Copy queued") : qsTr("Export queued")),
                progress: job.active ? job.progress : -1,
                actionText: qsTr("Cancel"), actionEnabled: !job.cancelling});
        }
        for (let i = notices.count - 1; i >= 0; --i) {
            const toast = notices.get(i);
            if (toast.kind === "export" && toast.phase === "working" && keys.indexOf(toast.toastKey) === -1)
                notices.remove(i);
        }
    }

    function syncDeletions() {
        const pending = root.queue.pending;
        const keys = [];
        for (const batch of pending) {
            const key = "delete:" + batch.id;
            keys.push(key);
            const status = root.queue.status(batch.id);
            update(key, {kind: "delete", jobId: batch.id,
                heading: status.committing ? qsTr("Deleting…") : qsTr("%1 removed").arg(status.label || qsTr("%1 capture(s)").arg(status.count || 0)),
                detail: status.committing ? qsTr("Finishing deletion") : qsTr("%1 s to undo").arg(status.secondsLeft || 0),
                progress: status.progress || 0,
                actionText: qsTr("Undo"), actionEnabled: !status.committing});
        }
        for (let i = notices.count - 1; i >= 0; --i) {
            const toast = notices.get(i);
            if (toast.kind === "delete" && keys.indexOf(toast.toastKey) === -1)
                notices.remove(i);
        }
    }

    function exportSettled(id, source, path, error, cancelled, copyOriginal) {
        const key = "export:" + id;
        if (!path && (cancelled || !error)) {
            dismiss(key);
            return;
        }
        update(key, {kind: "export", jobId: id,
            heading: path ? (copyOriginal ? qsTr("Copied") : qsTr("Exported"))
                : (copyOriginal ? qsTr("Copy failed") : qsTr("Export failed")),
            detail: path ? path.split("/").pop() : source.split("/").pop() + "\n" + error,
            phase: path ? "success" : "error", progress: -1,
            actionText: qsTr("Dismiss"), actionEnabled: true,
            expiresAt: Date.now() + (path ? root.successDuration : root.errorDuration)});
    }

    Component.onCompleted: {
        syncExports();
        syncDeletions();
    }
    Connections {
        target: root.exporter
        function onChanged() { root.syncExports(); }
        function onJobSettled(id, sourcePath, resultPath, error, cancelled, copyOriginal) {
            root.exportSettled(id, sourcePath, resultPath, error, cancelled, copyOriginal);
        }
    }
    Connections {
        target: root.queue
        function onPendingChanged() { root.syncDeletions(); }
    }
    Timer {
        interval: 100
        running: root.count > 0
        repeat: true
        onTriggered: {
            root.syncDeletions();
            const now = Date.now();
            for (let i = notices.count - 1; i >= 0; --i) {
                const expiry = notices.get(i).expiresAt;
                if (expiry > 0 && now >= expiry) notices.remove(i);
            }
        }
    }

    Column {
        id: stack
        width: root.availableWidth
        spacing: Theme.spacingSmall
        Repeater {
            model: notices
            delegate: Rectangle {
                id: toast
                required property string toastKey
                required property string kind
                required property string jobId
                required property string heading
                required property string detail
                required property string phase
                required property real progress
                required property string actionText
                required property bool actionEnabled
                readonly property color statusColor: phase === "success" ? Theme.successColor
                    : phase === "error" ? Theme.errorColor : Theme.accentColor
                objectName: "toast:" + toastKey
                width: stack.width
                height: toastContent.implicitHeight + Theme.spacingMedium * 2
                radius: Theme.radiusMedium
                color: phase === "working" ? Theme.cardColor : Qt.tint(Theme.cardColor, Theme.withOpacity(statusColor, 0.15))
                border.color: statusColor
                Behavior on color { ColorAnimation { duration: Theme.animationNormal } }
                Behavior on border.color { ColorAnimation { duration: Theme.animationNormal } }
                ColumnLayout {
                    id: toastContent
                    anchors.fill: parent
                    anchors.margins: Theme.spacingMedium
                    spacing: Theme.spacingSmall
                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacingTiny
                            Label {
                                Layout.fillWidth: true
                                text: toast.heading
                                textFormat: Text.PlainText
                                color: toast.phase === "working" ? Theme.textColor : toast.statusColor
                                font.bold: true
                                elide: Text.ElideMiddle
                            }
                            Label {
                                Layout.fillWidth: true
                                text: toast.detail
                                textFormat: Text.PlainText
                                color: Theme.textSecondaryColor
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                maximumLineCount: 3
                                elide: Text.ElideRight
                            }
                        }
                        TrophyButton {
                            objectName: "toastAction:" + toast.toastKey
                            text: toast.actionText
                            enabled: toast.actionEnabled
                            onClicked: {
                                if (toast.phase !== "working") root.dismiss(toast.toastKey);
                                else if (toast.kind === "delete") root.queue.undo(toast.jobId);
                                else if (toast.kind === "export") root.exporter.cancelJob(toast.jobId);
                            }
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        visible: toast.progress >= 0
                        implicitHeight: 4
                        radius: 2
                        color: Theme.borderColor
                        Rectangle {
                            width: parent.width * Math.max(0, Math.min(1, toast.progress))
                            height: parent.height
                            radius: parent.radius
                            color: toast.statusColor
                        }
                    }
                }
            }
        }
    }
}
