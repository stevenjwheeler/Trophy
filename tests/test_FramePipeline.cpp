/*
 * Trophy - Unit Tests
 * Frame pipeline: FrameDescriptor ownership, FrameDispatcher fan-out,
 * CircularFrameBuffer newest-first extraction.
 */

#include <QCoreApplication>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "core/CircularFrameBuffer.h"
#include "core/FrameDispatcher.h"
#include "core/FrameTypes.h"

using namespace TrophyBackend;

class FramePipelineTestEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
  }
};

static auto *env =
    testing::AddGlobalTestEnvironment(new FramePipelineTestEnvironment);

// --- helpers ---

// An fd we own and can validate. memfd keeps the test independent of
// any display server or PipeWire daemon.
static int makeFd() {
  int fd = memfd_create("trophy-test-frame", MFD_CLOEXEC);
  if (fd < 0) {
    int pipeFds[2];
    EXPECT_EQ(pipe(pipeFds), 0);
    close(pipeFds[0]);
    return pipeFds[1];
  }
  const char c = 'x';
  EXPECT_EQ(write(fd, &c, 1), (ssize_t)1);
  return fd;
}

static bool fdIsOpen(int fd) { return fcntl(fd, F_GETFD) != -1; }

static FrameDescriptor makeFrame(uint64_t pts, uint32_t width = 1280) {
  return FrameDescriptor(makeFd(), width, 720, /*format=*/1, pts,
                         /*pixelFmt=*/2, /*drmMod=*/3, /*stride=*/5120,
                         /*offset=*/0);
}

class CountingSink : public IFrameSink {
public:
  bool tryEnqueue(FrameDescriptor &&frame) override {
    std::lock_guard<std::mutex> lg(mutex);
    ptsValues.push_back(frame.pts);
    frames.push_back(std::move(frame));
    return true;
  }

  std::mutex mutex;
  std::vector<uint64_t> ptsValues;
  std::vector<FrameDescriptor> frames; // hold fds so they stay valid
};

// --- FrameDescriptor RAII ---

TEST(FrameDescriptorTest, MoveTransfersOwnership) {
  int fd = makeFd();
  ASSERT_TRUE(fdIsOpen(fd));

  FrameDescriptor moved;
  {
    FrameDescriptor original(fd, 64, 64, 1, 100);
    moved = std::move(original);
    EXPECT_EQ(original.fd, -1); // source must not double-close
    EXPECT_TRUE(fdIsOpen(moved.fd));
  }
  EXPECT_TRUE(fdIsOpen(moved.fd));
}

TEST(FrameDescriptorTest, DestructorClosesFd) {
  int fd = makeFd();
  { FrameDescriptor holder(fd, 64, 64, 1, 100); }
  EXPECT_FALSE(fdIsOpen(fd));
}

// --- FrameDispatcher ---

TEST(FrameDispatcherTest, FansOutToAllSinksWithDupedFds) {
  FrameDispatcher dispatcher;
  auto sinkA = std::make_shared<CountingSink>();
  auto sinkB = std::make_shared<CountingSink>();
  dispatcher.registerSink(sinkA);
  dispatcher.registerSink(sinkB);

  FrameDescriptor src(makeFd(), 1920, 1080, 1, 777);
  const int srcFd = src.fd;
  for (int i = 0; i < 3; ++i) {
    FrameDescriptor f(makeFd(), 1920, 1080, 1, 100 + i);
    dispatcher.dispatch(f);
  }

  // Source keeps its own fds (dispatcher duped per sink).
  EXPECT_TRUE(fdIsOpen(srcFd));
  ASSERT_EQ(sinkA->ptsValues.size(), 3u);
  ASSERT_EQ(sinkB->ptsValues.size(), 3u);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(sinkA->ptsValues[i], 100 + (uint64_t)i);
    EXPECT_EQ(sinkB->ptsValues[i], 100 + (uint64_t)i);
  }
  // Each sink got its own dup, distinct from fellow sinks.
  EXPECT_NE(sinkA->frames[0].fd, sinkB->frames[0].fd);
  EXPECT_TRUE(fdIsOpen(sinkA->frames[0].fd));
  EXPECT_TRUE(fdIsOpen(sinkB->frames[0].fd));
}

TEST(FrameDispatcherTest, UnregisterStopsDelivery) {
  FrameDispatcher dispatcher;
  auto sink = std::make_shared<CountingSink>();
  dispatcher.registerSink(sink);
  dispatcher.unregisterSink(sink);

  FrameDescriptor f = makeFrame(1);
  dispatcher.dispatch(f);
  EXPECT_TRUE(sink->ptsValues.empty());
}

TEST(FrameDispatcherTest, NoSinksDoesNotCrash) {
  FrameDispatcher dispatcher;
  FrameDescriptor f = makeFrame(1);
  const int fd = f.fd;
  EXPECT_NO_THROW(dispatcher.dispatch(f));
  EXPECT_TRUE(fdIsOpen(fd)); // caller retains ownership
}

// --- CircularFrameBuffer ---

// Poll extractLast until at least want frames are visible (worker thread
// drains asynchronously) or the timeout expires. Returns the last snapshot.
static std::vector<FrameDescriptor>
waitForFrames(CircularFrameBuffer &buf, size_t want, int timeoutMs = 5000) {
  std::vector<FrameDescriptor> snap;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  do {
    snap = buf.extractLast(1024);
    if (snap.size() >= want)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  return snap;
}

TEST(CircularFrameBufferTest, ExtractNewestFirstWithFieldsPreserved) {
  CircularFrameBuffer buf(8);
  buf.start();
  // The worker drains asynchronously and the handoff queue holds fewer
  // frames than the ring, so retry until every frame is accepted.
  for (uint64_t i = 0; i < 5; ++i) {
    bool accepted = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!accepted && std::chrono::steady_clock::now() < deadline) {
      accepted = buf.tryEnqueue(makeFrame(i, 100 + (uint32_t)i));
      if (!accepted)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(accepted) << "frame " << i << " never accepted";
  }

  ASSERT_EQ(waitForFrames(buf, 5).size(), 5u);
  auto out = buf.extractLast(3);
  buf.stop();

  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].pts, 4u); // newest first
  EXPECT_EQ(out[1].pts, 3u);
  EXPECT_EQ(out[2].pts, 2u);
  // Full descriptor survives the round trip (regression: extractLast
  // used to drop pixelFormat/modifier/stride/offset).
  EXPECT_EQ(out[0].width, 104u);
  EXPECT_EQ(out[0].height, 720u);
  EXPECT_EQ(out[0].pixelFormat, 2u);
  EXPECT_EQ(out[0].drmModifier, 3u);
  EXPECT_EQ(out[0].dmaBufStride, 5120);
  // Extracted fds are caller-owned dups, distinct from each other.
  EXPECT_TRUE(fdIsOpen(out[0].fd));
  EXPECT_NE(out[0].fd, out[1].fd);
}

TEST(CircularFrameBufferTest, WrapAroundKeepsNewest) {
  CircularFrameBuffer buf(4);
  buf.start();
  for (uint64_t i = 0; i < 6; ++i) {
    EXPECT_TRUE(buf.tryEnqueue(makeFrame(i)));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Wait until the newest frame (pts 5) has landed in the ring.
  std::vector<FrameDescriptor> out;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    out = buf.extractLast(4);
    if (!out.empty() && out[0].pts == 5)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  buf.stop();

  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0].pts, 5u);
  EXPECT_EQ(out[1].pts, 4u);
  EXPECT_EQ(out[2].pts, 3u);
  EXPECT_EQ(out[3].pts, 2u);
}

TEST(CircularFrameBufferTest, EmptyBufferExtractsNothing) {
  CircularFrameBuffer buf(8);
  buf.start();
  EXPECT_TRUE(buf.extractLast(4).empty());
  buf.stop();
}
