# Getting started

[Back to README](../README.md) · [User guide](user-guide.md)

## Install and launch

1. Download `Trophy-<version>-x86_64.AppImage` from the [latest release](https://github.com/stevenjwheeler/Trophy/releases/latest).
2. Make the downloaded file executable, using your file manager's permissions settings or `chmod +x <filename>`.
3. Open it from your file manager or run `./<filename>` in a terminal.

Keep the AppImage at a stable location if you enable launch-at-login. For a native prefix install, see [building from source](development.md).

CI also generates `.tar.xz`, `.deb` (amd64), and `.rpm` (x86_64) packages. When included in a release, choose the native package matching your distribution, or unpack the archive. Native packages still require compatible system dependencies; see [packaging notes](../packaging/README.md).

Current native build targets are Ubuntu 26.04 for DEB/archive and Fedora 44 for
RPM. These packages are not guaranteed compatible with older releases or other
Debian/RPM-based distributions.

### Verify the download

Download the release's `SHA256SUMS` beside the AppImage, then run:

```bash
sha256sum -c SHA256SUMS
```

The matching downloaded file should report `OK`; entries for packages you did not download may report missing files. This checks the file against the published checksum; an unsigned checksum alone does not prove publisher identity or authenticity.

## Desktop requirements

- A **Wayland** desktop session. Native Wayland and XWayland games can be captured; an X11 desktop session cannot.
- **PipeWire**, **xdg-desktop-portal**, and a suitable compositor-specific portal backend.
- Compatible GPU drivers for hardware encoding. Available encoders depend on the GPU, driver, and FFmpeg build; software H.264 is also supported. VA-API additionally requires a `libva` compatible with the host's VA driver (such as `radeonsi`/`iHD`); the AppImage prefers the host's `libva` over its bundled copy, so a bundled (possibly older) version never shadows a newer host driver. Set `TROPHY_PREFER_BUNDLED_LIBVA=1` to force the bundled copy, or `TROPHY_LIBVA_DIRS=/path[:/path]` to point the launcher at libva installed somewhere unusual.

Trophy writes an inventory of the runtime libraries it loaded to the log at startup, split into those taken from the host and those taken from the bundle. When reporting a problem, include that section: it shows which `libva`, driver and system libraries were actually in use.

The AppImage bundles application libraries, but still depends on compatible host system libraries, graphics drivers, PipeWire, and desktop portal services. It is not a guarantee of compatibility with every Linux distribution.

If the captured window changes size, Trophy keeps the recording's output canvas fixed, fitting the picture with black bars as needed. Supported NVIDIA and VA-API paths perform resize work on the GPU; unsupported DMA-BUF imports can request shared-memory fallback. Software encoding uses shared-memory capture. VA-API resize is implemented but still needs real AMD/Intel hardware validation.

Trophy includes window-monitor integrations for KDE/KWin, GNOME, Hyprland, and Sway. These do not all have equivalent live verification: KDE/KWin with NVIDIA has live capture evidence; GNOME helper installation/capture and Hyprland/Sway behavior still need broader real-session verification. Hardware encoding support likewise does not guarantee every DMA-BUF capture path works.

## First-run setup

1. Complete the setup wizard, choosing your recording location, video, and audio settings.
2. Explicitly install the required window helper when prompted. Ordinary startup does not install or enable compositor integrations.
3. Choose whether to enable optional game-detection reporters. Read the cautions below first.
4. Assign recording and clip/bookmark shortcuts, or configure native desktop shortcuts if the portal does not support them.
5. Start a game and authorize its capture when prompted. Saved approvals can restore capture on later sessions; stale approvals may need authorization again.

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

GNOME may require a logout/login before discovering a new extension or loading an update. Enable **Trophy Window Helper** in GNOME Extensions if necessary, then reopen Trophy. Setup reports Working only after receiving a valid window snapshot. If it does not, check for extension errors, unsupported Shell versions, or disabled user extensions. The extension targets Shell 45–51; real-session verification remains limited as noted above.

## Keyboard shortcuts

| Action | Behavior |
| --- | --- |
| Recording toggle | Start or stop a full recording |
| Clip / Bookmark | Bookmark an active full recording; otherwise save a DVR clip when ready |

The Global Shortcuts portal suggests F9 and F10, but **your assigned keys are authoritative**. Remap them in Settings or your desktop's shortcut configuration. There is no separate global bookmark shortcut.

Where portal shortcuts are unavailable, bind these commands with your desktop's native shortcut system:

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

On GNOME, add custom shortcuts in Settings → Keyboard pointing at the same commands. Avoid binding an action through both the portal and native configuration to the same key.

Commands contact an already-running Trophy through its user-private control socket. If Trophy is not running, they launch it rather than immediately starting capture. The explicit `--add-bookmark` command is also available for recording-only bookmarks.

## If something does not work

- **Capture unavailable or no picker:** check that you are in Wayland and that PipeWire, the portal, and the correct portal backend are running.
- **Window helper unavailable:** use the explicit Install/Repair action. On GNOME, check Extensions and try a fresh login.
- **Shortcut does nothing:** check the assigned key and portal support; use a native shortcut if needed.
- **Unexpected desktop audio:** game-specific audio may not have bound yet. See the [audio guide](user-guide.md#audio).
- **Hardware encoder unavailable:** check your driver and encoder support, or try software H.264.

For a bug report, use Trophy's report action or [open an issue](https://github.com/stevenjwheeler/Trophy/issues). Include your desktop, GPU/driver, Trophy version, and reproduction steps. Review logs for personal information before posting them.
