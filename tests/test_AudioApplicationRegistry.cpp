/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio/AudioApplicationRegistry.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <QScopeGuard>
#include <gtest/gtest.h>
#include <atomic>

namespace {
std::atomic<int> publicationDebugMessages{0};
}

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
TEST_F(AudioApplicationRegistryTest, PlaybackStreamChurnNotifiesWithoutRoutineDebugLogging) {
  AudioApplicationRegistry registry;
  QSignalSpy apps(&registry, &AudioApplicationRegistry::applicationsChanged);
  publicationDebugMessages = 0;
  const auto previous = qInstallMessageHandler(
      [](QtMsgType type, const QMessageLogContext &, const QString &) {
        if (type == QtDebugMsg)
          ++publicationDebugMessages;
      });
  const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
  const QVariantMap game{{"binary", "game"}, {"nodeId", 17}};
  const QVariantMap transient{{"binary", "browser"}, {"nodeId", 18}};
  accept(registry, {game});
  accept(registry, {game, transient});
  accept(registry, {game});
  accept(registry, {game});
  EXPECT_EQ(apps.count(), 3);
  EXPECT_EQ(registry.applications(), QVariantList{game});
  EXPECT_EQ(publicationDebugMessages.load(), 0);
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
