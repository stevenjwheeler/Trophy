#include "FrameTypes.h"
#include <unistd.h>

namespace TrophyBackend {

FrameDescriptor::~FrameDescriptor() {
  if (fd != -1) {
    ::close(fd);
    fd = -1;
  }
}

FrameDescriptor::FrameDescriptor(FrameDescriptor &&other) noexcept {
  fd = other.fd;
  width = other.width;
  height = other.height;
  format = other.format;
  pixelFormat = other.pixelFormat;
  drmModifier = other.drmModifier;
  dmaBufStride = other.dmaBufStride;
  dmaBufOffset = other.dmaBufOffset;
  pts = other.pts;
  bufferLease = std::move(other.bufferLease);
  requestCpuFallback = std::move(other.requestCpuFallback);
  other.fd = -1;
}

FrameDescriptor &FrameDescriptor::operator=(FrameDescriptor &&other) noexcept {
  if (this == &other)
    return *this;
  if (fd != -1)
    ::close(fd);
  fd = other.fd;
  width = other.width;
  height = other.height;
  format = other.format;
  pixelFormat = other.pixelFormat;
  drmModifier = other.drmModifier;
  dmaBufStride = other.dmaBufStride;
  dmaBufOffset = other.dmaBufOffset;
  pts = other.pts;
  bufferLease = std::move(other.bufferLease);
  requestCpuFallback = std::move(other.requestCpuFallback);
  other.fd = -1;
  return *this;
}

} // namespace TrophyBackend
