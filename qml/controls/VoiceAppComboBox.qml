import QtQuick

TrophyComboBox {
    id: control

    property var apps: []
    property string savedApp: ""
    signal appSelected(string binary)

    // Keep the saved identity in the model even before the app creates an
    // audio stream. Only explicit user activation may change that identity.
    model: {
        let names = [qsTr("None")];
        let found = false;
        for (let app of apps) {
            names.push(app.name);
            if (app.binary && app.binary.toLowerCase() === savedApp.toLowerCase())
                found = true;
        }
        if (savedApp && !found)
            names.push(savedApp);
        return names;
    }

    readonly property int savedIndex: {
        if (!savedApp) return 0;
        for (let i = 0; i < apps.length; ++i) {
            if (apps[i].binary && apps[i].binary.toLowerCase() === savedApp.toLowerCase())
                return i + 1;
        }
        return apps.length + 1;
    }
    // ComboBox resets its selection when its model is replaced. Restore it
    // after that reset, even if the saved index itself has not changed.
    function restoreSelection() { currentIndex = savedIndex; }
    onModelChanged: Qt.callLater(restoreSelection)
    onSavedIndexChanged: Qt.callLater(restoreSelection)
    Component.onCompleted: restoreSelection()
    displayText: model[savedIndex] || savedApp || qsTr("None")

    onActivated: function(index) {
        if (index === 0)
            appSelected("");
        else if (index > 0 && index <= apps.length && apps[index - 1].binary)
            appSelected(apps[index - 1].binary);
        // Selecting the retained offline entry leaves the saved value intact.
        Qt.callLater(restoreSelection);
    }
}
