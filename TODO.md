# Trophy - Game recording and clipping - TODO

## UI/UX Improvements
- [ ] Improve UI layout and polish
- [ ] Fix navigation rail icons (use text/Unicode icons instead of theme provider)
- [ ] Clean up debug fprintf statements from main.cpp and Application.cpp

## Core Recording Functionality

### Video Capture
- [ ] Implement X11 window capture functionality
  - Use MIT-SHM for efficient capture
  - Handle window focus detection
  - Support configurable frame rate
- [ ] Implement Wayland portal capture functionality
  - Use XDG Desktop Portal
  - Handle portal dialogs
  - Support monitor/window selection

### Audio Capture
- [ ] Implement PipeWire audio capture
  - Multi-track support (game, voice chat, microphone)
  - Device enumeration
  - Volume control and metering
- [ ] Implement PulseAudio capture fallback
  - Same multi-track support as PipeWire
  - Device enumeration

### Encoding
- [ ] Implement FFmpeg video encoding
  - NVENC support (NVIDIA)
  - VAAPI support (AMD/Intel)
  - x264 software fallback
  - Configurable bitrate, resolution, FPS
- [ ] Implement audio encoding
  - AAC encoding
  - Opus encoding (optional)
  - Multi-track audio support

### Muxing
- [ ] Implement video muxing
  - MKV container support
  - MP4 container support
  - Multi-track audio muxing
  - Metadata writing

## Testing & Integration
- [ ] Test actual recording functionality end-to-end
- [ ] Test storage limit enforcement and auto-cleanup
- [ ] Test game detection on various games
- [ ] Test on both X11 and Wayland
- [ ] Test with different GPU vendors (NVIDIA, AMD, Intel)

## Future Enhancements
- [ ] Keybinds for recording control (F9 for start/stop)
- [ ] Instant replay clipping feature
- [ ] System tray icon for background operation
- [ ] Performance overlay (FPS, recording indicator)
- [ ] Encoding presets (quality/performance balance)

## Documentation
- [ ] Add screenshots to README
- [ ] Write user guide
- [ ] Document build instructions for different distros
- [ ] Add troubleshooting section

## Completed ✓
- [x] Project structure and CMake configuration
- [x] Qt6/QML UI framework
- [x] Dark theme implementation
- [x] Settings management
- [x] Storage management
- [x] Recording library (database)
- [x] Notification service
- [x] Game detection framework
- [x] UI compilation and basic functionality
