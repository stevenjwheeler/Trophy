# Detection rewrite verification

Date: 2026-11-20. Branch: `detection-refactor-v2`, created directly
from `main` (`59bd79b5cdd69d718ac3697a877cbf9a44573bb3`). The previous
`detection-refactor` branch was not used.

## Build and automated checks

Local environment: Linux 7.3.4, Qt 6.12.0, PipeWire development/runtime
1.6.11 and FFmpeg 9 libraries. Build directory: `/tmp/opencode/trophy-build`.

```sh
cmake -S . -B /tmp/opencode/trophy-build -G Ninja \
  -DBUILD_TESTING=ON -DQT_QML_GENERATE_QMLLS_INI=OFF
cmake --build /tmp/opencode/trophy-build -j 6
QT_QPA_PLATFORM=offscreen ctest --test-dir /tmp/opencode/trophy-build \
  --output-on-failure -j 4
cmake --build /tmp/opencode/trophy-build --target Trophy_qmllint
cmake --install /tmp/opencode/trophy-build --prefix /tmp/opencode/trophy-agnostic-installed
python3 packaging/smoke_native.py /tmp/opencode/trophy-agnostic-installed \
  --log /tmp/opencode/agnostic-native-smoke.log
```

Results:

* All application and test targets built successfully.
* Final full-suite validation after compositor removal: **1,210 passed,
  15 skipped, zero failed** (1,225 CTest cases, 133.73 seconds).
  The preceding run completed 1,225 cases with one harness timeout: five
  DVR resize cases passed before the six-case group's 90-second wrapper limit.
  Native/DVR format groups now have a bounded 150-second wrapper budget and
  180-second outer CTest limit; other groups retain their previous limits.
  Earlier desktop-fallback targeted validation passed all 134 CTest cases,
  including six native recording/DVR fallback and recovery cases.
  Skips are explicit unavailable optional hardware/encoder or desktop-fixture
  conditions; they are not successful hardware tests.
* Native installed-prefix payload, linkage and isolated GUI startup/shutdown
  smoke passed in a fresh `/tmp/opencode/trophy-agnostic-installed` prefix.
  No graphics reporters, enrollment manifests or compositor helpers were installed.
  Full DEB/RPM/AppImage container builds were not run locally.
* `Trophy_qmllint` succeeded with 27 warnings, principally context/unqualified
  access warnings. Compiler builds retain existing Qt deprecation and
  test-macro dangling-else warnings.
* `clang-format` checks of new/replaced files and formatting of modified C++
  ranges passed; `git diff --check` passed.
* Clang analyzer ran on the three detection implementations and native
  PipeWire audio source. New detection code had no analyzer findings.
  The retained capture class has two opt-in advisories: calling its own
  virtual `stop()` in its destructor, and casting combined PipeWire C flags
  to their enum type. These were not treated as proven runtime defects.
  The analysis-only compilation database omits Qt's GCC-only
  `-mno-direct-extern-access` flag, which local clang-tidy cannot parse;
  production compile flags were not changed for this workaround.

Historical results: the pre-fallback full run passed 1,270 cases with 17 skips.
One later full run overlapped relinking and was invalid; the following run
overlapped removal of source-loaded helper/QML fixtures and is also not final
verification. It additionally exposed a DVR resize fixture's fixed-sleep race:
the fixture now waits for encoded frame progress and rotation settlement before
saving. Earlier compositor-removal targeted coverage passed the new process
picker tests; two obsolete window-ID assertions were subsequently removed.
Final logs: `/tmp/opencode/agnostic-full-tests-final.log`,
`/tmp/opencode/agnostic-qmllint.log`, `/tmp/opencode/agnostic-native-smoke.log`.
Logs live under `/tmp/opencode/` and are not bundled or committed.

## Polling cost

`detection_probe --benchmark` performs 100 iterations of each operation on the
local host, with 146 same-user readable processes and 49 installed Steam games:

| Operation | Mean CPU | Mean elapsed |
|---|---:|---:|
| Process census | 13.79 ms | 13.96 ms |
| Identification | 0.44 ms | 0.44 ms |
| Local Steam metadata | 3.79 ms | 3.99 ms |

Census plus identification at one scan/second costs about 1.4% of one core here.
Metadata every 30 scans averages about 0.013% of one core. The benchmark is
warm-cache and excludes PipeWire/audio/video work, so it is not a universal
performance guarantee. Census uses bounded kernel link reads, not filesystem
canonicalization or stale ownership caching. Evidence:
`/tmp/opencode/agnostic-benchmark.json`.

## Controlled automated coverage

`tests/test_GameDetectionService.cpp` covers:

* Native Steam payloads, missing Steam environment, explicit standalone
  definitions and interpreted Java/Python payloads.
* Simulated Proton/Wine payload paths, services, launch ancestry, separate
  instances of one game, matched grandchildren and helper membership.
* Excluding Steam, launchers, Discord and Firefox from game ownership.
* Real same-user `/proc` enumeration, child PPID/start ticks, 25 rapid native
  exits and inspection races.
* Simulated PID reuse, exec changes and reparenting. Actual forced kernel PID
  reuse was not performed.
* Real process identity checks and ambiguous PID-less executable rejection.
* Explicit manual-process pins without adding a game, invalid/wrong PIDs,
  and pins invalidated by exit.
* A real census-driven service detecting a native child under KDE, GNOME
  and Hyprland environment labels with nonexistent Wayland and PipeWire
  endpoints. This proves the core does not require either service for
  identification; it is **not** three live desktop-session tests.
* Upgrade cleanup retaining foreign preload/config entries.
* The process-based Add Game list, Wine payload display, exact native rule
  creation and rejection of stale PID/start, exec changes and exited selections.

Native private PipeWire tests (`tests/test_AppAudioGroup.cpp`,
`tests/run_private_pipewire.py`) use real PipeWire/WirePlumber graphs and
controlled PCM producers, not real games. They check:

* An instance existing before audio, a silent initial stream, late audible
  streams, multiple process members across different clients, summation on
  one clock, unrelated same-title/client streams, removal and recreation.
* Incomplete/updated node and client properties, current-info lookup,
  serial reuse rejection, wrong actual links, strict Voice isolation,
  explicit manual-process capture and daemon reconnection.
* Initial recording/DVR lookup through a generic portal with no compositor
  PID, using a pinned process/game instance.

`tests/test_RecordingFailureIntegration.cpp` retains the actual native
PipeWire capture, software H.264/AAC encoding, MKV muxing, DVR rotation and
clip pipeline, driven by synthetic video/audio and a private fake portal.
Merged/separate cases decode the resulting files and check audible PCM,
dimensions/timestamps, stream recreation/rate changes, service loss,
revoked permissions, stale replies, process exit and recovery.
Audible-output fixtures now wait for actual PCM evidence before deliberately
cutting a short recording; video frames alone do not establish audio
readiness. Six additional merged/separate cases verify audible desktop fallback
at recording/DVR startup and game → desktop → game transitions. Unit tests cover
fallback diagnostics/history and rejection of stale Voice lookup results while
the desktop mix is active.

## Real games and audio

Two installed games were run together on the available KWin Wayland host:

| Game | Launch used | Observed OS payload | Associated audio |
|---|---|---|---|
| Terraria, Steam 105600 | Direct native binary with Steam launch variables; SDL Wayland | `Terraria.bin.x86_64` | Correct native playback node |
| Cult of the Lamb, Steam 1313140 | Direct Proton Experimental `proton run` with Steam launch variables and compatibility directory | Wine `wine64-preloader`, argv `./Cult Of The Lamb.exe`, reparented to user systemd | Correct Wine playback node |

`tests/detection_probe.cpp` independently scanned `/proc`, read local Steam
metadata and queried native PipeWire, with no window monitor. It reported
two distinct instances and two different audio serials. Firefox's two
simultaneous playback streams remained unassigned.

The native node initially lacked node-level PID/binary fields; current
owning-client evidence and executable identity resolved it. The Proton node
reported a Wine-loader process identity, not an ELF game executable.
`detection_probe --sample` successfully captured PCM through the same
application-source/route-verification path used by recording:

* Terraria: 92,160 interleaved samples, RMS approximately **0.01141**.
* Cult of the Lamb: 94,208 interleaved samples, RMS approximately **0.00294**.

These samples were measured, not saved as recordings. After both game
processes exited, a fresh probe reported no game instances; unrelated
desktop playback remained separate. Local evidence:
`/tmp/opencode/two-games.json`, `/tmp/opencode/live-audio.json` and
`/tmp/opencode/post-exit.json`. Probe output can contain launch arguments;
review/redact it before sharing.

The direct launches are real-game tests, but do **not** establish that
launching through Steam's UI/pressure-vessel or another launcher has been
verified. An attempted `steam -applaunch` did not produce an observed game
in this session and is not counted as a passing launch test.

## Remaining unverified scenarios

* Real GNOME/Mutter and Hyprland sessions. Core independence is structurally
  enforced and tested without compositor endpoints, but desktop-specific
  portal capture still requires separate live sessions.
* A separate plain Wine/non-Steam Windows title, real third-party launcher
  trees, a real multi-helper game's audio and 32-bit game processes.
* Real Discord audio concurrently with the games. Its exclusion and Voice
  routing have controlled coverage; live Firefox coexistence was measured.
* Real-game device switches, stream recreation, audio starting late and
  unexpected exit during video recording. Controlled graph/pipeline tests
  cover these lifecycles; the real-game experiment measured identification
  and PCM capture, not complete live portal recording/DVR.
* Live portal permission dialogs, actual compositor-video recordings,
  anti-cheat acceptance, suspend/resume and all GPU/driver combinations.

See [architecture and limitations](game-detection.md) and
[Discord catalogue/legal research](discord-detectables.md). No new external
dependencies were added. Vulkan development requirements and injected
reporter/multilib build, package, CI and test machinery were removed.
Compositor backends/helpers, resources, installers, settings and their obsolete
tests were removed. Add Game uses the cached process census; manual recording
uses the ScreenCast portal. No compositor IPC or Trophy desktop helper remains.
