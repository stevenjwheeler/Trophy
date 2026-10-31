import QtQuick.Dialogs

FolderDialog {
    id: root
    required property var exporter
    property var sourcePaths: []
    signal queued()
    signal copyFailed(string message)
    title: qsTr("Copy original captures to folder")
    options: typeof app !== "undefined" && app && !app.useNativeDialogs ? FolderDialog.DontUseNativeDialog : 0
    onAccepted: {
        const destination = root.exporter.localFolderPath(selectedFolder);
        if (!destination) {
            root.copyFailed(qsTr("Choose a local folder for your copies."));
            return;
        }
        if (!root.exporter.copyOriginals(root.sourcePaths, destination)) {
            root.copyFailed(root.exporter.error || qsTr("No captures could be queued for copying."));
            return;
        }
        root.queued();
    }
}
