# Building and contributing

[Back to README](../README.md) · [Packaging notes](../packaging/README.md)

## Compositor-independent detection

Game detection and Add Game use the asynchronous Linux process census.
No KWin scripts, GNOME Shell extensions or Hyprland/Sway IPC are built,
installed or invoked. Window/screen selection and authorization belong to the
standard ScreenCast portal, not Trophy-specific desktop integrations.
Use `detection_probe --benchmark` to measure local census, launcher metadata and
identification cost; it does not open a compositor connection.

The Add Game list is a process picker, not a visible-window census. There is
no universally available cross-desktop Wayland API that gives Trophy a
reliable pre-authorization window-to-PID list. Desktop-entry, environment and
audio hints may help rank likely applications, but must not be represented
as proof that a process owns a visible window. See the
[detection architecture](game-detection.md).

Add Game defaults to **Likely applications**: known game primaries, Windows
payloads and executable matches from cached desktop application launchers.
**Show all processes** restores the other selectable process rows. Desktop
metadata is refreshed on the census worker alongside installed-game metadata;
opening/filtering the picker performs no new filesystem enumeration.

## Recording engine and output policies

`Application` shares one `RecordingEngine` between full recording and DVR.
The engine directly owns capture, encoders, recording audio and sessions.
`RecordingEngineCapture.cpp` contains its native implementation, not a separate
capture manager or pipeline object. `RecordingManager` contains its private
file writer; `ReplayManager` contains its private packet buffer and export jobs.
Neither manager receives native pipeline handles. Their boundary consists of
immutable encoded packets, stream descriptions, generation/timeline IDs and
pinned identity metadata.
Manager unit tests cover consumer policy and completion feedback; engine tests
cover producer demand and lifecycle. Private-service integration tests inspect
the shared engine rather than injecting capture resources into either manager.

Consumer attachment supplies demand. Compatible attachments reuse the approved
live input and encoder; releasing the last consumer drains and stops the
producer. Mode switches attach the successor before releasing the previous
consumer. An individual output failure closes only that output while surviving
consumers continue receiving the same stream.
Capture, encoding, recording-audio and permission/recovery changes therefore
apply to both full recording and DVR. File writing/completion and replay
retention/export remain separate consumer policies and need their own tests;
shared producer ownership is not a guarantee against mode-specific regressions.

`CaptureLifecycle` supplies instance-bound recovery, attempt-scoped source/token
events and shared authorization fences. Recovery and pipeline reconfiguration
belong to the engine, not either output. Full recording files completed native
generations as separate parts; DVR retains generations across verified handoffs.
Header-only generations are not usable replay inputs.

The DVR caches closed GOP packet blocks and holds the current GOP in memory.
Clip saves take an immutable request-time snapshot, reserving cached files and
copying the open GOP's packet references. Saving neither rotates nor recreates
the running encoder, capture or audio session. Snapshots remain valid through
pruning, consumer switches and buffer destruction. Export failure does not stop
buffering; real buffer-write or finalization failures close that consumer.
Snapshots retain the producer's video decode timestamps alongside cached GOPs;
export maps them through the actual container clock, including audio-induced
timestamp shifts. It does not infer decode order from presentation timestamps.

Closure completion uses the pinned instance's liveness independently of restore
permission. DVR drains native work immediately, then allows a bounded exit grace
for a still-running member or awaits the authoritative census across a verified
launch-context gap. Confirmed exit is quiet; a live/unclassified closure remains
terminal and fenced. Output failures take precedence over exit classification,
and explicit Stop cancels only delayed capture-closure feedback, not output errors.

Native resolution is chosen from each new capture, not a previous loader.
In-place size changes within the same capture keep that generation's encoded
canvas; persisting a GOP or saving a clip does not create a new generation.
`ReplayExporter` stream-copies compatible clips; clips spanning incompatible
video formats normalize video to the newest segment's canvas on the export
worker, preserving aspect ratio and copying audio. This fallback requires
libx264, uses single-threaded codecs, and never modifies the buffered originals.

## Dependencies

- CMake 3.21+, a C++20 compiler, and pkg-config.
- Qt 6 development packages: Core, Gui, Qml, Quick, QuickControls2, Multimedia, DBus, Concurrent, Network, and Widgets.
- FFmpeg development libraries: libavcodec, libavformat, libavutil, libswscale, and libswresample.
- PipeWire development libraries.
- libyaml and SQLite development libraries (read-only local launcher metadata).
- Optional libva/libva-drm for VA-API, libdrm and EGL for DMA-BUF support, and GBM for related tests.
- GoogleTest and Qt Test when building tests (enabled by default).

CMake currently declares Qt 6.5 as its minimum, but also uses newer Qt CMake policies such as QTP0004. Do not assume that declaration guarantees a successful build with every Qt 6.5 installation; use a recent Qt 6 toolchain. Distribution package versions and available FFmpeg encoders vary.

The FFmpeg executable is used by optional test fixtures, not production recording, previews, or export. Share export requires FFmpeg library support for libx264 and AAC encoding.

The AppImage does not use the distribution's FFmpeg. It builds one slim GPL
FFmpeg 9 with `packaging/ffmpeg/build-ffmpeg.sh`, which both the app and Qt
Multimedia load (`libavcodec.so.63`), so a single generation is bundled. Local
native builds, DEB/TXZ and RPM packages still link the distribution's FFmpeg.

### Example package lists

These are starting points, not a guarantee that every distribution release supplies sufficiently recent packages. Install a compiler/build toolchain too.

Arch/CachyOS:

```bash
sudo pacman -S --needed base-devel cmake qt6-base qt6-declarative qt6-multimedia \
    ffmpeg pipewire libva pkgconf libyaml sqlite \
    libdrm mesa gtest
```

Fedora (FFmpeg development package availability depends on enabled repositories):

```bash
sudo dnf install gcc-c++ make cmake qt6-qtbase-devel qt6-qtdeclarative-devel \
    qt6-qtmultimedia-devel ffmpeg-devel pipewire-devel libva-devel libyaml-devel sqlite-devel \
    libdrm-devel mesa-libEGL-devel \
    mesa-libgbm-devel gtest-devel
```

Ubuntu/Debian:

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev qt6-declarative-dev \
    qt6-multimedia-dev libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev libpipewire-0.3-dev libva-dev libyaml-dev libsqlite3-dev \
    libgtest-dev libdrm-dev libegl-dev libgbm-dev
```

## Build and install

```bash
git clone https://github.com/stevenjwheeler/Trophy.git
cd Trophy
cmake -S . -B ../trophy-build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build ../trophy-build --parallel
cmake --install ../trophy-build --prefix "$HOME/.local"
```

Use a build directory on a local filesystem if the source is on a cloud/FUSE mount. Such mounts can strip executable permissions or miss timely rebuilds. The sibling directory above is only an example; choose another path when needed.

For read-only source snapshots (for example container package builds), configure
with `-DQT_QML_GENERATE_QMLLS_INI=OFF` to disable source-tree IDE metadata writes.
Normal developer builds keep generation enabled by default.

The install includes `libTrophy.so` and desktop integration as well as the executable. **Do not copy only the binary.** Ensure `~/.local/bin` is on `PATH` for native CLI shortcuts. Reconfigure after changing branches/commits if you need the displayed configure-time version stamp refreshed.

Desktop and autostart entries use the reverse-DNS id
`io.github.stevenjwheeler.Trophy.desktop`. Do not rename them back to
`trophy.desktop`: xdg-desktop-portal derives a host app's id from the launching
systemd unit, and portal backends (notably GNOME's GlobalShortcuts provider)
reject ids without a dot. Global shortcuts are portal-only
(`org.freedesktop.portal.GlobalShortcuts`); keep new shortcut code on that path
rather than adding compositor-specific hooks. Runtime capture, shortcut binding
and configure requests all require the desktop's portal backend to be present.

## Development and tests

```bash
cmake -S . -B ../trophy-build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build ../trophy-build --parallel
QT_LOGGING_RULES="*.debug=true" ../trophy-build/trophy
```

Running the app this way uses your normal user configuration. For automated tests, isolate HOME and XDG paths:

```bash
test_root=$(mktemp -d)
mkdir -p "$test_root/home" "$test_root/config" "$test_root/data" "$test_root/cache"
mkdir -m 700 "$test_root/runtime"
HOME="$test_root/home" \
    XDG_CONFIG_HOME="$test_root/config" XDG_DATA_HOME="$test_root/data" \
    XDG_CACHE_HOME="$test_root/cache" XDG_RUNTIME_DIR="$test_root/runtime" \
    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    ctest --test-dir ../trophy-build --output-on-failure --timeout 300
```

Integration tests launch private D-Bus or PipeWire environments. Hardware and optional-codec tests skip when requirements are unavailable. Passing automated tests does not establish compatibility with every compositor, GPU, or game runtime. Process tests do not use a fixed reporter socket.

Isolated configuration and services do **not** isolate the physical GPU or its
memory. Do not run GPU encoder/import tests or export tests that probe hardware
while someone is gaming or validating live capture. Select software-only and
private-service groups instead, with at most two reduced-priority workers;
exclude GPU-capable groups and set `CUDA_VISIBLE_DEVICES=-1` in test children
as an additional safeguard, not a substitute for selecting safe tests. Run the
hardware groups separately when the GPU is available, and record exclusions.

`test_RecordingEngine` uses fake capture, encoding and packet outputs, gated
audio-lookup workers, and owned helper processes. It covers process-instance
fences, launcher/census handoffs, manual-source naming, startup audio layouts,
stale completions and explicit portal reselection without using desktop services
or the GPU. Run it with the isolated environment above and
`ctest -R 'RecordingEngineTest|Engine(Naming|AudioIdentity|Startup|Routing|Census|IdentityFence|StaleCallback|PortalFence)Test'`.
Routing-layout tests exercise the same planner used by native startup; they do
not establish PCM delivery. Keep the private-service recording/audio regressions
for real media and consumer-specific recording/DVR completion behavior.

For AppImage payload/startup smoke checks, see [packaging notes](../packaging/README.md). Those checks have narrower scope than live recording tests.

Package harness regressions can run independently without a build or desktop:

```sh
python3 tests/test_NativePackageSmoke.py
python3 tests/test_AppImageSmoke.py
python3 tests/test_NativeInstallSafety.py
```

These use synthetic payloads and owned fake processes, not real recording.
They cover missing/non-executable/escaping payloads, multilib and spaced paths,
wrong metadata/linkage, failed dependency inspection, unsafe tar extraction,
and false startup successes including QML binding/layout loops. They are also
registered with CTest. The install-harness safety cases never invoke a real
package manager. Real trusted native prefixes/tar.xz and AppImages should
separately pass their package smoke harness; fixture passes alone are not
artifact verification. CI additionally builds distro-native DEB/RPM packages
and checks installation, reinstallation, upgrade and removal in fresh matching
runtime containers; see [container checks](../packaging/containers/README.md).

### Isolated service, format and permission tests

The portal and application-audio suites can exercise service loss without
restarting your desktop services:

```sh
ctest --test-dir ../trophy-build --output-on-failure --timeout 120 \
    -R 'WaylandPortalCaptureTest|EveryStage/PortalMethodFailureTest|AppAudioGroupTest.PrivatePipeWire|RecordingFailureTest'
```

- Portal integration uses a private D-Bus with service activation disabled.
  Tests send session-closure signals, terminate their fake portal, check stale
  responses/native-event rejection, and create a new session afterward. The
  active-session fixture emulates the final native-video startup boundary; it
  does not capture a real compositor or exercise a permission dialog.
- Application lookup refreshes registry snapshots with current node properties
  through a bounded sync barrier, replacing changed property dictionaries so
  removed identities do not linger. Game lookup can fill missing identity
  fields from the playback node's owning client; direct node matches win, and conflicting
  process identities are not combined. Strict Voice binary matching and its
  routing verification do not inherit client identity. Lookup diagnostics
  distinguish no matching playback identity from connection/query failures.
  Updates after the query barrier are picked up by the existing session retry,
  not by a permanent registry subscription.
- Automatic recordings and DVR pin the detected game's identity at capture
  start. When the portal reports PID 0, audio setup and session retries retain
  that pinned game PID, rather than relying on a display title that may not
  match an engine-named stream (for example, `Wwise`). The pinned game instance
  takes precedence over capture-source metadata. Manual capture never borrows the current detected
  game's PID/title, and later detections cannot retarget an existing capture.
- Application audio uses a private PipeWire daemon, WirePlumber's policy-only
  profile and a null sink. Tests remove/recreate playback sources and stop/restart
  only the launcher's owned services, checking real PCM delivery, error handling,
  callback retirement and fresh-source recovery. Hardware monitors and system-bus
  access are disabled, and host PipeWire socket overrides are removed.
- Native audio needs Python, `pipewire`, `wireplumber` 0.5+, `pw-cli`, `pw-dump`
  and `dbus-run-session`; it skips explicitly when these are unavailable. Use
  CTest/the provided launchers, not private-test environment markers on your
  normal session bus. There is no graphics-reporter socket in the rewritten detector.
- `RecordingFailureTest` runs synthetic SHM video and a tone through the real
  private PipeWire graph, fake portal, recording/DVR managers and software
  H.264/AAC/MKV pipeline. Merged/separate cases cover portal closure, portal
  service exit and PipeWire daemon exit during recording, startup and live DVR
  buffering. Tests decode retained recordings and recovered recordings/clips,
  check stream timestamps, video dimensions and non-silent decoded PCM, and verify cleanup,
  no false save signal, stale-reply rejection and saved-token preservation.
  Window-handoff groups replace the real capture node after encoded frames,
  decode both saved parts, and check retained full-recording intent, repeated
  approved empty captures, explicit stop and terminal approval rejection.
  Additional recording/DVR groups renegotiate native SHM video dimensions/FPS
  (including odd sizes and restoration), recreate audio playback at 44.1/96/48kHz
  and mono/stereo, and check decoded colors, aspect padding, replacement tones
  and resumed audio timing. Audio recreation tests normal source rebinding,
  not in-place renegotiation of an application's fixed client format. DVR tests
  retain the producer across in-place changes and decode the combined clip, rejecting decoder errors
  as well as invalid API results.
  Permission cases close the fake portal grant, reject a saved token at
  SelectSources/Start, cancel an explicit authorization attempt, and revoke
  the actual capture client's read permission for the granted PipeWire node.
  They check partial-file/cache cleanup, explicit retry and no automatic picker.
  Token removal on reauthorization is modeled by test signal handlers; these
  fixtures do not instantiate the desktop Application/permission dialog or
  modify the real portal permission store.
  The game-exit group removes the real SHM video producer before/after an
  owned test process exits, and separately revokes read access while that
  process stays alive. Confirmed targeted-game exit must publish Saved, not
  an error; a living game's permission loss still fails. Capture drains
  immediately, with at most five seconds of owner-thread grace to classify
  ambiguous source/session disappearance. Service and output failures are
  not treated as normal game exit. These are controlled lifecycle identities,
  not a live game/compositor shutdown test.
  A separate transient-delivery group pauses video, audio, or both without
  removing nodes or revoking permissions. It requires the same capture/file
  to resume, one normal save with no reauthorization, a retained decoded video
  timestamp gap, aligned stream ends and audible resumed PCM. This models
   producer stalls, not system suspend/resume or compositor/driver recovery.
   The renegotiated-stall teardown group changes to odd-sized 15fps input,
   pauses both producer legs, then exercises duplicate manual stop, duplicate
   portal revocation or manager destruction in merged/separate audio modes.
   It decodes retained files and a fresh successor-manager recording, checking
   fixed canvas/stage colors, completion classification and token preservation.
  These tests also need the software H.264/AAC codecs and skip if unavailable.
  Run the individual CTest groups: each private-service child has a 90-second
  deadline, so combining all pipeline cases in one launcher is unsupported.

Recovery in these fixtures means a fresh explicitly requested capture, not a
claim of automatic recording resumption. The source-only suites do not verify
complete media files; the recording fixture does, for its synthetic software
pipeline. Real portal UI/permission-store behavior, GPU/DMA-BUF driver resets,
additional native scheduling/format combinations and suspend/resume still need
separate integration and hardware/session checks.

### Process detection

See the [detection design and limitations](game-detection.md). `detection_probe`
is a read-only developer executable that prints current game/process/audio
snapshots without opening a compositor connection or starting recording.

### Diagnostic kit

`packaging/make_test_kit.py` stages a diagnostic kit from a build; it is not a
release download. The kit does not bundle Qt or FFmpeg, so the target machine
must supply them. It runs a focused subset of unit and native GPU tests, not the
full CI suite, and needs no running Wayland session or live portal/PipeWire
services — the interactive preview and detection checks do need the desktop
services. GPU skips mean incomplete coverage, not successful GPU validation. See
the [packaging notes](../packaging/README.md#diagnostics-collection) for the
collector, the kit, and how to read its report.

## Contributing

Issues, focused pull requests, and documentation improvements are welcome. For fixes, include reproduction steps and a regression test where practical. Describe what you verified, and distinguish automated coverage from live desktop/game testing.

The application uses Qt Quick/QML for the interface and C++20 for recording, audio, detection, and library services. Video capture stays PipeWire/portal-based; share export uses native FFmpeg libraries. Preserve user authorization, worker/input lifetimes, and the distinction between disposable caches and durable annotations when changing these paths.
