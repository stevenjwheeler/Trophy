/*
 * Trophy - Unit Tests
 * EncoderFactory: detection list sanity, named creation, fallback behavior.
 */

#include <QCoreApplication>
#include <gtest/gtest.h>

#include "encoding/EncoderFactory.h"
#include "encoding/IEncoder.h"

using namespace TrophyBackend;

class EncoderFactoryTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new EncoderFactoryTestEnvironment);

TEST(EncoderFactoryTest, DetectionReturnsPrioritizedList) {
  auto encoders = EncoderFactory::detectAvailableEncoders();
  ASSERT_FALSE(encoders.empty()) << "at least software x264 is expected";

  // Sorted by ascending priority (hardware first, software last).
  for (size_t i = 1; i < encoders.size(); ++i)
    EXPECT_LE(encoders[i - 1].priority, encoders[i].priority);

  for (const auto &e : encoders) {
    EXPECT_FALSE(e.name.isEmpty());
    EXPECT_FALSE(e.description.isEmpty());
  }
}

TEST(EncoderFactoryTest, IsAvailableConsistentWithDetection) {
  auto encoders = EncoderFactory::detectAvailableEncoders();
  for (const auto &e : encoders)
    EXPECT_TRUE(EncoderFactory::isAvailable(e.type)) << e.name.toStdString();
}

TEST(EncoderFactoryTest, CreateX264WhenAvailable) {
  if (!EncoderFactory::isAvailable(IEncoder::EncoderType::Software_x264))
    GTEST_SKIP() << "libx264 unavailable";
  auto enc = EncoderFactory::create("x264");
  ASSERT_NE(enc, nullptr);
  EXPECT_FALSE(enc->isHardwareEncoder());
  EXPECT_EQ(enc->type(), IEncoder::EncoderType::Software_x264);
}

TEST(EncoderFactoryTest, CreateAutoFallsBackToBest) {
  auto enc = EncoderFactory::create("auto");
  ASSERT_NE(enc, nullptr) << "auto must always resolve to something usable";
}

TEST(EncoderFactoryTest, UnknownNameFallsBackInsteadOfNull) {
  auto enc = EncoderFactory::create("does-not-exist-encoder");
  ASSERT_NE(enc, nullptr) << "unknown names must fall back to best";
}

TEST(EncoderFactoryTest, CreateBestHonorsSoftwarePreference) {
  if (!EncoderFactory::isAvailable(IEncoder::EncoderType::Software_x264))
    GTEST_SKIP() << "libx264 unavailable";
  auto sw = EncoderFactory::createBest(false);
  ASSERT_NE(sw, nullptr);
  EXPECT_FALSE(sw->isHardwareEncoder());
}
