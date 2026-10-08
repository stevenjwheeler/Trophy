/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

struct SwrContext;

namespace TrophyBackend {

/**
 * @brief Locks one audio track to the wall clock.
 *
 * Video timestamps follow the system clock (capture side) while audio
 * timestamps count samples at the nominal rate. The sound card crystal
 * driving PipeWire is routinely ~100ppm off, so over a 2h+ session the
 * audio measurably lags or leads the video (verified live: +119ppm on
 * the game track). This corrector gently speeds/slows the track via
 * libswresample compensation (inaudible, bounded) so its sample count
 * tracks wall time instead of the crystal.
 *
 * Pure per-track state (no threads, no locking); the caller serializes
 * calls. A null SwrContext (alloc failure) degrades to pass-through.
 */
class AudioDriftCorrector {
public:
  /// Microseconds, monotonic. Defaults to steady_clock.
  using Clock = std::function<int64_t()>;

  explicit AudioDriftCorrector(int sampleRate, int channels,
                               Clock clock = {});
  ~AudioDriftCorrector();

  AudioDriftCorrector(const AudioDriftCorrector &) = delete;
  AudioDriftCorrector &operator=(const AudioDriftCorrector &) = delete;

  bool valid() const { return m_swr != nullptr; }

  /**
   * @brief Run one input chunk through the corrector.
   * @return corrected frame count (0 when SWR buffered everything);
   *         @a out points to an internal buffer valid until the next
   *         call. Pass-through (input pointer, same count) when idle
   *         inside the deadband or when invalid.
   */
  int process(const float *samples, int frameCount, const float *&out);

private:
  static int64_t steadyNowUs();

  SwrContext *m_swr = nullptr;
  int m_sampleRate = 48000;
  int m_channels = 2;
  Clock m_clock;

  int64_t m_startUs = -1; // wall time of first chunk (-1 = unprimed)
  int64_t m_lastUs = -1;  // wall time of previous chunk (gap detection)
  int64_t m_totalOut = 0; // cumulative corrected output samples
  double m_errorSmooth = 0.0; // low-passed error: callback arrival
                              // jitters with scheduler quanta, and the
                              // servo must track drift, not noise
  std::vector<float> m_out;

  // Correction only outside this deadband (20ms): hunting around zero
  // would color every chunk for no benefit.
  static constexpr double kDeadbandSamples = 960.0;
  // Per-call correction capped at 0.2% of the chunk: inaudible, and it
  // bounds convergence after long silences (mute gaps reset instead).
  static constexpr double kMaxRateShift = 0.002;
  // A gap longer than this re-baselines instead of correcting (mute /
  // source rebind, not drift).
  static constexpr int64_t kResetGapUs = 5000000;
};

} // namespace TrophyBackend
