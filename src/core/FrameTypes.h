#pragma once

#include <cstdint>
#include <memory>
#include <functional>

namespace TrophyBackend {

struct FrameDescriptor {
  int fd{-1};
  uint32_t width{0};
  uint32_t height{0};
  uint32_t format{0};
  uint32_t pixelFormat{0};
  uint64_t drmModifier{0};
  int dmaBufStride{0};
  int dmaBufOffset{0};
  uint64_t pts{0};
  std::shared_ptr<void> bufferLease;
  std::function<void()> requestCpuFallback;

  FrameDescriptor() = default;
  explicit FrameDescriptor(int fd_, uint32_t w, uint32_t h, uint32_t fmt,
                           uint64_t pts_, uint32_t pixelFmt = 0,
                           uint64_t drmMod = 0, int stride = 0,
                           int offset = 0)
      : fd(fd_), width(w), height(h), format(fmt), pixelFormat(pixelFmt),
        drmModifier(drmMod), dmaBufStride(stride), dmaBufOffset(offset), pts(pts_) {}

  // RAII
  ~FrameDescriptor();

  FrameDescriptor(const FrameDescriptor &) = delete;
  FrameDescriptor &operator=(const FrameDescriptor &) = delete;
  FrameDescriptor(FrameDescriptor &&other) noexcept;
  FrameDescriptor &operator=(FrameDescriptor &&other) noexcept;
};

} // namespace TrophyBackend
