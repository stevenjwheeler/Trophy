# Detection rewrite verification

Date: 2026-11-20. Branch: `detection-refactor-v2`, created directly
from `main` (`59bd79b5cdd69d718ac3697a877cbf9a44573bb3`). The previous
`detection-refactor` branch was not used.

## Current local baseline

The latest installed application payload passed **1,240 tests, 15 skipped,
zero failed** (1,255 CTest cases), plus native install/linkage/startup smoke.
A user-run Lethal Company loader-to-main-window handoff and normal game-exit
save were confirmed. GNOME/Hyprland live sessions, broader launcher/runtime
coverage, long-gap live restoration, full package builds and additional
GPU/driver combinations remain unverified.

This file is a development record, not a release changelog. Earlier counts,
the initial three-retry budget and original PCM-only experiment limitations
describe their respective stages; later follow-ups supersede them. Current
behavior is documented in [game-detection.md](game-detection.md).
Commit/install status in the entries below also describes its respective
stage. Local validation and installation do not themselves publish the
website or create a release.

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
* Initial full-suite validation after compositor removal: **1,210 passed,
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

The full-suite and benchmark numbers below precede the Wine-drive follow-up.
That follow-up adds worker-side resolution of the payload's relevant drive
mapping. Later targeted and combined verification is recorded separately.

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

## Wine drive-mapping follow-up

Local Steam UI testing of Limbus Company (1973530) exposed a path-dependent
miss: one launch used `S:\steamapps\common\Limbus Company\LimbusCompany.exe`,
whereas a relaunch used `Z:\run\media\...\LimbusCompany.exe`. The game's prefix
maps S: to its external Steam library. The original C:/Z:-only translation
missed the S: payload even though its Steam manifest and audio node existed.

The census now resolves the relevant prefix `dosdevices` mapping on its worker
and stores it in the snapshot. Regression cases cover equivalent S:/Z: Steam
identification, lower-case drive letters, relative link targets, loader argv,
prefix-specific C:/Z: overrides, broken/missing maps, drive-relative rejection
and snapshot immutability when a mapping is removed. Unresolved parent payloads
also stop ancestry grouping, so a broker with an unmapped drive cannot merge
separate Wine instances.

Follow-up validation: **51 targeted detection/audio CTest cases passed**, zero
failures, including the private PipeWire group (3.02 seconds).
Fresh-prefix installation/linkage/isolated startup smoke passed. The later
combined full-suite run below also includes this Wine-drive fix.
The live Z: launch identifies Limbus Company and associates its audio node.
A temporary `/proc` fixture replaying the observed S: argv/mapping against the
real installed Steam metadata resolves the identical executable and game ID.
The relaunch had removed S: from the real prefix, so that mapping was recreated
only in the owned temporary fixture, not in the user's prefix or game process.
An actual S:-based game relaunch after the fix remains to be tested.
Evidence: `/tmp/opencode/wine-drive-tests.log`,
`/tmp/opencode/limbus-fixed-detection.json`,
`/tmp/opencode/limbus-s-drive-fixed.json` and
`/tmp/opencode/wine-drive-native-smoke.log`.

## PipeWire cleanup/churn follow-up

Live Limbus recording repeatedly bound one unchanged playback node/serial and
fell back three seconds later. A separate native capture probe exposed a fatal
`unknown resource 46 op:7` (`core.destroy`) response during graph churn, not a
disappearing game. The previous code destroyed registry-bound observers as soon
as their global disappeared, before processing the resource's removal event.

Node/client observers now retain detached bindings until server removal, then
free the local proxy without issuing another remote destroy. Owned sibling-link
retirement tracks the exact native destroy sequence and its following sync
fence. Only a matching `ENOENT` during that bounded lifetime is nonfatal;
unrelated sequences, other object IDs, permission/transport errors and arbitrary
`unknown resource` text do not bypass capture failure.

Unit cases cover this error classification, fence expiration and preservation
of fatal untracked missing-resource errors. A private PipeWire regression churns
short-lived lookup clients, playback clients/nodes and owned sibling links,
checking continued PCM, capture liveness and no unrelated-audio inclusion.
Its first version (only long-lived playback clients) did not reproduce the old
bug; adding short-lived lookup-client churn made the old installed library
fail while the patched library passed. Initial targeted coverage passed all
34 CTest cases; a seven-second live probe remained capturing with 317,440 frames.
Combined verification, including the Wine-drive fix: **1,214 passed, 15 skipped,
zero failed** (1,229 CTest cases, 132.46 seconds). Fresh-prefix linkage and
isolated startup smoke passed.

The final negative control fails against the old installed library with one
capture error within 112 ms. The patched library passes the same regression.
A **20-second live Limbus audio capture** remained active, delivered **960,512
frames**, and had a maximum inter-buffer gap of **23.30 ms**, while the existing
old Trophy session was still generating graph churn. This is an audio probe,
not a new end-to-end portal video/DVR recording test.
Logs: `/tmp/opencode/audio-churn-full-tests.log`,
`/tmp/opencode/audio-churn-negative-control-final.log`,
`/tmp/opencode/limbus-audio-check-fixed.log` and
`/tmp/opencode/audio-churn-native-smoke.log`.

## Auto-record identity/UI follow-up

An already-approved Limbus detection stopped at whitelist authorization without
arming capture. Detection emitted references into its instance list, while
authorization synchronously rebuilt that list, including for an unchanged
definition. That invalidated the name still being used by the auto-start slot.

Detection updates now own name/key values, selection takes an instance snapshot
before emitting signals, and the production auto-start handler owns its signal
argument. Identical whitelist definitions do not reidentify processes or emit
rule-change signals. Custom-list bookkeeping can still be persisted without a
live-instance rebuild. Dashboard READY status uses `isGameRunning`; the removed
`hasVisibleWindow()` API and the remaining helper-repair-dialog reference are
not used.

All **50 targeted cases passed** (3.53 seconds). New cases exercise the real
production auto-start handler with both newly whitelisted and already-listed
approved games, asserting that the recording/audio-readiness request is armed.
They do not open a portal or encode a real recording. Additional cases cover
signal identity surviving a synchronous rule rebuild, no-op rule updates, and
the real dashboard's IDLE/READY/STARTING/SAVING/RECORDING bindings without any
window method on its detector.

Combined full-suite verification: **1,219 passed, 15 skipped, zero failed**
(1,234 CTest cases, 131.04 seconds). Application/test build, QML lint and
fresh-prefix installation/linkage/isolated startup smoke passed. Logs:
`/tmp/opencode/autorecord-full-tests.log`,
`/tmp/opencode/autorecord-qmllint.log` and
`/tmp/opencode/autorecord-native-smoke.log`.

The subsequent user-run session detected Limbus at 20:40:24.181, entered game
audio streaming at 20:40:24.551, and started recording at 20:40:24.623.
Logs through 20:41:45.718 showed no desktop fallback, capture errors, repeated
game rebinds or stale window-API warnings. This confirms live auto-start and
stable routing in that observed interval, not decoded-file/A/V verification.
The encoder auto-resolution diagnostic was subsequently changed from info to
debug; all 40 RecordingManager CTest cases passed (0.60 seconds).

## Quiet game-exit follow-up

The user-run Limbus session completed normally on game exit, but target removal
was reported as a wrong-app link, followed by target-not-found and repeated
queued native resource errors. Audio target disappearance now retires capture
without a capture-error signal. Unexpected route changes and live core/stream
failures remain failures. Registry observation is quiesced on terminal capture
and before native teardown, and queued errors cannot produce repeated warnings
after a terminal cause or an explicit stop.

New regressions exercise observer quiescence, late stream/core errors during
explicit stop, quiet target/client removal, prevention of replacement audio
leaking into a retired capture, and reinitialization restoring real error
handling. Existing wrong-route, daemon-loss and recording game-exit cases
remain enabled. All **37 targeted CTest cases passed** (15.47 seconds), including
the private PipeWire group and six native recording game-exit cases.
Full-suite verification: **1,221 passed, 15 skipped, zero failed**
(1,236 CTest cases, 249.98 seconds at reduced priority with two workers).
Fresh-prefix installation/linkage/isolated startup smoke passed.
No running user game or Trophy session was stopped for this work.
Logs: `/tmp/opencode/quiet-exit-full-tests.log` and
`/tmp/opencode/quiet-exit-native-smoke.log`.

## Observed mod-loader window handoff (initial investigation)

A subsequent user-run Lethal Company launch detected the game and started
recording at 20:53:43. The restored portal source negotiated 640×428, consistent
with the reported mod-loader window; generic portal metadata does not prove
which window was selected. At 20:55:09 the portal session closed while the game
playback node (126, serial 26206) remained available. After the five-second exit
grace period, recording reported source loss because the pinned game remained
alive. The generic recording-finished handler then armed DVR, not another full
recording. Current startup recovery only applies before recording begins and
requires a PID handoff, so it cannot cover this already-recording window change.

This remains a separate capture-recovery issue, not resolved by quiet audio
teardown. A fix must preserve the recorded portion and full-recording intent,
bound retries to the same pinned instance, retain permission/revocation safety,
and reacquire the replacement window through the portal (with explicit user
selection when the saved approval cannot restore it). Evidence:
`/tmp/opencode/lethal-latest-session.log`.

## Automatic captured-window recovery

The follow-up implements automatic recovery after proven capture-node
destruction, before or after recording starts, while the exact pinned game
instance remains selected and live. It does not depend on a PID change.
The completed part is finalized and published before a delayed full-recording
restart using the saved approval. The application suppresses DVR takeover
during recovery and does not auto-start DVR after terminal recording failure.
The initial implementation bounded retries to three per instance per
three-minute window, without resetting the budget on successful first frames.
That limit was subsequently removed by the instance-lifetime follow-up below.

Registry visibility loss is not by itself retryable. A bounded asynchronous
ordinary PipeWire connection compares the daemon cookie and original node
serial. An original node that remains present, a daemon change, missing
identity, or a failed probe stays a terminal/ambiguous loss. The probe also requires
readable default and target permissions, so absence from another restricted
connection is not mistaken for destruction. External portal
closure and service loss remain terminal. No continuation dialog or new
permission-bypass path is introduced. Restoration remains subject to portal
backend policy; live Lethal Company retesting is still required.

New manager regressions cover same-PID recovery, cancellation, removed
approval, another same-name instance, output finalization failure, ambiguous
permission loss, retry exhaustion and preservation of the earlier output.
A coordinator regression checks that publishing the saved part cannot resume
DVR. Private-service tests replace an actual video node after encoded frames,
validate both output parts, and check explicit stop and per-client permission
withdrawal. All **94 targeted CTest cases passed**, including the private
native replacement/stop/permission group (six parameterized cases). The final
full rebuild and suite passed **1,230 tests, 15 skipped, zero failures**
(1,245 CTest cases, 254.57 seconds). Fresh-prefix install/linkage/startup
smoke also passed. After final independent probe-permission hardening, the
full suite again passed **1,230 tests, 15 skipped, zero failures**
(1,245 CTest cases, 255.70 seconds), and the fresh-prefix native smoke passed.
Builds/tests used reduced priority and two workers.

The final payload was atomically installed and its hashes verified against
`/tmp/opencode/trophy-handoff`. The prior executable/library were backed up to
`/tmp/opencode/trophy-before-handoff-20261120-212633`. Running mapped files
were left intact; the new behavior takes effect on the next user restart.
No Trophy session or user game was stopped. Final logs:
`/tmp/opencode/handoff-final-tests.log` and
`/tmp/opencode/handoff-native-smoke.log`. Live Lethal Company retesting remains
pending; the changes remain uncommitted.

## Live Lethal Company handoff confirmation

The next user-run session confirmed the original fix: recording started at
21:27:29 from a 640×428 source, saved the first part at 21:28:56 (5,278 encoded
frames), restored the saved approval at 21:28:58 against a 2560×1440 source,
and continued full recording. At 21:32:52 game exit completed the second part
normally (13,937 encoded frames). There was no capture error or DVR takeover.
Evidence: `/tmp/opencode/handoff-user-latest-session.log`. This confirms a
quick live handoff, not a long launcher-to-main-window gap.

## Instance-lifetime recovery follow-up

At the user's request, recovery no longer has a three-attempt/three-minute
budget. Recoverable window loss continues to re-arm full recording while the
exact pinned instance remains alive, selected, approved and unblocked.
Attempts remain serialized, with a 1.5-second delay after cleanup. Explicit
stop, game exit, changed selection and removed approval still cancel recovery.
Successful portal grants that return an empty stream list are classified as
not-ready rather than permission rejection, allowing an approved automatic
capture to keep trying while the main window is absent.

This is not blanket retry of all portal failures: user cancellation, generic
saved-token rejection, external session closure, startup timeout, service
failure and output failure remain terminal. The portal does not reliably
distinguish a rejected restoration caused by absent windows from lost
approval; that ambiguity cannot safely be treated as permission to retry.

New regressions cover more than three consecutive destroyed windows, repeated
approved empty captures before readiness, and private-service long-gap
recovery/cancellation/rejection, including validation of both saved parts.
All **96 targeted CTest cases passed** (47.65 seconds), including six private
long-gap recovery/stop/rejection cases. Full validation passed **1,232 tests,
15 skipped, zero failures** (1,247 CTest cases, 265.10 seconds), plus native
install/linkage/startup smoke. Logs:
`/tmp/opencode/lifetime-retry-full-tests.log` and
`/tmp/opencode/lifetime-retry-native-smoke.log`.
Installation is deferred to the combined validated icon/retry payload.

## Steam icon-provider warning investigation

The recurring `image://gameicon/1659040/...` request refers to a historical
Steam recording, not the current Lethal Company capture. The provider supported
installed theme icons and the legacy `librarycache/<appid>_icon.jpg`, but the
user's readable 32×32 JPEG lives at
`librarycache/1659040/552be1d38866afd1c33f682323d325130e7d0ce6.jpg`.
The provider cached a miss, returned a null image and caused QML to warn when
recording delegates were recreated.

The follow-up adds bounded per-app hashed icon lookup, avoiding other apps,
large artwork, banners and nested hero/capsule directories. It increments the
source-cache policy version so old misses are rediscovered. Expected missing
artwork returns a transparent 1×1 sentinel; UI consumers keep their existing
letter/glyph fallback without asking Qt to load a null image. Malformed
provider requests still fail normally.

All **58 targeted CTest cases passed** (15.45 seconds), covering per-app hashed
icons, persistent selections, invalidation of old cached misses, avoiding
unrelated/capsule artwork, quiet missing recording artwork, Games/picker
letter fallback and recording-card bindings. A standalone read-only probe
against the user's actual Steam artwork returned a non-sentinel image
successfully (`/tmp/opencode/icon-lookup-probe.cpp`, exit 0). Combined full-suite
validation passed **1,237 tests, 15 skipped, zero failures** (1,252 CTest cases,
264.40 seconds). Fresh-prefix install/linkage/startup smoke also passed.
Logs: `/tmp/opencode/icon-fix-full-tests.log` and
`/tmp/opencode/icon-fix-native-smoke.log`.

The combined icon and instance-lifetime retry payload was installed atomically
from `/tmp/opencode/trophy-icon-fix` and installed hashes were verified.
Prior executable/library backup:
`/tmp/opencode/trophy-before-icon-retry-20261120-215256`.
Running Trophy/game sessions and mapped files were not interrupted. Both
changes take effect on the next user restart. Work remains uncommitted.

## Add Game likely-application view

The user requested a less noisy picker without returning to compositor-specific
window enumeration. The agreed default is **Likely applications**, with a
**Show all processes** switch. Suggestions are known game primaries, Windows
payloads and exact executable matches from cached non-hidden desktop application
launchers. Hidden/NoDisplay/terminal entries do not contribute hints, and XDG
override precedence is retained. The desktop index is refreshed on the census
worker with Steam metadata, never by opening or filtering the dialog.

All existing selectable process rows remain in the cached backend response.
Filtering does not change detection, ownership or add-rule validation.
Unrecognized native games and scripts can be selected using Show all processes.
Search applies within the chosen view, hidden selections are cleared, refresh
uses the existing cached census, and reopening restores the filtered default.
The UI does not claim that suggested processes own visible windows.

New regressions cover hint classification, unknown/native/Wine and duplicate
rows, desktop Exec parsing and hidden-entry overrides, default/all filtering,
PID/path search, selection clearing, reopening and Watch/Block routing.
Existing stale/changed/exited process tests remain enabled. All **86 targeted
CTest cases passed** (17.69 seconds). Full validation passed **1,240 tests,
15 skipped, zero failures** (1,255 CTest cases, 266.97 seconds), plus
fresh-prefix native install/linkage/startup smoke. Logs:
`/tmp/opencode/process-picker-full-tests.log` and
`/tmp/opencode/process-picker-native-smoke.log`.

The staged executable/library from `/tmp/opencode/trophy-process-picker` were
installed atomically and hashes verified. Backup:
`/tmp/opencode/trophy-before-process-picker-20261120-220916`.
The picker change takes effect on the next user restart; no running
Trophy session or game was interrupted. The added periodic desktop-index
refresh cost has not been benchmarked; older census/Steam-only measurements
are not a measurement of this additional work.

## Website compositor-support copy

The landing page now describes operation across Wayland compositors through
standard interfaces, rather than advertising removed KDE/GNOME/Hyprland/Sway
window integrations. It also removes outdated helper/graphics-reporter setup
and anti-cheat instructions. The getting-started source uses a feature-based
requirements table, removes the old helper Install/Repair step, and documents
the filtered Add Game view. Desktop-specific backend/tray examples remain
examples, not a support allow-list; portal/driver differences and incomplete
live validation remain explicit.

`python3 website/build_docs.py` passed, along with **206 local link/anchor
checks across five HTML pages** and stale-copy assertions. Generated docs
remain gitignored outputs. These are local file changes, not a deployed
website update; work remains uncommitted.

## Documentation consistency audit

Reviewed the README, current user/setup/development/detection guides, website
copy, packaging notes and dated research/verification context for the recent
rewrite. Corrected the README's remaining detection-integration setup claim,
added user-facing instance-lifetime handoff/retry conditions and separate-file
behavior, and qualified static-frame delivery as backend-dependent.
The verification record now opens with the current baseline and explicitly
distinguishes superseded retry budgets and original PCM-only experiments from
later live recording evidence. Historical observations remain intact.

Regenerated the public guides successfully and checked **215 local links and
anchors across five HTML pages**, including the new handoff sections.
This audit covers consistency of the recent changes, not a renewed live test
of every documented setting/platform. The public site/released versions are
not updated by local edits.

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

These original direct-launch experiments are real-game tests, but alone do **not** establish that
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
* Broader real-game device switches, stream recreation, delayed audio and
  unexpected exit during video recording. Controlled graph/pipeline tests
  cover these lifecycles. The original direct-launch experiment measured
  identification and PCM capture; later user-run Limbus and Lethal sessions
  add live recording/audio and quick window-handoff evidence, not coverage of
  every lifecycle or desktop.
* Live permission withdrawal/rejection across portal backends, long-gap
  replacement-window restoration, complete cross-desktop recording/DVR,
  anti-cheat acceptance, suspend/resume and additional GPU/driver combinations.

See [architecture and limitations](game-detection.md) and
[Discord catalogue/legal research](discord-detectables.md). No new external
dependencies were added. Vulkan development requirements and injected
reporter/multilib build, package, CI and test machinery were removed.
Compositor backends/helpers, resources, installers, settings and their obsolete
tests were removed. Add Game uses the cached process census; manual recording
uses the ScreenCast portal. No compositor IPC or Trophy desktop helper remains.
