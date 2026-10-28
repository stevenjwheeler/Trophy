# Standalone architecture-specific reporters

This builds the **existing** Vulkan layer and GL shim without Qt, FFmpeg, a
Vulkan runtime library, or the recorder application. The Vulkan loader supplies
downstream entry points; headers alone are sufficient to build the layer.
The socket path/protocol, opt-out and hook surface are unchanged.

Native build (C/C++ compiler, CMake, Vulkan headers and matching GoogleTest):

```sh
cmake -S reporters -B /tmp/opencode/reporters64 -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/opencode/reporters64 -j2
ctest --test-dir /tmp/opencode/reporters64 --output-on-failure
```

For **i386 ELF32** on x86 Linux, supply multilib libc/libstdc++ and a 32-bit
GoogleTest build. Do not accidentally link the host's 64-bit GoogleTest:

```sh
cmake -S /path/to/googletest -B /tmp/opencode/gtest32-build \
    -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 \
    -DCMAKE_INSTALL_PREFIX=/tmp/opencode/gtest32 -DBUILD_GMOCK=OFF
cmake --build /tmp/opencode/gtest32-build -j2
cmake --install /tmp/opencode/gtest32-build
cmake -S reporters -B /tmp/opencode/reporters32 \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/reporters/i686-linux.cmake" \
    -DCMAKE_PREFIX_PATH=/tmp/opencode/gtest32 -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/opencode/reporters32 -j2
ctest --test-dir /tmp/opencode/reporters32 --output-on-failure
```

The kernel must support executing i386 binaries to run the tests. Pass
`-DBUILD_TESTING=OFF` to build reporters alone; this is not runtime verification.
The reused fixture suites exercise actual reporter code with fake downstream/
socket operations, including forwarding, reconnects, ownership and frozen wire
messages. ELF/manifest checks verify the actual output architecture and absence
of unwanted Vulkan-loader/Qt/FFmpeg dependencies. They do not run a graphics
driver, a real Vulkan loader, Steam/Proton or an anti-cheat-protected game.

The generated `trophy_layer.json` sits beside its matching layer, uses a relative
library path, declares `library_arch`, and uses manifest version 1.2.1 (where that
field was introduced). Optional installation to a **scratch prefix** places
these files under `lib/trophy/reporters32` or `reporters64`, independent of the
distribution's system linker layout. Building/installing alone never enrolls
anything in loader directories or writes global preload settings.

To include a verified i386 pair in a normal x86_64 app install/AppImage, configure
the main build with `-DTROPHY_REPORTERS32_DIR=/path/to/reporters32-build`. It checks
both ELF headers, copies them into the dev layout and installs them under
`lib/trophy/reporters32`. Ordinary builds still require no 32-bit toolchain.
Native packages built with this option also need compatible i386 runtime
dependencies; don't suppress the package manager's dependency checks.

**The existing global reporter controls remain the controls. There is no
per-game OpenGL option or per-game configuration requirement.** After installing
a companion-enabled build, explicitly re-enabling Vulkan publishes both entries
in one atomic manifest; explicitly re-enabling GL publishes one durable global
preload path that the glibc loader resolves separately for ELF32/ELF64 programs.
Existing legacy enrollments stay legacy during startup refresh. GL changes
still need a fresh login; already-running programs retain their environment.

The GL path uses `$$PLATFORM` in environment.d so the environment generator hands
a literal `$PLATFORM` to the loader. It selects immutable architecture-specific
copies under the user's reporter store, without distro-dependent `$LIB` alias
collisions or loading both absolute paths into every process. Unknown loader
platforms/runtimes are not established by the controlled x86 glibc tests. The
Vulkan pair requires a loader supporting manifest 1.2.1; legacy single-library
enrollments keep manifest 1.0.0 compatibility until explicit re-enrollment.

Architecture-aware enrollment must preserve explicit consent,
durable AppImage helper paths, foreign preload entries and opt-outs, and avoid
feeding an ELF64 library to an ELF32 process (or the reverse). Per-process
loader/preload trials require a known-compatible test program and explicit
consent. Reporters are not certified safe for anti-cheat; the disabled flag
does not prevent a preloaded library from being mapped.
