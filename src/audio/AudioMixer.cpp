/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AudioMixer.h"
#include "IAudioSource.h"

#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <qlogging.h>

namespace TrophyBackend {

AudioMixer::AudioMixer(int sampleRate, int channels, QObject *parent)
    : QObject(parent), m_sampleRate(sampleRate), m_channels(channels) {
  // Initialize track configs (array is already default-constructed)
  m_tracks[0].config = {0, "Game Audio", 1.0f, false};
  m_tracks[1].config = {1, "Voice Chat", 1.0f, false};
  m_tracks[2].config = {2, "Microphone", 1.0f, false};
  // Atomic copies start at defaults (1.0 volume, unmuted) — already set by
  // the in-class member initializers, but be explicit for clarity.
  for (auto &track : m_tracks) {
    track.atomicVolume.store(track.config.volume, std::memory_order_relaxed);
    track.atomicMuted.store(track.config.muted, std::memory_order_relaxed);
  }

  qDebug() << "AudioMixer initialized:" << sampleRate << "Hz," << channels
            << "channels";
  m_sourceOperationTimer = new QTimer(this);
  m_sourceOperationTimer->setInterval(10);
  connect(m_sourceOperationTimer, &QTimer::timeout, this,
          &AudioMixer::pumpSourceOperations);
}

AudioMixer::~AudioMixer() {
  // Source callbacks must finish before tracks/QObject state is destroyed.
  // As with other QObject methods, concurrent public calls/start/destruction
  // are not supported. Retirement must not run inside an audio callback.
  stopAll();
  std::vector<std::shared_ptr<IAudioSource>> detached;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_started = false;
    for (auto &track : m_tracks) {
      if (track.sourceCallbackActive)
        track.sourceCallbackActive->accepting.store(false,
                                                   std::memory_order_release);
      if (track.source)
        detached.push_back(std::move(track.source));
      ++track.generation;
    }
  }
  for (auto &source : detached) {
    // Unpublish the mixer callback first so no further audio delivery can
    // touch this (partially destroyed) mixer; setCallback is an atomic
    // publish, never blocking.
    source->setCallback(nullptr);
    source->stop();
  }
  for (auto &lookup : m_discardedNodeLookups)
    lookup.worker.get();
}

void AudioMixer::publishSourceCallback(Track &track) {
  auto active = std::make_shared<SourceCallbackRegistration>();
  track.sourceCallbackActive = active;
  const int index = track.config.index;
  track.source->setCallback([this, index, active](const AudioFrame &frame) {
    {
      std::lock_guard<std::mutex> lock(active->mutex);
      if (!active->accepting.load(std::memory_order_acquire))
        return;
      ++active->inFlight;
    }
    struct Completion {
      SourceCallbackHandle active;
      ~Completion() {
        std::lock_guard<std::mutex> lock(active->mutex);
        if (--active->inFlight == 0)
          active->drained.notify_all();
      }
    } completion{active};
    onAudioReceived(index, frame.samples, frame.frameCount, active);
  });
}

int AudioMixer::semanticRoleForSource(const IAudioSource &source, int trackIndex) {
  if (source.sourceType() == IAudioSource::SourceType::Microphone)
    return 2;
  if (trackIndex == 0 || source.sourceType() == IAudioSource::SourceType::DesktopAudio)
    return 0;
  return 1;
}

void AudioMixer::applySourceRole(Track &track) {
  const int role = semanticRoleForSource(*track.source, track.config.index);
  // Preserve a legacy physical idle preference on the slot's FIRST bind,
  // unless an explicit logical preference already exists. A subsequent role
  // change must never carry the old role's physical mute into the new role.
  if (track.semanticRole < 0 && !m_semanticMuteConfigured[role]) {
    m_semanticMuted[role] = track.config.muted;
  }
  m_semanticMuteConfigured[role] = true;
  track.semanticRole = role;
  track.config.muted = m_semanticMuted[role];
  track.atomicMuted.store(track.config.muted, std::memory_order_release);
  track.peakLevel.store(0.0f);
}

void AudioMixer::drainSourceCallback(const SourceCallbackHandle &handle) {
  if (!handle)
    return;
  std::unique_lock<std::mutex> lock(handle->mutex);
  handle->accepting.store(false, std::memory_order_release);
  handle->draining = true;
  handle->drained.notify_all();
  handle->drained.wait(lock, [&] { return handle->inFlight == 0; });
}

bool AudioMixer::startRetainedSource(
    const std::shared_ptr<IAudioSource> &source, int trackIndex,
    uint64_t generation, const AudioConfig &config) {
  if (!source)
    return true;
  const int index = trackIndex;
  SourceCallbackHandle callback;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto &track = m_tracks[index];
    if (track.generation != generation || track.source != source || !m_started)
      return false;
    if (source->isCapturing())
      return true;
    // stopAll retires the previous registration. Never reactivate it:
    // retained old snapshots must stay retired across a restart.
    if (!track.sourceCallbackActive ||
        !track.sourceCallbackActive->accepting.load(std::memory_order_acquire))
      publishSourceCallback(track);
    callback = track.sourceCallbackActive;
  }
  const auto retireFailedAttempt = [&] {
    callback->accepting.store(false, std::memory_order_release);
    source->setCallback(nullptr);
    source->stop();
    drainSourceCallback(callback);
  };
  if (!source->initialize(config)) {
    qWarning() << "Failed to initialize audio source for track" << index;
    retireFailedAttempt();
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto &track = m_tracks[index];
    if (track.generation == generation && track.source == source)
      m_tracks[index].peakLevel.store(0.0f);
  }
  if (!source->start()) {
    qWarning() << "Failed to start audio source for track" << index;
    retireFailedAttempt();
    return false;
  }
  // A removeSource/stopAll/destructor may have detached this source while
  // the blocking calls above ran. Recheck under lock: on mismatch stop the
  // retained copy and leave the published slot untouched (never resurrect).
  bool current = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= 0 &&
        static_cast<size_t>(index) < m_tracks.size()) {
      const Track &track = m_tracks[index];
      current =
          (m_started && track.generation == generation && track.source == source);
    }
  }
  if (!current)
    source->stop();
  return current;
}

void AudioMixer::addSource(std::unique_ptr<IAudioSource> source,
                           int trackIndex) {
  settleSourceOperations();
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    qWarning() << "Invalid track index:" << trackIndex;
    return;
  }

  // unique_ptr boundary preserved for callers; shared ownership starts here.
  std::shared_ptr<IAudioSource> newSource(std::move(source));
  std::shared_ptr<IAudioSource> oldSource;
  SourceCallbackHandle oldCallback;
  bool started = false;
  uint64_t generation = 0;
  AudioConfig config;
  config.sampleRate = m_sampleRate;
  config.channels = m_channels;
  QString displayName;
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    Track &track = m_tracks[trackIndex];
    if (track.sourceCallbackActive)
      track.sourceCallbackActive->accepting.store(false,
                                                 std::memory_order_release);
    oldCallback = std::move(track.sourceCallbackActive);
    oldSource = std::move(track.source);
    track.source = newSource;
    ++track.generation;
    generation = track.generation;

    if (newSource) {
      applySourceRole(track); // Apply role preference BEFORE callback publication/start.
      publishSourceCallback(track);
      displayName = newSource->displayName();
    }
    started = m_started;
  }
  // Blocking PipeWire calls run on retained copies, never under m_mutex:
  // a slow stop() (pw_stream_destroy) no longer stalls add/remove/start.
  if (oldSource) {
    // Replacement has the same teardown invariant as removeSource:
    // prevent new deliveries from the retired source before stopping it.
    oldSource->setCallback(nullptr);
    oldSource->stop();
  }
  drainSourceCallback(oldCallback);

  if (newSource)
    qDebug() << "Audio source added to track" << trackIndex << ":"
             << displayName;

  // If the mixer is already running (e.g., a source added mid-recording by
  // the node-check timer), initialize and start it immediately — outside
  // the lock, with a generation check so a concurrent remove prevents
  // resurrection.
  if (newSource && started)
    startRetainedSource(newSource, trackIndex, generation, config);
}

void AudioMixer::removeSource(int trackIndex) {
  settleSourceOperations();
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return;
  }

  // Unpublish + bump generation under lock, then
  // null the callback and request stop on the retained copy outside it.
  // An in-flight start on the same copy converges via its generation
  // check (stops the orphan, never republishes). Nulling the callback
  // (atomic, non-blocking — mirrors the destructor) stops late audio
  // delivery from landing in onAudioReceived after a successor source
  // is slotted at this index.
  std::shared_ptr<IAudioSource> detached;
  SourceCallbackHandle callback;
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    Track &track = m_tracks[trackIndex];
    if (track.sourceCallbackActive)
      track.sourceCallbackActive->accepting.store(false,
                                                 std::memory_order_release);
    callback = std::move(track.sourceCallbackActive);
    detached = std::move(track.source);
    ++track.generation;
  }
  if (detached) {
    detached->setCallback(nullptr);
    detached->stop();
  }
  drainSourceCallback(callback);
}

void AudioMixer::setTrackVolume(int trackIndex, float volume) {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return;
  }

  float clamped = std::clamp(volume, 0.0f, 2.0f);
  m_tracks[trackIndex].config.volume = clamped;
  m_tracks[trackIndex].atomicVolume.store(clamped, std::memory_order_relaxed);
  emit trackConfigChanged(trackIndex);
}

bool AudioMixer::sourceIsCapturing(int trackIndex) const {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size())
    return false;
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto &source = m_tracks[trackIndex].source;
  return source && source->isCapturing();
}

void AudioMixer::setTrackMuted(int trackIndex, bool muted) {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return;
  }

  m_tracks[trackIndex].config.muted = muted;
  m_tracks[trackIndex].atomicMuted.store(muted, std::memory_order_relaxed);
  const int role = m_tracks[trackIndex].semanticRole;
  if (role >= 0 && m_tracks[trackIndex].source) {
    const bool changed = m_semanticMuted[role] != muted;
    m_semanticMuted[role] = muted;
    m_semanticMuteConfigured[role] = true;
    if (changed) emit semanticTrackMutedChanged(role, muted);
  }
  emit trackConfigChanged(trackIndex);
}

void AudioMixer::setSemanticTrackMuted(int role, bool muted) {
  if (role < 0 || role >= kTrackCount) return;
  const bool changed = m_semanticMuted[role] != muted;
  m_semanticMuted[role] = muted;
  m_semanticMuteConfigured[role] = true;
  std::array<bool, kTrackCount> physicalChanges{};
  for (auto &track : m_tracks) {
    if (track.semanticRole != role || !track.source) continue;
    const bool physicalChanged = track.config.muted != muted;
    track.config.muted = muted;
    track.atomicMuted.store(muted, std::memory_order_release);
    if (muted) track.peakLevel.store(0.0f);
    physicalChanges[track.config.index] = physicalChanged;
  }
  for (int track = 0; track < kTrackCount; ++track)
    if (physicalChanges[track]) emit trackConfigChanged(track);
  if (changed && m_semanticMuted[role] == muted)
    emit semanticTrackMutedChanged(role, muted);
}

bool AudioMixer::isSemanticTrackMuted(int role) const {
  return role >= 0 && role < kTrackCount && m_semanticMuted[role];
}

float AudioMixer::semanticTrackLevel(int role) const {
  if (role < 0 || role >= kTrackCount || m_semanticMuted[role]) return 0.0f;
  float peak = 0.0f;
  for (const auto &track : m_tracks)
    if (track.semanticRole == role && track.source && track.source->isCapturing())
      peak = std::max(peak, track.peakLevel.load());
  return peak;
}

float AudioMixer::trackVolume(int trackIndex) const {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return 0.0f;
  }
  return m_tracks[trackIndex].config.volume;
}

bool AudioMixer::isTrackMuted(int trackIndex) const {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return true;
  }
  return m_tracks[trackIndex].config.muted;
}

QString AudioMixer::trackName(int trackIndex) const {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return QString();
  }
  return m_tracks[trackIndex].config.name;
}

float AudioMixer::getTrackLevel(int trackIndex) const {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return 0.0f;
  }
  return m_tracks[trackIndex].peakLevel.load();
}

bool AudioMixer::startAll() {
  settleSourceOperations();
  // Snapshot retained copies under lock; blocking starts run outside it.
  struct Pending {
    std::shared_ptr<IAudioSource> source;
    int index;
    uint64_t generation;
  };
  std::vector<Pending> pending;
  AudioConfig config;
  config.sampleRate = m_sampleRate;
  config.channels = m_channels;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_started = true;
    for (auto &track : m_tracks) {
      if (track.source)
        pending.push_back({track.source, track.config.index,
                           track.generation});
    }
  }

  bool allStarted = true;
  for (auto &p : pending) {
    if (!startRetainedSource(p.source, p.index, p.generation, config))
      allStarted = false;
  }

  return allStarted;
}

bool AudioMixer::ensureSourcesStarted() {
  settleSourceOperations();
  // Start only sources that are not already capturing. DVR segment
  // switches keep PipeWire streams running across the handoff; blindly
  // restarting them would reopen a gap on every rotation.
  // Same lock-free pattern as startAll: snapshot, release, start, validate.
  struct Pending {
    std::shared_ptr<IAudioSource> source;
    int index;
    uint64_t generation;
  };
  std::vector<Pending> pending;
  AudioConfig config;
  config.sampleRate = m_sampleRate;
  config.channels = m_channels;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_started = true;
    for (auto &track : m_tracks) {
      // isCapturing() is atomic — safe inside the short critical section.
      if (track.source && !track.source->isCapturing())
        pending.push_back({track.source, track.config.index,
                           track.generation});
    }
  }

  bool allStarted = true;
  for (auto &p : pending) {
    // Recheck both generation and capturing before initializing.
    if (!startRetainedSource(p.source, p.index, p.generation, config))
      allStarted = false;
  }

  return allStarted;
}

void AudioMixer::stopAll() {
  // Cancel queued starts before joining native work. No worker can outlive
  // the mixer or its callback state, even if native teardown is slow.
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_started = false;
    for (auto &track : m_tracks) {
      ++track.generation;
      if (track.sourceCallbackActive)
        track.sourceCallbackActive->accepting.store(false,
                                                    std::memory_order_release);
      if (track.source) track.source->setCallback(nullptr);
    }
  }
  settleSourceOperations();
  // Detach semantics without unpublishing: sources stay slotted for the
  // next start, but every in-flight start is retired — the generation bump
  // makes its completion stop the orphan instead of reporting success
  // while the mixer is idle. Blocking stops run outside the lock.
  std::vector<std::shared_ptr<IAudioSource>> running;
  std::vector<SourceCallbackHandle> callbacks;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_started = false;
    for (auto &track : m_tracks) {
      ++track.generation;
      if (track.sourceCallbackActive) {
        track.sourceCallbackActive->accepting.store(false,
                                                   std::memory_order_release);
        callbacks.push_back(track.sourceCallbackActive);
      }
      if (track.source)
        running.push_back(track.source);
    }
  }
  for (auto &source : running) {
    source->setCallback(nullptr);
    source->stop();
  }
  for (auto &callback : callbacks)
    drainSourceCallback(callback);
}

void AudioMixer::addSourceAsync(std::unique_ptr<IAudioSource> source,
                                int trackIndex) {
  replaceSourceAsync(std::move(source), trackIndex);
}

void AudioMixer::removeSourceAsync(int trackIndex) {
  replaceSourceAsync(nullptr, trackIndex);
}

void AudioMixer::replaceSourceAsync(std::unique_ptr<IAudioSource> source,
                                    int index) {
  if (index < 0 || index >= kTrackCount)
    return;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto &track = m_tracks[index];
    if (track.sourceCallbackActive)
      track.sourceCallbackActive->accepting.store(false,
                                                  std::memory_order_release);
    if (track.source) {
      track.source->setCallback(nullptr);
      track.pendingRetirements.push_back(
          {std::move(track.source), std::move(track.sourceCallbackActive)});
    }
    track.source = std::shared_ptr<IAudioSource>(std::move(source));
    ++track.generation;
    track.pendingStart = m_started && bool(track.source);
    if (track.source)
      applySourceRole(track);
    if (track.source)
      publishSourceCallback(track);
  }
  pumpSourceOperations();
}

bool AudioMixer::sourceOperationPending(int index) const {
  if (index < 0 || index >= kTrackCount)
    return false;
  // Async bookkeeping is owner-thread-only, like QTimer and source creation.
  const auto &track = m_tracks[index];
  return track.operation || track.pendingStart ||
         !track.pendingRetirements.empty();
}

void AudioMixer::ensureSourcesStartedAsync() {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_started = true;
    for (auto &track : m_tracks)
      if (track.source && !track.source->isCapturing() && !track.operation) {
        if (!track.sourceCallbackActive ||
            !track.sourceCallbackActive->accepting.load(std::memory_order_acquire))
          publishSourceCallback(track);
        track.pendingStart = true;
      }
  }
  pumpSourceOperations();
}

void AudioMixer::stopSourcesAsync() {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_started = false;
    for (auto &track : m_tracks) {
      if (track.sourceCallbackActive)
        track.sourceCallbackActive->accepting.store(false,
                                                    std::memory_order_release);
      if (track.source) track.source->setCallback(nullptr);
    }
  }
  for (int index = 0; index < kTrackCount; ++index)
    removeSourceAsync(index);
}

void AudioMixer::pumpSourceOperations() {
  bool pending = false;
  for (auto it = m_discardedNodeLookups.begin();
       it != m_discardedNodeLookups.end();) {
    if (it->worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      it->worker.get();
      it = m_discardedNodeLookups.erase(it);
    } else {
      pending = true;
      ++it;
    }
  }
  for (auto &track : m_tracks) {
    if (track.operation) {
      if (track.operation->worker.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready) {
        pending = true;
        continue;
      }
      track.operation->worker.get();
      // Release retained QObjects here, on their owner thread, never in the
      // worker closure. The operation owns every native input until joined.
      track.operation.reset();
    }
    if (!track.pendingStart && track.pendingRetirements.empty())
      continue;
    auto operation = std::make_unique<SourceOperation>();
    operation->retired = std::move(track.pendingRetirements);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (track.pendingStart && m_started) {
        operation->start = track.source;
        operation->generation = track.generation;
      }
      track.pendingStart = false;
    }
    auto *work = operation.get();
    const int index = track.config.index;
    AudioConfig config;
    config.sampleRate = m_sampleRate;
    config.channels = m_channels;
    work->worker = std::async(std::launch::async, [this, work, index, config] {
      for (auto &retired : work->retired) {
        retired.source->stop();
        drainSourceCallback(retired.callback);
      }
      if (work->start)
        startRetainedSource(work->start, index, work->generation, config);
    });
    track.operation = std::move(operation);
    pending = true;
  }
  if (pending)
    m_sourceOperationTimer->start();
  else
    m_sourceOperationTimer->stop();
}

void AudioMixer::retainDiscardedNodeLookup(std::future<NodeLookupResult> lookup,
                                          int trackIndex) {
  if (!lookup.valid())
    return;
  m_discardedNodeLookups.push_back({trackIndex, std::move(lookup)});
  m_sourceOperationTimer->start();
}

bool AudioMixer::discardedNodeLookupPending(int trackIndex) const {
  return std::any_of(m_discardedNodeLookups.begin(), m_discardedNodeLookups.end(),
                     [trackIndex](const auto &lookup) {
    return lookup.trackIndex == trackIndex;
  });
}

void AudioMixer::settleSourceOperations() {
  m_sourceOperationTimer->stop();
  for (auto &track : m_tracks) {
    track.pendingStart = false;
    if (track.operation) {
      track.operation->worker.get();
      track.operation.reset();
    }
    for (auto &retired : track.pendingRetirements) {
      retired.source->stop();
      drainSourceCallback(retired.callback);
    }
    track.pendingRetirements.clear();
  }
  if (!m_discardedNodeLookups.empty())
    m_sourceOperationTimer->start();
}

AudioMixer::TrackAudioCallbackHandle
AudioMixer::setTrackAudioCallback(TrackAudioCallback callback) {
  TrackAudioCallbackHandle published;
  if (callback)
    published = std::make_shared<TrackAudioCallbackRegistration>(
        std::move(callback));
  m_trackAudioCallback.store(published, std::memory_order_release);
  return published;
}

void AudioMixer::drainTrackAudioCallback(
    const TrackAudioCallbackHandle &handle) {
  if (!handle)
    return;
  std::unique_lock<std::mutex> lock(handle->mutex);
  handle->accepting = false;
  // The registration owns the function, NOT its captured session/encoders.
  // Warn on slow callbacks, but never release the lifetime barrier early.
  constexpr auto kDrainTimeout = std::chrono::seconds(2);
  if (!handle->drained.wait_for(lock, kDrainTimeout,
                                [&] { return handle->inFlight == 0; })) {
    handle->drainTimedOut = true;
    handle->drained.notify_all();
    qWarning() << "Timed out waiting for in-flight track audio callbacks"
                  "(inFlight"
               << handle->inFlight << "); waiting for safe retirement";
    handle->drained.wait(lock, [&] { return handle->inFlight == 0; });
  }
}

void AudioMixer::onAudioReceived(int trackIndex, const float *samples,
                                 int frameCount,
                                 const SourceCallbackHandle &sourceCallback) {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return;
  }

  Track &track = m_tracks[trackIndex];

  // Calculate peak level for metering
  float peak = 0.0f;
  int totalSamples = frameCount * m_channels;

  for (int i = 0; i < totalSamples; i++) {
    float absVal = std::abs(samples[i]);
    if (absVal > peak) {
      peak = absVal;
    }
  }

  // Apply volume and mute — read from atomics for lock-free access on the
  // PipeWire callback thread (config fields are written from the UI thread).
  const bool muted = track.atomicMuted.load(std::memory_order_relaxed);
  const float volume = track.atomicVolume.load(std::memory_order_relaxed);
  if (muted) {
    // Muted tracks read flat zero so meters park immediately.
    track.peakLevel.store(0.0f);
  } else {
    peak *= volume;
    // Perceptual mapping: a linear peak parks typical loud content
    // (-20dB = 0.1) at a 10% sliver. Map -60..0dB onto 0..1 so -20dB
    // reads ~0.67 and -12dB ~0.8, while quiet content stays low.
    // Result stays in 0..1 (hotter-than-full-scale clamps at 1).
    float display = 0.0f;
    if (peak > 0.0f) {
      const float dB = 20.0f * std::log10(peak);
      display = std::clamp((dB + 60.0f) / 60.0f, 0.0f, 1.0f);
    }

    // Fast attack, smooth release. Release runs in display space, so the
    // bar falls linearly instead of diving logarithmically.
    const float currentPeak = track.peakLevel.load();
    track.peakLevel.store(display > currentPeak ? display
                                               : currentPeak * 0.95f);
  }

  // Emit level change signal for UI updates, decimated to ~10 Hz per track:
  // onAudioReceived runs per audio buffer (~50-90/s/track) on the PipeWire
  // thread and each emit queues into the GUI thread, while the meters poll
  // getTrackLevel() at 100 ms anyway. Timestamp is a relaxed atomic so this
  // stays lock-free on the callback thread.
  const int64_t nowUs =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  const int64_t lastEmitUs =
      track.lastLevelEmitUs.load(std::memory_order_relaxed);
  if (nowUs - lastEmitUs >= 100000) {
    track.lastLevelEmitUs.store(nowUs, std::memory_order_relaxed);
    emit trackLevelChanged(trackIndex, track.peakLevel.load());
  }

  // Forward audio to encoder callback (called on PipeWire thread)
  // A direct signal receiver above may have blocked while this source was
  // retired/replaced. Its old buffer must not reach a successor session.
  if (!sourceCallback->accepting.load(std::memory_order_acquire))
    return;
  const auto callback = m_trackAudioCallback.load(std::memory_order_acquire);
  if (!callback)
    return;
  {
    std::lock_guard<std::mutex> lock(callback->mutex);
    if (!callback->accepting)
      return;
    ++callback->inFlight;
  }
  struct CallbackCompletion {
    TrackAudioCallbackHandle registration;
    ~CallbackCompletion() {
      std::lock_guard<std::mutex> lock(registration->mutex);
      if (--registration->inFlight == 0)
        registration->drained.notify_all();
    }
  } completion{callback};
  // Muting removes sound, not time. Dropping a track's buffers stalls the
  // merged session's all-tracks gate and lets its other accumulators grow
  // without bound; separate tracks also need their sample clocks to advance.
  // Keep the borrowed source samples untouched (other consumers may own them).
  std::vector<float> silence;
  if (muted) {
    silence.assign(totalSamples, 0.0f);
    samples = silence.data();
  }
  callback->callback(trackIndex, samples, frameCount, m_sampleRate, m_channels);
}

} // namespace TrophyBackend
