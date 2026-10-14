#include <gtest/gtest.h>

#include "audio/PipeWireAudioSource.h"

using TrophyBackend::detail::waitForPipeWireSync;

TEST(PipeWireSyncWaitTest, CompletesWhenSyncArrives) {
  int iterations = 0;
  bool complete = false;
  EXPECT_TRUE(waitForPipeWireSync(
      [&](int waitMs) {
        EXPECT_GT(waitMs, 0);
        complete = ++iterations == 3;
        return 0;
      },
      [&] { return complete; }, 100));
  EXPECT_EQ(iterations, 3);
}

TEST(PipeWireSyncWaitTest, StopsOnIteratorError) {
  int iterations = 0;
  EXPECT_FALSE(waitForPipeWireSync(
      [&](int) {
        ++iterations;
        return -1;
      },
      [] { return false; }, 100));
  EXPECT_EQ(iterations, 1);
}

TEST(PipeWireSyncWaitTest, TimesOutWhenSyncNeverArrives) {
  int iterations = 0;
  EXPECT_FALSE(waitForPipeWireSync(
      [&](int) {
        ++iterations;
        return 0;
      },
      [] { return false; }, 5));
  EXPECT_GT(iterations, 0);
}
