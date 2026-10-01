#pragma once

#include "FrameDispatcher.h"
#include <functional>

namespace TrophyBackend {

class CallbackSink : public IFrameSink {
public:
  using Fn = std::function<bool(FrameDescriptor &&)>;
  explicit CallbackSink(Fn cb) : m_cb(std::move(cb)) {}
  bool tryEnqueue(FrameDescriptor &&frame) override { return m_cb(std::move(frame)); }

private:
  Fn m_cb;
};

} // namespace TrophyBackend
