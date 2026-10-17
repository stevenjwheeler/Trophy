// SPDX-License-Identifier: GPL-3.0-or-later
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const IFACE = `<node><interface name="org.trophy.WindowHelper1">
  <method name="GetWindows"><arg type="s" direction="out" name="windows"/></method>
</interface></node>`;

// A narrow read-only API, not a general Shell.Eval replacement. No timers or
// per-window signal handlers: the app polls while running and disable unexports.
export default class TrophyWindowHelper extends Extension {
    enable() {
        this._dbus = Gio.DBusExportedObject.wrapJSObject(IFACE, this);
        this._dbus.export(Gio.DBus.session, '/org/trophy/WindowHelper');
    }

    disable() {
        this._dbus?.unexport();
        this._dbus = null;
    }

    GetWindows() {
        const focus = global.display.focus_window;
        const windows = [];
        for (const actor of global.get_window_actors()) {
            const w = actor.get_meta_window();
            if (!w)
                continue;
            const type = w.get_window_type();
            // Keep taskbar-less games/dialogs, but never shell/panel surfaces.
            if (type !== Meta.WindowType.NORMAL && type !== Meta.WindowType.DIALOG &&
                type !== Meta.WindowType.MODAL_DIALOG)
                continue;
            const rect = w.get_frame_rect();
            const monitor = w.get_monitor();
            const output = monitor >= 0 ? global.display.get_monitor_geometry(monitor) : null;
            windows.push({
                id: w.get_stable_sequence(),
                appId: w.get_wm_class() || w.get_gtk_application_id() || '',
                title: w.get_title() || '',
                pid: w.get_pid(),
                fullscreen: w.is_fullscreen(),
                focused: w === focus,
                minimized: !!w.minimized,
                x: rect.x, y: rect.y, width: rect.width, height: rect.height,
                outputWidth: output?.width || 0, outputHeight: output?.height || 0,
            });
        }
        return JSON.stringify(windows);
    }
}
