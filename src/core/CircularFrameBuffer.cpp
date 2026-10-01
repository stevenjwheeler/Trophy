#include "CircularFrameBuffer.h"
#include <unistd.h>
#include <algorithm>

namespace TrophyBackend {

CircularFrameBuffer::CircularFrameBuffer(size_t capacityFrames)
    : m_queue(std::max<size_t>(4, capacityFrames / 4)), m_buffer(capacityFrames), m_capacityFrames(capacityFrames) {}

CircularFrameBuffer::~CircularFrameBuffer() { stop(); }

bool CircularFrameBuffer::tryEnqueue(FrameDescriptor &&frame) {
  // try to push into SPSC queue; caller expects non-blocking
  return m_queue.try_push(std::move(frame));
}

void CircularFrameBuffer::start() {
  if (m_running.exchange(true))
    return;
  m_worker = std::thread(&CircularFrameBuffer::workerLoop, this);
}

void CircularFrameBuffer::stop() {
  if (!m_running.exchange(false))
    return;
  if (m_worker.joinable())
    m_worker.join();
}

void CircularFrameBuffer::workerLoop() {
  FrameDescriptor fd;
  while (m_running.load()) {
    while (m_queue.try_pop(fd)) {
      // store into circular buffer (duplicate ownership already by FrameDescriptor)
      {
        std::lock_guard<std::mutex> lg(m_lock);
        // if existing slot has an fd, close it via destructor when overwritten
        m_buffer[m_writeIndex] = std::move(fd);
        m_writeIndex = (m_writeIndex + 1) % m_capacityFrames;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

std::vector<FrameDescriptor> CircularFrameBuffer::extractLast(size_t maxFrames) {
  std::vector<FrameDescriptor> out;
  std::lock_guard<std::mutex> lg(m_lock);
  size_t available = 0;
  // Count frames with valid fd
  for (size_t i = 0; i < m_capacityFrames; ++i) {
    if (m_buffer[i].fd != -1)
      ++available;
  }
  size_t toCopy = std::min(available, maxFrames);
  out.reserve(toCopy);
  // Copy from newest to oldest, anchored at the write index.
  // m_writeIndex points at the next slot to overwrite, so the newest
  // frame lives at (m_writeIndex - 1).
  for (size_t i = 0; i < toCopy; ++i) {
    size_t idx =
        (m_writeIndex + m_capacityFrames - 1 - i) % m_capacityFrames;
    if (m_buffer[idx].fd == -1)
      continue;
    // Duplicate fd for caller
    int dupfd = dup(m_buffer[idx].fd);
    if (dupfd == -1)
      continue;
    FrameDescriptor copy(dupfd, m_buffer[idx].width, m_buffer[idx].height,
                         m_buffer[idx].format, m_buffer[idx].pts,
                         m_buffer[idx].pixelFormat, m_buffer[idx].drmModifier,
                         m_buffer[idx].dmaBufStride, m_buffer[idx].dmaBufOffset);
    out.push_back(std::move(copy));
  }
  return out;
}

} // namespace TrophyBackend
