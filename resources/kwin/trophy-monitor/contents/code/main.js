/*
 * Trophy Monitor — KWin script (read-only).
 *
 * Pushes a JSON snapshot of all top-level windows to Trophy over D-Bus:
 *   service:   org.trophy.Monitor
 *   path:      /Monitor
 *   interface: org.trophy.Monitor
 *   method:    ReportWindows(QString json)
 *
 * Snapshot format: [{id, key, appId, resourceName, desktopFileName, title,
 *   pid, fullscreen, focused, minimized, x, y, width, height,
 *   outputWidth, outputHeight}, ...]
 * Extra keys are additive: older Trophy builds read only the keys they
 * know (WindowInfo::fromJson) and ignore the rest.
 *
 * The script never modifies windows. Every property access is guarded so
 * API differences between KWin versions degrade to safe defaults instead
 * of killing the script.
 *
 * Trophy reloads only this script on backend start so the initial snapshot
 * is delivered even after Trophy restarts with no intervening window event.
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
        // KWin 6 exposes windowList(); pre-6 used clientList().
        var list = [];
        try {
            var fn = workspace.windowList || workspace.clientList;
            if (fn)
                list = fn.call(workspace) || [];
        } catch (e) {
            list = [];
        }
        for (var i = 0; i < list.length; i++) {
            var c = list[i];
            // Known compositor surfaces never belong in the game picker,
            // even when active. Normal/dialog windows remain eligible when
            // a game opts out of taskbar/pager entries (e.g. launch splash).
            if (safeGet(c, "desktopWindow", false) ||
                safeGet(c, "dock", false) ||
                safeGet(c, "notification", false) ||
                safeGet(c, "criticalNotification", false))
                continue;
            if (!safeGet(c, "normalWindow", false) &&
                !safeGet(c, "dialog", false) &&
                safeGet(c, "skipTaskbar", false) &&
                safeGet(c, "skipPager", false) && !safeGet(c, "active", false))
                continue;
            var row = {
                id: safeGet(c, "windowId", 0),
                // Stable per-window identity for Wayland windows, where
                // windowId is 0 for every window. Trophy prefers this when
                // present and falls back to the numeric id.
                key: String(safeGet(c, "internalId", "")),
                appId: String(safeGet(c, "resourceClass", "")),
                resourceName: String(safeGet(c, "resourceName", "")),
                desktopFileName: String(safeGet(c, "desktopFileName", "")),
                title: String(safeGet(c, "caption", "")),
                pid: safeGet(c, "pid", 0),
                fullscreen: !!safeGet(c, "fullScreen", false),
                focused: !!safeGet(c, "active", false),
                minimized: !!safeGet(c, "minimized", false),
                modal: !!safeGet(c, "modal", false),
                x: 0, y: 0, width: 0, height: 0,
                outputWidth: 0, outputHeight: 0
            };
            try {
                var g = c.frameGeometry;
                if (g) {
                    row.x = g.x || 0;
                    row.y = g.y || 0;
                    row.width = g.width || 0;
                    row.height = g.height || 0;
                }
            } catch (e) { /* geometry optional: zeros mean unknown */ }
            try {
                // Screen index and desktop argument types vary by KWin
                // version (int vs object); resolve defensively and keep
                // geometry even when the output-area lookup fails.
                var scr = safeGet(c, "screen", 0);
                if (scr === undefined || scr === null)
                    scr = 0;
                var desk = null;
                try {
                    desk = workspace.currentDesktop;
                } catch (e) {
                    desk = null;
                }
                var area = null;
                var tried = false;
                try {
                    // KWin 6 window overload resolves its output/desktop
                    // directly, including Wayland windows without screen.
                    area = workspace.clientArea(KWin.FullScreenArea, c);
                    tried = true;
                } catch (e) {
                    tried = false;
                }
                if (!tried) {
                    try {
                        area = workspace.clientArea(KWin.FullScreenArea, scr, desk);
                    } catch (e2) {
                        try {
                            area = workspace.clientArea(KWin.FullScreenArea, scr);
                        } catch (e3) {
                            area = null;
                        }
                    }
                }
                if (area) {
                    row.outputWidth = area.width || 0;
                    row.outputHeight = area.height || 0;
                }
            } catch (e) { /* output area optional: zeros mean unknown */ }
            wins.push(row);
        }
        callDBus("org.trophy.Monitor", "/Monitor", "org.trophy.Monitor",
                 "ReportWindows", JSON.stringify(wins),
                 function () {});
    } catch (e) {
        // Never let a snapshot failure take the script down.
        try {
            print("trophy-monitor: snapshot failed: " + e);
        } catch (e2) {}
    }
}

var hookedWindows = [];

function hookWindow(win) {
    // Per-window hooks keep geometry/focus/fullscreen fresh between
    // workspace-level events (resize, move across outputs, minimize).
    // Each connect is independent: a missing signal must not skip the rest.
    // KWin window wrappers are QObjects; assigning ad-hoc JS properties
    // on them may fail. Keep identities in script-owned state instead.
    try {
        if (!win || hookedWindows.indexOf(win) !== -1)
            return;
        hookedWindows.push(win);
    } catch (e) {
        return;
    }
    var signals = ["frameGeometryChanged", "screenChanged", "activeChanged",
                   "fullScreenChanged", "minimizedChanged", "captionChanged"];
    for (var i = 0; i < signals.length; i++) {
        try {
            var sig = win[signals[i]];
            if (sig)
                sig.connect(snapshot);
        } catch (e) { /* signal absent on this KWin: skip */ }
    }
}

function onWindowAdded(win) {
    try {
        hookWindow(win);
    } catch (e) {}
    snapshot();
}

function onWindowRemoved(win) {
    var index = hookedWindows.indexOf(win);
    if (index !== -1)
        hookedWindows.splice(index, 1);
    snapshot();
}

function on(sig, fn) {
    try {
        if (sig)
            sig.connect(fn);
    } catch (e) { /* signal absent on this KWin: skip */ }
}

// Each connect is independent with runtime feature-detect: KWin 5 used
// client* names, KWin 6 uses window* names. A shared try/catch here used
// to abort the remaining connects when one name was missing, silently
// dropping focus/fullscreen pushes.
on(workspace.windowAdded, onWindowAdded);
on(workspace.windowRemoved, onWindowRemoved);
on(workspace.windowActivated || workspace.clientActivated, snapshot);
on(workspace.clientFullScreenSet, snapshot);
// Output/desktop changes move or resize the coverage rects used for
// borderless detection; without these they go stale until the next
// unrelated window event.
on(workspace.outputAdded, snapshot);
on(workspace.outputRemoved, snapshot);
on(workspace.outputResized, snapshot);
on(workspace.currentDesktopChanged, snapshot);
on(workspace.desktopLayoutChanged, snapshot);

// Hook already-existing windows so geometry changes push immediately.
try {
    var existing = [];
    try {
        var fn0 = workspace.windowList || workspace.clientList;
        if (fn0)
            existing = fn0.call(workspace) || [];
    } catch (e) {
        existing = [];
    }
    for (var k = 0; k < existing.length; k++) {
        try {
            hookWindow(existing[k]);
        } catch (e) {}
    }
} catch (e) {}

// Initial push so Trophy has data without waiting for a window event.
snapshot();
