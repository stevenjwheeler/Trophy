/* Deterministic delivery into the production Application result handler.
 * Application is constructed but never initialized: no services, workers,
 * portal, PipeWire, socket or QML engine are started.
 */
#include "core/Application.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace TrophyBackend;

namespace TrophyBackend {
class ApplicationAudioProbeTest : public ::testing::Test {
protected:
  Application &app = *static_cast<Application *>(QApplication::instance());
  void SetUp() override {
    app.m_audioProbeInflight = false;
    app.m_audioProbeTicket = 0;
    app.m_audioWaitGeneration = 1;
    app.m_captureAvailabilityGeneration = 1;
    app.m_captureAvailable = false;
    app.m_captureAvailabilityPending = true;
  }
  quint64 claim() { return app.claimAudioProbe(); }
  void supersede() { ++app.m_audioWaitGeneration; }
  bool inflight() const { return app.m_audioProbeInflight; }
  void deliver(quint64 ticket, quint64 generation,
               const std::function<void(bool)> &callback) {
    app.applyGameAudioProbeResult(ticket, generation, true, callback);
  }
  void deliverCapture(quint64 generation, bool available) {
    app.applyCaptureAvailability(generation, available);
  }
};
}

TEST_F(ApplicationAudioProbeTest, CaptureAvailabilityRejectsStaleResultAndNotifiesCachedState) {
  QSignalSpy availability(&app, &Application::captureAvailabilityChanged);
  QSignalSpy recording(&app, &Application::recordingAvailabilityChanged);
  deliverCapture(0, true);
  EXPECT_FALSE(app.captureAvailable());
  EXPECT_TRUE(app.captureAvailabilityPending());
  EXPECT_EQ(availability.count(), 0);
  deliverCapture(1, true);
  EXPECT_TRUE(app.captureAvailable());
  EXPECT_FALSE(app.captureAvailabilityPending());
  EXPECT_EQ(availability.count(), 1);
  EXPECT_EQ(recording.count(), 1);
  EXPECT_FALSE(app.canStartRecording()); // No manager/services initialized.
  EXPECT_EQ(app.recordingPhase(), "idle");
}

TEST_F(ApplicationAudioProbeTest, OldCompletionCannotReleaseReplacementClaim) {
  const quint64 old = claim();
  supersede();
  const quint64 current = claim();
  int calls = 0;
  deliver(old, 1, [&](bool) { ++calls; });
  EXPECT_TRUE(inflight());
  EXPECT_EQ(calls, 0);
  deliver(current, 2, [&](bool present) { EXPECT_TRUE(present); ++calls; });
  EXPECT_FALSE(inflight());
  EXPECT_EQ(calls, 1);
}

TEST_F(ApplicationAudioProbeTest, StaleSoleOwnerReleasesClaimWithoutApplying) {
  const quint64 ticket = claim();
  supersede();
  int calls = 0;
  deliver(ticket, 1, [&](bool) { ++calls; });
  EXPECT_FALSE(inflight());
  EXPECT_EQ(calls, 0);
}

TEST_F(ApplicationAudioProbeTest, DuplicateCompletionCannotReleaseNextPoll) {
  const quint64 previous = claim();
  int calls = 0;
  deliver(previous, 1, [&](bool) { ++calls; });
  const quint64 next = claim();
  deliver(previous, 1, [&](bool) { ++calls; });
  EXPECT_TRUE(inflight());
  EXPECT_EQ(calls, 1);
  deliver(next, 1, [&](bool) { ++calls; });
  EXPECT_FALSE(inflight());
  EXPECT_EQ(calls, 2);
}

TEST_F(ApplicationAudioProbeTest, ResultCallbackMayClaimReplacement) {
  const quint64 previous = claim();
  quint64 replacement = 0;
  int calls = 0;
  deliver(previous, 1, [&](bool) { ++calls; replacement = claim(); });
  EXPECT_TRUE(inflight());
  deliver(previous, 1, [&](bool) { ++calls; });
  EXPECT_TRUE(inflight());
  EXPECT_EQ(calls, 1);
  deliver(replacement, 1, [&](bool) { ++calls; });
  EXPECT_FALSE(inflight());
  EXPECT_EQ(calls, 2);
}

int main(int argc, char **argv) {
  const QString tempRoot = QDir("/tmp/opencode").exists()
                               ? QStringLiteral("/tmp/opencode") : QDir::tempPath();
  QTemporaryDir isolation(tempRoot + "/trophy-app-probe-XXXXXX");
  if (!isolation.isValid())
    return 2;
  for (const char *key : {"XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME",
                          "XDG_RUNTIME_DIR"}) {
    const QString path = isolation.path() + "/" + key;
    if (!QDir().mkpath(path))
      return 2;
    if (!QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
      return 2;
    qputenv(key, path.toUtf8());
  }
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  Application app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
