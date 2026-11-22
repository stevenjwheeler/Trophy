# Getting started

[Back to README](../README.md) · [User guide](user-guide.md)

## Install and launch

Check the [system requirements and package compatibility](#requirements) before downloading.

1. Download `Trophy-<version>-x86_64.AppImage` from the [latest release](https://github.com/stevenjwheeler/Trophy/releases/latest).
2. Move it somewhere permanent — for example `~/Applications/Trophy.AppImage` — rather than leaving it in `~/Downloads`. Launch-at-login and the desktop entry point at this exact path, and updates rewrite the file in place, so the folder has to stay put and be writable.
3. Make the downloaded file executable, using your file manager's permissions settings or `chmod +x <filename>`.
4. Open it from your file manager or run `./<filename>` in a terminal.

For a native prefix install, see [building from source](development.md).

CI also generates `.tar.xz`, `.deb` (amd64), and `.rpm` (x86_64) packages. When included in a release, choose the native package matching your distribution, or unpack the archive. Native packages still require compatible system dependencies; see [packaging notes](../packaging/README.md).

Choose the package for your system using the [requirements table](#package-compatibility).

### Verify the download

Download the release's `SHA256SUMS` beside the AppImage, then run:

```bash
sha256sum -c SHA256SUMS
```

The matching downloaded file should report `OK`; entries for packages you did not download may report missing files. This checks the file against the published checksum; an unsigned checksum alone does not prove publisher identity or authenticity.

## Desktop integration

An AppImage is not installed into your desktop's application directories, so the portal cannot resolve Trophy's application identity. That matters beyond the file picker: screen-capture approvals are stored per application, so an unregistered Trophy shares the generic entry used by any unregistered program.

To fix this, on first launch Trophy writes its own entry — `$XDG_DATA_HOME/applications/io.github.stevenjwheeler.Trophy.desktop`, usually `~/.local/share/applications/io.github.stevenjwheeler.Trophy.desktop`, plus an icon — before it contacts the portal. The AppImage itself is not copied or moved, and the built-in updater replaces it in place, so the entry keeps working across updates. The first run therefore already has a stable identity: no setup step and no restart.

An entry from a distribution package, AppImageLauncher, Gear Lever or your own `.desktop` file counts the same. If another launcher already lists Trophy, Trophy's entry is hidden from menus but still supplies the identity, so you do not get a duplicate.

If the entry is missing — a read-only `XDG_DATA_HOME`, or you removed it earlier — use **Settings → Desktop Integration → Integrate**. The identity and the system file-dialog backend are fixed when Trophy starts, so Trophy offers a restart to apply it. Until then file dialogs use Qt's own rather than the system dialog, and approvals made in the meantime belong to the unregistered identity, so Trophy asks for them again afterwards. **Remove** deletes the entry and stops Trophy recreating it.

## Requirements

These are the build targets and requirements for the current CI configuration.
Previously published releases may use different baselines; check their release
notes. A build target is not a claim that every desktop, driver or newer distro
version has been verified.

### Package compatibility

All distributed binaries target **x86_64 / amd64 Linux**. Qt/FFmpeg development
packages, a compiler and Python are not needed to run the ordinary application.

| Download | Build/test baseline | What your system must provide |
| --- | --- | --- |
| **AppImage** | **Ubuntu 24.04** SDK baseline; other compatible glibc-based distros may work | Compatible glibc/C++ runtime, normal desktop graphics/font libraries, GPU drivers, PipeWire client libraries/configuration, and `xdg-desktop-portal` with your desktop's portal backend. Qt and FFmpeg are bundled. Normal mounting needs working FUSE support; extract-and-run avoids FUSE. |
| **DEB** | **Ubuntu 26.04 amd64** | Compatible Ubuntu runtime packages, including Qt, FFmpeg, QML modules, PipeWire and `xdg-desktop-portal`. The package manager installs declared dependencies. Other Debian-based distros and newer Ubuntu releases are not automatically guaranteed compatible. |
| **RPM** | **Fedora 44 x86_64** | Compatible Fedora Qt, FFmpeg, Wayland, PipeWire and `xdg-desktop-portal` runtime packages, installed through the package manager. Other RPM-based distros and newer Fedora releases are not automatically guaranteed compatible. |
| **tar.xz** | **Ubuntu 26.04-compatible x86_64 environment** | Compatible native Qt/FFmpeg and desktop dependencies (including `xdg-desktop-portal` and a backend), supplied by you. Preserve the extracted directory layout. This is a native install-tree archive, **not a self-contained AppImage**. |

**Not sure which download to choose? Start with the AppImage.** It includes
Trophy's application libraries and is the recommended option if your system
does not match a native package listed above. It still needs a compatible
Wayland desktop, PipeWire and graphics drivers. If a package is incompatible,
choose another supported package rather than replacing core system libraries.
Detailed build requirements are in the [packaging notes](../packaging/README.md).

If the AppImage will not open because FUSE is unavailable, you can run it without
FUSE from a terminal. Replace `Trophy.AppImage` with your downloaded filename:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 ./Trophy.AppImage
```

**Using a native package on Fedora?** Some video codecs may not be included.
If software recording or MP4 export is unavailable, try the AppImage or follow
your distribution's guidance for installing the missing codecs.

### System requirements

- **A Wayland desktop session** with a compatible ScreenCast portal backend.
  Trophy is designed to work across Wayland compositors, without
  compositor-specific scripts, extensions or IPC. Portal capabilities and
  driver support still determine capture compatibility; not every combination
  has been live-tested. XWayland games can be captured; a native X11 desktop
  session cannot.
- **xdg-desktop-portal**, with the portal backend for your desktop. This is
  required for screen capture. Global shortcut binding also uses the portal
  when its backend implements `GlobalShortcuts`; native command bindings are
  an alternative when it does not. Native file dialogs use your desktop's
  available integration. On KDE Plasma install
  `xdg-desktop-portal-kde` (usually pulled in automatically); on GNOME install
  `xdg-desktop-portal-gnome`. These are examples, not a list of supported
  compositors: other Wayland desktops can use their compatible `ScreenCast`
  backend.
- **PipeWire**, which carries the captured video and audio streams.
- Compatible GPU drivers for hardware encoding: NVIDIA NVENC or AMD/Intel VA-API where supported. Hardware encoding is optional; software H.264 is also supported when the FFmpeg build includes it. Available codecs depend on the GPU, driver and FFmpeg build. VA-API additionally requires a `libva` compatible with the host's VA driver (such as `radeonsi`/`iHD`); the AppImage prefers the host's `libva` over its bundled copy, so a bundled (possibly older) version never shadows a newer host driver. Set `TROPHY_PREFER_BUNDLED_LIBVA=1` to force the bundled copy, or `TROPHY_LIBVA_DIRS=/path[:/path]` to point the launcher at libva installed somewhere unusual.
- A system tray host is optional. KDE Plasma provides one; on GNOME, install the
  **AppIndicator and KStatusNotifierItem Support** extension if you want a tray
  icon. On GNOME, Trophy also runs without one, including **Start minimized** and
  start-on-login (the window stays hidden and launching Trophy again shows it) — see
  [Closing and the tray](user-guide.md#closing-and-the-tray).

### Supported desktops

Trophy is not restricted to KDE, GNOME, Hyprland or Sway. Game detection and the
Add Game process picker are compositor-independent; capture uses the standard
ScreenCast portal. No Trophy-specific window integration is needed.

| Feature | Desktop requirement |
| --- | --- |
| Window/screen capture | Compatible `ScreenCast` portal backend and PipeWire |
| Global shortcut configuration in Trophy | Backend implementing `GlobalShortcuts` |
| Shortcut fallback | Native desktop bindings for Trophy's CLI commands |
| Tray icon | Optional system tray/AppIndicator host |

Saved-approval restoration, window versus screen selection, global shortcut
availability and DMA-BUF support vary by portal backend and driver. The
compositor-independent design does not mean every desktop/GPU combination has
been verified.

Trophy registers a reverse-DNS desktop entry
(`io.github.stevenjwheeler.Trophy.desktop`) so the portal can identify it.
Desktop portals require that identity: without it, GNOME silently discards
shortcut-configuration requests and approvals are shared with unregistered
apps. AppImage installs write the entry automatically on first launch; native
packages install it. After upgrading from a build that used the old bare
`trophy` id, you may need to authorize screen capture once more, and a fresh
login is needed for the renamed autostart entry to take effect.

Also see [Closing and the tray](user-guide.md#closing-and-the-tray) for what
closing the window does on each desktop.

Trophy writes an inventory of the runtime libraries it loaded to its detailed
`trophy.log` at startup, split into host and bundled libraries. This inventory
and encoded-stream start/stop messages are debug-level diagnostics, hidden from
normal console output; launch with `TROPHY_DEBUG=1 trophy` to show them there.
When reporting a problem, include the inventory section: it shows which `libva`,
driver and system libraries were actually in use.

The AppImage bundles application libraries, but still depends on compatible host system libraries, graphics drivers, PipeWire, and desktop portal services. It is not a guarantee of compatibility with every Linux distribution.

If the captured window changes size, Trophy keeps the recording's output canvas fixed, fitting the picture with black bars as needed. Supported NVIDIA and VA-API paths perform resize work on the GPU; unsupported DMA-BUF imports can request shared-memory fallback. Software encoding uses shared-memory capture. Synthetic hardware tests do not establish compatibility with every compositor, AMD/Intel driver or sustained recording workload.

Trophy does not install a compositor script/extension or use compositor IPC.
Game detection is separate from portal capture compatibility. Hardware encoding
support likewise does not guarantee every DMA-BUF capture path works.

## First-run setup

1. Complete the setup wizard, choosing your recording location, video, and audio settings.
2. Assign your recording and clip/bookmark shortcuts. If your desktop does not support configuring them in Trophy, use the [native keyboard shortcut instructions](#keyboard-shortcuts).
3. Start a game. If Trophy does not detect it, open **Games → Add Game**, select its process, and choose **Watch**.
4. Authorize capture when prompted and select your game window in the desktop's capture picker.

Manual recording is also available. When no game is tracked, the Record action opens the system capture picker. When a game is tracked, it targets that game.

## Game detection

Trophy polls Linux processes independently of windows, then associates PipeWire
playback streams with game instances. Native and Proton/Wine executables use
local launcher installation metadata and exact user rules. Shared storefronts,
Steam runtime infrastructure and Wine services are not treated as games. A
verified game-specific launcher can establish a game's identity while Trophy
waits for its game payload before automatic capture. Use Games to block
applications, define standalone games, or select among concurrent instances.

Supported local metadata sources include **Steam**, **Heroic** (Epic, GOG and
Amazon), **Legendary** (Epic), **Nile** (Amazon) and **Lutris**. Games without a
usable metadata match can be added manually through **Games → Add Game**; see
[Adding a standalone game](#adding-a-standalone-game).

Automatic recording and DVR wait for game audio during startup, with a bounded
fallback for silent games; see [Launcher and window handoffs](user-guide.md#launcher-and-window-handoffs).
When game audio cannot be captured, Trophy falls back to desktop audio and
retries game-audio capture every three seconds. The desktop mix includes other applications
and Voice; a separate Voice source is suppressed to avoid duplication.
The microphone remains independent. Capture diagnostics indicate fallback.
Screen capture still requires portal authorization.
No game process injection, graphics layer, root access or compositor PID is
needed. This is not an anti-cheat certification.

Trophy automatically cleans up its old graphics-reporter registration at startup.
If you upgrade while Steam or another launcher is already running, you may need
to sign out and back in to clear the old launch environment. See
[architecture and limitations](game-detection.md).

### Adding a standalone game

Start the game, open **Games → Add Game**, select its process and choose
**Watch** or **Block**. The default **Likely applications** view reduces
background-process noise; enable **Show all processes** if your game is not
listed. These suggestions are not a list of verified visible windows.
Rows show the executable/payload path and PID, including
Wine payloads rather than their loader. **Refresh** reads the latest cached
census. Trophy checks PID/start identity and payload again before saving a rule,
so an exited/reused process cannot silently become the wrong definition.

## Keyboard shortcuts

| Action | Behavior |
| --- | --- |
| Recording toggle | Start or stop a full recording |
| Clip / Bookmark | Bookmark an active full recording; otherwise save a DVR clip when ready |

Trophy registers its global shortcuts through the **xdg-desktop-portal
GlobalShortcuts** interface. On desktops with a compatible backend this is the recommended
path: the desktop owns the key grab, so the keys work while you are in-game,
and Trophy never installs a global keyboard hook.

- The first time you enable **Settings → Keyboard Shortcuts**, your desktop asks
  you to approve the two actions. It suggests `F9` (recording toggle) and `F10`
  (clip/bookmark); **your assigned keys are authoritative**.
- **Configure** opens your desktop's own shortcut editor (KDE System Settings or
  the GNOME shortcut editor). You can also remap the keys there.
- The portal keeps the key grab even if you turn the feature off; disabling it
  makes Trophy ignore activations rather than releasing the keys.
- There is no separate global bookmark shortcut.
- The portal identifies Trophy by its app id, so the reverse-DNS desktop entry
  described in [Supported desktops](#supported-desktops) must be present. After
  upgrading from an older build you may need to open Configure or re-approve
  once.

If your desktop's portal backend does not implement GlobalShortcuts, bind these
commands with your desktop's native shortcut system instead:

```bash
trophy --toggle-record
trophy --save-clip
```

These examples assume a native install with `trophy` on `PATH`. With an AppImage, substitute its absolute, stable path, followed by the same argument.

Sway example (`~/.config/sway/config`):

```ini
bindsym $mod+F9 exec trophy --toggle-record
bindsym $mod+F10 exec trophy --save-clip
```

Hyprland example (`~/.config/hypr/hyprland.conf`):

```ini
bind = $mainMod, F9, exec, trophy --toggle-record
bind = $mainMod, F10, exec, trophy --save-clip
```

On GNOME, native fallback shortcuts live in Settings → Keyboard → Custom
Shortcuts. Avoid binding an action through both the portal and native
configuration to the same key, or it may fire twice.

Commands contact an already-running Trophy through its user-private control socket. If Trophy is not running, they launch it rather than immediately starting capture. The explicit `--add-bookmark` command is also available for recording-only bookmarks.

## If something does not work

- **Capture unavailable or no picker:** check that you are in Wayland and that PipeWire, `xdg-desktop-portal` and the correct portal backend are running. On KDE Plasma that is `xdg-desktop-portal-kde`; on GNOME, `xdg-desktop-portal-gnome`.
- **Game missing from Add Game:** enable **Show all processes**, start the game and refresh. There is no compositor helper or Install/Repair step; detection uses Linux process metadata.
- **Shortcut does nothing or Configure does nothing:** make sure the reverse-DNS desktop entry is present (**Settings → Desktop Integration**) and restart Trophy after installing or removing it. Then retry **Settings → Keyboard Shortcuts → Configure**; if the desktop's portal backend still rejects it, use the native shortcut commands above. Avoid double-binding the same key through both paths.
- **Closing Trophy leaves no tray icon (GNOME):** install the AppIndicator/KStatusNotifierItem extension for a tray, or launch Trophy again to raise the hidden window. **Start minimized** and start-on-login use the same hidden background mode. See [Closing and the tray](user-guide.md#closing-and-the-tray).
- **Capture asks for approval again after an update:** portal approvals are stored per app id. Upgrading from a build that used the old `trophy` id requires one new approval.
- **Unexpected desktop audio:** game-specific audio may not have bound yet. See the [audio guide](user-guide.md#audio).
- **Hardware encoder unavailable:** check your driver and encoder support, or try software H.264.

For a bug report, use Trophy's report action or [open an issue](https://github.com/stevenjwheeler/Trophy/issues). Include your desktop, GPU/driver, Trophy version, and reproduction steps. Review logs for personal information before posting them.
