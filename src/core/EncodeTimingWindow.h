/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace TrophyBackend {

// A bounded diagnostic window, not a frame/buffer cache. Its private lock
// covers only copying/storing scalar samples; sorting never holds the lock.
class EncodeTimingWindow {
public:
  static constexpr int64_t windowUs = 5000000;
  static constexpr size_t capacity = 1024;
  struct Summary {
    size_t count = 0;
    double averageMs = 0;
    double maximumMs = 0;
    double p95Ms = 0;
  };

  void clear() {
    std::lock_guard lock(m_mutex);
    m_next = m_count = 0;
  }
  void record(int64_t completedUs, int64_t durationUs) {
    std::lock_guard lock(m_mutex);
    m_samples[m_next] = {completedUs, std::max(int64_t(0), durationUs)};
    m_next = (m_next + 1) % capacity;
    m_count = std::min(m_count + 1, capacity);
  }
  Summary summary(int64_t nowUs) const {
    std::array<int64_t, capacity> durations;
    size_t count = 0;
    {
      std::lock_guard lock(m_mutex);
      for (size_t i = 0; i < m_count; ++i) {
        const auto &sample = m_samples[i];
        if (sample.completedUs <= nowUs && nowUs - sample.completedUs < windowUs)
          durations[count++] = sample.durationUs;
      }
    }
    if (!count) return {};
    std::sort(durations.begin(), durations.begin() + count);
    double sumUs = 0;
    for (size_t i = 0; i < count; ++i) sumUs += durations[i];
    // Nearest-rank percentile (ceil(0.95 * count), one-based).
    return {count, sumUs / count / 1000.0, durations[count - 1] / 1000.0,
            durations[(95 * count + 99) / 100 - 1] / 1000.0};
  }

private:
  struct Sample { int64_t completedUs = 0, durationUs = 0; };
  mutable std::mutex m_mutex;
  std::array<Sample, capacity> m_samples{};
  size_t m_next = 0, m_count = 0;
};

} // namespace TrophyBackend
