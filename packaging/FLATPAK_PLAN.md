# Flatpak feature-parity plan

Status: revised proposal, 2026-10-15. No implementation or live verification implied.

## Non-negotiable distribution requirement

**One Flatpak installation. No separately installed host companion, host setup
package, terminal commands, or manual environment configuration.** Normal in-app
module consent and portal permission dialogs are acceptable; external setup is
not. Do not disguise a host installer or unrestricted host execution as a
sandbox integration solution.

Goal: match AppImage recording, automatic detection/recording, DVR, audio,
library/player/editor, shortcuts, tray, notifications, optional modules and login
startup on the same host. Desktop/backend limitations shared with AppImage must
be documented separately from Flatpak regressions.

Preserve PipeWire-only capture, opt-in modules, no blind process scanning, and
the frozen `/tmp/trophy-detection.sock` START/STOP reporter protocol. Native and
AppImage behavior must not regress.

## Critical feasibility gate — before promising parity

The current integrations assume host access that Flatpak does not automatically
provide. There is not yet a demonstrated single-install architecture satisfying
all the requirements below. Full parity is a research outcome, not an assured
implementation milestone.

- [ ] **Reporter delivery and enrollment:** find a supported way for host Vulkan
      and GL loaders, including Steam/Proton, to discover compatible reporters
      delivered by the Flatpak installation. Investigate applicable Flatpak
      extension mechanisms, but do not assume they enroll native host games.
      Payloads must be durable and visible in each target loader namespace;
      `/app` paths and sandbox config writes are not host enrollment.
- [ ] **Reporter communication:** prove host games can reach the frozen
      `/tmp/trophy-detection.sock` listener across the sandbox boundary without
      an externally installed proxy, broad host access, or a protocol change.
      Do not assume a filesystem permission exposes host `/tmp`; verify what
      Flatpak actually supports before proposing an implementation.
- [ ] **Process identity:** establish targeted liveness, Steam identity and
      naming without treating host PIDs as sandbox PIDs. Include PID reuse,
      namespaces and Steam/Proton. No blind `/proc` scans or pre-grant PipeWire
      discovery as a substitute for reporters.
- [ ] **Window context:** prove opt-in compositor integration can be installed
      and queried through supported, narrowly scoped mechanisms with no host
      tools/setup required. Evaluate KWin, GNOME, Hyprland and Sway separately.
      There is no assumption of a universal window-enumeration portal.
- [ ] **Distribution acceptance:** check any proposed extension, installation
      side effect and permission against Flatpak behavior and the intended
      repository's policies. Technical feasibility is not policy approval.
- [ ] **Coexistence:** establish ownership and recovery when native/AppImage
      Trophy already owns the frozen detection socket; never steal a live one.

Exit: a reproducible single-install proof for these integrations, documented
permissions and an agreed security model. If any required feature cannot meet
these constraints, report the conflict explicitly and pause the parity claim.
Do not silently introduce a companion, broaden permissions, change the protocol
or redefine a manual-only build as feature-equivalent.

## Phase 1 — reproducible sandboxed core

This work is independently useful, but is not full feature parity.

- [ ] Build in a clean SDK; inventory actual codecs, Qt multimedia plugins,
      PipeWire tools and hardware-driver/runtime requirements.
- [ ] Replace unrestricted session-bus access with justified narrow permissions.
      Portals are normally accessible without broad portal talk permissions.
      Review obsolete direct-input, KGlobalAccel, IPC and X11 permissions.
- [ ] Live-test ScreenCast creation, parent association, PipeWire FD handoff,
      cancellation, stale tokens, portal restart and Session.Close under the
      final filtered bus permissions.
- [ ] Verify direct PipeWire audio access independently from ScreenCast's
      restricted remote: game/voice/mic identification, monitor fallback,
      separate and merged tracks, mute/meters, late binding and device changes.
      Do not assume host/sandbox PID equality.
- [ ] Package completion-sound playback and verify software encoding, NVENC,
      VA-API and Qt playback using appropriate runtime driver extensions.
- [ ] Support Videos and approved custom directories, persistent access where
      available, export, snapshots, cache paths, rename/delete, sidecars,
      capacity/cleanup and DVR storage limits.

Exit: live record/play/edit/export with correct multi-track audio and clean
error/cancellation handling, without unrestricted bus access.

## Phase 2 — implement the proven host-integration mechanism

Start only after the feasibility gate succeeds. The implementation mechanism is
intentionally not specified until it is demonstrated to work within the
single-install constraint.

- [ ] Implement automatic reporter delivery and in-app opt-in enrollment using
      the validated mechanism, including uninstall and update/rollback.
- [ ] Validate host/Steam reporter ABI rather than shipping Flatpak-runtime
      binaries into unrelated host loaders. Assess 32-bit support separately;
      it is not an already-established AppImage feature.
- [ ] Integrate the proven detection transport and identity model while keeping
      blacklist/whitelist, pending/fullscreen/minimized gates, reconcile-on-launch,
      liveness, promotion and existing authorization behavior.
- [ ] Implement the proven compositor-context mechanisms and Add Game picker.
- [ ] Test game close during startup, late/stale results, service restarts,
      module opt-out and already-running games. Missing integrations must show
      an actionable, truthful state, not a working/installed badge.
- [ ] Restore automatic recording and DVR only after game identity and
      authorization are valid. Installation/module consent never authorizes
      capture on its own.

Exit: one Flatpak install plus normal in-app consent provides the same supported
game-detection/auto-record/DVR workflow as AppImage, with no external setup.

## Phase 3 — desktop integration and migration

- [ ] Implement Background portal startup/background consent where supported;
      test login startup. A sandbox autostart file is not host autostart.
      If the desktop lacks the required mechanism, record a parity blocker
      instead of adding a host-install workaround.
- [ ] Verify portal shortcuts, remapping, disable/re-enable and service restart.
- [ ] Verify tray, close-to-tray, launch activation and recording confirmation,
      matching the same desktop's AppImage capabilities.
- [ ] Verify notification actions and fullscreen behavior where supported,
      including pending-notice withdrawal when a game exits.
- [ ] Make update messaging repository-aware; Flatpak updates are not AppImage
      replacement. Review whether the existing updater needs network access.
- [ ] Offer explicit settings/library migration where access is available.
      Do not assume restore tokens transfer between application identities.
- [ ] Verify install/update/uninstall leaves user media and foreign enrollment
      untouched and never silently enables optional modules.

## Phase 4 — release gates

- [ ] CI builds installable bundles from pinned/verified sources, checks final
      permissions and validates desktop/AppStream metadata.
- [ ] Add isolated integration tests for the chosen enrollment/transport/identity
      mechanism, malformed input, restarts, version skew and coexistence.
- [ ] Retain fake-portal race/cancellation tests and add packaged startup/player
      smoke tests. Headless smoke tests do not prove capture.
- [ ] Compare Flatpak and AppImage on KDE, GNOME, Hyprland and Sway; test
      NVIDIA/NVENC, AMD/VA-API, Intel/VA-API and software encoding.
- [ ] Cover long-running multi-track recording/DVR, reboot/login, suspend,
      portal/PipeWire restart, disk-full, stale authorization and update/rollback.
- [ ] Publish the verified feature matrix and installation/permission details.
      Promote to parity only when the single-install gates actually pass;
      distribution review remains separate.

## Effort and libportal

Withdraw the earlier 4–8 week estimate: it assumed a separate host companion,
which does not satisfy the distribution requirement. Time-box the feasibility
prototype first, then estimate implementation from demonstrated mechanisms.
Do not promise a full-parity delivery date before that gate passes.

Libportal can reduce D-Bus maintenance and provide helpers such as Background
portal access in native and Flatpak builds. It neither supplies host reporter
enrollment nor bridges private `/tmp` or PID namespaces. Evaluate separately
against Qt/GLib event-loop integration, parent-window export, cancellation,
restore tokens and packaging. Preserve application lifecycle/authorization
guards regardless of library choice; migration is not a prerequisite here.
