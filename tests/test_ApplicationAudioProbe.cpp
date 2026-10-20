/* Deterministic delivery into the production Application result handler.
 * Application is never initialized: no portal, PipeWire, socket or QML engine.
 * The shared-action case explicitly owns a fake recording session worker.
 */
#include "core/Application.h"
#include "core/RecordingManager.h"
#include "core/RecordingSession.h"
#include "core/NotificationService.h"
#include "encoding/IEncoder.h"
#include "muxing/IMuxer.h"
#include "library/RecordingAnnotations.h"
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace TrophyBackend;

namespace {
class ActionCapture : public IVideoCaptureSource {
public:
  FrameCallback callback;
  bool initialize(const CaptureConfig &) override { return true; }
  void setFrameCallback(FrameCallback value) override { callback = std::move(value); }
  bool startCapture() override { return true; }
  void stopCapture() override {}
  bool isCapturing() const override { return true; }
  CaptureCapabilities capabilities() const override { return {}; }
  QSize nativeResolution() const override { return {64, 64}; }
  QString name() const override { return "fake"; }
};
class ActionEncoder : public IEncoder {
public:
  bool initialize(const EncoderConfig &) override { return true; }
  bool encodeFrame(const VideoFrame &, EncodedPacket &) override { return false; }
  int flush(std::vector<EncodedPacket> &) override { return 0; }
  EncoderConfig config() const override { return {}; }
  EncoderType type() const override { return EncoderType::Software_x264; }
  QString name() const override { return "fake"; }
  bool isHardwareEncoder() const override { return false; }
  std::vector<uint8_t> extraData() const override { return {}; }
  int codecId() const override { return 27; }
  int encodedPixelFormat() const override { return 0; }
};
class ActionMuxer : public IMuxer {
public:
  bool initialize(const QString &) override { return true; }
  int addStream(const StreamInfo &) override { return count++; }
  bool writeHeader() override { return true; }
  bool writePacket(const EncodedPacket &) override { return true; }
  bool finalize() override { return true; }
  QString outputPath() const override { return {}; }
  int64_t bytesWritten() const override { return 0; }
  QString formatName() const override { return "fake"; }
  int count = 0;
};
}

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
  void TearDown() override {
    app.m_recordingManager.reset();
    app.m_notificationService.reset();
  }
  void sharedClipBookmarksWithoutDvr(const QString &path) {
    app.m_notificationService = std::make_unique<NotificationService>();
    app.m_notificationService->setEnabled(false); // Never notify the real session.
    app.m_recordingManager = std::make_unique<RecordingManager>();
    auto &manager = *app.m_recordingManager;
    manager.m_currentFilePath = path;
    manager.m_captureSource = std::make_unique<ActionCapture>();
    manager.m_videoEncoder = std::make_unique<ActionEncoder>();
    manager.m_muxer = std::make_unique<ActionMuxer>();
    manager.m_session = std::make_unique<RecordingSession>(path, manager.m_captureSource.get(),
        manager.m_videoEncoder.get(), manager.m_muxer.get(), nullptr, true, 128, 0);
    ASSERT_TRUE(manager.m_session->start());
    manager.m_state = RecordingManager::RecordingState::Recording;
    auto *capture = static_cast<ActionCapture *>(manager.m_captureSource.get());
    VideoFrame frame{}; frame.timestamp = 90000000;
    capture->callback(frame);
    ASSERT_FALSE(app.m_replayManager);
    ASSERT_FALSE(app.m_settingsManager);
    app.requestReplay(); // Must route before any DVR/settings availability guard.
    const auto marks = RecordingAnnotations::bookmarks(path);
    ASSERT_EQ(marks.size(), 1);
    EXPECT_EQ(marks[0].toMap()["positionMs"].toLongLong(), 0);
    manager.stopRecording();
    app.requestReplay();
    EXPECT_EQ(RecordingAnnotations::bookmarks(path).size(), 1);
    app.m_recordingManager.reset(); // Join native retirement while fixtures exist.
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

TEST_F(ApplicationAudioProbeTest, SharedClipKeyBookmarksActiveRecordingWithoutDvr) {
  QTemporaryDir directory;
  const auto path = directory.filePath("recording.mkv");
  QFile file(path); ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
  sharedClipBookmarksWithoutDvr(path);
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
