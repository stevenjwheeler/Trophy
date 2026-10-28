/*
 * Trophy - Unit Tests
 * Test suite for RecordingManager storage enforcement behavior
 */

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <gtest/gtest.h>

#include "core/RecordingManager.h"
#include "library/RecordingAnnotations.h"
#include "capture/IVideoCaptureSource.h"
#include "core/StorageManager.h"
#include "settings/SettingsManager.h"
#include "muxing/IMuxer.h"
#include "core/RecordingSession.h"
#include "encoding/IEncoder.h"
#include "audio/AudioMixer.h"
#include "audio/IAudioSource.h"
#include "detection/GameDetectionService.h"
#include "detection/WaylandWindowMonitor.h"

using namespace TrophyBackend;

class RecordingManagerTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new RecordingManagerTestEnvironment);

// No portal or encoder is involved: capture waits until the test emits a real
// IVideoCaptureSource signal, using the manager's production connections.
class PendingCapture : public IVideoCaptureSource {
public:
  CaptureErrorKind errorKind = CaptureErrorKind::Other;
  CaptureErrorKind lastErrorKind() const override { return errorKind; }
  CaptureConfig config;
  mutable int resolutionQueries = 0;
  bool initialize(const CaptureConfig &value) override {
    config = value;
    return true;
  }
  void setFrameCallback(FrameCallback) override {}
  bool startCapture() override { return true; }
  void stopCapture() override {}
  bool isCapturing() const override { return true; }
  CaptureCapabilities capabilities() const override { return {}; }
  QSize nativeResolution() const override {
    ++resolutionQueries;
    return {};
  }
  QString name() const override { return QStringLiteral("Pending capture"); }
};

class BookmarkCapture : public PendingCapture {
public:
  FrameCallback callback;
  void setFrameCallback(FrameCallback value) override { callback = std::move(value); }
};

class FinalizationMuxer : public IMuxer {
public:
  explicit FinalizationMuxer(bool succeeds, bool writesSucceed = true)
      : m_succeeds(succeeds), m_writesSucceed(writesSucceed) {}
  bool initialize(const QString &) override { return true; }
  int addStream(const StreamInfo &) override { return 0; }
  bool writeHeader() override { return true; }
  bool writePacket(const EncodedPacket &) override { return m_writesSucceed; }
  bool finalize() override { return m_succeeds; }
  QString outputPath() const override { return {}; }
  int64_t bytesWritten() const override { return 0; }
  QString formatName() const override { return "fake"; }
private:
  bool m_succeeds;
  bool m_writesSucceed;
};

struct ManagerOperationGate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false, released = false;
  std::atomic<int> destructions{0};
  std::atomic<int> lookupCalls{0};
  QString firstVoice;
  void block() {
    std::unique_lock lock(mutex);
    entered = true;
    cv.notify_all();
    EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return released; }));
  }
  bool waitForEntry() {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(3), [&] { return entered; });
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    cv.notify_all();
  }
};

class GatedManagerMuxer : public FinalizationMuxer {
public:
  GatedManagerMuxer(std::shared_ptr<ManagerOperationGate> gate, bool succeeds)
      : FinalizationMuxer(succeeds), m_gate(std::move(gate)) {}
  ~GatedManagerMuxer() override { ++m_gate->destructions; }
  bool finalize() override {
    if (!m_finalized) {
      m_gate->block();
      m_finalized = true;
    }
    return FinalizationMuxer::finalize();
  }
private:
  std::shared_ptr<ManagerOperationGate> m_gate;
  bool m_finalized = false;
};

class ManagerOutputEncoder : public IEncoder {
public:
  bool initialize(const EncoderConfig &) override { return true; }
  bool encodeFrame(const VideoFrame &, EncodedPacket &) override { return false; }
  int flush(std::vector<EncodedPacket> &packets) override {
    EncodedPacket packet;
    packet.data = {1};
    packets.push_back(packet);
    return 1;
  }
  EncoderConfig config() const override { return {}; }
  EncoderType type() const override { return EncoderType::Software_x264; }
  QString name() const override { return "fake"; }
  bool isHardwareEncoder() const override { return false; }
  std::vector<uint8_t> extraData() const override { return {}; }
  int codecId() const override { return 27; }
  int encodedPixelFormat() const override { return 0; }
};

class ManagerRoleSource : public IAudioSource {
public:
  ManagerRoleSource(SourceType type, bool &destroyed) : m_type(type), m_destroyed(destroyed) {}
  ~ManagerRoleSource() override { m_destroyed = true; }
  bool initialize(const AudioConfig &) override { return true; }
  void setCallback(AudioCallback) override {}
  bool start() override { m_capturing = true; return true; }
  void stop() override { m_capturing = false; }
  bool isCapturing() const override { return m_capturing; }
  SourceType sourceType() const override { return m_type; }
  QString displayName() const override { return "role-source"; }
  QString identifier() const override { return "role-source"; }
  float peakLevel() const override { return 0; }
  void setVolume(float) override {}
  float volume() const override { return 1; }
private:
  SourceType m_type;
  bool &m_destroyed;
  std::atomic<bool> m_capturing{true};
};

class RecordingManagerTest : public ::testing::Test {
protected:
  // NOTE: declared first so it outlives everything that reads config paths.
  QTemporaryDir configHome;
  QByteArray oldXdgConfigHome;
  QTemporaryDir tempDir;

  void SetUp() override {
    ASSERT_TRUE(tempDir.isValid());
    ASSERT_TRUE(configHome.isValid());
    // Isolate from the user's real ~/.config/trophy config: SettingsManager
    // saves on destruction, which must never touch the real config dir.
    oldXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
  }

  void TearDown() override {
    if (oldXdgConfigHome.isNull())
      qunsetenv("XDG_CONFIG_HOME");
    else
      qputenv("XDG_CONFIG_HOME", oldXdgConfigHome);
  }

  void createDummyFile(const QString &name, qint64 size) {
    QFile f(tempDir.path() + "/" + name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(size, 'x'));
    f.close();
  }

  QVariantMap diagnosticHealth(RecordingManager &recorder) {
    // Exercise the real snapshot publication without constructing a codec.
    const auto state = recorder.m_state;
    recorder.m_session = std::make_unique<RecordingSession>(
        "unused.mkv", recorder.m_captureSource.get(), nullptr, nullptr, nullptr, false, 128, 0);
    recorder.m_state = RecordingManager::RecordingState::Recording;
    const auto result = recorder.captureHealth();
    recorder.m_state = state;
    recorder.m_session.reset();
    return result;
  }

  void bookmarkLifecycle() {
    createDummyFile("bookmarked.mkv", 64);
    const QString path = tempDir.filePath("bookmarked.mkv");
    StorageManager storage(tempDir.path());
    RecordingManager recorder;
    recorder.setStorageManager(&storage);
    EXPECT_FALSE(recorder.addBookmark());
    recorder.m_currentFilePath = path;
    auto capture = std::make_unique<BookmarkCapture>();
    auto *source = capture.get();
    recorder.m_captureSource = std::move(capture);
    recorder.m_videoEncoder = std::make_unique<ManagerOutputEncoder>();
    recorder.m_muxer = std::make_unique<FinalizationMuxer>(true);
    recorder.m_session = std::make_unique<RecordingSession>(
        path, source, recorder.m_videoEncoder.get(), recorder.m_muxer.get(),
        nullptr, true, 128, 0);
    ASSERT_TRUE(recorder.m_session->start());
    recorder.m_recordingTimer.start();
    recorder.setState(RecordingManager::RecordingState::Recording);
    EXPECT_FALSE(recorder.addBookmark()); // No media origin yet.
    ASSERT_TRUE(bool(source->callback));
    VideoFrame frame{};
    frame.timestamp = 90000000;
    source->callback(frame);
    ASSERT_TRUE(recorder.addBookmark());
    const auto marks = RecordingAnnotations::bookmarks(path);
    ASSERT_EQ(marks.size(), 1);
    EXPECT_EQ(marks[0].toMap()["positionMs"].toLongLong(), 0);
    recorder.stopRecording();
    EXPECT_FALSE(recorder.addBookmark());
    ASSERT_TRUE(waitForIdle(recorder));
    EXPECT_EQ(RecordingAnnotations::bookmarks(path).size(), 1);
  }

  bool waitForIdle(RecordingManager &recorder) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (recorder.state() != RecordingManager::RecordingState::Idle &&
           std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return recorder.state() == RecordingManager::RecordingState::Idle;
  }

  void gatedStop(bool succeeds) {
    const QString path = tempDir.path() + "/retained.mkv";
    createDummyFile("retained.mkv", 64);
    StorageManager storage(tempDir.path());
    RecordingManager recorder;
    recorder.setStorageManager(&storage);
    recorder.m_currentFilePath = path;
    recorder.m_currentGameName = "Retained Game";
    storage.setExcludedRecordingPath(path);
    recorder.m_captureSource = std::make_unique<PendingCapture>();
    QPointer<IVideoCaptureSource> capture(recorder.m_captureSource.get());
    recorder.m_videoEncoder = std::make_unique<ManagerOutputEncoder>();
    auto gate = std::make_shared<ManagerOperationGate>();
    recorder.m_muxer = std::make_unique<GatedManagerMuxer>(gate, succeeds);
    recorder.m_session = std::make_unique<RecordingSession>(
        path, recorder.m_captureSource.get(), recorder.m_videoEncoder.get(),
        recorder.m_muxer.get(), nullptr, true, 128, 0);
    ASSERT_TRUE(recorder.m_session->start());
    recorder.m_recordingTimer.start();
    recorder.setState(RecordingManager::RecordingState::Recording);
    QSignalSpy saved(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy finished(&recorder, &RecordingManager::recordingFinished);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    recorder.stopRecording();
    ASSERT_TRUE(gate->waitForEntry());
    const qint64 stoppedDuration = recorder.recordingDuration();
    const qint64 stoppedSize = recorder.recordingSize();
    bool heartbeat = false;
    QTimer::singleShot(0, [&] { heartbeat = true; });
    QCoreApplication::processEvents();
    EXPECT_TRUE(heartbeat);
    EXPECT_EQ(recorder.recordingDuration(), stoppedDuration);
    EXPECT_EQ(recorder.recordingSize(), stoppedSize);
    EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Stopping);
    EXPECT_EQ(recorder.currentFilePath(), path);
    EXPECT_FALSE(capture.isNull());
    EXPECT_EQ(gate->destructions, 0);
    EXPECT_EQ(finished.count(), 0);
    recorder.startRecording("Cannot reuse pipeline");
    recorder.stopRecording();
    EXPECT_EQ(recorder.currentFilePath(), path);
    EXPECT_EQ(errors.count(), 0);
    gate->release();
    ASSERT_TRUE(waitForIdle(recorder));
    EXPECT_TRUE(capture.isNull());
    EXPECT_EQ(gate->destructions, 1);
    EXPECT_EQ(saved.count(), succeeds ? 1 : 0);
    if (succeeds) {
      EXPECT_EQ(saved.at(0).at(1).toLongLong(), stoppedDuration);
    }
    EXPECT_EQ(errors.count(), succeeds ? 0 : 1);
    EXPECT_EQ(finished.count(), 1);
    EXPECT_TRUE(QFileInfo::exists(path)); // Partial outputs are preserved too.
    QCoreApplication::processEvents();
    EXPECT_EQ(finished.count(), 1);
  }

  void gatedStartupLookup(bool cancel = true) {
    SettingsManager settings;
    settings.setRecordingsPath(tempDir.path());
    settings.setVoiceChatApp("Old voice");
    settings.setAudioMode("game");
    StorageManager storage(tempDir.path());
    RecordingManager recorder;
    recorder.setSettingsManager(&settings);
    recorder.setStorageManager(&storage);
    QPointer<PendingCapture> capture;
    installPendingCapture(recorder, capture);
    auto gate = std::make_shared<ManagerOperationGate>();
    recorder.m_audioPreparationLookup = [gate](auto input) {
      if (++gate->lookupCalls == 1) {
        gate->firstVoice = input.voiceApp;
        gate->block();
      }
      return input;
    };
    int builds = 0;
    recorder.m_pipelineBuilder = [&] {
      ++builds;
      EXPECT_EQ(recorder.m_preparedAudio.voiceApp, "New voice");
      EXPECT_EQ(recorder.m_preparedAudio.micDevice, "new-mic");
      EXPECT_FALSE(recorder.m_preparedAudio.separate);
      return true;
    };
    QSignalSpy started(&recorder, &RecordingManager::recordingStarted);
    recorder.startRecording("Old game");
    recorder.finishPipelineAndStart();
    ASSERT_TRUE(gate->waitForEntry());
    EXPECT_EQ(gate->firstVoice, "Old voice");
    bool heartbeat = false;
    QTimer::singleShot(0, [&] { heartbeat = true; });
    QCoreApplication::processEvents();
    EXPECT_TRUE(heartbeat);
    EXPECT_EQ(builds, 0);
    if (cancel) {
      recorder.stopRecording();
      ASSERT_TRUE(waitForIdle(recorder));
    }
    settings.setVoiceChatApp("New voice");
    settings.setMicrophoneDevice("new-mic");
    settings.setSeparateAudioTracks(false);
    if (cancel) {
      recorder.startRecording("Replacement game");
      recorder.finishPipelineAndStart();
    }
    EXPECT_EQ(gate->lookupCalls, 1); // Old lookup retains the single preparation slot.
    gate->release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (recorder.state() == RecordingManager::RecordingState::Initializing &&
           std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Recording);
    EXPECT_EQ(recorder.currentGameName(), cancel ? "Replacement game" : "Old game");
    EXPECT_EQ(gate->lookupCalls, 2);
    EXPECT_EQ(builds, 1);
    EXPECT_EQ(started.count(), 1);
    recorder.stopRecording();
    EXPECT_TRUE(waitForIdle(recorder));
  }

  void disableLateAdoptedVoice() {
    SettingsManager settings;
    settings.setAudioMode("game");
    settings.setSeparateAudioTracks(false);
    settings.setVoiceChatApp("");
    bool micDestroyed = false, voiceDestroyed = false;
    AudioMixer mixer(48000, 2);
    mixer.addSource(std::make_unique<ManagerRoleSource>(
        IAudioSource::SourceType::Microphone, micDestroyed), 1);
    mixer.addSource(std::make_unique<ManagerRoleSource>(
        IAudioSource::SourceType::ApplicationAudio, voiceDestroyed), 2);
    RecordingManager recorder;
    recorder.setSettingsManager(&settings);
    recorder.setAudioMixer(&mixer);
    recorder.m_session = std::make_unique<RecordingSession>(
        "unused.mkv", nullptr, nullptr, nullptr, &mixer, false, 128, 3);
    recorder.m_session->setVoiceTrack(2);
    recorder.m_session->markSourceConnected(2);
    recorder.m_voiceTrack = -1; // Initial layout predates the late adoption.
    recorder.setState(RecordingManager::RecordingState::Recording);
    recorder.applyVoiceChatApp();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (mixer.sourceOperationPending(2) && std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(mixer.sourceOperationPending(2));
    EXPECT_TRUE(voiceDestroyed);
    EXPECT_FALSE(micDestroyed);
    EXPECT_TRUE(mixer.sourceIsCapturing(1));
    EXPECT_EQ(recorder.m_voiceTrack, 2);
  }

  void installPendingCapture(RecordingManager &recorder,
                             QPointer<PendingCapture> &capture) {
    recorder.m_captureFactory = [&capture]() {
      auto source = std::make_unique<PendingCapture>();
      capture = source.get();
      return source;
    };
  }

  void captureEndRace(int scenario) {
    QProcess game;
    game.start("sleep", {"30"});
    ASSERT_TRUE(game.waitForStarted());
    SettingsManager settings;
    settings.setRecordingsPath(tempDir.path());
    StorageManager storage(tempDir.path());
    WaylandWindowMonitor monitor;
    GameDetectionService detector;
    detector.setWindowMonitor(&monitor);
    detector.addToWhitelist("steam_app_42424242", "Exit fixture");
    WindowInfo window;
    window.pid = game.processId(); window.appId = "steam_app_42424242";
    window.focused = window.fullscreen = true;
    monitor.ingestSnapshot({window});
    ASSERT_EQ(detector.currentGameKey(), "steam_app_42424242");
    ASSERT_EQ(detector.currentGameName(), "Exit fixture");
    RecordingManager recorder;
    recorder.setSettingsManager(&settings);
    recorder.setStorageManager(&storage);
    recorder.setGameDetectionService(&detector);
    recorder.m_captureEndGraceMs = 200;
    QPointer<PendingCapture> capture;
    installPendingCapture(recorder, capture);
    recorder.m_pipelineBuilder = [&] {
      recorder.m_muxer = std::make_unique<FinalizationMuxer>(scenario != 4);
      return true;
    };
    QSignalSpy saved(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    QSignalSpy finished(&recorder, &RecordingManager::recordingFinished);
    recorder.startRecording(scenario == 5 ? QString() : QString("Exit fixture"));
    ASSERT_FALSE(capture.isNull());
    recorder.completePipelineAndStart();
    ASSERT_TRUE(recorder.isRecording());
    capture->errorKind = scenario == 3 ? IVideoCaptureSource::CaptureErrorKind::Other
                                     : IVideoCaptureSource::CaptureErrorKind::SourceUnavailable;
    if (scenario == 2) {
      window.minimized = true;
      monitor.ingestSnapshot({window});
      EXPECT_FALSE(detector.hasVisibleWindow()); // Not proof of game exit.
    }
    if (scenario == 0 || scenario == 3 || scenario == 4 || scenario == 5) {
      game.terminate();
      ASSERT_TRUE(game.waitForFinished());
    }
    if (scenario == 6) recorder.stopRecording();
    emit capture->captureError("source vanished");
    if (scenario == 7) {
      QCoreApplication::processEvents();
      recorder.onEncodingError("output write failed during stop");
    }
    if (scenario == 1) QTimer::singleShot(30, &game, [&] { game.terminate(); });
    ASSERT_TRUE(waitForIdle(recorder));
    const bool normal = scenario == 0 || scenario == 1 || scenario == 6;
    EXPECT_EQ(saved.size(), normal ? 1 : 0);
    EXPECT_EQ(errors.size(), normal ? 0 : 1);
    EXPECT_EQ(finished.size(), 1);
    EXPECT_TRUE(recorder.currentFilePath().isEmpty());
    if (game.state() != QProcess::NotRunning) {
      game.terminate();
      ASSERT_TRUE(game.waitForFinished());
    }
  }

  QString outputPath(RecordingManager &recorder, const QString &game) {
    recorder.m_currentGameName = game;
    return recorder.generateOutputPath();
  }

  void stopWithFinalization(bool succeeds, bool pipelineError) {
    StorageManager storage(tempDir.path());
    RecordingManager recorder;
    recorder.setStorageManager(&storage);
    recorder.m_muxer = std::make_unique<FinalizationMuxer>(succeeds);
    recorder.m_currentFilePath = tempDir.path() + "/Game.mkv";
    recorder.m_currentGameName = "Game";
    storage.setExcludedRecordingPath(recorder.m_currentFilePath);
    recorder.setState(RecordingManager::RecordingState::Recording);
    QSignalSpy stopped(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy finished(&recorder, &RecordingManager::recordingFinished);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    if (pipelineError)
      recorder.onEncodingError("Packet write failed");
    else
      recorder.stopRecording();
    ASSERT_TRUE(waitForIdle(recorder));
    EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Idle);
    EXPECT_TRUE(recorder.currentFilePath().isEmpty());
    EXPECT_TRUE(recorder.currentGameName().isEmpty());
    EXPECT_EQ(stopped.count(), succeeds && !pipelineError ? 1 : 0);
    EXPECT_EQ(finished.count(), 1);
    EXPECT_EQ(errors.count(), succeeds && !pipelineError ? 0 : 1);
  }

  void preserveFailedSessionOutput(bool writesSucceed, bool finalizeSucceeds) {
    const QString path = tempDir.path() + "/partial.mkv";
    createDummyFile("partial.mkv", 64);
    StorageManager storage(tempDir.path());
    RecordingManager recorder;
    recorder.setStorageManager(&storage);
    recorder.m_currentFilePath = path;
    recorder.m_currentGameName = "Game";
    storage.setExcludedRecordingPath(path);
    recorder.m_captureSource = std::make_unique<PendingCapture>();
    recorder.m_videoEncoder = std::make_unique<ManagerOutputEncoder>();
    recorder.m_muxer = std::make_unique<FinalizationMuxer>(finalizeSucceeds, writesSucceed);
    recorder.m_session = std::make_unique<RecordingSession>(
        path, recorder.m_captureSource.get(), recorder.m_videoEncoder.get(),
        recorder.m_muxer.get(), nullptr, true, 128, 0);
    ASSERT_TRUE(recorder.m_session->start());
    recorder.setState(RecordingManager::RecordingState::Recording);
    QSignalSpy saved(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy finished(&recorder, &RecordingManager::recordingFinished);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    recorder.stopRecording();
    ASSERT_TRUE(waitForIdle(recorder));
    EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Idle);
    EXPECT_EQ(saved.count(), 0);
    EXPECT_EQ(finished.count(), 1);
    EXPECT_EQ(errors.count(), 1);
    EXPECT_FALSE(recorder.m_session);
    EXPECT_FALSE(recorder.m_muxer);
    QFile partial(path);
    ASSERT_TRUE(partial.open(QIODevice::ReadOnly));
    EXPECT_EQ(partial.readAll(), QByteArray(64, 'x'));
  }

  void queuedOldAttemptCannotAffectReplacement(bool queueError) {
    SettingsManager settings;
    settings.setRecordingsPath(tempDir.path());
    settings.setVideoResolution("Native");
    StorageManager storage(tempDir.path());
    QPointer<PendingCapture> capture;
    RecordingManager recorder;
    recorder.setSettingsManager(&settings);
    recorder.setStorageManager(&storage);
    installPendingCapture(recorder, capture);
    QSignalSpy started(&recorder, &RecordingManager::recordingStarted);
    QSignalSpy stopped(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    QSignalSpy auth(&recorder, &RecordingManager::restoreAuthNeeded);
    QSignalSpy states(&recorder, &RecordingManager::stateChanged);
    QSignalSpy selection(&recorder, &RecordingManager::captureSelectionFinished);
    QSignalSpy names(&recorder, &RecordingManager::currentGameNameChanged);

    recorder.startRecording("Old Game", 11);
    ASSERT_EQ(recorder.state(), RecordingManager::RecordingState::Initializing);
    ASSERT_FALSE(capture.isNull());
    QPointer<PendingCapture> oldSource = capture;
    if (queueError)
      emit capture->captureError("Old attempt failed late");
    else
      emit capture->ready();
    // Do not process any events until the replacement owns the manager.
    recorder.stopRecording();
    ASSERT_TRUE(waitForIdle(recorder));
    ASSERT_TRUE(oldSource.isNull());
    recorder.startRecording("Replacement Game", 22);
    ASSERT_EQ(recorder.state(), RecordingManager::RecordingState::Initializing);
    ASSERT_FALSE(capture.isNull());
    const QPointer<PendingCapture> replacementSource = capture;
    const QString replacementPath = recorder.currentFilePath();
    ASSERT_FALSE(replacementPath.isEmpty());
    ASSERT_FALSE(recorder.m_session);
    const int stateCount = states.count();
    const int nameCount = names.count();

    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);

    EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Initializing);
    ASSERT_FALSE(replacementSource.isNull());
    EXPECT_EQ(capture.data(), replacementSource.data());
    EXPECT_EQ(recorder.m_captureSource.get(), replacementSource.data());
    EXPECT_FALSE(recorder.m_session); // Old ready cannot build a new session.
    EXPECT_EQ(selection.count(), 0); // Nor dismiss the replacement's guidance.
    EXPECT_EQ(replacementSource->resolutionQueries, 0);
    EXPECT_EQ(replacementSource->config.windowId, 22u);
    EXPECT_EQ(recorder.currentGameName(), "Replacement Game");
    EXPECT_EQ(recorder.currentFilePath(), replacementPath);
    EXPECT_EQ(states.count(), stateCount);
    EXPECT_EQ(names.count(), nameCount);
    EXPECT_EQ(started.count(), 0);
    EXPECT_EQ(stopped.count(), 0);
    EXPECT_EQ(errors.count(), 0);
    EXPECT_EQ(auth.count(), 0);
    EXPECT_FALSE(QFileInfo::exists(replacementPath));
    recorder.stopRecording();
  }
};

TEST_F(RecordingManagerTest,
       StartRecordingAbortsWhenStorageLimitReachedAndAutoCleanupDisabled) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager storage(tempDir.path());
  storage.setMaxSize(6000);
  storage.setAutoDeleteOldest(false);

  SettingsManager settings;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);

  QSignalSpy startedSpy(&recorder, &RecordingManager::recordingStarted);
  QSignalSpy stateSpy(&recorder, &RecordingManager::stateChanged);

  recorder.startRecording();

  EXPECT_FALSE(recorder.isRecording());
  EXPECT_EQ(startedSpy.count(), 0);
  ASSERT_GE(stateSpy.count(), 1);
  EXPECT_EQ(stateSpy.last()[0].toInt(),
            static_cast<int>(RecordingManager::RecordingState::Idle));
}

TEST_F(RecordingManagerTest,
       StartRecordingAbortsWhenAutoCleanupKeepsOnlyCurrentRecording) {
  createDummyFile("current.mkv", 7000);
  createDummyFile("old.mkv", 5000);

  StorageManager storage(tempDir.path());
  storage.setExcludedRecordingPath(tempDir.path() + "/current.mkv");
  storage.setMaxSize(6000);
  storage.setAutoDeleteOldest(true);

  SettingsManager settings;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);

  QSignalSpy startedSpy(&recorder, &RecordingManager::recordingStarted);
  QSignalSpy stateSpy(&recorder, &RecordingManager::stateChanged);

  recorder.startRecording();

  EXPECT_FALSE(recorder.isRecording());
  EXPECT_EQ(startedSpy.count(), 0);
  ASSERT_GE(stateSpy.count(), 1);
  EXPECT_EQ(stateSpy.last()[0].toInt(),
            static_cast<int>(RecordingManager::RecordingState::Idle));
  EXPECT_GT(storage.usedSize(), storage.maxSize());
}

// --- Captured-source naming (manual recording while a game is detected
// must not inherit the detected game when the portal captured
// something else) ---

TEST_F(RecordingManagerTest, GenericSourceLabelsCarryNoIdentity) {
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel(""));
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel("Unknown Game"));
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel("KWin Wayland"));
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel("kwin_wayland"));
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel("steam_app_1422450"));
  EXPECT_FALSE(RecordingManager::isGenericSourceLabel("Deadlock"));
  EXPECT_FALSE(RecordingManager::isGenericSourceLabel("deadlock.exe"));
  EXPECT_FALSE(RecordingManager::isGenericSourceLabel("Firefox"));
}

TEST_F(RecordingManagerTest, BookmarksRequireActiveVideoAndSurviveStop) {
  bookmarkLifecycle();
}

TEST_F(RecordingManagerTest, SourceLabelsAgreeAcrossTitleAndExeForms) {
  // Portal exe basenames agree with detector display names.
  EXPECT_TRUE(RecordingManager::sourceLabelsAgree("Deadlock", "deadlock.exe"));
  EXPECT_TRUE(RecordingManager::sourceLabelsAgree("deadlock.exe", "Deadlock"));
  EXPECT_TRUE(RecordingManager::sourceLabelsAgree("Firefox", "firefox"));
  // A different captured window disagrees with the detected game.
  EXPECT_FALSE(RecordingManager::sourceLabelsAgree("Firefox", "Deadlock"));
  EXPECT_FALSE(RecordingManager::sourceLabelsAgree("Deadlock", "Discord"));
  // Empty labels never agree (callers guard empties separately).
  EXPECT_FALSE(RecordingManager::sourceLabelsAgree("", "Deadlock"));
  EXPECT_FALSE(RecordingManager::sourceLabelsAgree("Deadlock", ""));
}

TEST_F(RecordingManagerTest,
       RepeatStartAttemptsWhileOverLimitNotifyEveryPress) {
  // Regression: record keybind / manual button while storage is full used
  // to abort silently after the first episode notice (latched signal, no
  // recordingError). Every explicit attempt must surface feedback.
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager storage(tempDir.path());
  storage.setMaxSize(6000);
  storage.setAutoDeleteOldest(false);

  SettingsManager settings;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);

  QSignalSpy startedSpy(&recorder, &RecordingManager::recordingStarted);
  QSignalSpy errorSpy(&recorder, &RecordingManager::recordingError);
  QSignalSpy cannotSpy(&storage,
                       &StorageManager::storageLimitCannotCleanup);

  for (int i = 0; i < 3; ++i) {
    recorder.startRecording();
    EXPECT_FALSE(recorder.isRecording());
  }

  EXPECT_EQ(startedSpy.count(), 0);
  // The in-app error dialog fires on every press, even with storage
  // notices toggled off; the notification signal fires on every press
  // too (forced, bypassing the episode latch).
  ASSERT_EQ(errorSpy.count(), 3);
  EXPECT_EQ(cannotSpy.count(), 3);
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(errorSpy.at(i).at(0).toString().contains("Storage full"));
  }
}

TEST_F(RecordingManagerTest, CancelInitializingDiscardsAlreadyQueuedReady) {
  SettingsManager settings;
  settings.setRecordingsPath(tempDir.path());
  StorageManager storage(tempDir.path());
  QPointer<PendingCapture> capture;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);
  installPendingCapture(recorder, capture);
  QSignalSpy started(&recorder, &RecordingManager::recordingStarted);
  QSignalSpy stopped(&recorder, &RecordingManager::recordingStopped);
  QSignalSpy errors(&recorder, &RecordingManager::recordingError);
  QSignalSpy names(&recorder, &RecordingManager::currentGameNameChanged);

  recorder.startRecording("Closing Game", 42);
  ASSERT_EQ(recorder.state(), RecordingManager::RecordingState::Initializing);
  ASSERT_FALSE(capture.isNull());
  const QString path = recorder.currentFilePath();
  ASSERT_FALSE(path.isEmpty());
  createDummyFile(QFileInfo(path).fileName(), 128);
  storage.setMaxSize(64);
  storage.setAutoDeleteOldest(true);
  EXPECT_FALSE(storage.enforceStorageLimit(true)); // In-progress file protected.

  emit capture->ready(); // Queued delivery survives disconnect/destruction.
  recorder.stopRecording();
  ASSERT_TRUE(waitForIdle(recorder));
  EXPECT_TRUE(capture.isNull());
  EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Idle);
  EXPECT_TRUE(recorder.currentFilePath().isEmpty());
  EXPECT_TRUE(recorder.currentGameName().isEmpty());
  EXPECT_EQ(names.count(), 2);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
  EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Idle);
  EXPECT_EQ(started.count(), 0);
  EXPECT_EQ(stopped.count(), 0); // Cancellation is not a completed recording.
  EXPECT_EQ(errors.count(), 0); // Late ready must not try to build a pipeline.
  EXPECT_TRUE(storage.enforceStorageLimit(true));
  EXPECT_FALSE(QFileInfo::exists(path)); // Cancellation released the exclusion.

  recorder.startRecording("Next Game");
  EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Initializing);
  ASSERT_FALSE(capture.isNull());
  EXPECT_EQ(capture->config.windowId, 0u);
  recorder.stopRecording();
}

TEST_F(RecordingManagerTest, QueuedOldReadyCannotStartReplacementAttempt) {
  queuedOldAttemptCannotAffectReplacement(false);
}

TEST_F(RecordingManagerTest, QueuedOldErrorCannotCancelReplacementAttempt) {
  queuedOldAttemptCannotAffectReplacement(true);
}

TEST_F(RecordingManagerTest, StaleRestoreSettlesBeforeReauthorizationAndRetry) {
  SettingsManager settings;
  settings.setRecordingsPath(tempDir.path());
  settings.setRestoreToken("Restored Game", "stale-token");
  StorageManager storage(tempDir.path());
  QPointer<PendingCapture> capture;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);
  installPendingCapture(recorder, capture);
  QSignalSpy auth(&recorder, &RecordingManager::restoreAuthNeeded);
  QSignalSpy started(&recorder, &RecordingManager::recordingStarted);
  QSignalSpy errors(&recorder, &RecordingManager::recordingError);

  recorder.startRecording("Restored Game");
  ASSERT_FALSE(capture.isNull());
  ASSERT_EQ(capture->config.waylandRestoreToken, "stale-token");
  EXPECT_TRUE(capture->config.windowOnly);
  const QString stalePath = recorder.currentFilePath();
  createDummyFile(QFileInfo(stalePath).fileName(), 128);
  storage.setMaxSize(64);
  storage.setAutoDeleteOldest(true);

  bool observedSettledState = false;
  QObject::connect(&recorder, &RecordingManager::restoreAuthNeeded, &recorder,
                   [&](const QString &game) {
    observedSettledState = true;
    EXPECT_EQ(game, "Restored Game");
    EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Idle);
    EXPECT_TRUE(recorder.currentFilePath().isEmpty());
    EXPECT_TRUE(capture.isNull());
    EXPECT_EQ(recorder.currentGameName(), game); // Prompt retains its identity.
    EXPECT_TRUE(storage.enforceStorageLimit(true));
    EXPECT_FALSE(QFileInfo::exists(stalePath)); // Exclusion cleared before emit.
    // Application owns token invalidation; simulate only that caller action,
    // then retry immediately to catch a manager left busy at signal delivery.
    settings.setRestoreToken(game, QString());
    recorder.startRecording(game);
  });

  emit capture->restoreTokenStale("stale-token");
  if (auth.isEmpty()) {
    ASSERT_TRUE(auth.wait(3000));
  }
  EXPECT_TRUE(observedSettledState);
  ASSERT_EQ(auth.count(), 1);
  EXPECT_EQ(auth.at(0).at(0).toString(), "Restored Game");
  ASSERT_FALSE(capture.isNull());
  EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Initializing);
  EXPECT_TRUE(capture->config.waylandRestoreToken.isEmpty());
  EXPECT_EQ(started.count(), 0);
  EXPECT_EQ(errors.count(), 0);
  recorder.stopRecording();
}

TEST_F(RecordingManagerTest, CaptureSourceLossAfterGameExitSavesNormally) { captureEndRace(0); }
TEST_F(RecordingManagerTest, CaptureSourceLossBeforeGameExitDrainsThenSavesNormally) { captureEndRace(1); }
TEST_F(RecordingManagerTest, CaptureSourceLossWhileGameLivesStillFails) { captureEndRace(2); }
TEST_F(RecordingManagerTest, CaptureServiceFailureIsNotHiddenByGameExit) { captureEndRace(3); }
TEST_F(RecordingManagerTest, CaptureGameExitDoesNotHideFinalizationFailure) { captureEndRace(4); }
TEST_F(RecordingManagerTest, ManualCaptureSourceLossDoesNotBorrowDetectedGameExit) { captureEndRace(5); }
TEST_F(RecordingManagerTest, QueuedCaptureLossAfterExplicitStopDoesNotTurnSaveIntoError) { captureEndRace(6); }
TEST_F(RecordingManagerTest, CaptureGameExitGraceDoesNotHideOutputFailure) { captureEndRace(7); }

TEST_F(RecordingManagerTest, DiagnosticGameIdentityIsPinnedAndManualPickerDoesNotBorrowIt) {
  SettingsManager settings;
  settings.setRecordingsPath(tempDir.path());
  StorageManager storage(tempDir.path());
  WaylandWindowMonitor monitor;
  GameDetectionService detector;
  detector.setWindowMonitor(&monitor);
  detector.addToWhitelist("steam_app_1422450", "Deadlock");
  detector.addToWhitelist("steam_app_999", "Deadlock");
  WindowInfo window;
  window.pid = 61010; window.appId = "steam_app_1422450";
  window.focused = window.fullscreen = true;
  monitor.ingestSnapshot({window});
  ASSERT_EQ(detector.currentGameName(), "Deadlock");
  EXPECT_TRUE(detector.captureIdentityInfo("Another game").isEmpty());
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);
  recorder.setGameDetectionService(&detector);
  QPointer<PendingCapture> capture;
  installPendingCapture(recorder, capture);
  recorder.startRecording("Deadlock");
  ASSERT_FALSE(capture.isNull());
  const auto first = diagnosticHealth(recorder);
  EXPECT_EQ(first["detectedGameKey"].toString(), "steam_app_1422450");
  EXPECT_EQ(first["detectedSteamAppId"].toString(), "1422450");
  EXPECT_EQ(first["detectedGamePid"].toLongLong(), 61010);
  EXPECT_FALSE(first.contains("sourcePid")); // never pretend this came from the portal
  detector.addToBlacklist("steam_app_1422450");
  window.pid = 61011; window.appId = "steam_app_999";
  monitor.ingestSnapshot({window});
  ASSERT_EQ(detector.currentGameKey(), "steam_app_999");
  EXPECT_EQ(diagnosticHealth(recorder)["detectedGameKey"], first["detectedGameKey"]);
  EXPECT_EQ(diagnosticHealth(recorder)["detectedGamePid"], first["detectedGamePid"]);
  recorder.stopRecording();
  ASSERT_TRUE(waitForIdle(recorder));
  recorder.startRecording(""); // a picker could choose something other than the detected game
  EXPECT_FALSE(diagnosticHealth(recorder).contains("detectedGameKey"));
  recorder.stopRecording();
  ASSERT_TRUE(waitForIdle(recorder));
}

TEST_F(RecordingManagerTest, RestoreTokenCallbacksKeepOriginatingIdentity) {
  SettingsManager settings;
  settings.setRecordingsPath(tempDir.path());
  settings.setRestoreTokenForIdentity("first", "Game.exe", "first-token");
  StorageManager storage(tempDir.path());
  WaylandWindowMonitor monitor;
  GameDetectionService detector;
  detector.setWindowMonitor(&monitor); // No listener/backend starts.
  detector.addToWhitelist("first", "Game.exe");
  detector.addToWhitelist("second", "Game.exe");
  WindowInfo window;
  window.pid = 61010;
  window.appId = "first";
  window.focused = true;
  window.fullscreen = true;
  monitor.ingestSnapshot({window});
  ASSERT_EQ(detector.currentGameKey(), "first");
  QPointer<PendingCapture> capture;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);
  recorder.setGameDetectionService(&detector);
  installPendingCapture(recorder, capture);
  recorder.startRecording("Game.exe");
  ASSERT_FALSE(capture.isNull());
  ASSERT_EQ(capture->config.waylandRestoreToken, "first-token");
  detector.addToBlacklist("first");
  window.pid = 61011;
  window.appId = "second";
  monitor.ingestSnapshot({window});
  ASSERT_EQ(detector.currentGameName(), "Game.exe");
  ASSERT_EQ(detector.currentGameKey(), "second");
  emit capture->restoreTokenUpdated("updated-first-token");
  EXPECT_EQ(settings.getRestoreToken("first"), "updated-first-token");
  EXPECT_TRUE(settings.getRestoreToken("second").isEmpty());
  QSignalSpy auth(&recorder, &RecordingManager::restoreAuthNeeded);
  emit capture->restoreTokenStale("updated-first-token");
  if (auth.isEmpty())
    ASSERT_TRUE(auth.wait(3000));
  ASSERT_EQ(auth.count(), 1);
  EXPECT_EQ(auth.at(0).at(1).toString(), "first");
}

TEST_F(RecordingManagerTest, QueuedStartupErrorReleasesOutputAndStorageExclusion) {
  SettingsManager settings;
  settings.setRecordingsPath(tempDir.path());
  StorageManager storage(tempDir.path());
  QPointer<PendingCapture> capture;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);
  installPendingCapture(recorder, capture);
  QSignalSpy errors(&recorder, &RecordingManager::recordingError);
  QSignalSpy started(&recorder, &RecordingManager::recordingStarted);

  recorder.startRecording("Failed Game");
  ASSERT_FALSE(capture.isNull());
  const QString path = recorder.currentFilePath();
  createDummyFile(QFileInfo(path).fileName(), 128);
  storage.setMaxSize(64);
  storage.setAutoDeleteOldest(true);
  emit capture->captureError("Portal denied capture");
  EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Initializing);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
  ASSERT_TRUE(waitForIdle(recorder));
  EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Idle);
  EXPECT_TRUE(capture.isNull());
  EXPECT_TRUE(recorder.currentFilePath().isEmpty());
  ASSERT_EQ(errors.count(), 1);
  EXPECT_EQ(errors.at(0).at(0).toString(), "Portal denied capture");
  EXPECT_EQ(started.count(), 0);
  EXPECT_TRUE(storage.enforceStorageLimit(true));
  EXPECT_FALSE(QFileInfo::exists(path));
}

TEST_F(RecordingManagerTest, OutputPathSkipsExistingBaseAndNumberedCollision) {
  SettingsManager settings;
  settings.setRecordingsPath(tempDir.path());
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  // Cover adjacent wall-clock seconds rather than assuming the test never
  // crosses a second boundary. Every candidate has both base and _2 occupied.
  const QDateTime now = QDateTime::currentDateTime();
  for (int offset = -5; offset <= 5; ++offset) {
    const QString stem = "Game_Name_" +
        now.addSecs(offset).toString("yyyy-MM-dd_HH-mm-ss");
    createDummyFile(stem + ".mkv", 17);
    createDummyFile(stem + "_2.mkv", 23);
  }
  const QString path = outputPath(recorder, "Game: / Name");
  EXPECT_EQ(QFileInfo(path).absolutePath(), tempDir.path());
  EXPECT_TRUE(QFileInfo(path).fileName().startsWith("Game_Name_"));
  EXPECT_TRUE(path.endsWith("_3.mkv"));
  EXPECT_FALSE(QFileInfo::exists(path));
  // Name allocation must not reserve/truncate any existing recording.
  EXPECT_EQ(QDir(tempDir.path()).entryList({"*.mkv"}, QDir::Files).size(), 22);
  for (const QString &name : QDir(tempDir.path()).entryList({"*.mkv"}, QDir::Files)) {
    QFile file(tempDir.path() + "/" + name);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), QByteArray(name.endsWith("_2.mkv") ? 23 : 17, 'x'));
  }
}

TEST_F(RecordingManagerTest, OutputPathUsesSafeFallbackAndAlwaysMkv) {
  SettingsManager settings;
  // Even an existing installation that selected MP4 now records as MKV.
  const auto settingsPath = settings.settingsFilePath();
  ASSERT_TRUE(QDir().mkpath(QFileInfo(settingsPath).path()));
  QFile legacy(settingsPath);
  ASSERT_TRUE(legacy.open(QIODevice::WriteOnly));
  legacy.write(R"({"video":{"format":"mp4"}})");
  legacy.close();
  settings.load();
  settings.setRecordingsPath(tempDir.path());
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  const QString sanitized = outputPath(recorder, "../ : ???");
  EXPECT_EQ(QFileInfo(sanitized).absolutePath(), tempDir.path());
  EXPECT_TRUE(QFileInfo(sanitized).fileName().startsWith("Game_"));
  EXPECT_TRUE(sanitized.endsWith(".mkv"));
  EXPECT_TRUE(QFileInfo(outputPath(recorder, QString())).fileName()
                  .startsWith("Recording_"));
  EXPECT_TRUE(outputPath(recorder, QString()).endsWith(".mkv"));
}

TEST_F(RecordingManagerTest, FinalizationFailureDoesNotEmitRecordingSaved) {
  stopWithFinalization(false, false);
}

TEST_F(RecordingManagerTest, PipelineFailureDoesNotEmitRecordingSaved) {
  stopWithFinalization(true, true);
}

TEST_F(RecordingManagerTest, SuccessfulFinalizationStillEmitsRecordingSaved) {
  stopWithFinalization(true, false);
}

TEST_F(RecordingManagerTest, SessionPacketFailureSuppressesSavedAndPreservesPartialFile) {
  preserveFailedSessionOutput(false, true);
}

TEST_F(RecordingManagerTest, SessionTrailerFailureSuppressesSavedAndPreservesPartialFile) {
  preserveFailedSessionOutput(true, false);
}

TEST_F(RecordingManagerTest, AsyncFinalizationKeepsHeartbeatAndRetainsStoppingPipeline) {
  gatedStop(true);
}

TEST_F(RecordingManagerTest, AsyncFinalizationFailureSettlesOnceWithoutSaved) {
  gatedStop(false);
}

TEST_F(RecordingManagerTest, StartupLookupSnapshotsInputsAndRejectsCancelledAttempt) {
  gatedStartupLookup();
}

TEST_F(RecordingManagerTest, SettingsChangeDuringStartupCoalescesLatestAudioConfiguration) {
  gatedStartupLookup(false);
}

TEST_F(RecordingManagerTest, DisablingLateAdoptedVoiceUsesActualSlotNotOriginalLayout) {
  disableLateAdoptedVoice();
}
