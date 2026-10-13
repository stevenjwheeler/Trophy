#include "FrameDispatcher.h"
#include <unistd.h>
#include <algorithm>

namespace TrophyBackend {

FrameDispatcher::FrameDispatcher() = default;
FrameDispatcher::~FrameDispatcher() = default;

void FrameDispatcher::registerSink(std::shared_ptr<IFrameSink> sink) {
  std::lock_guard<std::mutex> lg(m_lock);
  m_sinks.push_back(std::move(sink));
}

void FrameDispatcher::unregisterSink(std::shared_ptr<IFrameSink> sink) {
  std::lock_guard<std::mutex> lg(m_lock);
  m_sinks.erase(std::remove(m_sinks.begin(), m_sinks.end(), sink), m_sinks.end());
}

void FrameDispatcher::dispatch(const FrameDescriptor &frame) {
  // Snapshot the sinks under lock, then dispatch without holding it:
  // tryEnqueue is arbitrary sink code (a future blocking sink would stall
  // capture, and register/unregister from a callback would self-deadlock).
  std::vector<std::shared_ptr<IFrameSink>> sinks;
  {
    std::lock_guard<std::mutex> lg(m_lock);
    sinks = m_sinks;
  }
  for (size_t i = 0; i < sinks.size(); ++i) {
    auto &sink = sinks[i];
    // Duplicate fd per sink to give ownership to the sink (cheap dup syscall)
    int dupfd = -1;
    if (frame.fd != -1) {
      dupfd = dup(frame.fd);
      if (dupfd == -1) {
        continue;
      }
    }
    FrameDescriptor copy(dupfd, frame.width, frame.height, frame.format,
                             frame.pts, frame.pixelFormat, frame.drmModifier,
                             frame.dmaBufStride, frame.dmaBufOffset);
    bool ok = sink->tryEnqueue(std::move(copy));
    if (!ok) {
      // sink full or rejected; close dupfd by letting destructor close when leaving scope
    }
  }
}

} // namespace TrophyBackend
