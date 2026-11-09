/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace TrophyBackend {

// Only free buffers are pooled. Published snapshots are immutable and cannot
// be recycled until the last queue/encoder/still-frame reference disappears.
// The idle cache is bounded independently of the recording queue, including
// during resize. Outstanding snapshots may safely outlive the pool owner.
class CpuFramePool {
  friend class CpuFramePoolTestPeer;
  struct State {
    std::mutex mutex;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> idle;
    size_t bytes = 0;
  };
  std::shared_ptr<State> m_state = std::make_shared<State>();
public:
  static constexpr size_t maxIdleBytes = 64 * 1024 * 1024;
  static constexpr size_t maxIdleBuffers = 2;

  std::shared_ptr<const std::vector<uint8_t>> copy(const uint8_t *data, size_t bytes) {
    std::unique_ptr<std::vector<uint8_t>> buffer;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> obsolete;
    {
      std::lock_guard lock(m_state->mutex);
      for (auto it = m_state->idle.begin(); it != m_state->idle.end(); ++it) {
        if ((*it)->size() == bytes) {
          buffer = std::move(*it);
          m_state->bytes -= buffer->capacity();
          m_state->idle.erase(it);
          break;
        }
      }
      if (!buffer) {
        obsolete.swap(m_state->idle);
        m_state->bytes = 0;
      }
    }
    obsolete.clear(); // Resize eviction frees pixels outside the lock.
    if (!buffer) buffer = std::make_unique<std::vector<uint8_t>>(bytes);
    std::memcpy(buffer->data(), data, bytes);
    return {buffer.release(), [weak = std::weak_ptr<State>(m_state)](const auto *p) {
      std::unique_ptr<std::vector<uint8_t>> retired(const_cast<std::vector<uint8_t> *>(p));
      if (auto state = weak.lock()) {
        std::lock_guard lock(state->mutex);
        if (state->idle.size() < maxIdleBuffers &&
            retired->capacity() <= maxIdleBytes - state->bytes) {
          state->bytes += retired->capacity();
          state->idle.push_back(std::move(retired));
        }
      }
      // Large pixel allocations are freed outside the pool lock.
    }};
  }
};
} // namespace TrophyBackend
