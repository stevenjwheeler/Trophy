# User guide

[Back to README](../README.md) · [Setup and compatibility](getting-started.md)

## Full recordings and replay clips

Use **Record** or your recording-toggle shortcut to start a full recording, then toggle again to stop. Automatic recording can start sessions for detected, authorized games when enabled.

With DVR enabled and ready, the clip shortcut saves recent gameplay from the replay buffer. **During a full recording, that same shortcut creates a bookmark instead**, even if DVR is disabled. Replay length and recording quality are configurable in Settings.

Recordings use **MKV**. To create a broadly compatible MP4, use share export.

The Dashboard shows source connection/mute status, audio levels, approximate replay-buffer fill, and warnings for sustained video-queue drops. Connected does not necessarily mean audible; check the meters and play back a short test recording after changing your setup.

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
- Playback speed, seek controls, fullscreen, and snapshots.

Use the clip editor to select a range and **Snip** it into a new capture. Snipping copies encoded streams without re-encoding; the start snaps to a preceding video keyframe, so the result may include a little extra footage.

### Bookmarks

During a full recording, click **Bookmark moment** or press the clip/bookmark shortcut. Bookmarks use the captured-video timeline and are unavailable before video arrives.

In the player, click a yellow marker on any lane to jump to its stored time. The bookmark list offers Jump, Remove, and **Clip ±15 s**. Clip selects an editable range; it does not save immediately. Bookmarks do not protect a capture from cleanup.

## Export and copy

Choose **Share** in the player, or select library captures and choose **Export → Share-ready MP4**.

1. Choose original, 1080p, or 720p resolution and a quality or approximate size preset.
2. Review the audio tracks. Player-muted lanes start unchecked. Selected tracks are averaged into one stereo AAC track; no selected audio produces a silent video.
3. Choose a destination **outside the managed recording library**.
4. Start the export. Progress and cancellation appear in background toasts.

For batches, quality and destination apply to all captures, while audio choices remain per file. Expand Audio and use the capture picker to review each one. Jobs run one at a time and can be cancelled independently. The destination is remembered.

Exports use software H.264/YUV420P with fast-start MP4 and optional AAC audio, at up to 60fps. They never upscale or modify the original. Export is **whole-file**: snip first to share only a range. Size targets are approximate, not guaranteed upload limits. HDR-to-SDR conversion is unsupported; known HDR input is rejected.

A track containing desktop audio may already include voice chat or microphone monitoring. Adding those tracks again can potentially double their sound. Check on the video player before exporting.

**Export → Copy original files** queues unconverted copies through the same background queue. Video-only copies and MP4 exports do not include protection/bookmark annotations. Existing destination files are not overwritten. Captures in use by queued/native work cannot be renamed, deleted, or automatically cleaned up until that work settles, including after cancellation.

## Storage, protection, and deletion

Set your recording folder, storage limit, and automatic-cleanup preference in Settings. Automatic cleanup removes the oldest eligible captures; it skips protected captures and files currently reserved by recording/export/editing work.

Click a capture's **padlock** to protect it from automatic cleanup, and use **Protected only** in the filter to find protected items. Protection does not prevent explicit deletion or stop files counting toward storage usage. If no eligible space remains, increase the limit or unprotect/delete captures; Trophy does not remove protection to make room.

Manual deletion hides captures immediately and provides **10 seconds to Undo** before permanent deletion starts. Each batch has its own toast. Files still occupy disk space during that grace period. Quitting cancels uncommitted batches; hiding to the tray does not. Automatic cleanup is permanent and has no Undo.

### Back up your library metadata

Keep the library's **`.trophy/` directory** when moving or backing up captures. It holds protection, bookmarks, and audio-source history in per-file annotations.

In-app rename/delete handles this metadata. External video renames and video-only exports do not carry it. Unreadable protection metadata fails closed for automatic cleanup.

The **`.thumbnails/` directory** contains disposable thumbnails, filmstrips, waveforms, and preview caches. These can be regenerated; they are not where bookmarks or protection are stored.

## Appearance and closing

Settings offers Default, Charcoal, and AMOLED themes, accent colors, and launch/tray preferences.

The window close action and **Ctrl+Q** follow Trophy's close policy, including close-to-tray behavior and active-recording confirmation. Hiding the window is not quitting the app. Use the tray's Quit action when you intend to exit completely.
