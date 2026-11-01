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

For the **AppImage**, packaging rejects required symbol versions above
**GLIBC 2.39**, **GLIBCXX 3.4.32** and **CXXABI 1.3.14**, including requirements
from bundled libraries and plugins. These are build-policy ceilings, not a
guarantee of compatibility with every system meeting those numbers. An individual
artifact may require lower versions; the diagnostic AppImage build records its
actual minimum glibc in `bundle.json`. Do not upgrade system libraries just to
run Trophy—use a compatible package. See the
[packaging notes](../packaging/README.md#package-formats) for the detailed
baseline and dependency policy.

The AppImage is the recommended starting point when you are not on the exact
native-package target. If FUSE is unavailable, use extract-and-run with your
downloaded filename, for example:

```sh
APPIMAGE_EXTRACT_AND_RUN=1 ./Trophy.AppImage
```

Fedora's official FFmpeg build may omit libx264. Software H.264 recording and
H.264 share export require a codec-enabled FFmpeg provider; follow your distro's
guidance rather than assuming every codec ships with the native package.

### System requirements

- **A Wayland desktop session.** KDE Plasma (KWin) and GNOME (Mutter) are
  supported and verified. Hyprland and Sway have window-monitor integrations
  but less live verification. XWayland games can be captured; a native X11
  desktop session cannot.
- **xdg-desktop-portal**, with the portal backend for your desktop. This is
  required, not optional: screen capture, global shortcuts, notifications and
  native file dialogs all go through it. On KDE Plasma install
  `xdg-desktop-portal-kde` (usually pulled in automatically); on GNOME install
  `xdg-desktop-portal-gnome`. Other desktops need a backend that implements
  `ScreenCast`, `GlobalShortcuts` and `Notification`.
- **PipeWire**, which carries the captured video and audio streams.
- Compatible GPU drivers for hardware encoding: NVIDIA NVENC or AMD/Intel VA-API where supported. Hardware encoding is optional; software H.264 is also supported when the FFmpeg build includes it. Available codecs depend on the GPU, driver and FFmpeg build. VA-API additionally requires a `libva` compatible with the host's VA driver (such as `radeonsi`/`iHD`); the AppImage prefers the host's `libva` over its bundled copy, so a bundled (possibly older) version never shadows a newer host driver. Set `TROPHY_PREFER_BUNDLED_LIBVA=1` to force the bundled copy, or `TROPHY_LIBVA_DIRS=/path[:/path]` to point the launcher at libva installed somewhere unusual.
- A system tray host is optional. KDE Plasma provides one; on GNOME, install the
  **AppIndicator and KStatusNotifierItem Support** extension if you want a tray
  icon. Trophy runs without one — see [Closing and the tray](#closing-and-the-tray).

### Supported desktops

| Desktop | Window helper | Global shortcuts | Tray |
| --- | --- | --- | --- |
| **KDE Plasma (KWin)** | Bundled `trophy-monitor` KWin script | Portal `GlobalShortcuts` | Built in |
| **GNOME (Mutter)** | Bundled Trophy Window Helper extension | Portal `GlobalShortcuts` | Needs the AppIndicator extension |
| **Hyprland / Sway** | Built-in compositor integration | Portal if the backend implements it, otherwise native binds | Compositor-dependent |

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

Trophy writes an inventory of the runtime libraries it loaded to the log at startup, split into those taken from the host and those taken from the bundle. When reporting a problem, include that section: it shows which `libva`, driver and system libraries were actually in use.

The AppImage bundles application libraries, but still depends on compatible host system libraries, graphics drivers, PipeWire, and desktop portal services. It is not a guarantee of compatibility with every Linux distribution.

If the captured window changes size, Trophy keeps the recording's output canvas fixed, fitting the picture with black bars as needed. Supported NVIDIA and VA-API paths perform resize work on the GPU; unsupported DMA-BUF imports can request shared-memory fallback. Software encoding uses shared-memory capture. Synthetic hardware tests do not establish compatibility with every compositor, AMD/Intel driver or sustained recording workload.

Trophy includes window-monitor integrations for KDE/KWin, GNOME, Hyprland, and Sway. KDE Plasma and GNOME are supported and verified end to end, including the window helper, portal global shortcuts and capture. Hyprland and Sway have integrations with less live verification. Hardware encoding support likewise does not guarantee every DMA-BUF capture path works.

## First-run setup

1. Complete the setup wizard, choosing your recording location, video, and audio settings.
2. With an AppImage, Trophy registers its menu entry automatically on first launch, before it talks to the portal, so capture approvals and shortcuts belong to Trophy from the start. Nothing to confirm; **Settings → Desktop Integration** shows the entry and can remove it. Installing through an AppImage manager such as Gear Lever works too — Trophy keeps its own entry for the stable identity and hides it from menus so you are not offered a duplicate.
3. Explicitly install the required window helper when prompted. Ordinary startup does not install or enable compositor integrations.
4. Choose whether to enable optional game-detection reporters. Read the cautions below first.
5. Assign recording and clip/bookmark shortcuts. KDE Plasma and GNOME show the portal approval dialog; if your backend does not support portal shortcuts, bind the commands described under [Keyboard shortcuts](#keyboard-shortcuts) natively.
6. Start a game and authorize its capture when prompted. Saved approvals can restore capture on later sessions; stale approvals may need authorization again.

Manual recording is also available. When no game is tracked, the Record action opens the system capture picker. When a game is tracked, it targets that game.

## Game detection

Trophy combines optional Vulkan/OpenGL reporter events with game identity and window context. Detection is conservative: not every launcher, graphics API, or runtime is covered. Use the Games page to manage recognition and blocking rules.

Both reporters are opt-in. **Neither is certified compatible with anti-cheat systems.** Leave them disabled for protected games unless the game's policy permits them. Trophy does not bypass anti-cheat software.

The OpenGL option writes **session-wide `LD_PRELOAD` configuration**, which can load its library into unrelated applications too. Enable/disable changes require a fresh login environment; already-running launchers can retain the old environment. Disabling reporting is not the same as preventing a library from loading. Host/Steam-runtime and 32-bit reporter compatibility are not universally established.

See [reporter scope and packaging cautions](../packaging/README.md#reporter-scope-and-anti-cheat-caution) for more detail.

### KDE Plasma / KWin window helper

On KDE Plasma Wayland, Trophy uses the bundled **`trophy-monitor` KWin script** to read window identity, focus, fullscreen, and minimized state. The script supplies window context for game detection; screen capture is separately authorized through the portal.

Choose **Install helper** during setup or **Repair** in Settings to install and enable the script. Ordinary startup does not install a missing script or re-enable one you have disabled.

If tracking is unavailable, check the script in **System Settings → Window Management → KWin Scripts**, then use Trophy's Repair action if needed. Setup reports Working only after the script is enabled and Trophy receives a valid window snapshot—not merely because the script is installed.

### GNOME window helper

On GNOME Wayland, Trophy uses the bundled **Trophy Window Helper** extension for read-only window identity, focus, fullscreen, and minimized state. Screen capture is separately authorized through the portal; GNOME unsafe mode is not required.

Choose **Install helper** during setup or **Repair** in Settings. This copies the helper to `$XDG_DATA_HOME/gnome-shell/extensions/trophy-monitor@trophy` (normally under `~/.local/share`) and requests enabling it.

GNOME may require a logout/login before discovering a new extension or loading an update. Enable **Trophy Window Helper** in GNOME Extensions if necessary, then reopen Trophy. Setup reports Working only after receiving a valid window snapshot. If it does not, check for extension errors, unsupported Shell versions, or disabled user extensions. The extension supports GNOME Shell 45–51.

## Keyboard shortcuts

| Action | Behavior |
| --- | --- |
| Recording toggle | Start or stop a full recording |
| Clip / Bookmark | Bookmark an active full recording; otherwise save a DVR clip when ready |

Trophy registers its global shortcuts through the **xdg-desktop-portal
GlobalShortcuts** interface. On KDE Plasma and GNOME this is the recommended
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
- **Window helper unavailable:** use the explicit Install/Repair action. On GNOME, check Extensions and try a fresh login.
- **Shortcut does nothing or Configure does nothing:** make sure the reverse-DNS desktop entry is present (**Settings → Desktop Integration**) and restart Trophy after installing or removing it. Then retry **Settings → Keyboard Shortcuts → Configure**; if the desktop's portal backend still rejects it, use the native shortcut commands above. Avoid double-binding the same key through both paths.
- **Closing Trophy leaves no tray icon (GNOME):** install the AppIndicator/KStatusNotifierItem extension for a tray, or launch Trophy again to raise the hidden window. See [Closing and the tray](user-guide.md#closing-and-the-tray).
- **Capture asks for approval again after an update:** portal approvals are stored per app id. Upgrading from a build that used the old `trophy` id requires one new approval.
- **Unexpected desktop audio:** game-specific audio may not have bound yet. See the [audio guide](user-guide.md#audio).
- **Hardware encoder unavailable:** check your driver and encoder support, or try software H.264.

For a bug report, use Trophy's report action or [open an issue](https://github.com/stevenjwheeler/Trophy/issues). Include your desktop, GPU/driver, Trophy version, and reproduction steps. Review logs for personal information before posting them.
