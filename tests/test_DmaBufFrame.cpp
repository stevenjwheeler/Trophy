/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>
#include <climits>
#include <sys/mman.h>
#include "capture/IVideoCaptureSource.h"

using namespace TrophyBackend;

namespace {
struct Source {
  int fd = memfd_create("trophy-plane-test", MFD_CLOEXEC);
  Source() { if (fd >= 0) ftruncate(fd, 65536); }
  ~Source() { if (fd >= 0) close(fd); }
};
bool openFd(int fd) { return fcntl(fd, F_GETFD) >= 0; }
int duplicatedBeforeFailure = -1;
int failSecondDuplicate(int fd) {
  if (duplicatedBeforeFailure >= 0) return -1;
  return duplicatedBeforeFailure = fcntl(fd, F_DUPFD_CLOEXEC, 0);
}
}

TEST(DmaBufFrameTest, EveryPlaneIsOwnedCloexecAndLeaseLivesUntilLastSnapshot) {
  for (const uint32_t count : {1u, 2u, 4u}) {
    Source source;
    ASSERT_GE(source.fd, 0);
    DmaBufLayout layout;
    layout.count = count;
    for (uint32_t i = 0; i < count; ++i)
      layout.planes[i] = {source.fd, 256, i * 4096, 65536};
    auto lease = std::make_shared<int>(1);
    std::weak_ptr<int> weak = lease;
    auto owner = OwnedDmaBuf::retain(layout, lease);
    ASSERT_TRUE(owner);
    lease.reset();
    auto survivor = owner;
    const auto owned = owner->layout();
    for (uint32_t i = 0; i < count; ++i) {
      EXPECT_NE(owned.planes[i].fd, source.fd);
      EXPECT_NE(fcntl(owned.planes[i].fd, F_GETFD) & FD_CLOEXEC, 0);
      EXPECT_EQ(owned.planes[i].offset, layout.planes[i].offset);
    }
    close(source.fd); source.fd = -1;
    owner.reset();
    EXPECT_FALSE(weak.expired());
    for (uint32_t i = 0; i < count; ++i) EXPECT_TRUE(openFd(owned.planes[i].fd));
    survivor.reset();
    EXPECT_TRUE(weak.expired());
    for (uint32_t i = 0; i < count; ++i) EXPECT_FALSE(openFd(owned.planes[i].fd));
  }
}

TEST(DmaBufFrameTest, PartialDuplicationFailureClosesOnlyOwnedHandles) {
  Source source;
  DmaBufLayout layout;
  layout.count = 2;
  layout.planes[0].fd = layout.planes[1].fd = source.fd;
  duplicatedBeforeFailure = -1;
  EXPECT_FALSE(OwnedDmaBuf::retain(layout, {}, failSecondDuplicate));
  ASSERT_GE(duplicatedBeforeFailure, 0);
  EXPECT_FALSE(openFd(duplicatedBeforeFailure));
  EXPECT_TRUE(openFd(source.fd));
  layout.count = 0;
  EXPECT_FALSE(OwnedDmaBuf::retain(layout, {}));
  layout.count = 5;
  EXPECT_FALSE(OwnedDmaBuf::retain(layout, {}));
}

TEST(DmaBufFrameTest, QueuedAndRetainedFramesShareHandlesThroughMoveAndReplacement) {
  Source first, second;
  VideoFrame snapshot{};
  snapshot.isDmaBuf = true;
  snapshot.dmaBufPlanes.count = 2;
  snapshot.dmaBufPlanes.planes[0] = {first.fd, 256, 0, 65536};
  snapshot.dmaBufPlanes.planes[1] = {second.fd, 64, 32768, 65536};
  ASSERT_TRUE(snapshot.retainDmaBuf());
  const int oldFd = snapshot.dmaBufFd;
  const int auxFd = snapshot.dmaBufLayout().planes[1].fd;
  {
    QueuedFrame queued(snapshot);
    EXPECT_EQ(queued.dmaBufFd, oldFd); // No dup per filler or queued consumer.
    VideoFrame still = snapshot;
    snapshot = {};
    QueuedFrame moved(std::move(queued));
    EXPECT_EQ(queued.dmaBufFd, -1);
    EXPECT_TRUE(openFd(auxFd));
    QueuedFrame replacement(VideoFrame{});
    replacement = std::move(moved);
    EXPECT_EQ(replacement.dmaBufLayout().count, 2u);
    EXPECT_EQ(replacement.dmaBufFd, oldFd);
  }
  EXPECT_FALSE(openFd(oldFd));
  EXPECT_FALSE(openFd(auxFd));
}

TEST(DmaBufFrameTest, LegacySinglePlaneNormalizesAndInvalidInputCannotBecomeCpuPixels) {
  Source source;
  VideoFrame frame{};
  frame.isDmaBuf = true;
  frame.dmaBufFd = source.fd;
  frame.dmaBufStride = 256;
  frame.dmaBufOffset = 16;
  QueuedFrame queued(frame);
  EXPECT_TRUE(queued.isDmaBuf);
  EXPECT_NE(queued.dmaBufFd, source.fd);
  EXPECT_EQ(queued.dmaBufLayout().count, 1u);
  EXPECT_EQ(queued.dmaBufLayout().planes[0].offset, 16u);
  frame.dmaBufOffset = -1;
  QueuedFrame invalid(frame);
  EXPECT_FALSE(invalid.isDmaBuf);
  EXPECT_EQ(invalid.data, nullptr);
}
