# Trophy - Game recording and clipping

A modern, performant game session recording and clipping application for Linux with hardware-accelerated encoding, multi-track audio capture, and intelligent game detection.

Trophy is designed to have minimal performance impact while still capturing high definition game sessions and clips.

![License](https://img.shields.io/badge/license-GPL--3.0-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-green.svg)
![Qt](https://img.shields.io/badge/Qt-6.5+-41cd52.svg)

## Features

### Recording
- **Full sessions or clips** - Records full sessions from open to close, or custom clip lengths using a hotkey
- **Hardware-Accelerated Encoding** - Supports NVIDIA NVENC and AMD/Intel VA-API for minimal performance impact
- **Software Fallback** - x264 encoding when hardware acceleration isn't available
- **Multiple Container Formats** - MKV (crash-resilient) and MP4 output options
- **Configurable Quality** - Adjustable bitrate, resolution, frame rate, and encoder presets

### Audio
- **Multi-Track Recording** - Separate tracks for game audio, voice chat, and microphone
- **PipeWire Integration** - Modern Linux audio with application-specific capture
- **PulseAudio Fallback** - Broad compatibility with older systems
- **Real-Time Level Meters** - Visual feedback for all audio sources

### Game Detection
- **Automatic Detection** - Identifies games using Steam integration, window patterns, and process heuristics
- **Steam & Proton Support** - Detects native and Windows games running through Proton/Wine
- **Whitelist/Blacklist** - User-defined rules for custom game recognition
- **Auto-Record Option** - Automatically start recording when a game launches

### Storage Management
- **Size Limits** - Configure maximum storage usage for recordings
- **Auto-Cleanup** - Automatically delete oldest recordings when limit is reached
- **Organized Library** - Browse recordings with thumbnails and metadata

### User Interface
- **Modern QML Design** - Slick, dark-themed interface built with Qt6
- **Dashboard** - Quick overview of current game, audio levels, and storage
- **Video Browser** - Grid view of recordings with playback support
- **System Notifications** - Desktop notifications for recording start/stop

### Planned Features
- Global keybinds for recording control
- Instant replay / clip last N seconds
- Stream integration (Twitch, YouTube)
- Post-recording editing (trim, cut)

## Screenshots

*Coming soon*

## Requirements

### Runtime Dependencies
- Qt 6.5+
- FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample)
- PipeWire (recommended) or PulseAudio
- For NVIDIA: nvidia-utils (NVENC support)
- For AMD/Intel: libva (VA-API support)

### Build Dependencies
- CMake 3.21+
- C++20 compiler (GCC 11+ or Clang 13+)
- Qt6 development packages
- pkg-config

## Installation

### Arch Linux

```bash
# Install dependencies
sudo pacman -S cmake qt6-base qt6-declarative qt6-multimedia \
    ffmpeg pipewire libpulse libva pkgconf

# Clone and build
git clone https://github.com/your-username/trophy.git
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

## Architecture

Trophy is built with a modular architecture:

```
┌─────────────────────────────────────────────────────────┐
│                    QML User Interface                    │
├─────────────────────────────────────────────────────────┤
│                   Qt/C++ Manager Layer                   │
│  RecordingManager │ SettingsManager │ GameDetection     │
├─────────────────────────────────────────────────────────┤
│                    Service Layer                         │
│  IVideoCaptureSource │ IEncoder │ IAudioSource │ IMuxer │
├─────────────────────────────────────────────────────────┤
│                 Platform Implementations                 │
│  X11/Wayland Capture │ NVENC/VAAPI/x264 │ PipeWire/Pulse│
└─────────────────────────────────────────────────────────┘
```

## Contributing

Contributions are welcome! Please feel free to submit issues and pull requests.

### Development Setup

```bash
# Build with debug symbols
cmake .. -DCMAKE_BUILD_TYPE=Debug

# Run with verbose logging
QT_LOGGING_RULES="*.debug=true" ./trophy

### Running Tests

Trophy includes a comprehensive suite of unit tests using GoogleTest.

```bash
# Build with testing enabled
cmake -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure
```
```

## License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- [FFmpeg](https://ffmpeg.org/) - Multimedia framework
- [Qt](https://www.qt.io/) - Cross-platform application framework
- [PipeWire](https://pipewire.org/) - Modern Linux multimedia server

---

## Disclaimer

This project has been developed with AI assistance (Claude by Anthropic). Any and all generated code has been reviewed and verified for correctness, security, and adherence to best practices. The architecture, design decisions, and implementation details have been validated by the project maintainer.
