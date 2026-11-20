# Linux game detection and audio association

## Architecture

The implementation on `main` used Vulkan/OpenGL injected reporters and a
fixed local socket, fused with KWin scripts, GNOME extensions and
Hyprland/Sway window snapshots. Audio used PID queries, substring name
hints and automatic desktop-monitor fallback. It could miss games already
running, conflate names, and depended on window availability for capture
readiness.

The rewrite removes the reporter socket, injected libraries, enrollment UI,
reporter ABI/multilib machinery, and window-driven game identification.
`ToplevelRegistry`, `KWinProtocolBackend`, `WlrToplevelBackend`, and
`ProcessCensus` did not exist on the `main` base; no code from the previous
`detection-refactor` attempt was used.

There are three domain components:

* `src/detection/ProcessCensus.{h,cpp}` reads same-UID `/proc` records:
  executable, argv, cwd, PPID, start ticks and selected launch variables.
  It checks stat before/after inspection and rejects exit/reuse/exec races.
  Zombies, unreadable processes and oversized input are skipped.
* `src/detection/ProcessGames.{h,cpp}` identifies payloads and owns instance
  membership. Instance identity includes the launch PID/start ticks, not
  just title, executable or App ID. Related descendants are included, but
  sharing Steam as an ancestor or a Wine prefix alone is insufficient.
  Surviving unchanged members retain ownership after reparenting. Exec and
  PID reuse invalidate membership. Multiple instances of one title remain
  distinct unless launch ancestry proves they belong together.
* `src/detection/GameDetectionService.{h,cpp}` schedules scans off the UI
  thread, publishes instance snapshots, preserves the selected target until
  exit/explicit selection, and maintains the Games model/preferences.
  `instances` exposes IDs, identity, processes, associated audio streams
  and `waiting`/`associated` audio state; `selectInstance()` selects capture.

The process timer is one second, with at most one census outstanding.
Steam metadata is refreshed on the census worker at startup and every 30 scans.
This reads local `libraryfolders.vdf`/`appmanifest_*.acf` files to notice
installed/renamed games; it is not a network catalogue update. `/proc` reads
kernel-provided process metadata, not game binaries or gameplay state. Link targets use
bounded `readlink()` calls without repeatedly resolving filesystem components.
The worker still validates start time and executable before/after inspection;
the optimization does not cache process ownership across ticks.

Polling is not free: cost scales with process count, and large/slow Steam
libraries can delay that worker. On the local warm-cache benchmark (100
iterations, 146 same-user processes and 49 installed Steam manifests), census
CPU averaged 13.79 ms, identification 0.44 ms and metadata refresh 3.79 ms.
At one census per second this is about 1.4% of **one CPU core**, not 1.4% of the
whole machine; metadata contributes about 0.013% of one core averaged over
30 seconds. These numbers exclude PipeWire/audio/video work and are not a
worst-case or battery-life guarantee. Reproduce with `detection_probe --benchmark`.

Detection generally takes one to two ticks. Very short processes can exist
entirely between samples and are intentionally not guaranteed observable.
Reading every process's current metadata also catches exec transitions.
`CN_PROC` was considered and a temporary unprivileged subscription received
an acknowledgement on the test host (Linux 7.3.4). It is not used: kernel
configuration, permissions/namespaces and event loss vary, a complete census
is still necessary, and polling avoids another monitoring backend. There is
no eBPF, ptrace, injected library, privilege requirement or X11 detection.

## Identification and catalogues

`ProcessGames::steamLibrary()` reads local Steam libraryfolders and app
manifests, including extra drives and common Flatpak locations.
An executable must be inside an installed app's directory; a matching
Steam App ID is strong corroboration when available. Inherited
`SteamAppId` alone does not make Steam, Proton, a runtime helper or Wine
service a game. Local manifests provide display names and `steam_app_<id>`
identity keys, not the identity of a particular running instance.

Native Linux payloads are ordinary executable paths. Wine loader argv is
interpreted only at the payload position, not by searching arbitrary
arguments for `.exe`. CWD-relative paths and Wine Z:/C: paths are resolved.
Wine services/installers/crash handlers and generic launchers are excluded.
Unknown non-Steam games need exact user definitions; a generic executable
named `Game.exe` is not automatically a game.

`GameDefinitions.h` contains original, locally verified Trophy definitions.
The existing Games allow-list supplies user definitions, overriding those
names; it accepts exact executable basenames, paths, `exe:/path`,
`wine:<prefix>:<payload-path>` or `steam_app_<id>`. Existing blacklist and
portal-token settings remain authoritative. No fuzzy executable matching,
network catalogue dependency, factory registry or catalogue plugin API is
introduced. A future independently licensed catalogue can supply the same
small key/name map; no Discord content is distributed or downloaded by
Trophy for detection.

## PipeWire

`src/audio/PipeWireAudioSource.cpp` uses native registry enumeration and
bounded core-sync barriers. It binds playback nodes
(`Stream/Output/Audio`) and their clients, then reads current info rather
than relying on announce-time dictionaries. Nodes are capture targets;
clients are identity evidence, not capture targets. Missing identity can
be filled from an owning client only when process properties do not
contradict the node. Identity dictionaries are replaced on info changes.
Numeric node IDs are not durable; capture pins `object.serial` and verifies
the actual incoming graph links.

Process membership is the primary audio evidence. A present nonmember PID
is a veto, including Firefox/Discord; it is not repaired by a friendly title.
PID evidence is checked against the census executable/start identity and
current `/proc` state. Without a PID, only an exact payload basename
(binary or Wine's executable app name), resolving to one live instance,
is accepted. Display-title substrings are not evidence. If two instances
could own a PID-less stream, it remains unassigned. App IDs and binary
metadata are application-controlled, not authenticated security identities.
This protects against accidental routing, not a deliberately malicious
same-user process spoofing all of its metadata.

`src/audio/AppAudioGroup.h` observes live nodes/ports/links for an owned
capture, verifies its pinned target and links other independently associated
playback nodes from the same instance into its graph-clock input. Thus
multiple helpers, clients and streams can contribute without concatenating
audio timelines. Different-instance routes are rejected; Voice still uses
strict binary verification. A removed primary is rebound by recording's
existing retry against a fresh serial; late streams, changed devices and
daemon reconnection use the same bounded lookup/retry path.

The service's independent audio scan runs on a worker and reports stream
snapshots without blocking process discovery. Audio that appears after a
game starts does not change the game identity.

## Recording integration

Recording and DVR pin `detectedGameInstance` at capture start. Their
initial lookup and session retries use that exact instance, not a later
namesake, compositor PID or portal display label. Portal capture and restore
tokens remain separate from process identification. When owned game audio is
unavailable or capture fails, Game mode captures the default desktop monitor.
This deliberately includes other desktop applications, including Voice.
The separate Voice source is suppressed to avoid duplication; the microphone
remains independent. Session audio checks run every three seconds and retry
game capture, replacing the monitor when owned audio becomes available.
Diagnostics expose `desktopFallback`, Voice inclusion and System audio history.
Fallback does not assign unrelated desktop streams to a game.
Manual application capture can pin an explicitly selected portal process
by PID/start identity without classifying it as a game; absent metadata
is not repaired by borrowing the current game.
Desktop-only audio remains an explicit recording setting. Existing audio mixer,
encoding, muxing, clip/DVR and portal lifecycles are retained.

All Trophy-specific compositor integrations have been removed: KWin scripts,
GNOME Shell extensions, Hyprland/Sway IPC, window enumeration and helper setup.
Add Game lists cached process payloads/paths/PIDs and revalidates PID/start and
payload before saving an exact executable/Wine-prefix rule. No window class is
used. Manual recording uses the standard ScreenCast portal; detected-game
recording retains its pinned instance. Manual titles/live title updates are not
available unless the portal supplies a usable label. Without a portal PID,
manual capture falls back to desktop audio rather than guessing application
ownership. Portal authorization and backend compatibility are still required.

## Verification and boundaries

See [verification record](game-detection-verification.md) for commands,
observations and unverified scenarios. In particular:

* Polling cannot guarantee detection of a process that lives less than a tick.
* Sandboxing/hidepid, inaccessible environments/executables or PID namespaces
  can prevent confident attribution. Missing evidence triggers desktop fallback,
  not guessed ownership. A muted/unavailable desktop monitor still cannot
  provide audible output; fallback does not eliminate device failures.
* Wine processes already reparented before the first scan may lack proof of
  shared launch ancestry. They are not merged by shared prefix alone.
* Unknown launchers/helpers, unusual Wine argv/drive mappings and non-Steam
  games can need user rules. A launcher living in a game directory with an
  unusual non-launcher name may itself qualify; no universal catalogue-free
  heuristic can perfectly distinguish every launcher.
* Interpreter recognition covers direct script/assembly arguments and
  `java -jar`. Option-heavy or module-based interpreter commands are not
  generally recognized.
* Existing non-Steam rules keyed only by a compositor window class remain
  stored, but may need conversion to an executable/path definition. A
  window class is no longer process-identification evidence.
* A game instance ends when its proven member set ends; a deliberately
  persistent non-excluded game helper can keep it alive.
* Missing metadata, ambiguous basenames and stale process identities are
  not reasons to capture a desktop monitor or guess another game.
