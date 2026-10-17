// SPDX-License-Identifier: GPL-3.0-or-later
// Execute the shipped extension with controlled GNOME API objects. This is not
// a real Shell/driver integration test.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(process.argv[2], 'utf8');
let exported = null;
let unexports = 0;
const game = {
    get_window_type: () => 0,
    get_stable_sequence: () => 42,
    get_wm_class: () => null,
    get_gtk_application_id: () => 'native-game',
    get_title: () => 'Game "quoted"',
    get_pid: () => 123,
    is_fullscreen: () => true,
    minimized: false,
    get_monitor: () => 1,
    get_frame_rect: () => ({x: 1920, y: 0, width: 1280, height: 720}),
};
let actors = [game, {...game, get_window_type: () => 4}];
const context = vm.createContext({
    Extension: class {},
    Gio: {DBus: {session: {}}, DBusExportedObject: {
        wrapJSObject: (xml, instance) => {
            assert.match(xml, /org.trophy.WindowHelper1/);
            assert.doesNotMatch(xml, /Eval/);
            return {
                export: (_bus, path) => { exported = {path, instance}; },
                unexport: () => { exported = null; ++unexports; },
            };
        },
    }},
    Meta: {WindowType: {NORMAL: 0, DIALOG: 1, MODAL_DIALOG: 2}},
    global: {
        display: {focus_window: game, get_monitor_geometry: () => ({width: 1280, height: 720})},
        get_window_actors: () => actors.map(w => ({get_meta_window: () => w})),
    },
});
vm.runInContext(source.replace(/^import .*;$/gm, '').replace('export default class', 'class') +
    '\nglobalThis.Helper = TrophyWindowHelper;', context);
const helper = new context.Helper();
helper.enable();
assert.equal(exported.path, '/org/trophy/WindowHelper');
let rows = JSON.parse(helper.GetWindows());
assert.equal(rows.length, 1); // explicit shell/panel surfaces excluded
assert.equal(rows[0].appId, 'native-game');
assert.equal(rows[0].pid, 123);
assert.equal(rows[0].focused, true);
assert.equal(rows[0].outputWidth, 1280);
assert.equal(rows[0].title, 'Game "quoted"');
game.minimized = true;
assert.equal(JSON.parse(helper.GetWindows())[0].minimized, true);
game.get_monitor = () => -1;
assert.equal(JSON.parse(helper.GetWindows())[0].outputWidth, 0);
actors = [];
assert.deepEqual(JSON.parse(helper.GetWindows()), []);
helper.disable();
helper.disable();
assert.equal(exported, null);
assert.equal(unexports, 1);
helper.enable();
assert.ok(exported);
helper.disable();
assert.equal(unexports, 2);
