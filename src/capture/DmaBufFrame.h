/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <array>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace TrophyBackend {

struct DmaBufPlane {
  int fd = -1;
  int32_t stride = 0;
  uint32_t offset = 0;
  uint32_t size = 0; // Producer extent when supplied; zero can mean unknown.
};

struct DmaBufLayout {
  static constexpr uint32_t maxPlanes = 4;
  uint32_t count = 0;
  std::array<DmaBufPlane, maxPlanes> planes{};
};

// Immutable allocation handles shared by the queue and retained still frame.
// Handles pin storage; the separate producer lease prevents pixel reuse.
class OwnedDmaBuf {
public:
  using Duplicate = int (*)(int);
  static std::shared_ptr<const OwnedDmaBuf> retain(
      const DmaBufLayout &borrowed, std::shared_ptr<void> lease,
      Duplicate duplicate = duplicateFd) {
    if (!borrowed.count || borrowed.count > DmaBufLayout::maxPlanes)
      return {};
    auto owned = std::shared_ptr<OwnedDmaBuf>(new OwnedDmaBuf);
    owned->m_lease = std::move(lease);
    for (uint32_t i = 0; i < borrowed.count; ++i) {
      const int fd = borrowed.planes[i].fd >= 0 ? duplicate(borrowed.planes[i].fd) : -1;
      if (fd < 0)
        return {}; // The partially populated owner releases all prior duplicates.
      owned->m_layout.planes[i] = borrowed.planes[i];
      owned->m_layout.planes[i].fd = fd;
      ++owned->m_layout.count;
    }
    return owned;
  }
  ~OwnedDmaBuf() {
    for (uint32_t i = 0; i < m_layout.count; ++i)
      ::close(m_layout.planes[i].fd);
  }
  OwnedDmaBuf(const OwnedDmaBuf &) = delete;
  OwnedDmaBuf &operator=(const OwnedDmaBuf &) = delete;
  const DmaBufLayout &layout() const { return m_layout; }

private:
  static int duplicateFd(int fd) { return fcntl(fd, F_DUPFD_CLOEXEC, 0); }
  OwnedDmaBuf() = default;
  DmaBufLayout m_layout;
  std::shared_ptr<void> m_lease;
};
} // namespace TrophyBackend
