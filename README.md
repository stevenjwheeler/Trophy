<div align="center">
  <img src="resources/icons/trophy.svg" width="96" height="96" />
  <h1>Trophy</h1>
  <p><strong>Game recorder and clipping for Linux</strong></p>
</div>

Trophy is a modern, performant game session recording and clipping application for Linux, built exclusively for Wayland. It features hardware-accelerated encoding, multi-track audio capture, and intelligent game detection.

> [!WARNING]
> **Native X11 sessions are not supported.** Trophy is designed specifically for Wayland. While XWayland applications can be recorded perfectly fine, the application must be run within a Wayland compositor (like KWin/KDE, GNOME Shell, or Hyprland).

[![Latest Release](https://img.shields.io/github/v/release/stevenjwheeler/trophy?color=blue&label=Latest%20Release)](https://github.com/stevenjwheeler/trophy/releases/latest)
![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-green.svg)
![Qt](https://img.shields.io/badge/Qt-6.10+-41cd52.svg)

## Quick Start (AppImage)

The easiest way to use Trophy on any Linux distribution (including Arch/CachyOS, Fedora, and Ubuntu) is to download the prebuilt AppImage.

1. **Download:** Grab the latest `Trophy-x86_64.AppImage` from the [Releases Page](https://github.com/your-username/trophy/releases/latest).
2. **Make it executable:** `chmod +x Trophy-*.AppImage`

## Features

### Recording
- **Full sessions or clips** - Records full sessions from open to close, or custom clip lengths using a hotkey
- **Hardware-Accelerated Encoding** - Supports NVIDIA NVENC and AMD/Intel VA-API for minimal performance impact
- **Multiple encoder options** - AV1, HEVC, and H.264 hardware encoding
- **Software Fallback** - x264 encoding when hardware acceleration isn't available
- **Zero-Copy Capture** - DMA-BUF support for minimal performance impact
- **Wayland Focused** - Supports recording native Wayland and XWayland windows via PipeWire
- **Multiple Container Formats** - MKV (crash-resilient) and MP4 output options
- **Configurable Quality** - Adjustable bitrate, resolution, frame rate, and encoder presets

### Audio
- **Multi-Track Recording** - Separate tracks for game audio, voice chat, and microphone
- **PipeWire Integration** - Modern Linux audio with application-specific capture
- **Real-Time Level Meters** - Visual feedback for all audio sources

### Game Detection
- **Vulkan Detection Layer** - High-fidelity detection using a custom Vulkan implicit layer
- **Instant Response** - Games are detected the moment `vkCreateInstance` is called
- **Zero-Config** - Trophy automatically registers its detection layer on first launch
- **Steam & Proton Support** - Definitively identifies Windows games running through Proton
- **Whitelist/Blacklist** - User-defined rules for custom game recognition
- **Auto-Record Option** - Automatically start recording when a game launches

### Storage Management
- **Size Limits** - Configure maximum storage usage for recordings
- **Auto-Cleanup** - Automatically delete oldest recordings when limit is reached
- **Organized Library** - Browse recordings with thumbnails and metadata

### User Interface
- **Modern QML Design** - Slick, dark-themed interface built with Qt6
- **Dashboard** - Quick overview of recordings, current game, audio levels, current settings, and storage
- **System Notifications** - Desktop notifications for recording start/stop

### Planned Features
- Stream integration (Twitch, YouTube)
- Post-recording editing (trim, cut)
- Integrated video player
- Detection for OpenGL and other games

### Keyboard Shortcuts

F9 toggles recording and F10 saves a clip. On KDE and Hyprland these
work globally (even in-game) via the system shortcuts portal — accept
the approval dialog on first launch, and remap them anytime in
Settings → Keyboard Shortcuts.

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

## Screenshots
*Coming soon*

## Requirements

### Runtime Dependencies
- Qt 6.10+
- FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample)
- PipeWire (Required)
- For NVIDIA: nvidia-utils (NVENC support)
- For AMD/Intel: libva (VA-API support)
- Vulkan Loader & Headers (for game detection)

### Build Dependencies
- CMake 3.21+
- C++20 compiler (GCC 11+ or Clang 13+)
- Qt6 development packages
- Vulkan SDK or Headers
- pkg-config

## Building from source

### Arch Linux

```bash
# Install dependencies
sudo pacman -S cmake qt6-base qt6-declarative qt6-multimedia \
    ffmpeg pipewire libpulse libva pkgconf

# Clone and build
git clone https://github.com/stevenjwheeler/trophy.git
cd trophy
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

### Fedora

```bash
# Install dependencies
sudo dnf install cmake qt6-qtbase-devel qt6-qtdeclarative-devel \
    qt6-qtmultimedia-devel ffmpeg-devel pipewire-devel \
    pulseaudio-libs-devel libva-devel

# Clone and build as above
```

### Ubuntu/Debian

```bash
# Install dependencies
sudo apt install cmake qt6-base-dev qt6-declarative-dev \
    qt6-multimedia-dev libavcodec-dev libavformat-dev \
    libavutil-dev libswscale-dev libswresample-dev \
    libpipewire-0.3-dev libpulse-dev libva-dev

# Clone and build as above
```

## Usage

### Basic Usage

1. Launch Trophy
2. Start your game - it will be automatically detected
3. Click **Record** or press **F9** to start recording
4. Press **F9** again or click **Stop** to end recording
5. Find your recordings in the **Recordings** tab

### Configuration

Access **Settings** to configure:

- **Video**: Encoder, bitrate, FPS, resolution, format
- **Audio**: Input devices, separate track recording, bitrate
- **Storage**: Recording location, size limit, auto-cleanup
- **Detection**: Auto-detect, auto-record, whitelist/blacklist

### Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| F9 | Start/Stop recording |
| Ctrl+Q | Quit application |

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
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Run with verbose logging
QT_LOGGING_RULES="*.debug=true" ./trophy
```

### Running Tests
Trophy includes a comprehensive suite of unit tests using GoogleTest.

```bash
# Build with testing enabled
cmake -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure
```

## License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- [FFmpeg](https://ffmpeg.org/) - Multimedia framework
- [Qt](https://www.qt.io/) - Cross-platform application framework
- [PipeWire](https://pipewire.org/) - Modern Linux multimedia server

---

## Disclaimer

This project has been developed with AI assistance (Claude by Anthropic). Any and all generated code has been reviewed and verified for correctness, security, and adherence to best practices. Any and all architecture, design decisions, and implementation details have been validated by the project maintainer.
