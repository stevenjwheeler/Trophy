# Linux game detection and audio association

## Architecture

Before the process-based rewrite, Trophy used Vulkan/OpenGL injected reporters and a
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

There are five domain components:

* `src/detection/ProcessCensus.{h,cpp}` reads same-UID `/proc` records:
  executable, argv, cwd, PPID, start ticks and selected launch variables.
  It checks stat before/after inspection and rejects exit/reuse/exec races.
  Zombies, unreadable processes and oversized input are skipped.
* `src/detection/LauncherLibrary.{h,cpp}` reads installed-game metadata from
  local launchers into one catalogue. Steam discovery lives here alongside
  Heroic, Legendary, Nile, Lutris and Faugus; process identification does not enumerate
  launcher files.
  The Games page prelists non-runtime catalogue entries. Running games lead in
  Now Playing; inactive games group by local launcher provenance, with ready
  rows first and denied rows last. Explicit custom rules retain
  their own group; missing catalogue metadata never deletes those rules.
  Presentation keys and grouping do not enroll games or grant capture approval.
  Prefix-free Windows metadata may expose an exact-executable block veto, but
  cannot inherit a prefix-scoped restore token. Known prefix-scoped identities
  retain independent rows and approvals.
  Steam support tools are identified by exact known Steam IDs or explicit
  `toolmanifest.vdf` compatibility-layer roles, never title substrings. They
  remain in the worker catalogue to exclude their own processes, are omitted
  from launcher game counts, and appear only with Default Exclusions enabled.
  Prerequisite installers and Xalia also use the read-only Excluded state; their
  internal detection vetoes cannot be overridden by row controls.
  Collapsed sections retain one invisible representative in the filter model
  so their QML headings survive without instantiating every hidden row. Counts
  reflect all search/default-filter matches, independently of collapse state.
  The legacy desktop-app default veto list is retired through a versioned load
  migration; recorded custom definitions and later explicit Deny edits survive.
* `src/detection/GameLaunchContext.{h,cpp}` attests a private runtime targeting
  an already identified payload and checks installation, prefix and ancestry
  boundaries. This reusable mechanism applies to ordinary catalogue games as
  well as game-specific bootstraps; it performs no filesystem enumeration.
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
Launcher metadata is refreshed on the census worker at startup, on local metadata
notifications and by an independent 60-second fallback timer, not by counting
process scans. File notifications are coalesced over 250 ms; repeated events do
not extend the deadline. There is no minimum scan interval after a file change.
The worker resolves watches for metadata files and parent directories, including
the nearest existing parent of missing paths, Steam's secondary library
directories/manifests, GOG product info, Amazon library/Fuel metadata and Lutris
YAML/SQLite WAL files. Completed refreshes repair watches after atomic replacement,
deletion or directory creation. Watch targets are bounded to 8192; unavailable
watches and filesystems without reliable notifications retain the fallback.

New, unfamiliar or respawning processes never request a library refresh.
The process census uses the cached catalogue until metadata changes, the timed
fallback, or an explicit request refreshes it.
All triggers, including **Rescan now**, share one pending refresh flag and the
existing non-overlapping worker. Manual rescans and location changes request
refresh immediately, without the file-notification debounce. Changes during a refresh queue a follow-up;
only changed location preferences invalidate an in-flight result. Stop cancels
the timers and watches. Scan status has a separate notification from library
results; unchanged results do not republish the library list. The setup wizard
keeps the previous count/icons visible during background scans, including a
completed empty result, and only shows initial loading before the first result.

This notices installed/renamed games without a network catalogue update. `/proc` reads
kernel-provided process metadata, not game binaries or gameplay state. Link targets use
bounded `readlink()` calls without repeatedly resolving filesystem components.
The worker still validates start time and executable before/after inspection;
the optimization does not cache process ownership across ticks.

Polling is not free: cost scales with process count, and large/slow Steam
libraries can delay that worker. On the local warm-cache benchmark (100
iterations, 146 same-user processes and 49 installed Steam manifests), census
CPU averaged 13.79 ms, identification 0.44 ms and metadata refresh 3.79 ms.
At one census per second this is about 1.4% of **one CPU core**, not 1.4% of the
whole machine; that metadata measurement would contribute about 0.006% of one
core with only the 60-second fallback (file changes can add refreshes).
These numbers exclude PipeWire/audio/video work and are not a
worst-case or battery-life guarantee. Reproduce with `detection_probe --benchmark`.
They predate both the desktop-launcher hint index used by the filtered Add Game
picker and the additional non-Steam metadata readers; those periodic worker
costs have not yet been benchmarked.

Detection generally takes one to two ticks. Very short processes can exist
entirely between samples and are intentionally not guaranteed observable.
Reading every process's current metadata also catches exec transitions.
`CN_PROC` was considered and a temporary unprivileged subscription received
an acknowledgement on the test host (Linux 7.3.4). It is not used: kernel
configuration, permissions/namespaces and event loss vary, a complete census
is still necessary, and polling avoids another monitoring backend. There is
no eBPF, ptrace, injected library, privilege requirement or X11 detection.

## Identification and catalogues

`LauncherLibrary::installed()` supplies all supported launcher sources through
one catalogue:

| Launcher | Local source | Recognition evidence |
| --- | --- | --- |
| Steam | `steamapps/libraryfolders.vdf` and `appmanifest_*.acf` | Installed app directory, with App ID corroboration when present |
| Legendary / Heroic Epic | `legendary/installed.json` or `heroic/legendaryConfig/legendary/installed.json` under XDG config | Exact installed executable |
| Heroic GOG | `heroic/gog_store/installed.json` and the installed product's `goggame-<id>.info` | Matching product ID and primary file launch task |
| Nile / Heroic Amazon | `nile/` or `heroic/nile_config/nile/` under XDG config | Installed ID joined to local `library.json` title and `fuel.json` main executable |
| Lutris | XDG data `lutris/pga.db` and XDG config `lutris/games/<configpath>.yml` | Installed Linux/Wine entry's exact executable, plus configured Wine prefix |
| Faugus | XDG data `faugus-launcher/games.json` (also checks the config directory and standard Flatpak data/config locations) | Exact configured `path`, with configured `prefix` for Windows entries; explicit `Linux-Native` ELF entries need no Wine prefix |

Steam discovery reads local libraryfolders and app
manifests, including extra drives and common Flatpak locations. Heroic and Lutris
also check their standard Flatpak metadata directories, as does Faugus.
**Settings → Game Libraries**
shows every supported launcher, including **Not detected** entries, discovered
metadata locations, read errors and usable-game counts. Each launcher's dropdown
lists its usable installed games. **Rescan now** requests a fresh background scan.
Legendary's `LEGENDARY_CONFIG_PATH` is honored when inherited by Trophy and set
to an absolute directory.
Users can add up to 16 custom metadata directories per launcher, remove them,
disable automatic locations or reset to automatic discovery. Overrides supplement
automatic discovery unless it is disabled; canonical paths are deduplicated.
Custom Lutris directories contain `pga.db` and a `games/` YAML subdirectory;
automatic native/Flatpak discovery pairs separate config and data directories.
Custom Faugus directories contain `games.json`. Faugus entries must name an
existing ELF (explicit `Linux-Native` runner) or PE payload with an absolute host
Wine prefix. Home-relative `~/` paths are supported; arbitrary environment
expansion, command/script launch tasks, Steam-ID entries and additional-app batch
mode are not inferred. Shared storefronts are excluded. A configured Faugus
path does not attest an installation root, so it cannot grant generic-bootstrap,
directory-wide, prefix-wide or private-runtime sibling ownership.
An executable must be inside an installed app's directory; a matching
Steam App ID is strong corroboration when available. Inherited
`SteamAppId` alone does not make Steam, Proton, a runtime helper or Wine
service a game. Local manifests provide display names and `steam_app_<id>`
identity keys, not the identity of a particular running instance.

Recognition provenance distinguishes saved Steam App ID approvals from custom
executable rules. A verified installation match reports `Steam`, including when
the game already has a saved App ID approval. An App ID rule without a matching
installation path reports `Steam App ID rule`, not a claimed manifest match.

Other launcher entries do **not** grant directory-wide or prefix-wide
ownership. Initial matching requires the configured executable's exact host path.
Conflicting names for the same payload are not automatically selected;
an explicit user rule can resolve them. User names override launcher names,
and blocks remain authoritative. All sources preserve independent concurrent
instances and use the existing runtime process/audio checks.

A generic `Launcher.exe` (or game-specific `*Launcher` binary) is normally
excluded, but can establish an instance when local installed-game metadata
names its exact executable inside a verified installation. The worker records
that bootstrap role and installation scope; its canonical executable must not
escape the installation through a symlink. Known shared storefronts such as
Epic Games Launcher, Ubisoft Connect and EA App remain excluded even when
configured as an entry's launch executable. An executable basename, user
display name, Steam variable or shared Wine prefix cannot enable the exception.

Bootstrap descendants must have observed process ancestry, a payload inside
the same installation and the same Wine prefix as the bootstrap. Unrelated
siblings and out-of-installation children are not adopted. Once an unchanged
child has been observed, it can retain the game key/instance ID after the
bootstrap exits or the child is reparented; PID reuse, payload changes, blocks
and runtime-prefix changes invalidate that membership. This feeds the existing
debounce and recording recovery without granting additional capture permission
or bypassing the desktop portal.

Known metadata-backed bootstraps establish detection identity but do not arm
automatic capture. Once a verified non-bootstrap member joins the instance,
it becomes the primary even while the launcher remains open. Pending recording
enters audio settling on that promotion; authorization
notifications for new games are also deferred past the bootstrap. The wait is
instance-scoped, has no launcher timeout and is cancelled by Stop, blocking or
instance replacement. It is process readiness, not proof of a visible window.
Unclassified launchers/loaders and pre-existing launcher-bound portal tokens
cannot be guaranteed to retarget automatically by this policy.

UMU/pressure-vessel can make an identified executable and its successor siblings under a private
`pv-adverb` instead of Linux parent/child processes. This case requires the
runtime's command after `--` to explicitly invoke Proton's
`waitforexitandrun` (optionally through `umu-shim`/Python) with the exact
identified payload, plus ancestry
through `srt-bwrap`/`bwrap` to an `umu-run` invocation targeting that same
payload. The observed runtime PID/start time, executable and command are then
pinned as the private launch context. Subsequent in-installation payloads must
resolve to that exact context with unchanged identity and the same runtime
prefix. Another private runtime or a shared Wine broker is an ancestry
boundary; a common storefront ancestor, broker name or prefix is insufficient.
Runtime/broker processes are never game members and cannot keep an otherwise
finished game alive.

Steam runtime infrastructure (including SRT loggers, client containers and
architecture-prefixed launcher services) is excluded even when its executable
matches an installed Steam Linux Runtime tool. This filters helper executables,
not games launched inside the runtime.
The worker also reads `toolmanifest.vdf`: installations whose compatibility
layer is `container-runtime` exclude their infrastructure payloads, including
new diagnostics not covered by executable-name filters. A game outside that
tool installation is not excluded because its parent runs the runtime.

Capture-window loss can arrive between census ticks, after the original
payload exits but before its already-running successor enters the cached
membership. For proven window destruction or an approved empty capture, the
recorder can retain full-recording intent while that same selected/approved
instance is being resolved against its unchanged private launch context.
Native recording is drained during this census-settling phase; a new capture
attempt still requires a verified live game member. This has no wall-clock
handoff/retry cutoff: the next authoritative census either establishes the
successor or removes the instance. A runtime helper alone cannot keep a
census-confirmed finished game alive. Explicit Stop, changed instance,
removed approval or dead runtime cancel pending work. Ambiguous permission
loss is not promoted into this recoverable path.

Launch-context attestation is not restricted to `*Launcher` filenames.
Ordinary Steam and other catalogue games use the same mechanism when their
installation scope is unambiguous. Siblings that also have their own matching
catalogue entry can retain the original launch identity rather than appearing
as a second game. Explicit different user rules remain independent, and blocks
on the established launch are honored during the handoff. User-defined seeds
can participate when catalogue metadata independently verifies their installation;
a user name/path rule alone never grants ownership of the whole executable
directory. Direct/descendant detection keeps its existing behavior when there
is no attested runtime context.

This is not retrospective executable discovery: a game already running after
an unobserved bootstrap exit still needs direct executable evidence or an exact
user rule. Handoffs without observed ancestry or an attested private launch
context are not inferred from installation or prefix membership alone.
Debug logs report launch tracking members (PID, start time, parent and
payload), newly observed unowned processes in the tracked installation, and
loss of all verified members. These diagnostics do not log process arguments
or portal tokens and do not change matching decisions.
The `Game detected` info message also reports recognition provenance, such as
`from "Steam"`, `from "Heroic (Epic)"` or `from "Lutris, user rule"`.
Metadata and rule sources are retained together when both apply, and the
established source attribution survives verified process handoffs.

Launcher files are read only, on the census worker. SQLite opens existing
Lutris databases read-only with a short lock timeout and query work budget.
JSON/YAML reads and catalogue entries are bounded; missing, malformed,
oversized and unsupported entries are skipped. No launcher is executed,
no account credentials are read, and no online login or catalogue is needed.
Non-Steam executable files are checked for an ELF/PE header to avoid treating
wrapper scripts as game binaries or verified bootstraps.

Coverage is intentionally conservative: native launcher metadata in standard
XDG locations, common Steam/Heroic/Lutris Flatpak paths and user-added metadata
directories are supported. Launcher-specific executable overrides,
emulator/ROM entries, script-only launch tasks, and
Wine drive-letter paths in launcher configuration may still need an exact
Trophy rule. GOG detection currently requires a matching `.info` primary file
task; Linux packages that only expose `start.sh` are not inferred from their
installation directory. Amazon Fuel files must be valid JSON. Reading a
Flatpak Steam library from native Trophy is not support for running Trophy
inside Flatpak.

Native Linux payloads are ordinary executable paths. Wine loader argv is
interpreted only at the payload position, not by searching arbitrary
arguments for `.exe`. CWD-relative paths and Wine drive paths are resolved.
The census worker resolves the relevant `<WINEPREFIX>/dosdevices/<letter>:`
symlink, including custom Steam library drives such as S: and relative targets.
This mapping is stored in the process snapshot; identification and picker
rendering do not perform filesystem lookups. Explicit mappings override C:/Z:
defaults. Broken mappings and unknown/drive-relative paths are rejected rather
than inferred from CWD or an inherited Steam App ID.
Wine services/installers/crash handlers and unverified generic launchers are excluded.
Games without usable launcher metadata need exact user definitions; a generic executable
named `Game.exe` is not automatically a game.

The Games allow-list supplies user definitions and display names; it accepts
exact executable basenames, paths, `exe:/path`,
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

`src/audio/AudioApplicationRegistry.{h,cpp}` maintains one persistent discovery
connection shared by game status and the Voice application picker. A dedicated
worker observes node/client additions, current-info property changes and removals.
It publishes changed playback metadata only after core-sync barriers; announce-only
identity is never promoted to verified current properties. Proxy storage remains
stable until removal, and discovery binds at most 4096 node/client objects.
An idle observer blocks on native events rather than periodically reconnecting or
enumerating applications. Picker refresh reads the same cached snapshot.

Stream snapshots contain metadata, not cached game ownership. The process census
worker rechecks association against live process identity without another PipeWire
enumeration; changed stream metadata invalidates in-flight association results.
Audio that appears after a game starts does not change the game identity.
Service loss clears cached node IDs immediately; reconnect attempts back off from
250 ms to five seconds and rebuild current-info state. Shutdown interrupts both the
native wait and reconnect delay, and rejects queued publications from older runs.
Application-change notifications/logging occur only when the cached list changes.
Capture target lookup, strict Voice identity verification and the recording
engine's owned-audio fallback/retry policy remain separate and unchanged.

## Recording integration

Automatic full recordings and DVR both use asynchronous owned-audio settling
after payload readiness. A present initial probe starts capture; otherwise two
consecutive successful polls settle audio. Ten resolved polls trigger the
silent-game fallback (2-second timer, with slow in-flight probes neither
overlapping nor consuming ticks). Stop and identity changes invalidate queued
results. Audio is a startup hint, not proof that a window exists or a guarantee
of silent portal restoration. Session audio preparation, desktop fallback and
owned-game retry remain in place. Neither path bypasses portal selection,
approval or process-identity checks.

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

PipeWire registry observers retire node/client proxies on global removal and
free them locally after the proxy's server-removal notification. This avoids
destroying already-removed resources during short-lived lookup-client churn.
For owned sibling links, an `ENOENT` core error is nonfatal only when its
request sequence matches a destroy issued by this capture, and only until the
following sync fence completes. Untracked errors, disconnects, permission
failures and rejected routes retain their failure/fallback behavior.
Capture failures log the source and reason; core failures include ID, sequence
and result. Error text alone never authorizes ignoring a failure.
An observed audio-target removal retires that capture quietly rather than
calling it a wrong-app route. It still stops PCM and reports capture stopped;
the owner falls back/retries if recording continues, or finalizes when the
game exits. Terminal captures quiesce registry observers immediately. Explicit
stop and an already-reported terminal cause do not produce additional warnings
from queued native teardown events.

Manual application capture can pin an explicitly selected portal process
by PID/start identity without classifying it as a game; absent metadata
is not repaired by borrowing the current game.
Desktop-only audio remains an explicit recording setting. Existing audio mixer,
encoding, muxing, clip/DVR and portal lifecycles are retained.

### Captured-window handoff

Saved handoff parts are filed immediately without a session-end chime. Their
completion feedback remains deferred until either a replacement capture starts
(continuation, still quiet) or recovery ends without a replacement (one normal
saved-recording notification/chime). A briefly surviving process at window
shutdown must not permanently suppress completion when the game then exits.

The portal's `Session::Closed` signal has its own terminal classification,
separate from proven node/window destruction. It can mean source shutdown or
revoked sharing and never grants automatic recapture. Recording drains and
preserves the current part, settles the pinned instance against the census
instead of declaring the departed bootstrap to be game exit. This terminal
permission boundary is not the automatic launcher-handoff mechanism.

If the granted node was independently reported removed *before* `Closed`,
the already-pending destruction check is allowed to finish while frame delivery
retires immediately. Confirmed destruction can then take the existing recovery
path; an inconclusive check remains terminal. `Closed` alone never starts this
probe, and Stop/new-attempt generations discard late results. This preserves
source-loss evidence when portal/native events race; it cannot guarantee
handoff when the backend supplies only an ambiguous closure.

An explicit recording restart uses a fresh window-only portal selection with no saved
restore token. Existing tokens are not deleted just because the session ended.
An instance-scoped selection fence blocks automatic full recording and DVR
until explicit fresh selection succeeds or that instance ends. Stopping,
changing selection and switching back cannot bypass the fence; concurrent
instances retain independent fences. Manual recordings do not inherit a
detected-game resume target. Cancelled/empty fresh selections do not fall back
to an older closed grant.

When a captured window disappears while its exact pinned game instance remains
alive, recording and DVR use the same capture lifecycle to retry the existing
portal approval after 1.5 seconds. A PID change is not required. Full recording
saves the completed part in the library and continues into a new output file.
DVR retains its cached GOP packet blocks and appends the new capture generation;
an in-progress clip export keeps its immutable packet snapshot and cache-file
leases. Saving a clip does not rotate capture or restart the encoder. The window gap is not
counted as buffered footage. Recovery has no
attempt-count or elapsed-time cutoff while that exact game instance is alive.
Each attempt is delayed by 1.5 seconds after teardown; requests never overlap.
A successful portal grant with an empty stream list is also retryable for an
approved automatic game capture, allowing the main window to appear later.
Explicit stop, game exit, a different selected
instance, removed approval, blocked capture, and output/finalization failure
cancel recovery.

DVR uses the recording engine's configured resolution. A replacement capture
in native mode uses the new window's native size, not the launcher's. Clip
exports that span different sizes normalize video to the newest segment's
canvas while preserving aspect ratio; compatible exports still stream-copy.

PipeWire registry removal alone is ambiguous. An asynchronous ordinary
connection checks the daemon cookie and original node serial. If the original
node still exists, the independent connection lacks readable default/target
permissions, or the check is inconclusive, capture stops rather than
attempting restoration. An external portal session closure or service failure
also stops recovery. No new Trophy continuation dialog is added, and no
permission is bypassed: the portal backend still controls whether a saved
approval can restore the replacement window without a picker.
Generic portal rejection, user cancellation, request timeout and service
failures remain terminal rather than being assumed to mean "window not ready."

All Trophy-specific compositor integrations have been removed: KWin scripts,
GNOME Shell extensions, Hyprland/Sway IPC, window enumeration and helper setup.
Add Game lists cached process payloads/paths/PIDs and revalidates PID/start and
payload before saving an exact executable/Wine-prefix rule. No window class is
used. The picker defaults to **Likely applications** (known game primaries,
Windows payloads and exact cached desktop-launcher executable matches).
**Show all processes** exposes the remaining selectable rows, including
unknown native games. Filtering is presentation only: it does not detect
additional games, prove window visibility, change stored rules or authorize
capture. Desktop launcher metadata refreshes on the census worker with the
local Steam catalogue; no filesystem enumeration is done when the picker
opens. Hidden/non-displayed/terminal launchers are not application hints, and
higher-priority XDG desktop entries mask lower-priority ones.
Manual recording uses the standard ScreenCast portal; detected-game
recording retains its pinned instance. Manual titles/live title updates are not
available unless the portal supplies a usable label. Without a portal PID,
manual capture falls back to desktop audio rather than guessing application
ownership. Portal authorization and backend compatibility are still required.

## Limitations and testing

See the [development guide](development.md#development-and-tests) for testing
instructions. Important boundaries:

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
