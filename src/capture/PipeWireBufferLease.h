/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <pipewire/pipewire.h>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include "capture/PipeWireVideoFormat.h"

namespace TrophyBackend {

// A dup'd DMA-BUF fd pins storage, not its contents. Keep the producer's
// buffer dequeued until all queued/encoded consumers release their lease.
// Returns happen on the native loop, never from an arbitrary encoder thread.
class PipeWireBufferLease : public std::enable_shared_from_this<PipeWireBufferLease> {
public:
  bool initialize(pw_loop *loop, pw_stream *stream) {
    m_loop = loop;
    m_stream = stream;
    m_event = pw_loop_add_event(loop, [](void *data, uint64_t) {
      auto *self = static_cast<PipeWireBufferLease *>(data);
      std::vector<std::shared_ptr<Entry>> returned;
      bool sharedMemory = false;
      {
        std::lock_guard lock(self->m_mutex);
        returned.swap(self->m_returned);
        sharedMemory = self->m_requestSharedMemory;
        self->m_requestSharedMemory = false;
      }
      for (const auto &entry : returned) {
        // remove_buffer runs on this same loop, never concurrently here.
        if (entry->buffer)
          pw_stream_queue_buffer(self->m_stream, entry->buffer);
      }
      if (sharedMemory) {
        uint8_t storage[512];
        auto builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        const spa_pod *format = buildPipeWireVideoFormat(&builder);
        pw_stream_update_params(self->m_stream, &format, 1);
      }
    }, this);
    return m_event != nullptr;
  }

  std::shared_ptr<void> retain(pw_buffer *buffer) {
    auto entry = std::make_shared<Entry>();
    entry->buffer = buffer;
    {
      std::lock_guard lock(m_mutex);
      m_entries[buffer] = entry;
    }
    return std::shared_ptr<void>(buffer, [self = shared_from_this(), entry](void *) {
      std::lock_guard lock(self->m_mutex);
      if (self->m_event && entry->buffer) {
        self->m_returned.push_back(entry);
        pw_loop_signal_event(self->m_loop, self->m_event);
      }
    });
  }

  // PipeWire can remove dequeued buffers during format renegotiation.
  void removed(pw_buffer *buffer) {
    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(buffer);
    if (it != m_entries.end()) {
      if (auto entry = it->second.lock())
        entry->buffer = nullptr;
      m_entries.erase(it);
    }
  }

  void requestSharedMemory() {
    std::lock_guard lock(m_mutex);
    if (m_event && !m_fallbackRequested) {
      m_fallbackRequested = true;
      m_requestSharedMemory = true;
      pw_loop_signal_event(m_loop, m_event);
    }
  }

  // Only after the loop has stopped/joined, before stream destruction.
  // Late leases then release harmlessly without touching native resources.
  void deactivate() {
    std::lock_guard lock(m_mutex);
    if (m_event)
      pw_loop_destroy_source(m_loop, m_event);
    m_event = nullptr;
    m_returned.clear();
    m_entries.clear();
    m_stream = nullptr;
    m_loop = nullptr;
  }

private:
  struct Entry { pw_buffer *buffer = nullptr; };
  std::mutex m_mutex;
  pw_loop *m_loop = nullptr;
  pw_stream *m_stream = nullptr;
  spa_source *m_event = nullptr;
  std::vector<std::shared_ptr<Entry>> m_returned;
  std::unordered_map<pw_buffer *, std::weak_ptr<Entry>> m_entries;
  bool m_fallbackRequested = false;
  bool m_requestSharedMemory = false;
};

} // namespace TrophyBackend
