#pragma once

#include "FrameTypes.h"

#include <memory>
#include <vector>
#include <mutex>

namespace TrophyBackend {

class IFrameSink {
public:
  virtual ~IFrameSink() = default;
  virtual bool tryEnqueue(FrameDescriptor &&frame) = 0;
};

class FrameDispatcher {
public:
  FrameDispatcher();
  ~FrameDispatcher();

  void registerSink(std::shared_ptr<IFrameSink> sink);
  void unregisterSink(std::shared_ptr<IFrameSink> sink);

  // Called from capture thread. Non-blocking.
  void dispatch(const FrameDescriptor &frame);

private:
  std::vector<std::shared_ptr<IFrameSink>> m_sinks;
  std::mutex m_lock;
};

} // namespace TrophyBackend
