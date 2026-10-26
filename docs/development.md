# Building and contributing

[Back to README](../README.md) · [Packaging notes](../packaging/README.md)

## Dependencies

- CMake 3.21+, a C++20 compiler, and pkg-config.
- Qt 6 development packages: Core, Gui, Qml, Quick, QuickControls2, Multimedia, DBus, Concurrent, Network, and Widgets.
- FFmpeg development libraries: libavcodec, libavformat, libavutil, libswscale, and libswresample.
- PipeWire development libraries and Vulkan loader/headers.
- Optional libva/libva-drm for VA-API, libdrm and EGL for DMA-BUF support, and GBM for related tests.
- GoogleTest and Qt Test when building tests (enabled by default).

CMake currently declares Qt 6.5 as its minimum, but also uses newer Qt CMake policies such as QTP0004. Do not assume that declaration guarantees a successful build with every Qt 6.5 installation; use a recent Qt 6 toolchain. Distribution package versions and available FFmpeg encoders vary.

The FFmpeg executable is used by optional test fixtures, not production recording, previews, or export. Share export requires FFmpeg library support for libx264 and AAC encoding.

### Example package lists

These are starting points, not a guarantee that every distribution release supplies sufficiently recent packages. Install a compiler/build toolchain too.

Arch/CachyOS:

```bash
sudo pacman -S --needed base-devel cmake qt6-base qt6-declarative qt6-multimedia \
    ffmpeg pipewire libva pkgconf vulkan-headers vulkan-icd-loader \
    libdrm mesa gtest
```

Fedora (FFmpeg development package availability depends on enabled repositories):

```bash
sudo dnf install gcc-c++ make cmake qt6-qtbase-devel qt6-qtdeclarative-devel \
    qt6-qtmultimedia-devel ffmpeg-devel pipewire-devel libva-devel \
    vulkan-headers vulkan-loader-devel libdrm-devel mesa-libEGL-devel \
    mesa-libgbm-devel gtest-devel
```

Ubuntu/Debian:

```bash
sudo apt install build-essential cmake pkg-config qt6-base-dev qt6-declarative-dev \
    qt6-multimedia-dev libavcodec-dev libavformat-dev libavutil-dev \
    libswscale-dev libswresample-dev libpipewire-0.3-dev libva-dev \
    libvulkan-dev libgtest-dev libdrm-dev libegl-dev libgbm-dev
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

The install includes `libTrophy.so`, reporter libraries, and desktop integration as well as the executable. **Do not copy only the binary.** Ensure `~/.local/bin` is on `PATH` for native CLI shortcuts. Reconfigure after changing branches/commits if you need the displayed configure-time version stamp refreshed.

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

**Quit running Trophy instances before socket tests.** The reporter protocol uses the fixed `/tmp/trophy-detection.sock`; HOME/XDG isolation does not relocate it. Certain integration tests launch their own private D-Bus or PipeWire environment. Hardware and optional-codec tests skip when requirements are unavailable. Passing automated tests does not establish compatibility with every compositor, GPU, or game runtime.

For AppImage payload/startup smoke checks and reporter ABI inspection, see [packaging notes](../packaging/README.md). Those checks have narrower scope than live recording tests.

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
- Application audio uses a private PipeWire daemon, WirePlumber's policy-only
  profile and a null sink. Tests remove/recreate playback sources and stop/restart
  only the launcher's owned services, checking real PCM delivery, error handling,
  callback retirement and fresh-source recovery. Hardware monitors and system-bus
  access are disabled, and host PipeWire socket overrides are removed.
- Native audio needs Python, `pipewire`, `wireplumber` 0.5+, `pw-cli`, `pw-dump`
  and `dbus-run-session`; it skips explicitly when these are unavailable. Use
  CTest/the provided launchers, not private-test environment markers on your
  normal session bus. These suites alone do not use the reporter socket.
- `RecordingFailureTest` runs synthetic SHM video and a tone through the real
  private PipeWire graph, fake portal, recording/DVR managers and software
  H.264/AAC/MKV pipeline. Merged/separate cases cover portal closure, portal
  service exit and PipeWire daemon exit during recording, startup and DVR
  handoff. Tests decode retained recordings and recovered recordings/clips,
  check stream timestamps, video dimensions and non-silent decoded PCM, and verify cleanup,
  no false save signal, stale-reply rejection and saved-token preservation.
  Additional recording/DVR groups renegotiate native SHM video dimensions/FPS
  (including odd sizes and restoration), recreate audio playback at 44.1/96/48kHz
  and mono/stereo, and check decoded colors, aspect padding, replacement tones
  and resumed audio timing. Audio recreation tests normal source rebinding,
  not in-place renegotiation of an application's fixed client format. DVR tests
  rotate across changes and decode the combined clip, rejecting decoder errors
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
  These tests also need the software H.264/AAC codecs and skip if unavailable.
  Run the individual CTest groups: each private-service child has a 90-second
  deadline, so combining all pipeline cases in one launcher is unsupported.

Recovery in these fixtures means a fresh explicitly requested capture, not a
claim of automatic recording resumption. The source-only suites do not verify
complete media files; the recording fixture does, for its synthetic software
pipeline. Real portal UI/permission-store behavior, GPU/DMA-BUF driver resets,
additional native scheduling/format combinations and suspend/resume still need
separate integration and hardware/session checks.

## Contributing

Issues, focused pull requests, and documentation improvements are welcome. For fixes, include reproduction steps and a regression test where practical. Describe what you verified, and distinguish automated coverage from live desktop/game testing.

The application uses Qt Quick/QML for the interface and C++20 for recording, audio, detection, and library services. Video capture stays PipeWire/portal-based; share export uses native FFmpeg libraries. Preserve user authorization, worker/input lifetimes, and the distinction between disposable caches and durable annotations when changing these paths.
