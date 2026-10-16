# Packaging status

Native prefix installs and the CI-produced AppImage are the supported
distribution paths. Build outside the repository's CloudDrive mount.

## Flatpak — experimental

See [the feature-parity plan](FLATPAK_PLAN.md) for the proposed architecture,
implementation phases and release gates.

`io.github.stevenjwheeler.Trophy.yml` is a development manifest, not a
feature-equivalent release package. Manual capture through the desktop
portal is the intended evaluation path; it still needs a packaged live
smoke test.

Known unsupported integrations:

- Host Vulkan/GL reporters cannot reach the listener in Flatpak's private
  `/tmp`. The reporter socket protocol remains unchanged.
- Vulkan manifests, GL enrollment and KWin script installation currently
  target sandbox paths, not the host integration locations. Do not enable
  these optional modules expecting host game detection to work.
- Hyprland/Sway tools and host compositor sockets are not bundled/exposed.
- Login autostart needs a Background portal implementation or a validated
  host-config bridge. The generated `flatpak run` command is host-valid,
  but an entry written to sandbox config is not a host autostart entry.
- Hardware encoders and NVIDIA runtime extensions need packaged testing.

Do not attempt to resolve these limitations by granting blanket host
filesystem access or replacing the frozen reporter protocol. A supported
host-integration design and real-session tests are required before promoting
the manifest out of experimental status.

## Verification

An installed tree needs the executable, `libTrophy.so`, both reporter
libraries, desktop/icon metadata, and the KWin script. Check `readelf -d`
for the executable's prefix-relative RUNPATH and `ldd` with
`LD_LIBRARY_PATH` unset. Native/AppImage AppStream metadata must launch
`trophy.desktop`; Flatpak metadata launches its renamed app-id desktop file.

Run a non-capture AppImage smoke test with:

```sh
python3 packaging/smoke_appimage.py /path/to/Trophy.AppImage --log /tmp/trophy-smoke.log
```

The harness uses temporary HOME/XDG directories, a private D-Bus with service
activation disabled, offscreen/software rendering, and disabled optional
integrations. It checks GUI initialization, QML errors, bundled platform/media
plugins and reporters, and graceful shutdown. It does **not** verify real
Wayland rendering, portal capture, hardware encoding or host autostart.

AppImages use `packaging/AppRun` to execute the actual `usr/bin/trophy` binary
with bundle-first library lookup; do not replace it with a root-level binary
symlink. Include Wayland platform, shell and graphics-integration plugins
explicitly: an offscreen launch alone cannot establish their presence.

When opted in, portable-bundle reporter libraries are copied atomically to
content-addressed files in `$XDG_DATA_HOME/trophy/reporters` (normally
`~/.local/share/trophy/reporters`). Enrollment must never reference a temporary
AppImage mount. Old helper versions remain available for already-running games.
Previously enrolled GL configurations refresh on startup; session activation
still follows the existing next-login behavior. Disabled modules are not
installed by lookup/status checks. Host/Steam-runtime ABI compatibility and
32-bit reporter support are not established by the smoke test.

## Static reporter ABI checks

### Reporter scope and anti-cheat caution

Neither reporter is certified compatible with anti-cheat systems. No hooking
change, allowlist or disabled-reporting flag can guarantee that a loaded
library will be accepted. Keep optional reporters off for protected games
unless the game's policy explicitly permits them. Trophy does not inspect,
hide from, or bypass anti-cheat software.

The current OpenGL toggle writes **session-wide** `LD_PRELOAD` configuration:
it loads the shim into unrelated dynamically linked programs, not just games.
Turning it off affects future login environments; already-running launchers
and their children can retain the old preload. Log out and back in after
disabling. `DISABLE_TROPHY_DETECTION=1` stops shim reporting, but cannot prevent
the dynamic loader from loading a library named in `LD_PRELOAD`.

Prefer leaving global enrollment off and, for a known-compatible native game,
scoping preload to its launch command (example for the native `~/.local`
64-bit install):

```sh
env LD_PRELOAD="$HOME/.local/lib/trophy/libtrophy_gl_shim.so${LD_PRELOAD:+:$LD_PRELOAD}" /absolute/path/to/game
```

This also affects that game's child processes. It is not an anti-cheat safety
guarantee or a validated Steam/Proton/32-bit recipe. AppImage mount paths must
not be used as persistent launch settings. A future per-game enrollment UI
should replace the broad session toggle, with explicit migration/consent,
durable paths, loader-path validation and architecture/runtime checks.

Shim coverage deliberately remains limited to exported GLX/EGL/SDL swap
functions. Games resolving entry points privately may bypass it; do not add
`dlsym`/get-proc-address interception simply to increase coverage without a
separate compatibility review. Remaining reporter verification includes real
loader/driver execution, Vulkan fork/unload and allocation/thread-failure paths,
and host/Steam ABI tests. Fake native-operation tests do not establish these.

Run without loading the reporters or starting a game:

```sh
python3 packaging/check_reporter_abi.py /path/libtrophy_layer.so /path/libtrophy_gl_shim.so
python3 packaging/check_reporter_abi.py --sysroot /path/materialized-sysroot /path/libtrophy_layer.so
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_ReporterAbi.py
```

The Linux checker uses `readelf` and host `ldconfig -p` (or libraries inside
the supplied sysroot). It checks ELF class/machine, dependency closure,
required version definitions and strong versioned symbol exports. Inspection
is bounded; absent/ambiguous providers fail rather than silently selecting
another architecture. A sysroot never falls back to host providers. Exit codes
are 0 for a static pass, 1 for detected mismatches and 2 for an inconclusive
inspection/tool error. Reports are JSON; redirect generated evidence under
`/tmp/opencode`. This does not simulate RPATH/RUNPATH, environment overrides,
unversioned/weak symbol resolution, driver loading or loader interposition.

The 2026-10-13 local x86_64 helpers passed against materialized host providers.
Both require `GLIBC_2.34`; the Vulkan helper also requires
`GLIBCXX_3.4.32`. Installed Sniper `3.0.20260805.254768` mtree metadata lists
`libc-2.31.so` and `libstdc++.so.6.0.28`: an **older-base ABI advisory**, not
a verified Steam/Proton failure. Its content-addressed `files/` tree is not
a materialized sysroot, and pressure-vessel can select override providers.
Metadata names alone do not prove exported symbols or actual provider
selection. Native host results do not establish Steam-runtime compatibility.

Isolated trivial C and C++ `-m32` compile/link probes succeeded locally; no
probe binary was run. The current helpers are ELF64/x86_64 and cannot serve
32-bit processes. Those probes do not establish 32-bit Vulkan dependencies,
reporter builds, enrollment or game compatibility. No live game, host install,
SDK installation or compiler/linker ABI workaround was used for these checks.
