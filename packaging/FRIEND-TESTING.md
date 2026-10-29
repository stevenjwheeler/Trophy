# Trophy portable friend test — experimental

This is a trusted-code testing kit, not a system installer. **Do not run as root
or with sudo.** Checksums detect accidental damage, not a malicious publisher.
Only use a bundle received from someone you trust. No automatic uploads occur.

## Run

Keep `Trophy.AppImage`, `friend_test.py`, `bundle.json` and this guide together on
a local executable filesystem. You need x86_64 Linux, Python 3.9+ and a compatible
GPU/driver. No Docker, compiler, namespaces, development environment or system
installation is required. The launcher never installs missing prerequisites.
Your Wayland desktop must already provide PipeWire and its client libraries;
the AppImage uses that matching host installation for actual capture/audio.
Use the Python launcher rather than opening the AppImage directly: the launcher
provides the private Trophy settings, controlled test run and result collection.
If its library baseline is too new for your system, it will collect that result
without launching the incompatible AppImage. Do not upgrade system libraries for
this test; ask the sender for an older-baseline build.

```sh
python3 friend_test.py
# Or select just one part:
python3 friend_test.py --mode tests
python3 friend_test.py --mode preview
```

Automated tests are bundled in the **same AppImage as Trophy**, with the same
Qt/FFmpeg dependencies and access to the real host GPU drivers. They exercise
NVENC or VA-API encoding/decoding, DMA-BUF import, GPU resize/padding, fallback,
importer lifetime, and source-free core/audio units. There is no bundled operating
system or repository source snapshot. This is trusted code, not a security sandbox.

The 13 selected test executables use private HOME/settings/cache/runtime paths
and cannot connect to the normal D-Bus/PipeWire sockets. They do not use Trophy's
fixed detection socket. Native GPU access remains enabled; offscreen Qt does not
force native EGL/CUDA/VA-API tests onto software rendering.

This is a **focused subset**, not the entire CI suite: source-path-dependent UI,
live-service and detection-socket integration tests are intentionally excluded.
Each executable has a five-minute timeout. Failures, crashes and incomplete runs
are retained. If expected NVIDIA or AMD/Intel VA-API GPU cases skip or go missing,
the result says **GPU coverage incomplete**, never fully validated. Hybrid-GPU
systems report both expected paths; either can be unavailable or unsupported.

## Interactive detection and recording

The AppImage runs without installation or a development environment. It uses
your real Wayland desktop, portal and PipeWire. Trophy settings/cache/state and
recordings go into a new local workspace, printed by the launcher. Automatic
recording, replay, deletion, updates, shortcuts and autostart initially stay off.
Do not change the recordings folder to an existing library for this test.

Real HOME, runtime directory, session bus and user data directory remain visible:
Steam identity and reporter/compositor discovery need them. This preview is
**not filesystem-sandboxed**. Close any existing Trophy instance first.

1. In the wizard, read the helper explanation. **Install/Repair is your consent
   to add/enable the compositor helper**, not a full application install.
2. Prefer a non-protected game you know. Vulkan reporting is optional and requires
   explicit enrollment, then starting a **fresh game process**. Games already
   running can sometimes be recognized from their windows; this does not prove
   a reporter was loaded. Do not enable reporters for anti-cheat/protected games.
3. Leave global OpenGL detection OFF. It needs fresh-login preload activation
   and affects unrelated programs. This is not needed for most Proton/Vulkan games.
4. Note whether Trophy shows the correct game and tracking status, whether it
   follows game close, and whether manually starting a short recording works.
   Inspect video and Game/Voice/Mic audio. Include game/GPU/driver/desktop details
   in the feedback. Portal recording approval is still required.
5. Quit Trophy (tray **Quit**, not merely hiding the window) to let the launcher
   finish. Ctrl+C interrupts an owned session and still attempts a report.

GNOME may need a logout/login to discover a newly installed extension. KWin and
GNOME setup is environment-dependent; report missing tools rather than installing
packages just to satisfy the script. Hyprland/Sway use their existing IPC tools.

## What can remain, and cleanup

Detection consent can create files outside the local workspace:

- KWin: `~/.local/share/kwin/scripts/trophy-monitor` and script enable state.
- GNOME: `~/.local/share/gnome-shell/extensions/trophy-monitor@trophy` and extension
  enable state. Disable the Trophy extension using your Extensions application
  before removing its files; logout/login may be needed.
- Vulkan enrollment: Trophy's implicit-layer JSON under the real user data
  `vulkan/implicit_layer.d` and content-addressed reporter copies under
  `trophy/reporters` (usually below `~/.local/share`). Disable Vulkan detection
  in Trophy before quitting; don't delete copies while games still map them.
- Portal approvals/shortcut choices may be remembered by the desktop itself.

Paths follow your existing `XDG_DATA_HOME` if set. The launcher's isolated config
means an OpenGL toggle would not configure a normal fresh-login environment;
leave it off. Do not enable autostart or integrations unrelated to the test.

**Never blindly remove an existing helper/reporters or overwrite a pre-existing
setup.** This first version does not automatically roll back compositor state.
For KWin, disable/remove only the newly installed Trophy script using System
Settings → Window Management → KWin Scripts. For GNOME, disable/remove only the
new Trophy extension. If it existed before the test, leave it alone and contact
the sender for cleanup guidance. You can retain the small helpers until you
switch to the release AppImage instead.

The printed workspace is deliberately retained (it can contain recordings).
After reviewing/saving anything wanted, remove **that specific directory** yourself.
No existing settings, recordings, services or helpers are automatically deleted.

## Return the report

The launcher creates `Trophy-results-<timestamp>.tar.gz` in the current directory.
Inspect it before sending. It contains an allowlisted set of test results/logs,
build hashes, kernel/desktop details and your feedback. Recordings, settings,
saved portal tokens, environment dumps and home-directory contents are excluded.
Home/workspace paths and potentially secret-bearing log lines are redacted on a
best-effort basis. Game titles, device names and other private log text may remain:
**review matters**. Reports are never sent automatically.

## Developer: preparing a kit

Build the app and tests, and stage an AppDir from that same build. Then:

```sh
python3 packaging/build_friend_bundle.py \
  --appdir /path/to/AppDir --build /path/to/build \
  --linuxdeploy /path/to/linuxdeploy-x86_64.AppImage --version 0.9.7-friend-test \
  --output /tmp/opencode/Trophy-friend-kit
```

Build on the pinned Ubuntu 24.04 SDK baseline, not a rolling-release desktop.
The builder gates both the staged payload and finished AppImage at GLIBC 2.39,
GLIBCXX 3.4.32 and CXXABI 1.3.14 and writes `abi-report.json`. See
[the developer container](containers/README.md#appimage-baseline-builder) for a
local build without installing dependencies on the host. Docker is developer
tooling only; it is not sent to friends or needed to run the kit.

Use `--qt-plugin` with the normal linuxdeploy Qt-plugin environment when the
AppDir has only been staged, rather than already deployed. The builder checks
app/library build IDs match the build directory, adds the allowlisted test
binaries and dependencies, and installs a diagnostic-only AppRun. The regular
release AppRun and CI remain unchanged. No vendor GPU driver should be bundled:
NVIDIA/Mesa/Intel driver access must match the normal AppImage on the host.

Send only the four files listed above, not the developer `AppDir`. `bundle.json`
records payload hashes, revision/dirty state and ELF build IDs. Use a suitable
build baseline: an AppImage does not make a newer glibc compatible with older
distributions. Validate the exact AppImage's native GPU tests and startup before
distribution. Fixture tests alone are not portable runtime/hardware validation.
