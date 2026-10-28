# Automatic Update Installation — Plan

Status: planning only. Not implemented. Repository is private until release, so
update checks currently 404; treat this as post-release work.

## Problem

Trophy ships four artifacts (`AppImage`, `.deb`, `.rpm`, `.tar.xz`) plus native
prefix installs. Trophy is **not** in any apt/dnf repository, so package-manager
"update" commands are not available: every non-AppImage install is a manual
download + local install. Only the AppImage can plausibly self-replace, and even
that is a file swap, not a package upgrade.

## 1. Detect installation kind

New helper (e.g. `src/core/InstallInfo`) returning an enum:

- `AppImage` — `qgetenv("APPIMAGE")` non-empty (the AppImage runtime sets it
  to the original file path), or `APPDIR` non-empty and the resolved executable
  path lives under it. The replacement target is the `APPIMAGE` path.
- `DebianPackage` — `dpkg -S <execpath>` succeeds (file owned by dpkg).
- `RpmPackage` — `rpm -qf <execpath>` succeeds.
- `PortablePrefix` — executable not owned by any package manager and not an
  AppImage (typical extracted tar.xz / `~/.local` manual install).
- `Unknown` — fallback; never attempt an automatic install.

Notes:
- Run package-manager queries read-only and cache the result; they can be slow.
- `APPIMAGE` env can be inherited by a native install (see
  `PersistentReporterLib::isBundled`), so require the executable canonical path
  to actually live under `APPDIR` before calling it an AppImage.
- Distro family can be inferred from which package manager owns the file, not
  from `/etc/os-release` alone.

## 2. Resolve the matching release asset

Extend `UpdateChecker::onReplyFinished` to parse the `assets` array from the
GitHub release and select by install kind:

| Kind            | Asset pattern                     |
|-----------------|-----------------------------------|
| AppImage        | `Trophy-<ver>-x86_64.AppImage`    |
| DebianPackage   | `Trophy-<ver>-amd64.deb`          |
| RpmPackage      | `Trophy-<ver>-x86_64.rpm`         |
| PortablePrefix  | `Trophy-<ver>-Linux.tar.xz`       |

Also read the asset's `digest` (sha256) or the published `SHA256SUMS` for
verification. Emit the chosen asset URL + digest in `checkFinished`.

## 3. Update flows

### AppImage — automatic (phase 2)

1. Add an "Update now" action (Info page / notification) enabled only when
   `installKind == AppImage` and not recording.
2. Download the asset to a temp file in the **same directory** as the current
   AppImage (so the final rename is atomic and same-filesystem).
3. Verify SHA-256 against the release digest.
4. `chmod +x` the downloaded file.
5. Spawn a tiny helper script that:
   - waits for the current process PID to exit (`kill -0` poll),
   - moves the current AppImage to `<name>.old` (rollback copy),
   - moves the new file into place,
   - relaunches the AppImage,
   - removes the `.old` copy.
6. Quit Trophy so the helper can replace the file.
7. `syncAutostartFile()` already keys off `APPIMAGE`; the path is unchanged,
   so autostart keeps working.

Safety: never update while recording; keep `.old` for rollback; abort and clean
up on any checksum/network failure.

### Native packages (.deb/.rpm) — manual (phase 1)

Trophy is not published in any apt/dnf repository, so package-manager "update"
commands do not apply. The user must download the new package and install it
locally. The app must not self-elevate or run package managers with root.

- Notify the user that an update is available with the release link.
- Offer "Download update" which saves the matching package to a user-chosen
  location (default `~/Downloads`).
- Show the exact local-install command to run, e.g.
  `sudo apt install ./Trophy-<ver>-amd64.deb` or
  `sudo dnf install ./Trophy-<ver>-x86_64.rpm` (local file install, not a repo
  upgrade), with a copy button.
- Optionally open the downloaded file's folder; do not run privileged commands
  silently.

### Portable prefix / tar.xz — manual (phase 1)

- Notify + link + download. Offer a "Extract and replace" helper that stages
  the new tree beside the current prefix and swaps it on next launch, but default
  to manual instructions to avoid clobbering user data. Keep it conservative.

### Unknown — never auto-update; link only.

## 4. UI changes

- InfoPage: show install kind ("AppImage", "Debian package", "RPM package",
  "Portable install") and, when an update is available, an action button whose
  label/behaviour depends on kind ("Update now" vs "Download update" vs
  "Open release page").
- Add a download progress indicator.
- Settings: optionally expose install kind (read-only) for support.

## 5. CI / packaging changes

- Keep asset names stable (already normalized in CI).
- Ensure `SHA256SUMS` covers the AppImage and all packages (already done).
- Optional: embed AppImage update information (`--updateinformation`) for
  compatibility with third-party AppImageUpdate tools. Not required for the
  full-download approach.
- No change to native package build; guidance relies on existing artifacts.

## 6. Testing

Force a synthetic update without a network or a real release by launching with
`TROPHY_FORCE_FAKE_UPDATE=1` (optionally `TROPHY_FAKE_UPDATE_VERSION=99.0.0`).
The check then reports a newer version and a matching asset URL so the install
UI can be exercised end to end.

- Unit tests for `InstallInfo::detect()` using mocked env/paths.
- Unit tests for asset selection in `UpdateChecker`.
- Integration test for helper-script generation (no real replacement).
- Existing AppImage smoke test remains the validation for the bundle.

## 7. Rollout

1. Phase 1 (low risk): detection + guided messaging for all kinds. Ship first.
2. Phase 2: AppImage automatic replacement.
3. Phase 3: optional portable-prefix staging helper.

## Constraints / cautions

- The repository is private until release; update checks 404 today. Gate any
  "update available" UI on a valid, parsed release tag.
- Never replace a package-manager-owned binary; never run privileged commands
  without explicit user action.
- Verify checksums before any replacement.
- Do not update while recording; disable the action during capture.
- Anti-cheat/compatibility: updating the binary does not migrate enrolled
  reporters automatically; existing enrollment paths remain valid.
