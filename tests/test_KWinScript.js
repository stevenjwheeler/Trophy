// Run the real script against a small KWin-6-shaped API. No compositor
// state is modified. This checks behavior, not actual API availability.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

function signal() {
    const callbacks = [];
    return { connect(fn) { callbacks.push(fn); },
             emit(...args) { callbacks.forEach(fn => fn(...args)); } };
}
const game = {
    windowId: 0, internalId: 'game-uuid', resourceClass: 'game',
    caption: 'Game', pid: 4242, normalWindow: true,
    skipTaskbar: true, skipPager: true, active: false,
    frameGeometry: { x: 0, y: 0, width: 1920, height: 1080 },
    frameGeometryChanged: signal(), minimizedChanged: signal(),
    fullScreenChanged: signal(), activeChanged: signal()
};
// Reject ad-hoc properties, as some QObject wrappers do.
Object.preventExtensions(game);
const dock = { dock: true, active: true, caption: 'Panel' };
const workspace = {
    windowList: () => [game, dock],
    windowAdded: signal(), windowRemoved: signal(), windowActivated: signal(),
    clientArea(option, window) {
        assert.equal(window, game);
        return { width: 1920, height: 1080 };
    }
};
let rows = [], reports = 0;
vm.runInNewContext(fs.readFileSync(process.argv[2], 'utf8'), {
    workspace, KWin: { FullScreenArea: 0 }, print() {},
    callDBus(service, path, iface, method, json) {
        rows = JSON.parse(json);
        reports++;
    }
});
assert.equal(rows.length, 1); // Normal taskbar-less game kept; active dock excluded.
assert.equal(rows[0].key, 'game-uuid');
assert.equal(rows[0].outputWidth, 1920);
const initial = reports;
game.frameGeometry.width = 1280;
game.frameGeometryChanged.emit();
assert.equal(reports, initial + 1);
assert.equal(rows[0].width, 1280);
workspace.windowActivated.emit(game);
assert.equal(reports, initial + 2); // No clientActivated alias required.
