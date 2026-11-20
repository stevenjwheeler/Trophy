# Trophy task list

## Add Game picker usability

- [x] Implement the agreed portable UX: default **Likely applications** with
  **Show all processes** for unknown native/Wine games and other selectable
  processes. Suggestions use cached desktop executable matches, known game
  primaries and Windows payloads; they are not a verified window-only list.
- [x] Preserve cached asynchronous enumeration and
  selection-time PID/start-time/executable/payload revalidation. Cover
  background services, unknown games, interpreters, duplicate processes,
  process exit/reuse, search, refresh, Watch and Block in regressions.
- [x] Do not label heuristic matches “windowed.” Inherited `WAYLAND_DISPLAY`,
  an installed desktop entry or an audio stream does not prove a visible
  window. The ScreenCast portal supplies a window picker, not a portable
  pre-authorization window-to-process census.
- [ ] Benchmark the added periodic desktop-launcher hint refresh on large
  XDG application directories; older census/Steam-only numbers exclude it.
- [ ] Collect live feedback on default suggestions for additional native,
  interpreted and third-party-launcher games; Show all remains the fallback.

## Detection coverage

- [ ] Design a suitably licensed game catalogue for additional non-Steam
  executable-to-game-name matches. Unknown non-Steam games currently require
  a built-in definition or an exact user rule.
- [ ] Define catalogue provenance, updates, override precedence, ambiguous
  executable handling and offline behavior. Do not distribute or fetch the
  Discord database as Trophy's catalogue.

## Capture and recovery validation

- [ ] Live-test long loader-to-main-window gaps, not just quick handoffs.
  Process-lifetime recovery has no attempt-count or elapsed-time cutoff for
  recoverable absence, but generic token rejection, permission loss, portal
  timeout and service/output failures remain terminal.
- [ ] Investigate whether supported portal backends can distinguish a missing
  replacement window from rejected approval. Do not retry an ambiguous
  rejection as though it proves a temporary missing window.
- [ ] Expand live native/Proton/plain-Wine and third-party-launcher testing on
  GNOME, KDE and Hyprland, including simultaneous instances and revoked access.
- [ ] Validate the rewritten behavior in DEB, RPM, tar.xz and full AppImage
  builds, and across available GPUs/encoders. Local native smoke is not full
  packaging or desktop compatibility validation.

## Completed in the current working tree

- [x] Compositor-independent, unprivileged process-based detection, local
  Steam names, native/Wine payload resolution and exact custom rules.
- [x] Pinned concurrent instances and conservative PipeWire audio ownership;
  desktop-audio fallback and game-audio recovery without duplicate Voice.
- [x] Quiet expected audio-target exit, while retaining real route/service
  errors.
- [x] Automatic full-recording window handoff: save the first part, retain
  the same instance and approval, and avoid DVR takeover.
- [x] Replace the initial three-attempt/three-minute recovery budget with
  instance-lifetime retries for recoverable source loss/approved empty capture.
- [x] Support Steam's hashed per-app icon cache, invalidate old cached misses,
  and retain quiet missing-artwork fallbacks.
- [x] Local validation of the combined installed payload: **1,240 passed,
  15 skipped, zero failed** (1,255 CTest cases), plus native install/startup
  smoke. A user-run Lethal Company quick handoff was confirmed in the logs.
- [x] Update landing-page/generated-guide sources to describe
  compositor-independent operation and remove old helper/reporter setup
  instructions, while retaining portal/driver requirements and testing limits.
- [x] Audit recent documentation changes: remove the README's stale detection
  setup claim, document handoff retries/separate output parts in the user guide,
  and distinguish historical verification evidence from the current baseline.

These completed items describe the development branch; do not assume they
are in a published release. Evidence and limitations:
[game-detection-verification.md](docs/game-detection-verification.md).

## Deferred

- [ ] Flatpak support: deferred at the user's request. Full parity needs a
  verified host-process visibility strategy, portal-provided PipeWire video
  connections and sandbox-compatible per-application audio; it is not simply
  a `/proc` filesystem permission.
