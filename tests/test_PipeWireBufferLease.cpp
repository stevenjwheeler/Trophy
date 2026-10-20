/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "capture/PipeWireBufferLease.h"
#include "capture/IVideoCaptureSource.h"
#include "core/FrameDispatcher.h"
#include "core/CallbackSink.h"

using namespace TrophyBackend;

static int returnedCount;
static pw_buffer *lastReturned;
static int fallbackCount;
static bool fallbackHasModifier;
extern "C" int __wrap_pw_stream_queue_buffer(pw_stream *, pw_buffer *buffer) {
  ++returnedCount;
  lastReturned = buffer;
  return 0;
}
extern "C" int __wrap_pw_stream_update_params(pw_stream *, const spa_pod **params,
                                                uint32_t count) {
  ++fallbackCount;
  fallbackHasModifier = count != 1 || pipeWireVideoHasModifier(params[0]);
  return 0;
}

class PipeWireBufferLeaseTest : public ::testing::Test {
protected:
  void SetUp() override {
    pw_init(nullptr, nullptr);
    loop = pw_loop_new(nullptr);
    ASSERT_NE(loop, nullptr);
    pw_loop_enter(loop);
    leases = std::make_shared<PipeWireBufferLease>();
    ASSERT_TRUE(leases->initialize(loop, nullptr));
    returnedCount = 0;
    lastReturned = nullptr;
    fallbackCount = 0;
    fallbackHasModifier = true;
  }
  void TearDown() override {
    leases->deactivate();
    leases.reset();
    pw_loop_leave(loop);
    pw_loop_destroy(loop);
  }
  pw_loop *loop = nullptr;
  std::shared_ptr<PipeWireBufferLease> leases;
};

TEST_F(PipeWireBufferLeaseTest, LastConsumerReturnsOnNativeLoopNotEncoderThread) {
  pw_buffer buffer{};
  auto first = leases->retain(&buffer);
  auto second = first;
  first.reset();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 0);
  std::thread worker([lease = std::move(second)]() mutable { lease.reset(); });
  worker.join();
  EXPECT_EQ(returnedCount, 0);
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 1);
  EXPECT_EQ(lastReturned, &buffer);
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 1);
}

TEST_F(PipeWireBufferLeaseTest, LateLeaseAfterNativeTeardownIsHarmless) {
  pw_buffer buffer{};
  auto lease = leases->retain(&buffer);
  leases->deactivate();
  std::thread worker([lease = std::move(lease)]() mutable { lease.reset(); });
  worker.join();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 0);
}

TEST_F(PipeWireBufferLeaseTest, PendingReturnIsDiscardedAtTeardown) {
  pw_buffer buffer{};
  leases->retain(&buffer).reset();
  leases->deactivate();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 0);
}

TEST_F(PipeWireBufferLeaseTest, WorkerReleaseCanRaceStoppedLoopDeactivation) {
  pw_buffer buffer{};
  auto lease = leases->retain(&buffer);
  std::atomic<bool> go{false};
  std::thread worker([&, lease = std::move(lease)]() mutable {
    while (!go.load()) std::this_thread::yield();
    leases->requestSharedMemory();
    lease.reset();
  });
  go.store(true);
  leases->deactivate();
  worker.join();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 0);
  EXPECT_EQ(fallbackCount, 0);
}

TEST_F(PipeWireBufferLeaseTest, ImportFailureRenegotiatesSharedMemoryOnceOnLoop) {
  std::thread worker([&] {
    leases->requestSharedMemory();
    leases->requestSharedMemory();
  });
  worker.join();
  EXPECT_EQ(fallbackCount, 0);
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(fallbackCount, 1);
  EXPECT_FALSE(fallbackHasModifier);
  leases->requestSharedMemory();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(fallbackCount, 1);
  leases->deactivate();
  leases->requestSharedMemory();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(fallbackCount, 1);
}

TEST_F(PipeWireBufferLeaseTest, QueuedFrameMovesKeepProducerPinned) {
  pw_buffer buffer{};
  VideoFrame frame{};
  frame.bufferLease = leases->retain(&buffer);
  auto queued = std::make_unique<QueuedFrame>(frame);
  frame.bufferLease.reset();
  auto moved = std::make_unique<QueuedFrame>(std::move(*queued));
  queued.reset();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 0);
  moved.reset();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 1);
}

TEST_F(PipeWireBufferLeaseTest, RemovedBufferIsNotReturnedDuringRenegotiation) {
  pw_buffer buffer{};
  auto lease = leases->retain(&buffer);
  leases->removed(&buffer);
  lease.reset();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 0);
  // Reuse of the address must not resurrect the old entry.
  leases->retain(&buffer).reset();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 1);
}

TEST_F(PipeWireBufferLeaseTest, DispatcherCopiesAndMovesKeepProducerPinned) {
  pw_buffer buffer{};
  FrameDescriptor descriptor;
  descriptor.bufferLease = leases->retain(&buffer);
  std::unique_ptr<FrameDescriptor> retained;
  FrameDispatcher dispatcher;
  dispatcher.registerSink(std::make_shared<CallbackSink>([&](FrameDescriptor &&frame) {
    retained = std::make_unique<FrameDescriptor>(std::move(frame));
    return true;
  }));
  dispatcher.dispatch(descriptor);
  descriptor.bufferLease.reset();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 0);
  retained.reset();
  pw_loop_iterate(loop, 0);
  EXPECT_EQ(returnedCount, 1);
}
