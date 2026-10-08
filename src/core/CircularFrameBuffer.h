#pragma once

#include "FrameTypes.h"
#include "FrameDispatcher.h"
#include "SpscRing.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <optional>

namespace TrophyBackend {

class CircularFrameBuffer : public IFrameSink {
public:
  explicit CircularFrameBuffer(size_t capacityFrames = 300); // ring of frame handles (fds), not pixels
  ~CircularFrameBuffer();

  // Sink API used by FrameDispatcher: non-blocking
  bool tryEnqueue(FrameDescriptor &&frame) override;

  // Extract last N frames (or last ~N seconds depending on PTS); duplicates fds for caller
  std::vector<FrameDescriptor> extractLast(size_t maxFrames);

  void start();
  void stop();

private:
  void workerLoop();

  SpscRing<FrameDescriptor> m_queue;
  std::vector<FrameDescriptor> m_buffer; // protected by m_lock when accessed for extract
  size_t m_writeIndex{0}; // next slot to overwrite; guarded by m_lock
  std::atomic<bool> m_running{false};
  std::thread m_worker;
  std::mutex m_lock;
  size_t m_capacityFrames{0};
};

} // namespace TrophyBackend
