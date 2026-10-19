<div align="center">
  <img src="resources/icons/trophy.svg" width="96" height="96" />
  <h1>Trophy</h1>
  <p><strong>Game recorder and clipping for Linux</strong></p>
</div>

Trophy is a modern, performant game session recording and clipping application for Linux, built exclusively for Wayland. It features hardware-accelerated encoding, multi-track audio capture, and intelligent game detection.

> [!WARNING]
> **Native X11 sessions are not supported.** Trophy is designed specifically for Wayland. While XWayland applications can be recorded perfectly fine, the application must be run within a Wayland compositor (like KWin/KDE, GNOME Shell, or Hyprland).

[![Latest Release](https://img.shields.io/github/v/release/OasisLab-Software/Trophy?color=blue&label=Latest%20Release)](https://github.com/OasisLab-Software/Trophy/releases/latest)
![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-green.svg)
![Qt](https://img.shields.io/badge/Qt-6.10+-41cd52.svg)

## Quick Start (AppImage)

The easiest way to use Trophy on any Linux distribution (including Arch/CachyOS, Fedora, and Ubuntu) is to download the prebuilt AppImage.

1. **Download:** Grab the latest `Trophy-<version>-x86_64.AppImage` from the [Releases Page](https://github.com/OasisLab-Software/Trophy/releases/latest).
2. **Make it executable:** `chmod +x Trophy-*.AppImage`

## Features

### Recording
- **Full sessions or clips** - Records full sessions from open to close, or custom clip lengths using a hotkey
- **Hardware-Accelerated Encoding** - Supports NVIDIA NVENC and AMD/Intel VA-API for minimal performance impact
- **Multiple encoder options** - AV1, HEVC, and H.264 hardware encoding
- **Software Fallback** - x264 encoding when hardware acceleration isn't available
- **DMA-BUF Support** - Uses compatible DMA-BUF paths when the compositor and encoder support them; falls back to CPU-backed capture otherwise
- **Wayland Focused** - Supports recording native Wayland and XWayland windows via PipeWire
- **Resilient Recording, Easy Sharing** - Record in crash-resilient MKV, then use Export to share for a compatible MP4 copy
- **Configurable Quality** - Adjustable bitrate, resolution, frame rate, and encoder presets

### Audio
- **Multi-Track Recording** - Separate tracks for game audio, voice chat, and microphone
- **PipeWire Integration** - Modern Linux audio with application-specific capture
- **Multi-Stream Applications** - Combines matching playback streams from the same application/client, including games with a silent first stream
- **Desktop Safety Net** - Uses the desktop mix while game audio is unavailable; separate voice capture waits until game-specific capture binds to avoid doubling
- **Real-Time Level Meters** - Visual feedback for all audio sources

### Game Detection
- **Vulkan Detection Layer** - High-fidelity detection using a custom Vulkan implicit layer
- **Reporter-Based Detection** - Vulkan/OpenGL events combine with Steam identity and focused/fullscreen window evidence; helper apps are filtered
- **Window Helper** - Required compositor integration is installed or repaired only with your explicit consent
- **Guided Setup** - Choose the detection layer (Vulkan and/or OpenGL shim) during setup; Trophy registers it for you and it can be changed anytime in Settings
- **Steam & Proton Support** - Recognizes supported Steam games and resolves native/Wine identities conservatively
- **Whitelist/Blacklist** - User-defined rules for custom game recognition
- **Auto-Record Option** - Automatically start recording when a game launches

Vulkan and OpenGL reporters are opt-in. Session-wide OpenGL preload changes
require a fresh login environment; anti-cheat compatibility is not guaranteed.

### Storage Management
- **Size Limits** - Configure maximum storage usage for recordings
- **Auto-Cleanup** - Automatically delete oldest recordings when limit is reached
- **Organized Library** - Grid/list layouts, search, capture-type filters, sorting, renaming and bulk deletion
- **Animated Hover Previews** - Silent cached previews in both layouts, rendered as background-decoded images rather than starting a video player for each item

### User Interface
- **Modern QML Design** - Slick, dark-themed interface built with Qt6
- **Dashboard** - Quick overview of recordings, current game, audio levels, current settings, and storage
- **System Notifications** - Desktop notifications for recording start/stop
- **Built-In Player and Clip Editor** - Play recordings with filmstrips, per-track waveforms and audio mutes; trim clips with keyframe-aligned stream-copy cuts
- **Appearance Settings** - Default, Charcoal and AMOLED themes with configurable accent colors

### Planned Features
- Stream integration (Twitch, YouTube)

### Keyboard Shortcuts

Trophy asks to bind its recording toggle and Save Clip shortcuts during
setup (the wizard's Keyboard Shortcuts step) through the system Global
Shortcuts portal. The portal suggests **F9** (toggle recording) and
**F10** (clip / bookmark) as defaults, but the keys are yours to assign —
accept the dialog to bind them, and remap them anytime in
Settings → Keyboard Shortcuts or in the portal's own shortcut dialog.
The **same clip key** adds a bookmark during a full recording, and saves a replay
clip in DVR mode. There is no separate bookmark shortcut. `trophy --save-clip`
uses the same context-sensitive behavior, even when DVR is disabled while recording.
On KDE and Hyprland they work globally, even in-game.

Where the portal can't reach (GNOME, Sway), bind Trophy's commands
with your compositor's native key system instead:

```ini
# Sway (~/.config/sway/config)
bindsym $mod+F9 exec trophy --toggle-record
bindsym $mod+F10 exec trophy --save-clip

# Hyprland (~/.config/hypr/hyprland.conf)
bind = $mainMod, F9, exec, trophy --toggle-record
bind = $mainMod, F10, exec, trophy --save-clip
```

GNOME: Settings → Keyboard → View and Customize Shortcuts →
Custom Shortcuts, pointing at the same two commands. The commands
talk to the already-running Trophy over a user-private socket
(`$XDG_RUNTIME_DIR/trophy-control.sock`); if Trophy isn't running,
they just launch it.

### Keeping and finding moments

- Click a capture's **padlock** to protect it from automatic storage cleanup.
  The Library filter includes **Protected only**. Explicit deletion still works.
  If protected/current captures prevent cleanup, increase the limit or manually
  unprotect/delete captures; Trophy never removes protection to make room.
- During a full recording, use **Bookmark moment** or your clip key (also
  `trophy --save-clip`; the explicit `--add-bookmark` command remains available).
  The player shows yellow clickable markers across video/audio tracks and a
  bookmark list with Jump, **Clip ±15 s**, and Remove. Clip selects an editable
  range; it does not immediately save a clip. Normal keyframe snapping applies.
  Bookmarks do not automatically protect a capture from cleanup.
- The Dashboard shows live source connection/mute state, desktop audio fallback,
  approximate replay-buffer fill and a warning for sustained video-queue drops.
  Connection status does not prove audible content; check the audio meters.
  During desktop fallback the primary input is **System** and the redundant
  Voice meter is hidden; both revert when game-specific audio binds.

New recordings also save audio-source history in `.trophy/`. The player labels
the primary lane **System**, **Game**, or **Game / System**, and hides the Voice
lane only when fallback was used and Voice never delivered separate audio.
Mic retains its original stream index when Voice is hidden. Clips inherit
conservative source history (mixed-source clips may retain an unused lane).
Older files without history, or files changed externally after that history
was saved, retain their existing lanes rather than guessing from silence.

Protection and bookmarks are stored atomically in the library's **`.trophy/`**
directory, separately from regenerable caches. Keep that directory when moving
or backing up a library. In-app rename/delete follows the metadata; external
file renames and video-only exports do not carry it. Unreadable annotation files
are treated as protected rather than risking automatic deletion.

### Share-ready MP4 export

Open a capture in the player and choose **Share MP4**, or select captures in
the Library and use **Export → Share-ready MP4**. Choose original/1080p/720p
resolution, a quality or approximate size preset, and which audio tracks to
include. Selected tracks are averaged into **one stereo AAC track**, with their
timing offsets preserved. Selecting no audio creates a silent video. A source
opened from the player starts with its muted audio lanes unchecked; review the
track choices before exporting. A source
track that already contains desktop audio may also include Voice/Mic; selecting
those again can double their sound.

For multiple captures, quality and destination apply to the whole batch. Expand
**Audio** and use the capture picker to review each file's independent track
choices. **Export N MP4s** queues one copy per capture, processed one at a time
in the background; each queued export can be cancelled independently.

Export re-encodes to H.264/YUV420P MP4 (up to 60fps), never upscales, and enables
fast-start playback. It exports the **whole file**: snip a range first if needed.
Size targets are approximate, not guaranteed upload limits. HDR-to-SDR conversion
is not supported. The FFmpeg libraries need libx264 and AAC encoder support;
there is no additional `ffmpeg` executable requirement.

Choose an export folder **outside the managed recording library**, so cleanup
cannot delete your share copy or original to make room. Original files are
read-only during export and held against in-app rename/deletion/cleanup while
in use. Progress, cancellation and **Run in background** are available; a
background progress panel can reopen the dialog. Existing files are never
overwritten and incomplete exports are removed. **Export → Copy original files**
still provides the original, unconverted single/bulk copy operation.

### Undo deletion

Manual deletion removes captures from the library view immediately and shows a
toast with **Undo** and a filling progress bar. You have **10 seconds** before
permanent deletion starts. Each batch has its own toast; one deletion never
silently replaces another batch's undo opportunity.

During the grace period, videos and their protection/bookmark/cache files stay
in place and still occupy disk space. Undo restores the view without rewriting
anything. Quitting Trophy before expiry cancels pending deletion (hiding to the
tray does not quit it). Automatic storage cleanup is still permanent and does
not show Undo. If a capture changes externally during the countdown, its batch
is cancelled rather than deleting a replacement file.

### GNOME window helper

On GNOME Wayland, Trophy uses the bundled **Trophy Window Helper** Shell
extension for read-only window identity, focus, fullscreen and minimized state.
Choose **Install helper** in setup (or **Repair** in Settings) to copy it to
`$XDG_DATA_HOME/gnome-shell/extensions/trophy-monitor@trophy` (normally under
`~/.local/share`) and request enabling it. Ordinary startup never installs or
enables extensions. GNOME unsafe mode is **not** required.

GNOME may not discover a newly copied extension until you **log out and back
in**. Then enable **Trophy Window Helper** in GNOME Extensions if necessary and
reopen Trophy. Setup only reports Working after a valid window snapshot arrives;
an installed extension alone is not sufficient. If it remains unavailable, check
GNOME Extensions for unsupported-version/errors or disabled user extensions.
Updating an already loaded extension can also require logout/login.

The extension targets GNOME Shell 45–51's modern extension API. Automated tests
cover its snapshot logic and Trophy's private-D-Bus integration; real GNOME
session/capture verification is still pending. Screen capture remains separately
authorized through the ScreenCast portal.

## Screenshots
*Coming soon*

## Requirements

### Runtime Dependencies
- Qt 6.10+
- FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample)
- PipeWire (Required)
- xdg-desktop-portal and a suitable compositor-specific portal backend for capture; global shortcut support depends on that backend
- For NVIDIA: nvidia-utils (NVENC support)
- For AMD/Intel: libva (VA-API support)
- Vulkan loader (headers are required when building)

### Build Dependencies
- CMake 3.21+
- C++20 compiler (GCC 11+ or Clang 13+)
- Qt6 development packages
- Vulkan SDK or Headers
- pkg-config
- GoogleTest and Qt Test (tests are enabled by default; use `-DBUILD_TESTING=OFF` to omit them)

The FFmpeg command-line tool is used by optional media test fixtures, not by
production preview generation. Distro package versions must satisfy the Qt
requirements above.

## Building from source

```bash
# Install dependencies (Arch/CachyOS example)
sudo pacman -S cmake qt6-base qt6-declarative qt6-multimedia \
    ffmpeg pipewire libpulse libva pkgconf vulkan-headers vulkan-icd-loader \
    libdrm mesa gtest

# Clone and build
git clone https://github.com/OasisLab-Software/Trophy.git
cd Trophy
cmake -S . -B ../trophy-build -DCMAKE_BUILD_TYPE=Release
cmake --build ../trophy-build --parallel
cmake --install ../trophy-build --prefix "$HOME/.local"
```

The sibling build directory is just an example. If your sources live on a
cloud/FUSE mount, choose a local filesystem for the build directory. The install
includes `libTrophy.so` and detection reporters as well as `~/.local/bin/trophy`;
do not copy only the executable.

### Fedora

```bash
# Install dependencies
sudo dnf install cmake qt6-qtbase-devel qt6-qtdeclarative-devel \
    qt6-qtmultimedia-devel ffmpeg-devel pipewire-devel \
    pulseaudio-libs-devel libva-devel vulkan-headers vulkan-loader-devel \
    libdrm-devel mesa-libEGL-devel mesa-libgbm-devel gtest-devel

# Clone and build as above
```

### Ubuntu/Debian

```bash
# Install dependencies
sudo apt install cmake qt6-base-dev qt6-declarative-dev \
    qt6-multimedia-dev libavcodec-dev libavformat-dev \
    libavutil-dev libswscale-dev libswresample-dev \
    libpipewire-0.3-dev libpulse-dev libva-dev libvulkan-dev libgtest-dev \
    libdrm-dev libegl-dev libgbm-dev

# Clone and build as above
```

## Usage

### Basic Usage

1. Launch Trophy and complete setup, explicitly installing the window helper and choosing any optional detection reporters and shortcuts.
2. Start a supported game. When Trophy detects it, authorize window capture if prompted; saved approvals allow later automatic starts without another picker when restoration succeeds.
3. Click **Record** or press your assigned recording shortcut (suggested **F9**) to start recording.
4. Press the recording shortcut again or click **Stop** to end recording. Your clip/bookmark shortcut marks moments during a full recording; with DVR enabled and ready, it saves the trailing buffer instead.
5. Find recordings and clips in the **Home** library. Click an item to open the built-in player and clip editor.

### Hover previews and local caches

Hover over a grid card or list row to see a silent, short preview with a gentle
fade. Trophy prepares a small sample video alongside filmstrip and waveform
caches after saving a recording or clip. All live in the recording folder's
`.thumbnails` directory. Older captures prepare missing previews on first hover,
leaving the static thumbnail visible until ready.

A background worker turns the preview into frame images; hovering flips through
those pictures instead of opening a video player. A bounded memory cache reuses
recent frames. The sample is selected from a random section of the recording,
with a random starting offset within that cached sample on each hover. There is
a short 50 ms hover delay and a 180 ms fade-in.

These are disposable caches, not changes to your recordings. Recognized cache
files follow library renames and deletions and are regenerated when needed.

### Configuration

Access **Settings** to configure:

- **Video**: Encoder, bitrate, FPS, resolution, format
- **Audio**: Input devices, separate track recording, bitrate
- **Storage**: Recording location, size limit, auto-cleanup
- **Detection**: Auto-detect, auto-record, whitelist/blacklist
- **Appearance**: Theme, accent color and persisted library layout

### Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Recording toggle (suggested F9) | Start/Stop recording |
| Clip / Bookmark (suggested F10) | Save a DVR clip, or bookmark the active full recording |
| Ctrl+Q | Quit application |

The recording-toggle and Save-Clip keys are user-assigned through the
Global Shortcuts portal; F9/F10 are only the suggested defaults.

## Checksum verification

To ensure the integrity and authenticity of Trophy, we provide cryptographic verification for all official releases.

### Verify File Integrity (SHA-256)
Every release includes a `SHA256SUMS` file. You can verify that your AppImage was not corrupted or tampered with during download by running:

```bash
# 1. Download both the AppImage and SHA256SUMS to the same folder
# 2. Run the check
sha256sum -c SHA256SUMS
```
Expected Output: `Trophy-X.X.X-x86_64.AppImage: OK`

## Architecture

Trophy is built with a modular architecture:

```
┌───────────────────────────────────────────────────────────┐
│                    QML User Interface                     │
├───────────────────────────────────────────────────────────┤
│                   Qt/C++ Manager Layer                    │
│  RecordingManager │ SettingsManager │ GameDetection       │
├───────────────────────────────────────────────────────────┤
│                    Service Layer                          │
│  IVideoCaptureSource │ IEncoder │ IAudioSource │ IMuxer   │
├───────────────────────────────────────────────────────────┤
│                 Platform Implementations                  │
│  Wayland Capture   │  NVENC/VAAPI/x264  │   PipeWire      │
└───────────────────────────────────────────────────────────┘
```

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

### Development Setup

```bash
# Build with debug symbols
cmake -S . -B ../trophy-build -DCMAKE_BUILD_TYPE=Debug
cmake --build ../trophy-build --parallel

# Run with verbose logging
QT_LOGGING_RULES="*.debug=true" ../trophy-build/trophy
```

### Running Tests
Trophy includes a comprehensive suite of unit tests using GoogleTest.

```bash
# Build with testing enabled
cmake -S . -B ../trophy-build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build ../trophy-build --parallel

# Run with isolated settings/runtime paths
test_root=$(mktemp -d)
mkdir -m 700 "$test_root/runtime"
XDG_CONFIG_HOME="$test_root/config" XDG_RUNTIME_DIR="$test_root/runtime" \
    QT_QPA_PLATFORM=offscreen \
    ctest --test-dir ../trophy-build --output-on-failure --timeout 300
```

Quit running Trophy instances before socket tests: the reporter protocol uses
the fixed `/tmp/trophy-detection.sock`, which XDG isolation alone does not move.
Hardware/optional-codec tests skip when their requirements are unavailable;
automated tests do not replace live compositor and GPU verification.

## License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- [FFmpeg](https://ffmpeg.org/) - Multimedia framework
- [Qt](https://www.qt.io/) - Cross-platform application framework
- [PipeWire](https://pipewire.org/) - Modern Linux multimedia server

---

## Disclaimer

This project has been developed with AI assistance (Claude by Anthropic). Any and all generated code has been reviewed and verified for correctness, security, and adherence to best practices. Any and all architecture, design decisions, and implementation details have been validated by the project maintainer.
