# Isolated Qt/FFmpeg leak investigation

These are diagnostic programs, not regression tests or production settings.
They never supply media to a decoder, encode frames, capture, install, or use
the host session bus. Hardware comparison modes **do** initialize discovery
drivers. Do not interpret software-only results as hardware leak-cleanliness.

## Build standalone baselines

From the repository root (GCC; the Qt protected-symbol flag is needed here):

```sh
mkdir -p /tmp/opencode/player-leak-investigation
c++ -mno-direct-extern-access -fsanitize=address,undefined -fno-omit-frame-pointer -g tests/resources/qt_media_leak_probe.cpp -o /tmp/opencode/player-leak-investigation/qt_media_leak_probe $(pkg-config --cflags --libs Qt6Gui Qt6Qml Qt6Multimedia)
c++ -fsanitize=address,undefined -fno-omit-frame-pointer -g tests/resources/ffmpeg_discovery_leak_probe.cpp -o /tmp/opencode/player-leak-investigation/ffmpeg_discovery_leak_probe $(pkg-config --cflags --libs libavcodec libavutil)
```

`run_qt_leak_probe.py` creates private HOME/XDG directories and a private bus
whose config has **no service activation directories**. Each command has a
60-second timeout. It sets:

```text
QT_QPA_PLATFORM=offscreen
QT_QUICK_BACKEND=software
QT_MEDIA_BACKEND=ffmpeg
QT_DISABLE_HW_TEXTURES_CONVERSION=1
QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,
QT_FFMPEG_ENCODING_HW_DEVICE_TYPES=,
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
```

It removes inherited LSAN_OPTIONS; no leak suppressions are added. The comma
is Qt's documented empty hardware device list, not an unset/empty variable.
These Qt private APIs can change:
https://doc.qt.io/qt-6/advanced-ffmpeg-configuration.html

## Compare baselines and real fixtures

```sh
python3 tests/resources/run_qt_leak_probe.py /tmp/opencode/player-leak-investigation/qt_media_leak_probe app
python3 tests/resources/run_qt_leak_probe.py /tmp/opencode/player-leak-investigation/qt_media_leak_probe engine
python3 tests/resources/run_qt_leak_probe.py /tmp/opencode/player-leak-investigation/qt_media_leak_probe media
python3 tests/resources/run_qt_leak_probe.py --hardware-discovery /tmp/opencode/player-leak-investigation/qt_media_leak_probe media
python3 tests/resources/run_qt_leak_probe.py /tmp/opencode/player-leak-investigation/ffmpeg_discovery_leak_probe both
python3 tests/resources/run_qt_leak_probe.py /tmp/opencode/player-leak-investigation/test_PlayerProbe --gtest_repeat=25
python3 tests/resources/run_qt_leak_probe.py /tmp/opencode/player-leak-investigation/test_SettingsManager --gtest_filter=-SettingsManagerTest.AvailableEncoders*
```

The player executable forces software discovery before creating QGuiApplication
because its fake library intentionally returns empty media URLs. For hardware
comparisons use the standalone baseline, not that fixture.

Qt's device-list variables **do not** disable Trophy EncoderFactory probes.
The software settings comparison must compile only its EncoderFactory object
with HAVE_NVENC/HAVE_VAAPI undefined. Existing scratch sanitizer objects can be
reused without changing CMake, generated files, or production sources:

```sh
python3 - <<'PY'
import pathlib, shlex, subprocess
root = pathlib.Path('/tmp/opencode/trophy-remux-sanitize/tests')
out = pathlib.Path('/tmp/opencode/player-leak-investigation')
for target in ('test_PlayerProbe', 'test_SettingsManager'):
    flags = (root / f'CMakeFiles/{target}.dir/flags.make').read_text()
    opts = []
    for key in ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS'):
        opts += shlex.split(next(line.split(' = ', 1)[1] for line in flags.splitlines()
                                 if line.startswith(key + ' = ')))
    obj = out / (target + '.o')
    subprocess.run(['c++', *opts, '-c', str(pathlib.Path.cwd() / f'tests/{target}.cpp'),
                    '-o', str(obj)], check=True)
    link = shlex.split((root / f'CMakeFiles/{target}.dir/link.txt').read_text())
    link = [str(obj) if arg == f'CMakeFiles/{target}.dir/{target}.cpp.o' else arg
            for arg in link if not arg.startswith('-Wl,--dependency-file=')]
    link[link.index('-o') + 1] = str(out / target)
    subprocess.run(link, cwd=root, check=True)
    if target == 'test_SettingsManager':
        factory = out / 'EncoderFactory-software.o'
        subprocess.run(['c++', *opts, '-UHAVE_NVENC', '-UHAVE_VAAPI', '-c',
                        str(pathlib.Path.cwd() / 'src/encoding/EncoderFactory.cpp'),
                        '-o', str(factory)], check=True)
        link = [str(factory) if arg.endswith('/__/src/encoding/EncoderFactory.cpp.o')
                else arg for arg in link]
        link[link.index('-o') + 1] = str(out / 'test_SettingsManager-software')
        subprocess.run(link, cwd=root, check=True)
PY
python3 tests/resources/run_qt_leak_probe.py /tmp/opencode/player-leak-investigation/test_SettingsManager-software
```

This requires the scratch targets' existing generated moc and dependency
objects to match the source tree; it is not a replacement for a normal build.

## Observations (2026-10-13, this host only)

- Application-only and QML-engine-only baselines: no sanitizer leak report,
  with both default discovery and software discovery.
- Standalone QMediaPlayer, normal stack destruction plus deferred-delete/event
  flush: software discovery passes; default discovery reports **3521 bytes / 29
  allocations**. This contains the same CUDA, NVIDIA, libdbus and unknown-module
  families as the old PlayerProbe report (**3485 bytes / 29 allocations**).
  The libdbus family persists on the private no-activation bus, so merely
  removing host-bus interaction is not enough.
- PlayerProbe: **4 tests**, then **25 repetitions (100 executions)**, pass
  ASan/UBSan with leak detection enabled in the software-only private environment.
- Settings: **60 tests** excluding discovery pass; the native discovery tests
  still report **1053 bytes / 14 allocations**. The complete software-only
  test-build variant passes **62 tests**. QCoreApplication is now owned and
  normally destroyed by the test environment; that change does not remove
  hardware-discovery leaks and was not a reproduced production leak.
- Standalone FFmpeg (no Trophy or Qt objects), contexts freed: NVENC **231
  bytes / 5 allocations**; VA-API **366 bytes / 8 allocations**; sequential
  NVENC + VA-API **1053 bytes / 14 allocations**, matching the settings failure.
  NVENC returns CUDA_ERROR_OUT_OF_MEMORY on this host; this is not an actual
  OS allocation-exhaustion test.

Unsuppressed logs are under `/tmp/opencode/player-leak-investigation/`.
`baseline-media-loader.log` and `settings-loader.log` add `LD_DEBUG=files`:
the loader's per-process base/size ranges identify *newly reproduced* unknown
allocation PCs, rather than guessing from the old ASLR addresses:

```text
libvulkan_lvp.so                  +0x17dafe
libnvidia-glcore.so.615.78.08      +0x9e78d1, +0x9e7b8d
libnvidia-eglcore.so.615.78.08     +0x97d384, +0x97dcb0
```

The settings reproduction's unknown PCs map to the same NVIDIA EGL offsets.
The original unknown PCs cannot be conclusively symbolized from that old log
alone. CUDA/libdbus/driver dependencies remain unsuppressed; a 48-byte settings
allocation reported at pthread_cond_broadcast also occurs in standalone NVENC.
These baselines establish dependency-only reproductions, **not** a universal
exoneration of Trophy or exhaustive media/driver lifecycle coverage.
