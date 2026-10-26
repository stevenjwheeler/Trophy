/*
 * Trophy - Unit Tests
 * AudioDriftCorrector: wall-clock servo over a fake clock. A source
 * running 119ppm fast (measured live on the game track) must converge
 * to wall rate; a perfect source must pass through untouched.
 */

#include <QCoreApplication>
#include <cmath>
#include <gtest/gtest.h>
#include <vector>

#include "audio/AudioDriftCorrector.h"

using namespace TrophyBackend;

class DriftCorrectorTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new DriftCorrectorTestEnvironment);

class AudioDriftCorrectorTest : public ::testing::Test {
protected:
  static constexpr int kRate = 48000;
  static constexpr int kChannels = 2;
  static constexpr int kChunk = 1024;

  double nowExactUs = 0.0; // fractional accumulator: quantizing the
                           // clock per chunk would itself drift ~125ppm
                           // and swamp the measurement
  std::vector<float> sine;

  void SetUp() override {
    ASSERT_TRUE(true);
    sine.resize(static_cast<size_t>(kChunk) * kChannels);
    for (size_t i = 0; i < sine.size(); ++i)
      sine[i] = 0.2f * std::sin(2.0f * 3.14159265f * i / 48.0f);
  }

  AudioDriftCorrector makeCorrector() {
    return AudioDriftCorrector(kRate, kChannels,
                               [this] { return (int64_t)nowExactUs; });
  }

  // Feed totalFrames with the wall clock advancing as if the source
  // ran at rate*rateScale (1.0 = perfect, 1.000119 = 119ppm fast).
  // Returns total corrected output frames.
  int64_t run(AudioDriftCorrector &corr, int64_t totalFrames,
              double rateScale) {
    int64_t fed = 0, got = 0;
    const float *out = nullptr;
    while (fed < totalFrames) {
      const int n =
          static_cast<int>(std::min<int64_t>(kChunk, totalFrames - fed));
      const int outFrames = corr.process(sine.data(), n, out);
      EXPECT_GE(outFrames, 0);
      EXPECT_NE(out, nullptr);
      got += outFrames;
      fed += n;
      // Wall time for the n input frames at the *source's* true rate.
      nowExactUs += n * 1000000.0 / (kRate * rateScale);
    }
    return got;
  }
};

TEST_F(AudioDriftCorrectorTest, PerfectSourcePassesThrough) {
  AudioDriftCorrector corr = makeCorrector();
  ASSERT_TRUE(corr.valid());
  const int64_t totalIn = static_cast<int64_t>(kRate) * 60; // 60s
  const int64_t totalOut = run(corr, totalIn, 1.0);
  // SWR priming delay only: no correction inside the deadband.
  EXPECT_LE(std::llabs(totalOut - totalIn), 256);
}

TEST_F(AudioDriftCorrectorTest, FastSourceConvergesToWallRate) {
  AudioDriftCorrector corr = makeCorrector();
  ASSERT_TRUE(corr.valid());
  // 10 wall-minutes at 119ppm fast (the live-measured game-track skew).
  const int64_t wallFrames = static_cast<int64_t>(kRate) * 600;
  const int64_t fedFrames =
      static_cast<int64_t>(wallFrames * 1.000119);
  const int64_t totalOut = run(corr, fedFrames, 1.000119);
  // Output follows the wall clock, not the crystal...
  EXPECT_LE(std::llabs(totalOut - wallFrames), 2048);
  // ...by removing the excess (not by passing it through).
  EXPECT_LT(totalOut, fedFrames - 1000);
}

TEST_F(AudioDriftCorrectorTest, SlowSourceConvergesToWallRate) {
  AudioDriftCorrector corr = makeCorrector();
  ASSERT_TRUE(corr.valid());
  const int64_t wallFrames = static_cast<int64_t>(kRate) * 600;
  const int64_t fedFrames =
      static_cast<int64_t>(wallFrames * 0.999881);
  const int64_t totalOut = run(corr, fedFrames, 0.999881);
  EXPECT_LE(std::llabs(totalOut - wallFrames), 2048);
  EXPECT_GT(totalOut, fedFrames + 1000);
}

TEST_F(AudioDriftCorrectorTest, CorrectionRateIsBounded) {
  AudioDriftCorrector corr = makeCorrector();
  ASSERT_TRUE(corr.valid());
  // Prime, then dump an hour of audio "instantly": the error is huge
  // but each call may only trim a bounded inaudible slice.
  const float *out = nullptr;
  EXPECT_GT(corr.process(sine.data(), kChunk, out), 0);
  nowExactUs += 1000;
  for (int i = 0; i < 10; ++i) {
    const int got = corr.process(sine.data(), kChunk, out);
    EXPECT_GE(got, static_cast<int>(kChunk * 0.99));
    nowExactUs += 1000;
  }
}

TEST_F(AudioDriftCorrectorTest, LongSilenceRebaselines) {
  AudioDriftCorrector corr = makeCorrector();
  ASSERT_TRUE(corr.valid());
  const float *out = nullptr;
  // Phase 1: 10s of perfect audio.
  int64_t got1 = 0;
  for (int i = 0; i < 10 * kRate / kChunk; ++i) {
    got1 += corr.process(sine.data(), kChunk, out);
    nowExactUs += kChunk * 1000000.0 / kRate;
  }
  // Phase 2: 60s mute gap (no callbacks at all).
  nowExactUs += 60000000.0;
  // Phase 3: audio resumes; output must track input again, not try to
  // "correct" the minute of silence.
  int64_t got3 = 0;
  const int64_t in3 = static_cast<int64_t>(kRate) * 10;
  int64_t fed = 0;
  while (fed < in3) {
    got3 += corr.process(sine.data(), kChunk, out);
    fed += kChunk;
    nowExactUs += kChunk * 1000000.0 / kRate;
  }
  EXPECT_LE(std::llabs(got3 - in3), 512);
  (void)got1;
}

TEST_F(AudioDriftCorrectorTest, InvalidInputIsSafe) {
  AudioDriftCorrector corr = makeCorrector();
  const float *out = reinterpret_cast<const float *>(0x1234);
  EXPECT_EQ(corr.process(nullptr, kChunk, out), 0);
  EXPECT_EQ(corr.process(sine.data(), 0, out), 0);
  EXPECT_EQ(corr.process(sine.data(), -5, out), 0);
}

TEST_F(AudioDriftCorrectorTest, ExplicitResetMatchesFreshServoWithoutOldPcm) {
  auto corrector = makeCorrector();
  ASSERT_TRUE(corrector.valid());
  run(corrector, kRate, 1.0);
  nowExactUs += 2000000;
  corrector.reset();
  ASSERT_TRUE(corrector.valid());
  auto fresh = makeCorrector();
  ASSERT_TRUE(fresh.valid());
  std::vector<float> silence(kChunk * kChannels, 0);
  for (int block = 0; block < 20; ++block) {
    const float *resetOutput = nullptr, *freshOutput = nullptr;
    const int resetFrames = corrector.process(silence.data(), kChunk, resetOutput);
    const int freshFrames = fresh.process(silence.data(), kChunk, freshOutput);
    ASSERT_EQ(resetFrames, freshFrames);
    for (int i = 0; i < resetFrames * kChannels; ++i)
      EXPECT_FLOAT_EQ(resetOutput[i], freshOutput[i]);
    nowExactUs += kChunk * 1000000.0 / kRate;
  }
}
