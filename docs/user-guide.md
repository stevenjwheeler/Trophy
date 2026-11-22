# User guide

[Back to README](../README.md) · [Setup and compatibility](getting-started.md)

## Full recordings and replay clips

Use **Record** or your recording-toggle shortcut to start a full recording, then toggle again to stop. Automatic recording can start sessions for detected, authorized games when enabled.

With DVR enabled and ready, the clip shortcut saves recent gameplay from the replay buffer. **During a full recording, that same shortcut creates a bookmark instead**, even if DVR is disabled. Replay length and recording quality are configurable in Settings.

Saving a DVR clip keeps the current capture, encoder and audio running. The save
may briefly wait for a keyframe and segment finalization. If preparing the clip
fails, Trophy reports that save failure without stopping a healthy DVR buffer.

Recordings use **MKV**. To create a broadly compatible MP4, use share export.

### Games and library entries

The **Games** tab prelists usable installed games from supported local launcher
libraries, grouped by launcher. **Custom Games** contains manually added rules
and saved games without current launcher metadata. **Default Exclusions** stays
hidden unless you enable its toggle. Search filters all groups.
Click a group heading to collapse or expand it. Counts still include its
matching games while collapsed. Running games appear in **Now Playing** at the
top without row action buttons. Within each other group, approved games come
first, unapproved games next and denied games last.
Installed Proton versions, Steam Linux Runtimes, anti-cheat runtimes and Steamworks
redistributables appear under Default Exclusions, not their launcher's game list.
They show **Excluded** and cannot be enabled as games from this table.
The prerequisite installers `dxsetup` and `vc_redist`, and the Xalia support
helper, use the same read-only Excluded state rather than offering rule controls.
Browsers, chat apps, editors and other general desktop applications are no longer
seeded as default exclusions. Old saved defaults are cleaned up when settings
load; recorded custom rules and new Deny choices are retained.

Status stays on each row: **Waiting for launch** (unapproved and not running),
**Waiting for approval** (running without saved approval), **Ready to capture**,
**Active** (running with saved approval), or **Denied**. Finding a game in a
library never grants recording permission. Use **Prompt / Deny** to control
automatic capture policy and **Revoke** to remove saved capture approval.
Hover these controls for an explanation; clicking them also shows a brief
in-app notice. Revoke forgets Trophy's saved approval, not desktop-wide permission.
Only custom entries without saved approval have **Remove**; revoke approval first
if needed. Launcher entries and internal helpers have no Remove control.
Hover a game name for its identity details.

If a game is missing, start it and use **Games → Add Game**. Launcher metadata
remains local and read-only. Trophy does not launch games or modify launcher files.

### Game library locations

Open **Settings → Game Detection & Libraries → Game Libraries** to see Steam, Heroic, Legendary, Nile,
Lutris and Faugus, including launchers marked **Not detected**. Detection refers to local
metadata, not a running launcher. Each dropdown lists the usable installed games
found for that launcher: use the arrow beside its game count to open the list.
The table's status indicators distinguish detected, missing and invalid metadata.
**Manage** opens each launcher's location settings; **Rescan now** refreshes
the list in the background.
Trophy also watches local metadata for changes, so completed installations can
appear without a manual refresh. File-change bursts are combined over 250 ms;
an independent 60-second check covers missed notifications. Starting an
unfamiliar process does not trigger a library refresh. **Rescan now** and
location changes request an immediate refresh. These checks never run overlapping library
scans. The setup wizard keeps the previous game count and launcher icons visible
while refreshing in the background, updating them only when results change.

Automatic discovery checks standard XDG locations and common Steam, Heroic and
Lutris and Faugus Flatpak locations. Use **Add location…** for a custom launcher metadata
directory, not a game installation folder. The section explains which directory
each launcher expects. You can add multiple locations, remove them, turn off
**Use automatic locations** to replace the defaults, or **Reset to automatic**.
For a custom Lutris location, the selected directory needs both `pga.db` and the
`games/` YAML files.
For Faugus, select the directory containing `games.json` (normally
`~/.local/share/faugus-launcher`). Exact Windows game paths with their configured
Wine prefix and explicit Linux-native executable entries are supported. Steam
shortcuts, shared storefronts, script/batch launch tasks and generic launchers
without installation evidence may still need an exact Trophy rule.

All reads are local and read-only. An empty list does not necessarily mean you
have no games: unsupported launch tasks or missing executable metadata cannot
be used for automatic recognition. Missing external drives do not remove saved
game rules.

### Launcher and window handoffs

Automatic full recording and DVR wait past a locally verified game-specific launcher until
the actual game payload joins that same tracked instance. Trophy then waits for
owned game audio before requesting capture, without an extra launcher-resume
step. Audio present on the first check allows an immediate start; otherwise
two consecutive successful polls settle it. Silent games start after ten
resolved polls, normally about 20 seconds (slow audio queries can take longer).
A launcher left open does not become the capture target just because it has
been running longer.
This process check does not prove window visibility: the portal still owns
source selection and authorization. When owned game audio is not yet available,
Trophy retains desktop-audio fallback and retries owned game audio.

An orange dot and **Waiting for window** on the Dashboard indicate that automatic
full recording is waiting for game startup or audio readiness before requesting
capture. This is a startup status, not a guarantee that a particular window has
been found. Use Stop to cancel the pending recording. Closing a launcher without
starting the game clears the wait and returns Trophy to Idle when nothing else
is selected.

If a captured loader/splash window disappears while the **same game instance**
remains alive and selected, Trophy retries capture using the saved portal
approval for both full recording and DVR. It waits 1.5 seconds after cleanup
before each attempt; there is no retry-count or total elapsed-time cutoff for
recoverable window loss or an approved capture that returns no stream yet.
The game can keep the same PID or hand off to another verified member of the
same instance.

For full recording, continuation is a **new recording file**, not an automatic merge.
The gap between parts is not recorded. Intermediate handoff parts are saved
quietly; completion notification and sound follow your settings when recording
ends. If a suspected handoff turns out to be game exit, completion feedback is
delivered after recovery is cancelled rather than suppressed permanently.
DVR keeps its valid buffered footage and appends packets from the new capture. You can
still save buffered footage while it waits; the window gap is not buffered video.
In native-resolution mode, a replacement window is recorded at its own native
size. Saving a clip across different resolutions takes longer: Trophy converts
the video to the newest segment's resolution, preserving aspect ratio.
This conversion requires the software H.264 encoder (libx264); compatible clips
still save without re-encoding.
Stop cancels pending recovery; game exit,
changed selection, removed approval or blocked capture also prevents another
attempt. A generic portal rejection, permission loss, startup timeout, or
service/output failure is not treated as a temporarily absent window. Portal
backends still control whether the replacement can be restored silently.

If the desktop portal closes the entire sharing session without confirmed
window-loss evidence, Trophy saves any recorded footage and stops capture rather
than assuming sharing permission still applies. A confirmed window-loss handoff
can still recover when a portal closure arrives at the same time. This is
distinct from waiting past a known launcher before capture.
If sharing ends as the game exits, DVR settles the tracked instance's exit
before reporting a capture error. Confirmed game exit stops DVR quietly and
leaves already-saved clips intact; real output/finalization failures still report.
An explicit recording start after session closure uses fresh portal authorization,
not the old closed grant. Cancellation or denial never starts an automatic retry.

### Interrupted-file repair

After an interrupted recording (for example, a crash), Trophy automatically
rebuilds missing duration and seeking information in the background. This is
lossless and keeps the same filename, bookmarks and protection. Active recordings
are skipped. Recovery is triggered on startup and library changes, not by
periodic polling; newly added unfinished files get a delayed quiet-period check.
Recovery temporarily needs approximately one recording's worth of
free disk space; if it cannot finish safely, the original is left unchanged.
Content that never reached disk cannot be recovered.

### Frame rate and capture info

**Settings → Frame Rate** limits full recordings and DVR without limiting the game's FPS. Choose **∞ (Uncapped)** to keep every delivered frame instead. Uncapped capture can increase processing load; the actual rate depends on the game, compositor and encoder. Fixed limits skip excess frames without speeding up playback or changing audio timing. Capture Info can therefore show capture FPS above the selected limit while encode FPS stays near it; intentional rate limiting is not a queue-overflow drop.

Some capture backends deliver new frames only when the picture changes, so a loading screen, menu or paused game may stop producing frames. Trophy repeats the last frame at the target rate in that case, keeping the recording timeline moving. A repeated frame compresses to almost nothing. Recordings made before this change can still contain long-duration frames.

The Dashboard shows source connection/mute status, audio levels, approximate replay-buffer fill, and warnings for sustained video-queue drops. Connected does not necessarily mean audible; check the meters and play back a short test recording after changing your setup.

While recording or DVR is active, click **ⓘ** beside the recording control for live **Capture Info**:

- Input/output resolution, detected game identity/Steam App ID/PID pinned at capture start, separate portal-reported source/application/PID, and the transport actually delivering frames (DMA-BUF or shared memory), including observed changes. A generic portal label such as `kwin_wayland` identifies the compositor's stream, not the game; missing portal fields do not erase the detector's known identity. Manual picker recordings do not borrow an unrelated detected game's identity.
- Target and measured capture/encode FPS, target video bitrate, and measured audio/video output-payload bitrate (excluding container overhead). Both bitrate readouts show the active encoder configuration's VBR/CBR mode. In VBR the encoder's peak-rate ceiling is automatically one-third above the selected target (for example, 15 Mbps target → 20 Mbps peak), with a two-second rate-control buffer. Legacy saved maximum-bitrate values are ignored. Short measurement windows can still overshoot because of frame-size granularity and the rate-control buffer. CBR is unchanged. The **Encoder Preset** setting selects the NVENC p1–p7 preset or the x264 preset; VA-API encoders currently keep their driver default. Encode FPS counts inputs accepted by the codec, not packets emitted; codecs can delay output. The measurement interval is shown, and rates need an initial sample before appearing.
- Current/peak frame-queue depth, frames skipped by the FPS cap, queue/encoder drops, GPU/CPU path counters, and session-average encoder-call time. Recent average, maximum and 95th percentile cover the last five seconds, capped at 1024 completed calls; old samples expire during a stall. **Skipped by FPS cap** counts frames the rate limiter deliberately discarded, so a gap between delivered frames and the frames in the file reads as intentional limiting rather than loss.
- Encoded audio stream codec, sample rate, channels, separate/mixed layout and source connection status.
- The Audio tab also refreshes per-source signal diagnostics at 100 ms: peak and RMS over approximately one second, five-second peak hold/full-scale warning, full-scale sample count since source binding, mute state and age of the last received audio buffer. Measurements use 100 ms buckets and post-mute source PCM before recording startup gating, mixing and encoding. Zero is **−∞ dBFS** (digital silence); **0 dBFS** is full scale. RMS averages sample power across channels, not LUFS. Full-scale samples warn of clipping risk, not proof of audible distortion or final mixed-track clipping; these are not true-peak or physical microphone-loudness measurements. Missing buffers show unavailable readings rather than fake silence. Replacing/restarting a source resets its history; saving buffered footage does not. Voice included in the System mix has no separate signal measurements, and Mic retains its semantic identity.
- Session duration and DVR coverage, retained segment count (cached groups of encoded pictures, including the current in-memory group), and retained video including the safety margin beyond the configured replay window.

Video/session values refresh every second; source signal diagnostics refresh every 100 ms while the Audio tab is open. Recording and DVR use the same capture/encoding engine. Caching encoded picture groups and saving clips do not restart that engine or reset its counters; a source handoff or producer rebuild starts a new session. DVR session duration and buffer coverage are separate. Source fields may be generic or unreported by the portal. Observed transport changes do not establish why a fallback happened. Encoder-call time is not end-to-end latency, and drop counters do not cover every compositor/PipeWire loss.

## Discord Rich Presence

Enable **Settings → Discord Rich Presence → Enable Rich Presence** to share Trophy's status with the Discord desktop app. It is **off by default** and needs no Discord login or account token in Trophy.

- **Recording** includes an elapsed timer and takes priority over **Instant replay ready** while DVR is running.
- **Show Game Names** is on by default; turn it off to share only the capture status. While presence and game-name sharing are enabled, Trophy looks up the active Steam game's public hub icon and checks its public image URL. Available game icons appear with a small Trophy badge; otherwise only the Trophy icon appears. No local icons are uploaded. Disabling presence or game names cancels these lookups and hides game artwork.
- **Show When Idle** is off by default. Enable it to show **Ready to record** even while Trophy is idle in the tray. Otherwise presence disappears when neither recording nor DVR is active.

Discord's activity privacy settings still apply, and Discord controls which activity it displays when games publish their own presence. Trophy reconnects when Discord restarts. Native and standard Flatpak socket locations are checked; sandboxed installations may still need IPC socket access. This does not enable presence in the Discord web client.

## Audio

Choose **Game** or **Desktop** capture. Game mode supports separate or mixed tracks; Desktop mode mixes audio. Select your microphone and optional voice-chat application in Settings.

The voice-application list updates automatically as PipeWire playback streams
appear, disappear or change; no manual refresh is needed. Both the wizard and
Audio Recording settings follow the same live metadata cache. If PipeWire restarts, stale
entries clear and discovery reconnects in the background; your saved voice
selection is retained.

When game-specific audio is unavailable, Trophy deliberately falls back to the **desktop mix** rather than losing the game sound. The Dashboard calls this source **System** and hides the redundant Voice row. Separate voice capture resumes when game-specific capture binds, avoiding a second copy of voice chat already present in the desktop mix.

The desktop mix may contain other applications and monitored microphone audio. Keep that in mind when recording or choosing export tracks.

New captures store source history. The player labels the primary lane **System**, **Game**, or **Game / System**, and hides a redundant Voice lane only when history proves it never received separate audio. Other tracks keep their original identities. Older or externally modified files retain their lanes rather than guessing from silence. Clip history is conservative and may retain an unused lane.

## Find and review captures

The library offers grid/list views, search, capture-type filters, sorting, renaming, and bulk actions. Hover over an item for a short silent preview; older captures may need a moment to prepare one.

Open a capture in the built-in player to use:

- A filmstrip and per-track audio waveforms with shared zoom and pan.
- Per-track volume controls: click a track's speaker button to open its volume popup. Dragging it to zero mutes that lane; the mute button in the popup provides a one-click toggle.
- Timeline bookmarks and an editable clip range.
- Playback speed, seek controls, fullscreen, and snapshots. The screenshot button captures the current frame and opens a Save dialog so you can choose the PNG's name and location; cancelling saves nothing.

Use the clip editor to select a range and **Snip** it into a new capture. Snipping copies encoded streams without re-encoding; the start snaps to a preceding video keyframe, so the result may include a little extra footage.

### Bookmarks

During a full recording, click **Bookmark moment** or press the clip/bookmark shortcut. Bookmarks use the captured-video timeline and are unavailable before video arrives.

In the player, click a yellow marker on any lane to jump to its stored time. The bookmark list offers Jump, Remove, and **Clip ±15 s**. Clip selects an editable range; it does not save immediately. Bookmarks do not protect a capture from cleanup.

After recording, seek to a moment in the player and use **Add bookmark at current position** in the tracks toolbar. These bookmarks use the same saved list and timeline markers.

## Export and copy

Choose **Share** in the player, or select library captures and choose **Export → Share-ready MP4**.

1. Review the audio tracks. Each track has a gain bar; the player's per-lane volumes carry over, and a track at zero is left out. Selected tracks are mixed into **one stereo AAC track**, because chat apps and social sites cannot switch between tracks and AAC-LC is the audio format every player accepts. Gains are summed without automatically raising the remaining tracks when another is muted. Peaks exceeding full scale are clipped; lower the track gains if the mix is too loud. The player's master listening volume does not change the export. When exactly one stereo AAC track is selected at full volume, it is copied unchanged instead of re-encoded; a reduced volume needs a re-encode. Mixing costs an audio encode, so three tracks take noticeably longer than one; the dialog shows an estimate for the current selection before you start.
2. Choose a destination **outside the managed recording library**.
3. Start the export. Progress and cancellation appear in background toasts.

The copy is a **remux**: the video bitstream is copied unchanged into an MP4 container with fast-start layout, so it finishes in seconds, loses no quality, and never modifies the original. Only the audio is re-encoded, to produce the single mixed track. Exports are **whole-file**: snip first to share only a range.

Because the video is copied, the MP4 keeps the recording's codec — so the copy is only universally shareable when the recording is H.264. A recording that is already H.264 therefore defaults to the copy, which is lossless and already produces the H.264 + AAC file every site accepts. Convert stays available in that case for one reason: it is the only way to shrink the picture.

HEVC and AV1 recordings cannot be copied into something a phone will reliably play, so the dialog **defaults to Convert to H.264 (compatibility)** for them and still offers the copy for cases where it does not matter. Converting re-encodes the video to H.264 (hardware encoder when available, x264 otherwise) and mixes the selected tracks into one stereo AAC track, producing the same H.264 + AAC shape as an H.264 copy. It decodes and re-encodes the whole file, so it takes minutes rather than seconds — the dialog quotes an estimate, and a machine without a hardware encoder is much slower. The optional resolution choice (original, 1080p, 720p) never upscales.

Record with VA-API H.264, NVENC H.264 or x264 if you mostly share your recordings: that keeps every export on the copy path. Copied HEVC is tagged `hvc1` so Apple's players accept it, but browsers and older players still may not.

A copy also preserves the recording's timing exactly. An older recording with a long still stretch keeps that stretch as one long frame, which some players treat as a freeze; **Convert** re-encodes and fills those stretches with repeated frames. That is the only way to remove them — repeating a frame in a copy would break the encoded predictions that follow it.

Audio is decoded on demand using an independent reading cursor for each selected track. Only the next decoded block is retained, so long video gaps and unusual track interleaving do not accumulate audio in memory or impose a recording-length limit. This can require additional file reading, especially with several selected tracks. Defensive limits still reject abnormal decoded frames; the original remains intact if export fails.

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

### Closing and the tray

The window close action and **Ctrl+Q** follow Trophy's close policy, including
**Minimize to tray on close**, **Start minimized** and active-recording
confirmation. Hiding the window is not quitting the app, and a recording keeps
running.

- **KDE Plasma** has a system tray, so closing with the option enabled leaves a
  Trophy icon with Show/Hide, Start/Stop, Save Clip and Quit.
- **GNOME** does not ship a system tray by default. A tray icon needs the
  **AppIndicator and KStatusNotifierItem Support** extension. Without it,
  closing Trophy still keeps it running in the background, but there is no tray
  icon: **launch Trophy again from the app grid to bring the window back** (the
  second launch raises the existing instance instead of starting a second copy).
  **Start minimized** and start-on-login behave the same way there: Trophy opens
  hidden in the background and shows a short notification saying so; launch
  Trophy again to open the window. GNOME's **Background Apps** list only covers
  Flatpak applications, so a native or AppImage Trophy does not appear there.
- On **Hyprland/Sway**, enable **Start minimized** only if you can restore the
  window through a native shortcut or by launching Trophy again.

Use the tray's **Quit** action (or **Ctrl+Q**) when you intend to exit
completely.
