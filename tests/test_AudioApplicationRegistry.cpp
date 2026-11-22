/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio/AudioApplicationRegistry.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <gtest/gtest.h>

namespace TrophyBackend {
class AudioApplicationRegistryTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "audio-registry-test";
      static char *argv[] = {name, nullptr};
      static QCoreApplication app(argc, argv);
    }
  }
  static void accept(AudioApplicationRegistry &registry, const QVariantList &rows,
                     bool available = true) {
    registry.m_running = true; // No native worker for publication-only tests.
    registry.publish(registry.m_generation, rows, available);
  }
  static void stale(AudioApplicationRegistry &registry, const QVariantList &rows) {
    registry.publish(registry.m_generation - 1, rows, true);
  }
};
TEST_F(AudioApplicationRegistryTest, OnlyChangedSnapshotsNotifyAndOldGenerationsAreIgnored) {
  AudioApplicationRegistry registry;
  QSignalSpy apps(&registry, &AudioApplicationRegistry::applicationsChanged);
  QSignalSpy availability(&registry, &AudioApplicationRegistry::availabilityChanged);
  const QVariantList rows{QVariantMap{{"binary", "game"}, {"nodeId", 17}}};
  accept(registry, rows);
  EXPECT_EQ(apps.count(), 1);
  EXPECT_EQ(availability.count(), 1);
  accept(registry, rows);
  EXPECT_EQ(apps.count(), 1);
  stale(registry, {});
  EXPECT_EQ(registry.applications(), rows);
  accept(registry, {}, false);
  EXPECT_TRUE(registry.applications().isEmpty());
  EXPECT_EQ(apps.count(), 2);
  EXPECT_EQ(availability.count(), 2);
  EXPECT_FALSE(registry.available());
}
TEST_F(AudioApplicationRegistryTest, StopClearsSnapshotAndRejectsQueuedPublications) {
  AudioApplicationRegistry registry;
  const QVariantList rows{QVariantMap{{"nodeId", 17}}};
  accept(registry, rows);
  registry.stop();
  EXPECT_TRUE(registry.applications().isEmpty());
  EXPECT_FALSE(registry.available());
  stale(registry, rows);
  EXPECT_TRUE(registry.applications().isEmpty());
}
TEST_F(AudioApplicationRegistryTest, ReentrantStopOnAvailabilityDoesNotRepublishStaleApps) {
  AudioApplicationRegistry registry;
  QObject::connect(&registry, &AudioApplicationRegistry::availabilityChanged,
                   &registry, [&] { if (registry.available()) registry.stop(); });
  accept(registry, {QVariantMap{{"nodeId", 17}}});
  EXPECT_TRUE(registry.applications().isEmpty());
  EXPECT_FALSE(registry.available());
}
} // namespace TrophyBackend
