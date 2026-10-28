/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace TrophyBackend {

// Scalar diagnostics only: no PCM retention. 100 ms buckets keep the recent
// windows bounded regardless of the source's buffer size/callback cadence.
class AudioSignalStats {
public:
  struct Summary {
    bool received = false, recent = false, held = false, clipping = false;
    int64_t ageMs = 0;
    double peak = 0, rms = 0, heldPeak = 0;
    uint64_t clippedSamples = 0, totalClippedSamples = 0;
  };

  void record(int64_t nowUs, double peak, double squares,
              uint64_t samples, uint64_t clipped) {
    if (!samples || nowUs < 0) return;
    const int64_t epoch = nowUs / bucketUs;
    std::lock_guard lock(m_mutex);
    auto &bucket = m_buckets[epoch % m_buckets.size()];
    if (bucket.epoch != epoch) bucket = Bucket{epoch};
    bucket.peak = std::max(bucket.peak, peak);
    bucket.squares += squares;
    bucket.samples += samples;
    bucket.clipped += clipped;
    m_lastReceivedUs = nowUs;
    m_totalClipped += clipped;
  }

  Summary summary(int64_t nowUs) const {
    std::array<Bucket, 64> buckets;
    Summary result;
    {
      std::lock_guard lock(m_mutex);
      if (m_lastReceivedUs < 0) return result;
      result.received = true;
      result.ageMs = std::max(int64_t(0), nowUs - m_lastReceivedUs) / 1000;
      result.totalClippedSamples = m_totalClipped;
      buckets = m_buckets;
    }
    const int64_t epoch = nowUs / bucketUs;
    double squares = 0;
    uint64_t samples = 0;
    for (const auto &bucket : buckets) {
      const auto age = epoch - bucket.epoch;
      if (bucket.epoch < 0 || age < 0 || age >= 50) continue;
      result.held = true;
      result.heldPeak = std::max(result.heldPeak, bucket.peak);
      result.clipping |= bucket.clipped > 0;
      if (age >= 10) continue;
      result.recent = true;
      result.peak = std::max(result.peak, bucket.peak);
      squares += bucket.squares;
      samples += bucket.samples;
      result.clippedSamples += bucket.clipped;
    }
    if (samples) result.rms = std::sqrt(squares / samples);
    return result;
  }

private:
  static constexpr int64_t bucketUs = 100000;
  struct Bucket {
    int64_t epoch = -1;
    double peak = 0, squares = 0;
    uint64_t samples = 0, clipped = 0;
  };
  mutable std::mutex m_mutex;
  std::array<Bucket, 64> m_buckets{};
  int64_t m_lastReceivedUs = -1;
  uint64_t m_totalClipped = 0;
};

} // namespace TrophyBackend
