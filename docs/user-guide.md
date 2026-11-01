# User guide

[Back to README](../README.md) · [Setup and compatibility](getting-started.md)

## Full recordings and replay clips

Use **Record** or your recording-toggle shortcut to start a full recording, then toggle again to stop. Automatic recording can start sessions for detected, authorized games when enabled.

With DVR enabled and ready, the clip shortcut saves recent gameplay from the replay buffer. **During a full recording, that same shortcut creates a bookmark instead**, even if DVR is disabled. Replay length and recording quality are configurable in Settings.

Recordings use **MKV**. To create a broadly compatible MP4, use share export.

**Settings → Frame Rate** limits full recordings and DVR without limiting the game's FPS. Choose **∞ (Uncapped)** to keep every delivered frame instead. Uncapped capture can increase processing load; the actual rate depends on the game, compositor and encoder. Fixed limits skip excess frames without speeding up playback or changing audio timing. Capture Info can therefore show capture FPS above the selected limit while encode FPS stays near it; intentional rate limiting is not a queue-overflow drop.

The Dashboard shows source connection/mute status, audio levels, approximate replay-buffer fill, and warnings for sustained video-queue drops. Connected does not necessarily mean audible; check the meters and play back a short test recording after changing your setup.

While recording or DVR is active, click **ⓘ** beside the recording control for live **Capture Info**:

- Input/output resolution, detected game identity/Steam App ID/PID pinned at capture start, separate portal-reported source/application/PID, and the transport actually delivering frames (DMA-BUF or shared memory), including observed changes. A generic portal label such as `kwin_wayland` identifies the compositor's stream, not the game; missing portal fields do not erase the detector's known identity. Manual picker recordings do not borrow an unrelated detected game's identity.
- Target and measured capture/encode FPS, target video bitrate, and measured audio/video output-payload bitrate (excluding container overhead). Both bitrate readouts show the active encoder configuration's VBR/CBR mode; this is not a promise of an enforced maximum bitrate. Encode FPS counts inputs accepted by the codec, not packets emitted; codecs can delay output. The measurement interval is shown, and rates need an initial sample before appearing.
- Current/peak frame-queue depth, queue/encoder drops, GPU/CPU path counters, and session-average encoder-call time. Recent average, maximum and 95th percentile cover the last five seconds, capped at 1024 completed calls; old samples expire during a stall.
- Encoded audio stream codec, sample rate, channels, separate/mixed layout and source connection status.
- The Audio tab also refreshes per-source signal diagnostics at 100 ms: peak and RMS over approximately one second, five-second peak hold/full-scale warning, full-scale sample count since source binding, mute state and age of the last received audio buffer. Measurements use 100 ms buckets and post-mute source PCM before recording startup gating, mixing and encoding. Zero is **−∞ dBFS** (digital silence); **0 dBFS** is full scale. RMS averages sample power across channels, not LUFS. Full-scale samples warn of clipping risk, not proof of audible distortion or final mixed-track clipping; these are not true-peak or physical microphone-loudness measurements. Missing buffers show unavailable readings rather than fake silence. Replacing/restarting a source resets its history; DVR segment rotation alone does not. Voice included in the System mix has no separate signal measurements, and Mic retains its semantic identity.
- Session duration and DVR coverage, retained segment count (including current footage and segments still finalizing), and retained video including the safety margin beyond the configured replay window.

Video/session values refresh every second; source signal diagnostics refresh every 100 ms while the Audio tab is open. DVR counters cover the **current segment** and reset at rotation, not the whole replay buffer; DVR session duration and buffer coverage are separate. Source fields may be generic or unreported by the portal. Observed transport changes do not establish why a fallback happened. Encoder-call time is not end-to-end latency, and drop counters do not cover every compositor/PipeWire loss.

## Discord Rich Presence

Enable **Settings → Discord Rich Presence → Enable Rich Presence** to share Trophy's status with the Discord desktop app. It is **off by default** and needs no Discord login or account token in Trophy.

- **Recording** includes an elapsed timer and takes priority over **Instant replay ready** while DVR is running.
- **Show Game Names** is on by default; turn it off to share only the capture status. While presence and game-name sharing are enabled, Trophy looks up the active Steam game's public hub icon and checks its public image URL. Available game icons appear with a small Trophy badge; otherwise only the Trophy icon appears. No local icons are uploaded. Disabling presence or game names cancels these lookups and hides game artwork.
- **Show When Idle** is off by default. Enable it to show **Ready to record** even while Trophy is idle in the tray. Otherwise presence disappears when neither recording nor DVR is active.

Discord's activity privacy settings still apply, and Discord controls which activity it displays when games publish their own presence. Trophy reconnects when Discord restarts. Native and standard Flatpak socket locations are checked; sandboxed installations may still need IPC socket access. This does not enable presence in the Discord web client.

## Audio

Choose **Game** or **Desktop** capture. Game mode supports separate or mixed tracks; Desktop mode mixes audio. Select your microphone and optional voice-chat application in Settings.

When game-specific audio is unavailable, Trophy deliberately falls back to the **desktop mix** rather than losing the game sound. The Dashboard calls this source **System** and hides the redundant Voice row. Separate voice capture resumes when game-specific capture binds, avoiding a second copy of voice chat already present in the desktop mix.

The desktop mix may contain other applications and monitored microphone audio. Keep that in mind when recording or choosing export tracks.

New captures store source history. The player labels the primary lane **System**, **Game**, or **Game / System**, and hides a redundant Voice lane only when history proves it never received separate audio. Other tracks keep their original identities. Older or externally modified files retain their lanes rather than guessing from silence. Clip history is conservative and may retain an unused lane.

## Find and review captures

The library offers grid/list views, search, capture-type filters, sorting, renaming, and bulk actions. Hover over an item for a short silent preview; older captures may need a moment to prepare one.

Open a capture in the built-in player to use:

- A filmstrip and per-track audio waveforms with shared zoom and pan.
- Individual audio mute controls.
- Timeline bookmarks and an editable clip range.
- Playback speed, seek controls, fullscreen, and snapshots. The screenshot button captures the current frame and opens a Save dialog so you can choose the PNG's name and location; cancelling saves nothing.

Use the clip editor to select a range and **Snip** it into a new capture. Snipping copies encoded streams without re-encoding; the start snaps to a preceding video keyframe, so the result may include a little extra footage.

### Bookmarks

During a full recording, click **Bookmark moment** or press the clip/bookmark shortcut. Bookmarks use the captured-video timeline and are unavailable before video arrives.

In the player, click a yellow marker on any lane to jump to its stored time. The bookmark list offers Jump, Remove, and **Clip ±15 s**. Clip selects an editable range; it does not save immediately. Bookmarks do not protect a capture from cleanup.

After recording, seek to a moment in the player and use **Add bookmark at current position** in the tracks toolbar. These bookmarks use the same saved list and timeline markers.

## Export and copy

Choose **Share** in the player, or select library captures and choose **Export → Share-ready MP4**.

1. Review the audio tracks. Player-muted lanes start unchecked. Selected tracks are mixed into **one stereo AAC track** (no selected audio produces a silent video), because chat apps and social sites cannot switch between tracks.
2. Choose a destination **outside the managed recording library**.
3. Start the export. Progress and cancellation appear in background toasts.

The copy is a **remux**: the video bitstream is copied unchanged into an MP4 container with fast-start layout, so it finishes in seconds, loses no quality, and never modifies the original. Only the audio is re-encoded, to produce the single mixed track. Exports are **whole-file**: snip first to share only a range.

Because the video is copied, the MP4 keeps the recording's codec. H.264 is accepted everywhere; HEVC or AV1 recordings produce a file some sites and players may reject (the dialog warns when this applies). Record with VA-API H.264, NVENC H.264 or x264 for the widest compatibility.

For batches, the destination applies to all captures while audio choices remain per file. Expand Audio and use the capture picker to review each one. Jobs run one at a time and can be cancelled independently. The destination is remembered.

Copying keeps the original dimensions and frame rate; there are no resolution or size presets. If a capture needs downscaling, a size limit or a different codec, transcode it with another tool — sharing here deliberately avoids re-encoding video.

A track containing desktop audio may already include voice chat or microphone monitoring. Adding those tracks again can potentially double their sound. Check on the video player before exporting.

**Export → Copy original files** queues unconverted copies through the same background queue. Video-only copies and MP4 exports do not include protection/bookmark annotations. Existing destination files are not overwritten. Captures in use by queued/native work cannot be renamed, deleted, or automatically cleaned up until that work settles, including after cancellation.

## Storage, protection, and deletion

Set your recording folder, storage limit, and automatic-cleanup preference in Settings. Automatic cleanup removes the oldest eligible captures; it skips protected captures and files currently reserved by recording/export/editing work.

Click a capture's **padlock** to protect it from automatic cleanup, and use **Protected only** in the filter to find protected items. Protection does not prevent explicit deletion or stop files counting toward storage usage. If no eligible space remains, increase the limit or unprotect/delete captures; Trophy does not remove protection to make room.

Deleting protected captures always asks for confirmation, even if you skipped ordinary deletion confirmations. Confirmed deletion still provides the Undo period below.

Manual deletion hides captures immediately and provides **10 seconds to Undo** before permanent deletion starts. Each batch has its own toast. Files still occupy disk space during that grace period. Quitting cancels uncommitted batches; hiding to the tray does not. Automatic cleanup is permanent and has no Undo.

### Back up your library metadata

Keep the library's **`.trophy/` directory** when moving or backing up captures. It holds protection, bookmarks, and audio-source history in per-file annotations.

In-app rename/delete handles this metadata. External video renames and video-only exports do not carry it. Unreadable protection metadata fails closed for automatic cleanup.

The **`.thumbnails/` directory** contains disposable thumbnails, filmstrips, waveforms, and preview caches. These can be regenerated; they are not where bookmarks or protection are stored.

## Updates

Trophy can check GitHub Releases for a newer version. The check runs automatically once per day while the app is open, and you can run it manually from **Info → Check for updates**. If a newer version is available, Trophy shows the version and a download/install action; on an AppImage install it can update automatically. After installing, Trophy replaces the AppImage, restarts itself and shows a "Trophy updated" notification naming the old and new versions — including when you have **Start minimized** enabled, so the restart is never silent. If the check fails, Trophy reports that it could not check for updates instead of claiming you are up to date.

## Appearance and closing

Settings offers Original (formerly Default), Charcoal, and AMOLED themes, accent colors, and launch/tray preferences. New settings default to AMOLED with the Rose accent.

The window close action and **Ctrl+Q** follow Trophy's close policy, including close-to-tray behavior and active-recording confirmation. Hiding the window is not quitting the app. Use the tray's Quit action when you intend to exit completely.
