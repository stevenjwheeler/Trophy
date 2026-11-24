/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <thread>
#include "PipeWireLeaseFixture.h"

TEST(PipeWireBufferLeaseTest, WorkerReleaseReturnsBufferOnceOnNativeLoop) {
  PipeWireLeaseFixture f;
  ASSERT_TRUE(f.leases);
  pw_buffer buffer{};
  auto lease = f.leases->retain(&buffer);
  auto consumer = lease;
  lease.reset();
  f.drain();
  EXPECT_TRUE(f.returned.empty());
  std::thread worker([held = std::move(consumer)]() mutable { held.reset(); });
  worker.join();
  EXPECT_TRUE(f.returned.empty()); // Not returned on the worker thread.
  f.drain();
  ASSERT_EQ(f.returned.size(), 1u);
  EXPECT_EQ(f.returned[0], &buffer);
  f.drain();
  EXPECT_EQ(f.returned.size(), 1u);
}

TEST(PipeWireBufferLeaseTest, RemovedBufferIsNotReturnedEvenIfReleaseWasAlreadyQueued) {
  PipeWireLeaseFixture f;
  ASSERT_TRUE(f.leases);
  pw_buffer buffer{};
  auto lease = f.leases->retain(&buffer);
  lease.reset(); // Native return is pending, not delivered yet.
  f.leases->removed(&buffer);
  f.drain();
  EXPECT_TRUE(f.returned.empty());
}

TEST(PipeWireBufferLeaseTest, FallbackIsOneShotAndLateLeasesAreHarmlessAfterDeactivation) {
  PipeWireLeaseFixture f;
  ASSERT_TRUE(f.leases);
  pw_buffer buffer{};
  auto lease = f.leases->retain(&buffer);
  f.leases->requestSharedMemory();
  f.leases->requestSharedMemory();
  f.drain();
  EXPECT_EQ(f.fallbacks, 1);
  f.leases->requestSharedMemory();
  f.drain();
  EXPECT_EQ(f.fallbacks, 1);
  f.leases->deactivate();
  std::thread worker([held = std::move(lease)]() mutable { held.reset(); });
  worker.join();
  f.leases->requestSharedMemory();
  f.drain();
  EXPECT_TRUE(f.returned.empty());
  EXPECT_EQ(f.fallbacks, 1);
}
