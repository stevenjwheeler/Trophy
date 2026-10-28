#include "capture/EglDisplayLease.h"
#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <thread>
#include <vector>

using namespace TrophyBackend;

namespace {
EGLDisplay display(unsigned value) { return reinterpret_cast<EGLDisplay>(uintptr_t(value)); }
struct FakeEgl {
  std::mutex mutex;
  std::condition_variable changed;
  int initialized = 0, terminated = 0, violations = 0;
  bool failNext = false, blockTerminate = false, entered = false, release = false;
  std::unordered_map<EGLDisplay, bool> active;
  EGLBoolean initialize(EGLDisplay value) {
    std::lock_guard lock(mutex);
    ++initialized;
    if (failNext) { failNext = false; return EGL_FALSE; }
    if (active[value]) ++violations;
    active[value] = true;
    return EGL_TRUE;
  }
  EGLBoolean terminate(EGLDisplay value) {
    std::unique_lock lock(mutex);
    ++terminated;
    if (blockTerminate) {
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release; });
    }
    if (!active[value]) ++violations;
    active[value] = false;
    return EGL_TRUE;
  }
};
EglDisplayRegistry registry(const std::shared_ptr<FakeEgl> &fake) {
  return EglDisplayRegistry(
      [fake](EGLDisplay value, EGLint *, EGLint *) { return fake->initialize(value); },
      [fake](EGLDisplay value) { return fake->terminate(value); });
}
}

TEST(EglDisplayLeaseTest, SameNativeHandleInitializesOnceAndTerminatesAfterLastOwner) {
  auto fake = std::make_shared<FakeEgl>();
  auto pool = registry(fake);
  auto first = pool.acquire(display(1));
  auto query = pool.acquire(display(1)); // aliases are keyed by native handle, not a path
  ASSERT_TRUE(first); ASSERT_TRUE(query);
  EXPECT_EQ(first.get(), query.get());
  EXPECT_EQ(fake->initialized, 1);
  first.reset();
  EXPECT_EQ(fake->terminated, 0);
  EXPECT_TRUE(fake->active[display(1)]);
  query.reset();
  EXPECT_EQ(fake->terminated, 1);
  EXPECT_FALSE(fake->active[display(1)]);
  query.reset();
  EXPECT_EQ(fake->terminated, 1);
}

TEST(EglDisplayLeaseTest, MovesTransferOwnershipWithoutReinitializationOrEarlyTermination) {
  auto fake = std::make_shared<FakeEgl>(); auto pool = registry(fake);
  auto first = pool.acquire(display(1));
  auto second = pool.acquire(display(1));
  EglDisplayLease moved(std::move(first));
  EXPECT_FALSE(first); EXPECT_TRUE(moved);
  second = std::move(moved);
  EXPECT_FALSE(moved); EXPECT_TRUE(second);
  EXPECT_EQ(fake->initialized, 1); EXPECT_EQ(fake->terminated, 0);
  second = {};
  EXPECT_EQ(fake->terminated, 1);
}

TEST(EglDisplayLeaseTest, SeparateDisplaysHaveIndependentLifetimes) {
  auto fake = std::make_shared<FakeEgl>(); auto pool = registry(fake);
  auto first = pool.acquire(display(1)); auto second = pool.acquire(display(2));
  EXPECT_EQ(fake->initialized, 2);
  first.reset();
  EXPECT_FALSE(fake->active[display(1)]); EXPECT_TRUE(fake->active[display(2)]);
  EXPECT_EQ(fake->terminated, 1);
  second.reset();
  EXPECT_EQ(fake->terminated, 2);
  EXPECT_EQ(fake->violations, 0);
}

TEST(EglDisplayLeaseTest, FailedInitializeDoesNotCreateAnOwnerAndCanRetry) {
  auto fake = std::make_shared<FakeEgl>(); auto pool = registry(fake);
  auto healthy = pool.acquire(display(1));
  fake->failNext = true;
  auto failed = pool.acquire(display(2));
  EXPECT_FALSE(failed); EXPECT_EQ(fake->terminated, 0);
  EXPECT_TRUE(fake->active[display(1)]);
  auto retry = pool.acquire(display(2));
  EXPECT_TRUE(retry); EXPECT_EQ(fake->initialized, 3);
  retry.reset(); healthy.reset();
  EXPECT_EQ(fake->terminated, 2);
}

TEST(EglDisplayLeaseTest, LeaseRetainsRegistryStateUntilItsOwnRelease) {
  auto fake = std::make_shared<FakeEgl>(); EglDisplayLease lease;
  { auto pool = registry(fake); lease = pool.acquire(display(1)); }
  EXPECT_EQ(fake->terminated, 0);
  lease.reset();
  EXPECT_EQ(fake->terminated, 1);
}

TEST(EglDisplayLeaseTest, EmptyDisplayAndMovedFromLeasesAreNoOps) {
  auto fake = std::make_shared<FakeEgl>(); auto pool = registry(fake);
  auto empty = pool.acquire(EGL_NO_DISPLAY);
  EXPECT_FALSE(empty); empty.reset();
  EglDisplayLease moved(std::move(empty)); moved.reset();
  EXPECT_EQ(fake->initialized, 0); EXPECT_EQ(fake->terminated, 0);
}

TEST(EglDisplayLeaseTest, ConcurrentAcquisitionsCannotTerminateAnExistingOwner) {
  auto fake = std::make_shared<FakeEgl>(); auto pool = registry(fake);
  auto owner = pool.acquire(display(1));
  std::barrier start(8);
  std::vector<std::thread> threads;
  std::atomic<int> successes{0};
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
      start.arrive_and_wait();
      for (int iteration = 0; iteration < 100; ++iteration) {
        auto lease = pool.acquire(display(1));
        if (lease.get() == display(1)) ++successes;
      }
    });
  }
  for (auto &thread : threads) thread.join();
  EXPECT_EQ(successes, 800);
  EXPECT_EQ(fake->initialized, 1); EXPECT_EQ(fake->terminated, 0);
  owner.reset();
  EXPECT_EQ(fake->terminated, 1); EXPECT_EQ(fake->violations, 0);
}

TEST(EglDisplayLeaseTest, LastReleaseAndNewAcquisitionSerializeNativeTerminationAndInit) {
  auto fake = std::make_shared<FakeEgl>(); auto pool = registry(fake);
  auto owner = pool.acquire(display(1));
  fake->blockTerminate = true;
  std::thread retiring([lease = std::move(owner)]() mutable { lease.reset(); });
  bool entered;
  {
    std::unique_lock lock(fake->mutex);
    entered = fake->changed.wait_for(lock, std::chrono::seconds(2), [&] { return fake->entered; });
  }
  std::atomic<bool> attempting{false}, acquired{false};
  std::thread successor([&] {
    attempting = true;
    auto lease = pool.acquire(display(1));
    acquired = bool(lease);
  });
  while (!attempting.load()) std::this_thread::yield();
  // Release the gate unconditionally so failed assertions never strand threads.
  bool acquiredBeforeRelease;
  {
    std::lock_guard lock(fake->mutex);
    acquiredBeforeRelease = acquired.load();
    fake->release = true;
    fake->changed.notify_all();
  }
  retiring.join(); successor.join();
  EXPECT_TRUE(entered); EXPECT_FALSE(acquiredBeforeRelease); EXPECT_TRUE(acquired);
  EXPECT_EQ(fake->initialized, 2); EXPECT_EQ(fake->terminated, 2);
  EXPECT_EQ(fake->violations, 0);
}

TEST(EglDisplayLeaseTest, ConcurrentUnpinnedStartStopDoesNotDoubleInitialize) {
  auto fake = std::make_shared<FakeEgl>(); auto pool = registry(fake);
  std::barrier start(6);
  std::vector<std::thread> threads;
  for (int i = 0; i < 6; ++i)
    threads.emplace_back([&] {
      start.arrive_and_wait();
      for (int iteration = 0; iteration < 100; ++iteration) {
        auto lease = pool.acquire(display(1));
        std::this_thread::yield();
      }
    });
  for (auto &thread : threads) thread.join();
  EXPECT_EQ(fake->initialized, fake->terminated);
  EXPECT_GT(fake->initialized, 0);
  EXPECT_EQ(fake->violations, 0);
}
