/*
 * Trophy Monitor — KWin script (read-only).
 *
 * Pushes a JSON snapshot of all top-level windows to Trophy over D-Bus:
 *   service:   org.trophy.Monitor
 *   path:      /Monitor
 *   interface: org.trophy.Monitor
 *   method:    ReportWindows(QString json)
 *
 * Snapshot format: [{id, appId, title, pid, fullscreen, focused}, ...]
 *
 * The script never modifies windows. Every property access is guarded so
 * API differences between KWin versions degrade to safe defaults instead
 * of killing the script.
 */

function safeGet(client, prop, fallback) {
    try {
        var v = client[prop];
        return (v === undefined || v === null) ? fallback : v;
    } catch (e) {
        return fallback;
    }
}

function snapshot() {
    try {
        var wins = [];
        var list = workspace.windowList();
        for (var i = 0; i < list.length; i++) {
            var c = list[i];
            // Skip invisible helper surfaces (panel, desktop, notifications).
            try {
                if (c.skipTaskbar && c.skipPager && !c.active)
                    continue;
            } catch (e) { /* property missing: keep the window */ }
            wins.push({
                id: safeGet(c, "windowId", 0),
                appId: String(safeGet(c, "resourceClass", "")),
                title: String(safeGet(c, "caption", "")),
                pid: safeGet(c, "pid", 0),
                fullscreen: !!safeGet(c, "fullScreen", false),
                focused: !!safeGet(c, "active", false),
                x: 0, y: 0, width: 0, height: 0,
                outputWidth: 0, outputHeight: 0
            });
            try {
                var g = c.frameGeometry;
                if (g) {
                    wins[wins.length - 1].x = g.x || 0;
                    wins[wins.length - 1].y = g.y || 0;
                    wins[wins.length - 1].width = g.width || 0;
                    wins[wins.length - 1].height = g.height || 0;
                }
                var area = workspace.clientArea(KWin.FullScreenArea,
                                                c.screen,
                                                workspace.currentDesktop);
                if (area) {
                    wins[wins.length - 1].outputWidth = area.width || 0;
                    wins[wins.length - 1].outputHeight = area.height || 0;
                }
            } catch (e) { /* geometry optional: zeros mean unknown */ }
        }
        callDBus("org.trophy.Monitor", "/Monitor", "org.trophy.Monitor",
                 "ReportWindows", JSON.stringify(wins),
                 function () {});
    } catch (e) {
        // Never let a snapshot failure take the script down.
    }
}

try {
    workspace.windowAdded.connect(snapshot);
    workspace.windowRemoved.connect(snapshot);
    workspace.clientActivated.connect(snapshot);
    workspace.clientFullScreenSet.connect(snapshot);
} catch (e) {
    // Older KWin without one of these signals: snapshot still works on load.
}

// Initial push so Trophy has data without waiting for a window event.
snapshot();
