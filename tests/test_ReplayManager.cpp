/*
 * Trophy - Unit Tests
 * ReplayManager: segment window selection, pruning, and save guards.
 * No portal/pipeline needed (pure logic + inactive-state signals).
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <gtest/gtest.h>

#include "core/ReplayManager.h"
#include "library/RecordingAnnotations.h"
#include "core/RecordingSession.h"
#include "capture/IVideoCaptureSource.h"
#include "audio/AudioMixer.h"
#include "core/StorageManager.h"
#include "settings/SettingsManager.h"
#include "encoding/IEncoder.h"
#include "detection/GameDetectionService.h"
#include "detection/WaylandWindowMonitor.h"
#include "muxing/IMuxer.h"

using namespace TrophyBackend;

class ReplayManagerTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new ReplayManagerTestEnvironment);

using Seg = ReplayManager::Segment;

class ReplayFakeCapture : public IVideoCaptureSource {
public:
  bool initialize(const CaptureConfig &) override { return true; }
  void setFrameCallback(FrameCallback) override {}
  bool startCapture() override { return true; }
  void stopCapture() override {}
  bool isCapturing() const override { return true; }
  CaptureCapabilities capabilities() const override { return {}; }
  QSize nativeResolution() const override { return {}; }
  QString name() const override { return "fake"; }
};

class ReplayOutputEncoder : public IEncoder {
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
  int codecId() const override { return 27; } // H.264; no native encoding
  int encodedPixelFormat() const override { return 0; }
};

class ReplayOutputMuxer : public IMuxer {
public:
  ReplayOutputMuxer(bool writeOk, bool finalizeOk)
      : writeOk(writeOk), finalizeOk(finalizeOk) {}
  bool initialize(const QString &) override { return true; }
  int addStream(const StreamInfo &) override { return 0; }
  bool writeHeader() override { return true; }
  bool writePacket(const EncodedPacket &) override { return writeOk; }
  bool finalize() override { return finalizeOk; }
  QString outputPath() const override { return {}; }
  int64_t bytesWritten() const override { return 0; }
  QString formatName() const override { return "fake"; }
private:
  bool writeOk;
  bool finalizeOk;
};

struct ReplayOperationGate {
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
    std::lock_guard lock(mutex); released = true; cv.notify_all();
  }
};

class ReplayGatedMuxer : public ReplayOutputMuxer {
public:
  ReplayGatedMuxer(std::shared_ptr<ReplayOperationGate> gate, bool succeeds)
      : ReplayOutputMuxer(true, succeeds), gate(std::move(gate)) {}
  ~ReplayGatedMuxer() override { ++gate->destructions; }
  bool finalize() override {
    if (!finalized) { gate->block(); finalized = true; }
    return ReplayOutputMuxer::finalize();
  }
private:
  std::shared_ptr<ReplayOperationGate> gate;
  bool finalized = false;
};

class ReplayFakeAudioSource : public IAudioSource {
public:
  explicit ReplayFakeAudioSource(bool &destroyed,
                                 SourceType type = SourceType::ApplicationAudio)
      : destroyed(destroyed), type(type) {}
  ~ReplayFakeAudioSource() override { destroyed = true; }
  bool initialize(const AudioConfig &) override { return true; }
  void setCallback(AudioCallback) override {}
  bool start() override { return true; }
  void stop() override {}
  bool isCapturing() const override { return true; }
  SourceType sourceType() const override { return type; }
  QString displayName() const override { return "voice"; }
  QString identifier() const override { return "voice"; }
  float peakLevel() const override { return 0; }
  void setVolume(float) override {}
  float volume() const override { return 1; }
private:
  bool &destroyed;
  SourceType type;
};

namespace TrophyBackend {

class ReplayManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(cacheDir.isValid());
    ASSERT_TRUE(configHome.isValid());
    oldConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    oldTempDir = qgetenv("TMPDIR");
    oldCacheHome = qgetenv("XDG_CACHE_HOME");
    const QString temp = cacheDir.path() + "/temp";
    ASSERT_TRUE(QDir().mkpath(temp));
    qputenv("TMPDIR", temp.toUtf8());
    qputenv("XDG_CACHE_HOME", (cacheDir.path() + "/cache").toUtf8());
  }
  void TearDown() override {
    if (oldConfigHome.isNull()) qunsetenv("XDG_CONFIG_HOME");
    else qputenv("XDG_CONFIG_HOME", oldConfigHome);
    if (oldTempDir.isNull()) qunsetenv("TMPDIR");
    else qputenv("TMPDIR", oldTempDir);
    if (oldCacheHome.isNull()) qunsetenv("XDG_CACHE_HOME");
    else qputenv("XDG_CACHE_HOME", oldCacheHome);
  }

  // Creates a non-empty file so existence checks are meaningful.
  QString makeSegFile(const QString &name) {
    const QString path = cacheDir.path() + "/" + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("fake-segment");
    f.close();
    return path;
  }

  QTemporaryDir cacheDir;
  QTemporaryDir configHome;
  QByteArray oldConfigHome;
  QByteArray oldTempDir;
  QByteArray oldCacheHome;

  // Friendship is not inherited by TEST_F bodies, so private access
  // lives in these fixture helpers (same pattern as the detection tests).
  void fabricateMidRecordingState(ReplayManager &mgr, const QString &closed,
                                  const QString &open) {
    mgr.m_segments = {{closed, 0.0, 60.0}};
    mgr.m_openSegmentPath = open;
    mgr.m_openSegmentStart = 60.0;
  }
  bool hasBufferedState(const ReplayManager &mgr) const {
    return !mgr.m_segments.isEmpty() || !mgr.m_openSegmentPath.isEmpty();
  }
  // Friendship is not inherited by TEST_F bodies, so private access
  // lives here (same pattern as the fabrication helpers above).
  QString defaultCacheDir(ReplayManager &mgr) { return mgr.cacheDir(); }
  QString legacyCacheDir() { return ReplayManager::legacyCacheDir(); }
  void forceRunningState(ReplayManager &mgr, SettingsManager *settings,
                         StorageManager *storage) {
    mgr.m_settings = settings;
    mgr.m_storage = storage;
    mgr.m_state = ReplayManager::State::Running;
  }
  QString setupKey(ReplayManager &mgr) { return mgr.audioSetupKey(); }
  void beginDiagnosticCapture(ReplayManager &mgr, SettingsManager &settings,
                              GameDetectionService &detector) {
    mgr.m_settings = &settings;
    mgr.m_gameDetector = &detector;
    mgr.m_gameName = "Deadlock";
    mgr.m_captureFactory = [] { return std::make_unique<ReplayFakeCapture>(); };
    ASSERT_TRUE(mgr.beginCapture());
    mgr.m_session = std::make_unique<RecordingSession>(
        "unused.mkv", mgr.m_capture.get(), nullptr, nullptr, nullptr, false, 128, 0);
    mgr.m_state = ReplayManager::State::Running;
  }
  void verifyCoverageDiagnostics() {
    ReplayManager mgr;
    mgr.m_state = ReplayManager::State::Running;
    mgr.m_dvrClock.start();
    mgr.m_session = std::make_unique<RecordingSession>(
        "not-started.mkv", nullptr, nullptr, nullptr, nullptr, false, 128, 0);
    mgr.m_segments = {{"retained.mkv", 0, 60}};
    auto retiring = std::make_unique<ReplayManager::RetiringSegment>();
    retiring->segment = {"retiring.mkv", 60, 10};
    mgr.m_retiringSegments.push_back(std::move(retiring));
    const auto health = mgr.captureHealth();
    EXPECT_DOUBLE_EQ(health["bufferSeconds"].toDouble(), 30.0);
    EXPECT_DOUBLE_EQ(health["retainedVideoSeconds"].toDouble(), 70.0);
    EXPECT_EQ(health["retainedSegments"].toInt(), 2); // no delivered current video yet
    EXPECT_EQ(health["retiringSegments"].toInt(), 1);
    EXPECT_GE(health["dvrSessionSeconds"].toDouble(), 0.0);
    // These synthetic retirement entries have no owned native work.
    mgr.m_retiringSegments.clear();
    mgr.m_segments.clear();
    mgr.m_session.reset();
    mgr.m_state = ReplayManager::State::Idle;
    EXPECT_TRUE(mgr.captureHealth().isEmpty());
  }
  ReplayFakeCapture *replaceCapture(ReplayManager &mgr) {
    mgr.m_capture = std::make_unique<ReplayFakeCapture>();
    mgr.m_gameName = "Replacement";
    mgr.m_state = ReplayManager::State::StartingCapture;
    mgr.connectCaptureSignals();
    return static_cast<ReplayFakeCapture *>(mgr.m_capture.get());
  }
  bool isStarting(const ReplayManager &mgr) const {
    return mgr.m_state == ReplayManager::State::StartingCapture;
  }
  void preparePruningSave(ReplayManager &mgr, SettingsManager &settings,
                          const QString &pinned) {
    mgr.m_settings = &settings;
    mgr.m_segments = {{pinned, 0, 60}, {makeSegFile("later.mkv"), 60, 60},
                      {makeSegFile("latest.mkv"), 120, 60}};
    mgr.m_saveInputPaths = {pinned};
  }
  void prune(ReplayManager &mgr) { mgr.pruneOldSegments(1000); }
  void releaseSaveInputs(ReplayManager &mgr) { mgr.m_saveInputPaths.clear(); }
  void deliverAudioHistory(ReplayManager &mgr, const QString &path, const QJsonObject &history) {
    mgr.m_saving = true;
    mgr.m_saveDone = std::make_shared<std::atomic<bool>>(true);
    mgr.m_saveResult = std::make_shared<ReplayManager::ReplaySaveResult>();
    mgr.m_saveResult->ok = true; mgr.m_saveResult->path = path;
    mgr.m_saveResult->audioHistory = history;
    mgr.m_saveThread = std::thread([] {});
    mgr.pollRetirements();
  }
  void retireOutput(bool writeOk, bool finalizeOk) {
    ReplayManager mgr;
    const QString good = makeSegFile("closed-good.mkv");
    const QString open = makeSegFile("open.mkv");
    fabricateMidRecordingState(mgr, good, open);
    mgr.m_capture = std::make_unique<ReplayFakeCapture>();
    mgr.m_encoder = std::make_unique<ReplayOutputEncoder>();
    mgr.m_muxer = std::make_unique<ReplayOutputMuxer>(writeOk, finalizeOk);
    mgr.m_session = std::make_unique<RecordingSession>(
        open, mgr.m_capture.get(), mgr.m_encoder.get(), mgr.m_muxer.get(),
        nullptr, true, 128, 0);
    ASSERT_TRUE(mgr.m_session->start());
    const bool success = writeOk && finalizeOk;
    EXPECT_TRUE(mgr.retireOpenSegment(120)); // Accepted; result settles asynchronously.
    EXPECT_TRUE(mgr.m_openSegmentPath.isEmpty());
    EXPECT_FALSE(mgr.m_session);
    EXPECT_FALSE(mgr.m_muxer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!mgr.m_retiringSegments.empty() && std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(mgr.m_retiringSegments.empty());
    EXPECT_EQ(mgr.m_segments.size(), success ? 2 : 1);
    EXPECT_EQ(QFileInfo::exists(open), success);
    EXPECT_TRUE(QFileInfo::exists(good));
    const auto selected = ReplayManager::selectForWindow(mgr.m_segments, 120, 120);
    EXPECT_EQ(selected.size(), success ? 2 : 1);
    for (const auto &segment : selected) {
      if (!success) {
        EXPECT_NE(segment.path, open);
      }
    }
  }
  void prepareLateVoiceRouting(ReplayManager &mgr, SettingsManager &settings) {
    mgr.m_settings = &settings;
    mgr.m_state = ReplayManager::State::Running;
    mgr.m_session = std::make_unique<RecordingSession>(
        "unused.mkv", nullptr, nullptr, nullptr, nullptr, false, 128, 3);
    // Merged initial layout: game=0, mic=1. Voice joined later in slot 2.
    mgr.m_session->setVoiceTrack(2);
    mgr.m_session->markSourceConnected(0);
    mgr.m_session->markSourceConnected(2);
    mgr.m_session->setGameIsFallback(false);
    mgr.m_micTrack = 1;
    mgr.m_voiceTrack = -1; // manager's original layout predates late adoption
    mgr.m_lookupCacheValid = true;
    mgr.m_lookupInflight = true; // no host worker is launched in the test
  }
  void waitSourceOperations(ReplayManager &mgr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    const auto pending = [&] {
      for (int track = 0; track < AudioMixer::kTrackCount; ++track)
        if (mgr.m_audioMixer->sourceOperationPending(track)) return true;
      return false;
    };
    while (pending() && std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(pending());
  }
  void gatedRetirement(bool fullStop, bool succeeds, bool cancelRestart = false) {
    SettingsManager settings;
    settings.setInstantReplayEnabled(true);
    ReplayManager mgr;
    mgr.setSettingsManager(&settings);
    mgr.m_captureFactory = [] { return std::make_unique<ReplayFakeCapture>(); };
    mgr.m_state = ReplayManager::State::Running;
    mgr.m_gameName = "Retained game";
    const QString path = makeSegFile("retiring.mkv");
    mgr.m_openSegmentPath = path;
    mgr.m_capture = std::make_unique<ReplayFakeCapture>();
    QPointer<IVideoCaptureSource> capture(mgr.m_capture.get());
    mgr.m_encoder = std::make_unique<ReplayOutputEncoder>();
    auto gate = std::make_shared<ReplayOperationGate>();
    mgr.m_muxer = std::make_unique<ReplayGatedMuxer>(gate, succeeds);
    mgr.m_session = std::make_unique<RecordingSession>(
        path, mgr.m_capture.get(), mgr.m_encoder.get(), mgr.m_muxer.get(),
        nullptr, true, 128, 0);
    ASSERT_TRUE(mgr.m_session->start());
    QSignalSpy failed(&mgr, &ReplayManager::replayFailed);
    if (fullStop) mgr.stopDvr();
    else EXPECT_TRUE(mgr.retireOpenSegment(60));
    ASSERT_TRUE(gate->waitForEntry());
    bool heartbeat = false;
    QTimer::singleShot(0, [&] { heartbeat = true; });
    QCoreApplication::processEvents();
    EXPECT_TRUE(heartbeat);
    EXPECT_EQ(mgr.m_state, fullStop ? ReplayManager::State::Stopping : ReplayManager::State::Running);
    EXPECT_FALSE(capture.isNull());
    EXPECT_TRUE(QFileInfo::exists(path));
    EXPECT_EQ(gate->destructions, 0);
    EXPECT_EQ(mgr.m_retiringSegments.size(), 1u);
    if (fullStop) {
      mgr.startDvr("Retained game");
      EXPECT_EQ(mgr.m_state, ReplayManager::State::Stopping);
      EXPECT_EQ(mgr.m_capture.get(), capture.data());
      if (cancelRestart) mgr.stopDvr();
    }
    gate->release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!mgr.m_retiringSegments.empty() || mgr.m_state == ReplayManager::State::Stopping) &&
           std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(mgr.m_retiringSegments.empty());
    EXPECT_EQ(gate->destructions, 1);
    EXPECT_EQ(failed.count(), !fullStop && !succeeds ? 1 : 0);
    EXPECT_EQ(QFileInfo::exists(path), !fullStop && succeeds);
    if (fullStop || !succeeds) {
      EXPECT_EQ(mgr.m_state, fullStop && !cancelRestart ? ReplayManager::State::StartingCapture : ReplayManager::State::Idle);
      EXPECT_TRUE(capture.isNull());
    } else {
      EXPECT_TRUE(mgr.isActive());
      EXPECT_EQ(mgr.m_segments.size(), 1);
    }
    QCoreApplication::processEvents();
    EXPECT_EQ(failed.count(), !fullStop && !succeeds ? 1 : 0);
  }
  void gatedStartup(bool replace) {
    SettingsManager settings;
    settings.setAudioMode("game");
    settings.setVoiceChatApp("old-voice");
    ReplayManager mgr;
    mgr.setSettingsManager(&settings);
    mgr.m_state = ReplayManager::State::StartingCapture;
    mgr.m_gameName = "Game";
    mgr.m_capture = std::make_unique<ReplayFakeCapture>();
    auto gate = std::make_shared<ReplayOperationGate>();
    mgr.m_nodeLookup = [gate](uint32_t, const QStringList &, const QString &voice) {
      if (++gate->lookupCalls == 1) { gate->firstVoice = voice; gate->block(); }
      ReplayManager::DvrNodeLookup result;
      result.voiceNodeName = voice;
      return result;
    };
    int builds = 0;
    QSignalSpy selection(&mgr, &ReplayManager::captureSelectionFinished);
    mgr.m_segmentBuilder = [&] {
      ++builds;
      EXPECT_EQ(mgr.m_lookupCache.voiceNodeName, "new-voice");
      EXPECT_EQ(mgr.m_lookupAudioKey, mgr.audioSetupKey());
      return true;
    };
    mgr.onCaptureReady();
    EXPECT_EQ(selection.count(), 1); // Picker guidance ends before audio setup.
    ASSERT_TRUE(gate->waitForEntry());
    EXPECT_EQ(gate->firstVoice, "old-voice");
    bool heartbeat = false;
    QTimer::singleShot(0, [&] { heartbeat = true; });
    QCoreApplication::processEvents();
    EXPECT_TRUE(heartbeat);
    settings.setVoiceChatApp("new-voice");
    settings.setMicrophoneDevice("new-mic");
    settings.setSeparateAudioTracks(false);
    if (replace) {
      mgr.m_capture = std::make_unique<ReplayFakeCapture>();
      ++mgr.m_lookupGeneration;
      mgr.m_gameName = "Replacement";
      mgr.m_captureReadyForPipeline = false;
    }
    gate->release();
    if (replace) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (mgr.m_lookupWorker.valid() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      EXPECT_EQ(gate->lookupCalls, 1); // Never query before the replacement's portal grant.
      EXPECT_EQ(builds, 0);
      mgr.onCaptureReady();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!mgr.isActive() && std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(mgr.isActive());
    EXPECT_EQ(gate->lookupCalls, 2);
    EXPECT_EQ(builds, 1);
  }
  void stopRetainsHeldRemuxInputs() {
    ReplayManager mgr;
    const QString path = makeSegFile("pinned-remux.mkv");
    mgr.m_segments = {{path, 0, 60}};
    mgr.m_saveInputPaths = {path};
    mgr.m_state = ReplayManager::State::Running;
    mgr.m_saveDone = std::make_shared<std::atomic<bool>>(false);
    auto gate = std::make_shared<ReplayOperationGate>();
    mgr.m_saveThread = std::thread([gate, done = mgr.m_saveDone] {
      gate->block(); done->store(true, std::memory_order_release);
    });
    ASSERT_TRUE(gate->waitForEntry());
    mgr.stopDvr();
    bool heartbeat = false;
    QTimer::singleShot(0, [&] { heartbeat = true; });
    QCoreApplication::processEvents();
    EXPECT_TRUE(heartbeat);
    EXPECT_EQ(mgr.m_state, ReplayManager::State::Stopping);
    EXPECT_TRUE(QFileInfo::exists(path));
    EXPECT_EQ(mgr.m_saveInputPaths, QStringList{path});
    gate->release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (mgr.m_state != ReplayManager::State::Idle && std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(mgr.m_state, ReplayManager::State::Idle);
    EXPECT_FALSE(QFileInfo::exists(path));
    EXPECT_FALSE(mgr.m_saveThread.joinable());
  }

  void semanticPreferencesSurviveIdleAndRelayout() {
    bool micDestroyed = false, voiceDestroyed = false;
    bool newMicDestroyed = false, newVoiceDestroyed = false;
    ReplayManager mgr;
    mgr.setSemanticTrackMuted(2, true); // Store even while idle/no source.
    mgr.setSemanticTrackMuted(1, false);
    EXPECT_TRUE(mgr.isSemanticTrackMuted(2));
    EXPECT_FALSE(mgr.isSemanticTrackMuted(1));
    mgr.m_audioMixer->addSource(std::make_unique<ReplayFakeAudioSource>(
        micDestroyed, IAudioSource::SourceType::Microphone), 1);
    mgr.m_audioMixer->addSource(std::make_unique<ReplayFakeAudioSource>(voiceDestroyed), 2);
    mgr.m_state = ReplayManager::State::Running;
    EXPECT_TRUE(mgr.m_audioMixer->isTrackMuted(1));
    EXPECT_FALSE(mgr.m_audioMixer->isTrackMuted(2));
    mgr.setSemanticTrackMuted(1, true);
    mgr.setSemanticTrackMuted(2, false);
    EXPECT_FALSE(mgr.m_audioMixer->isTrackMuted(1));
    EXPECT_TRUE(mgr.m_audioMixer->isTrackMuted(2));
    mgr.m_audioMixer->stopSourcesAsync();
    waitSourceOperations(mgr);
    EXPECT_TRUE(micDestroyed);
    EXPECT_TRUE(voiceDestroyed);
    mgr.setSemanticTrackMuted(2, true);
    mgr.setSemanticTrackMuted(1, false);
    // A separate-track rebuild moves mic to 2 and voice back to 1.
    mgr.m_audioMixer->addSource(std::make_unique<ReplayFakeAudioSource>(newVoiceDestroyed), 1);
    mgr.m_audioMixer->addSource(std::make_unique<ReplayFakeAudioSource>(
        newMicDestroyed, IAudioSource::SourceType::Microphone), 2);
    EXPECT_FALSE(mgr.m_audioMixer->isTrackMuted(1));
    EXPECT_TRUE(mgr.m_audioMixer->isTrackMuted(2));
    EXPECT_TRUE(mgr.isSemanticTrackMuted(2));
    EXPECT_FALSE(mgr.isSemanticTrackMuted(1));
  }
  void expectRetainedRouting(ReplayManager &mgr, bool cachedVoiceFound) {
    mgr.m_lookupCache.gameNodeId = 42;
    mgr.m_lookupCache.voiceNodeId = cachedVoiceFound ? 43 : 0xFFFFFFFFu;
    int count = -1, voiceTrack = -1;
    bool gameFound = false, voiceFound = false;
    QStringList hints;
    uint32_t pid = 0;
    mgr.setupAudioSources(count, hints, pid, gameFound, voiceTrack, voiceFound,
                          mgr.m_micTrack, false);
    EXPECT_EQ(count, 3);
    EXPECT_EQ(voiceTrack, 2);
    EXPECT_EQ(mgr.m_micTrack, 1);
    EXPECT_TRUE(voiceFound);
  }
  void expectFallbackRouting(ReplayManager &mgr, bool separate) {
    mgr.m_lookupCacheValid = true;
    mgr.m_lookupAudioKey = mgr.audioSetupKey();
    mgr.m_lookupCache.gameNodeId = 0xFFFFFFFFu;
    mgr.m_lookupCache.voiceNodeId = 43;
    int count = -1, voiceTrack = -1, micTrack = -1;
    bool gameFound = false, voiceFound = false;
    QStringList hints;
    uint32_t pid = 0;
    mgr.setupAudioSources(count, hints, pid, gameFound, voiceTrack, voiceFound,
                          micTrack, false);
    EXPECT_FALSE(gameFound);
    EXPECT_FALSE(voiceFound); // Voice is already present in the desktop mix.
    EXPECT_EQ(voiceTrack, separate ? 1 : -1);
    EXPECT_EQ(micTrack, separate ? 2 : 1);
    EXPECT_EQ(count, 2); // Fallback + mic; separate stream count is set by session.
    EXPECT_EQ(mgr.m_settings->separateAudioTracks(), separate);
  }
  void addLateVoiceSource(ReplayManager &mgr, bool &destroyed) {
    mgr.m_audioMixer->addSource(
        std::make_unique<ReplayFakeAudioSource>(destroyed), 2);
  }
  void prepareStaleToken(ReplayManager &mgr, const QString &game) {
    mgr.m_gameName = game;
    mgr.m_state = ReplayManager::State::StartingCapture;
    mgr.m_rotationTimer->start(60000);
  }
  void rejectRestoreToken(ReplayManager &mgr) {
    mgr.onRestoreTokenStale("rejected-token");
  }
  bool isIdleAndTimerStopped(const ReplayManager &mgr) const {
    return mgr.m_state == ReplayManager::State::Idle &&
           !mgr.m_rotationTimer->isActive();
  }
  quint64 prepareLookup(ReplayManager &mgr) {
    mgr.m_lookupInflight = true;
    return ++mgr.m_lookupGeneration;
  }
  void deliverLookup(ReplayManager &mgr, quint64 generation, uint32_t id) {
    ReplayManager::DvrNodeLookup result;
    result.gameNodeId = id;
    result.gameNodeName = QString("game-%1").arg(id);
    result.gameNodeSerial = QString::number(id + 100);
    result.gameFound = true;
    result.voiceNodeId = id + 1;
    result.voiceNodeName = "voice";
    result.voiceNodeSerial = QString::number(id + 101);
    result.voiceFound = true;
    mgr.applyLookupCacheResult(generation, result);
  }
  void expectLookup(const ReplayManager &mgr, bool valid, uint32_t id) {
    EXPECT_EQ(mgr.m_lookupCacheValid, valid);
    EXPECT_FALSE(mgr.m_lookupInflight);
    if (!valid)
      return;
    EXPECT_EQ(mgr.m_lookupCache.gameNodeId, id);
    EXPECT_EQ(mgr.m_lookupCache.gameNodeName, QString("game-%1").arg(id));
    EXPECT_EQ(mgr.m_lookupCache.gameNodeSerial, QString::number(id + 100));
    EXPECT_TRUE(mgr.m_lookupCache.gameFound);
    EXPECT_EQ(mgr.m_lookupCache.voiceNodeId, id + 1);
    EXPECT_EQ(mgr.m_lookupCache.voiceNodeName, "voice");
    EXPECT_EQ(mgr.m_lookupCache.voiceNodeSerial, QString::number(id + 101));
    EXPECT_TRUE(mgr.m_lookupCache.voiceFound);
  }
};

} // namespace TrophyBackend

using TrophyBackend::ReplayManagerTest;

static QList<Seg> threeSegments() {
  // 60s segments starting at 0/60/120.
  return {{"s0.mkv", 0.0, 60.0}, {"s1.mkv", 60.0, 60.0}, {"s2.mkv", 120.0, 60.0}};
}

TEST_F(ReplayManagerTest, DiagnosticIdentitySurvivesANamesakeDetectionChange) {
  SettingsManager settings;
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
  ReplayManager mgr;
  beginDiagnosticCapture(mgr, settings, detector);
  const auto first = mgr.captureHealth();
  EXPECT_EQ(first["detectedSteamAppId"].toString(), "1422450");
  EXPECT_EQ(first["detectedGamePid"].toLongLong(), 61010);
  EXPECT_FALSE(first.contains("sourcePid"));
  detector.addToBlacklist("steam_app_1422450");
  window.pid = 61011; window.appId = "steam_app_999";
  monitor.ingestSnapshot({window});
  ASSERT_EQ(detector.currentGameKey(), "steam_app_999");
  EXPECT_EQ(mgr.captureHealth()["detectedGameKey"], first["detectedGameKey"]);
  EXPECT_EQ(mgr.captureHealth()["detectedGamePid"], first["detectedGamePid"]);
}

TEST_F(ReplayManagerTest, CoverageDiagnosticsIncludeRetiringButNotEmptyCurrentSegment) {
  verifyCoverageDiagnostics();
}

TEST_F(ReplayManagerTest, FailedPacketWriteIsNotEnlistedForReplay) {
  retireOutput(false, true);
}

TEST_F(ReplayManagerTest, SavedReplayPublishesAudioHistoryBeforeCompletion) {
  ReplayManager manager;
  const auto path = makeSegFile("saved.mkv");
  AudioRoutingInfo history;
  history.known = true; history.system = true; history.separate = true;
  history.primary = 0; history.voiceTrack = 1; history.micTrack = 2;
  bool presentAtSignal = false;
  QObject::connect(&manager, &ReplayManager::replaySaved, &manager,
      [&](const QString &file, double) {
        presentAtSignal = RecordingAnnotations::audioRouting(file).value("system").toBool();
      });
  deliverAudioHistory(manager, path, history.toJson());
  EXPECT_TRUE(presentAtSignal);
  EXPECT_EQ(RecordingAnnotations::audioRouting(path).value("voiceTrack").toInt(), 1);
}

TEST_F(ReplayManagerTest, FailedTrailerIsNotEnlistedForReplay) {
  retireOutput(true, false);
}

TEST_F(ReplayManagerTest, SuccessfulOutputIsEnlistedForReplay) {
  retireOutput(true, true);
}

TEST_F(ReplayManagerTest, SelectForWindowKeepsCoveringSegments) {
  auto segs = threeSegments();
  // now=180, window=30: only the tail segment covers it.
  auto sel = ReplayManager::selectForWindow(segs, 180.0, 30.0);
  ASSERT_EQ(sel.size(), 1);
  EXPECT_EQ(sel.front().path, "s2.mkv");
  EXPECT_DOUBLE_EQ(sel.front().startOffsetSec, 120.0);

  // Window spanning the boundary keeps both, oldest first.
  sel = ReplayManager::selectForWindow(segs, 180.0, 70.0);
  ASSERT_EQ(sel.size(), 2);
  EXPECT_EQ(sel[0].path, "s1.mkv");
  EXPECT_EQ(sel[1].path, "s2.mkv");

  // Empty input selects nothing.
  EXPECT_TRUE(ReplayManager::selectForWindow({}, 180.0, 30.0).isEmpty());
}

TEST_F(ReplayManagerTest, PruneListKeepsMarginAndMinimum) {
  auto segs = threeSegments();
  // now=180, window=30, margin=60: horizon=90. s0 ends at 60 < 90,
  // but 3 segments with min-keep 2 protects... s0 is prunable.
  auto dead = ReplayManager::pruneList(segs, 180.0, 30.0);
  ASSERT_EQ(dead.size(), 1);
  EXPECT_EQ(dead.front(), "s0.mkv");

  // Fresh buffer: nothing past the horizon.
  dead = ReplayManager::pruneList(segs, 100.0, 30.0);
  EXPECT_TRUE(dead.isEmpty());

  // Minimum retention: two segments are never pruned even when old.
  QList<Seg> two{{"a.mkv", 0.0, 60.0}, {"b.mkv", 60.0, 60.0}};
  dead = ReplayManager::pruneList(two, 1000.0, 30.0);
  EXPECT_TRUE(dead.isEmpty());
}

TEST_F(ReplayManagerTest, MigratesLegacyCacheDirOnce) {
  // Hermetic via a fake XDG_CACHE_HOME: fabricate the previous default
  // (<cache>/Trophy/trophy/replay) with a stale segment, then resolve the
  // default cache dir. The legacy dir must be gone afterwards.
  QTemporaryDir fakeCache;
  ASSERT_TRUE(fakeCache.isValid());
  const QByteArray oldCacheHome = qgetenv("XDG_CACHE_HOME");
  qputenv("XDG_CACHE_HOME", fakeCache.path().toUtf8());

  const QString legacy = legacyCacheDir();
  // Guard: never let this test touch the real cache.
  ASSERT_FALSE(legacy.isEmpty());
  ASSERT_TRUE(legacy.startsWith(fakeCache.path()));
  ASSERT_TRUE(QDir().mkpath(legacy));
  const QString stale = legacy + "/stale_000.mkv";
  {
    QFile f(stale);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("fake-segment");
  }

  ReplayManager mgr;
  EXPECT_EQ(defaultCacheDir(mgr), QDir::tempPath() + "/trophy/replay");
  EXPECT_FALSE(QFileInfo::exists(legacy));

  if (oldCacheHome.isNull())
    qunsetenv("XDG_CACHE_HOME");
  else
    qputenv("XDG_CACHE_HOME", oldCacheHome);
}

TEST_F(ReplayManagerTest, DefaultCacheDirIsRamBackedTemp) {
  // No settings: the fallback must be /tmp (QDir::tempPath()), not the
  // persistent system cache — DVR segments are transient by design.
  ReplayManager mgr;
  const QString dir = defaultCacheDir(mgr);
  EXPECT_EQ(dir, QDir::tempPath() + "/trophy/replay");
  EXPECT_TRUE(QDir(dir).exists());
  // Clean up after ourselves: the call mkpaths the dir.
  QDir(QDir::tempPath() + "/trophy").removeRecursively();
}

TEST_F(ReplayManagerTest, InactiveByDefaultAndSaveFailsCleanly) {
  ReplayManager mgr;
  EXPECT_FALSE(mgr.isActive());
  EXPECT_TRUE(mgr.currentGame().isEmpty());

  QSignalSpy failedSpy(&mgr, &ReplayManager::replayFailed);
  mgr.saveReplay();
  ASSERT_EQ(failedSpy.count(), 1);

  // Pausing an idle manager is a safe no-op.
  mgr.setPaused(true);
  mgr.setPaused(false);
  EXPECT_FALSE(mgr.isActive());
}

TEST_F(ReplayManagerTest, DesktopAudioSetupKeyIgnoresSeparateFlag) {
  // Desktop mode always records one merged track, so the separate flag
  // must neither change the session key (no spurious source rebuild)
  // nor leak into multi-stream desktop files. Game mode keeps the
  // distinction. SettingsManager persists on every setter, so isolate
  // the config home first (never touch the user's real config).
  QTemporaryDir fakeConfig;
  ASSERT_TRUE(fakeConfig.isValid());
  const QByteArray oldConfigHome = qgetenv("XDG_CONFIG_HOME");
  qputenv("XDG_CONFIG_HOME", fakeConfig.path().toUtf8());

  {
    SettingsManager settings;
    ReplayManager mgr;
    mgr.setSettingsManager(&settings);

    settings.setAudioMode("desktop");
    settings.setSeparateAudioTracks(false);
    const QString desktopMerged = setupKey(mgr);
    ASSERT_FALSE(desktopMerged.isEmpty());
    EXPECT_TRUE(desktopMerged.endsWith("|mix"));
    settings.setSeparateAudioTracks(true);
    EXPECT_EQ(setupKey(mgr), desktopMerged);

    settings.setAudioMode("game");
    settings.setSeparateAudioTracks(false);
    const QString gameMixed = setupKey(mgr);
    EXPECT_TRUE(gameMixed.endsWith("|mix"));
    settings.setSeparateAudioTracks(true);
    const QString gameSeparate = setupKey(mgr);
    EXPECT_TRUE(gameSeparate.endsWith("|sep"));
    EXPECT_NE(gameSeparate, gameMixed);
  } // SettingsManager persists here, while XDG_CONFIG_HOME is isolated.

  if (oldConfigHome.isNull())
    qunsetenv("XDG_CONFIG_HOME");
  else
    qputenv("XDG_CONFIG_HOME", oldConfigHome);
}

TEST_F(ReplayManagerTest, SaveReplayRefusesWhenStorageFull) {
  // Hermetic config home: SettingsManager persists on destruction, so
  // it must die before the env override is restored (scoped below).
  QTemporaryDir fakeConfig;
  ASSERT_TRUE(fakeConfig.isValid());
  const QByteArray oldConfigHome = qgetenv("XDG_CONFIG_HOME");
  qputenv("XDG_CONFIG_HOME", fakeConfig.path().toUtf8());

  QTemporaryDir libDir;
  ASSERT_TRUE(libDir.isValid());
  {
    QFile f(libDir.path() + "/Game_2026-01-15_20-30-00.mkv");
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(8192, 'x'));
  }
  {
    SettingsManager settings;
    StorageManager storage(libDir.path());
    storage.setMaxSize(1024); // over limit: 8192 used > 1024 max
    ASSERT_TRUE(storage.isOverLimit());

    ReplayManager mgr;
    forceRunningState(mgr, &settings, &storage);
    QSignalSpy failedSpy(&mgr, &ReplayManager::replayFailed);
    QSignalSpy savedSpy(&mgr, &ReplayManager::replaySaved);
    mgr.saveReplay();

    EXPECT_EQ(failedSpy.count(), 1);
    EXPECT_EQ(savedSpy.count(), 0);
    EXPECT_TRUE(failedSpy.first().first().toString().contains("Storage"));
    // Refused before the gapless switch/remux: no clip (or .part) file
    // appeared next to the source.
    EXPECT_EQ(QDir(libDir.path()).entryList({"*.mkv"}, QDir::Files).size(),
              1);
  }

  if (oldConfigHome.isNull())
    qunsetenv("XDG_CONFIG_HOME");
  else
    qputenv("XDG_CONFIG_HOME", oldConfigHome);
}

TEST_F(ReplayManagerTest, StopDvrClearsOpenAndClosedSegments) {
  ReplayManager mgr;
  // Fabricate a mid-recording state: one rotated (closed) segment plus
  // the currently open one. stopDvr must delete both — the open file
  // used to leak because only m_segments was cleaned.
  const QString closed = makeSegFile("seg_000.mkv");
  const QString open = makeSegFile("seg_001.mkv");
  fabricateMidRecordingState(mgr, closed, open);
  ASSERT_TRUE(hasBufferedState(mgr));

  mgr.stopDvr();

  EXPECT_FALSE(QFile::exists(closed));
  EXPECT_FALSE(QFile::exists(open));
  EXPECT_FALSE(hasBufferedState(mgr));
  EXPECT_FALSE(mgr.isActive());
}

TEST_F(ReplayManagerTest, StaleRestoreTokenPreservesGameAfterTeardown) {
  ReplayManager mgr;
  const QString game = QString::fromUtf8("Game: édition");
  const QString closed = makeSegFile("stale_closed.mkv");
  const QString open = makeSegFile("stale_open.mkv");
  fabricateMidRecordingState(mgr, closed, open);
  prepareStaleToken(mgr, game);
  QSignalSpy authSpy(&mgr, &ReplayManager::restoreAuthNeeded);
  bool deliveredAfterTeardown = false;
  QObject::connect(&mgr, &ReplayManager::restoreAuthNeeded, &mgr,
                   [&](const QString &name) {
                     deliveredAfterTeardown =
                         name == game && mgr.currentGame().isEmpty() &&
                         isIdleAndTimerStopped(mgr) && !hasBufferedState(mgr) &&
                         !QFile::exists(closed) && !QFile::exists(open);
                   });

  rejectRestoreToken(mgr);

  ASSERT_EQ(authSpy.count(), 1);
  EXPECT_EQ(authSpy.first().first().toString(), game);
  EXPECT_TRUE(deliveredAfterTeardown);
  EXPECT_TRUE(mgr.currentGame().isEmpty());
}

TEST_F(ReplayManagerTest, CurrentLookupGenerationPublishesCompleteResult) {
  ReplayManager mgr;
  const quint64 generation = prepareLookup(mgr);
  deliverLookup(mgr, generation, 42);
  expectLookup(mgr, true, 42);
}

TEST_F(ReplayManagerTest, LookupDeliveryAfterStopCannotReviveCache) {
  ReplayManager mgr;
  const quint64 generation = prepareLookup(mgr);
  mgr.stopDvr();
  deliverLookup(mgr, generation, 42);
  expectLookup(mgr, false, 0);
}

TEST_F(ReplayManagerTest, SupersededLookupCannotOverwriteNewerCache) {
  ReplayManager mgr;
  const quint64 staleGeneration = prepareLookup(mgr);
  const quint64 currentGeneration = prepareLookup(mgr);
  deliverLookup(mgr, currentGeneration, 84);
  deliverLookup(mgr, staleGeneration, 42);
  expectLookup(mgr, true, 84);
}

TEST_F(ReplayManagerTest, QueuedOldCaptureEventsCannotStopReplacement) {
  for (bool ready : {false, true}) {
    ReplayManager mgr;
    QSignalSpy selection(&mgr, &ReplayManager::captureSelectionFinished);
    auto *old = replaceCapture(mgr);
    if (ready)
      emit old->ready();
    else
      emit old->captureError("retired error");
    replaceCapture(mgr); // destroys sender, but already queued calls survive
    QCoreApplication::processEvents();
    EXPECT_TRUE(isStarting(mgr));
    EXPECT_EQ(selection.count(), 0);
    EXPECT_EQ(mgr.currentGame(), "Replacement");
  }
}

TEST_F(ReplayManagerTest, RotationPruningRetainsSaveWorkerInputs) {
  QTemporaryDir config;
  ASSERT_TRUE(config.isValid());
  const QByteArray oldConfig = qgetenv("XDG_CONFIG_HOME");
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  {
    // No worker or PipeWire: drive actual pruning with owned input paths.
    SettingsManager settings;
    ReplayManager mgr;
    const QString pinned = makeSegFile("saving.mkv");
    preparePruningSave(mgr, settings, pinned);
    prune(mgr);
    EXPECT_TRUE(QFile::exists(pinned));
    releaseSaveInputs(mgr);
    prune(mgr);
    EXPECT_FALSE(QFile::exists(pinned));
  }
  if (oldConfig.isNull())
    qunsetenv("XDG_CONFIG_HOME");
  else
    qputenv("XDG_CONFIG_HOME", oldConfig);
}

TEST_F(ReplayManagerTest, RotationKeepsLateVoiceAndMicSlotsDespiteCachedAvailability) {
  QTemporaryDir config;
  ASSERT_TRUE(config.isValid());
  const QByteArray oldConfig = qgetenv("XDG_CONFIG_HOME");
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  {
    SettingsManager settings;
    settings.setAudioMode("game");
    settings.setSeparateAudioTracks(false);
    settings.setVoiceChatApp("voice");
    ReplayManager mgr;
    prepareLateVoiceRouting(mgr, settings);
    expectRetainedRouting(mgr, true);
    expectRetainedRouting(mgr, false);
  }
  if (oldConfig.isNull())
    qunsetenv("XDG_CONFIG_HOME");
  else
    qputenv("XDG_CONFIG_HOME", oldConfig);
}

TEST_F(ReplayManagerTest, MissingGameKeepsDesktopFallbackWithoutDoublingVoice) {
  QTemporaryDir config;
  ASSERT_TRUE(config.isValid());
  const QByteArray oldConfig = qgetenv("XDG_CONFIG_HOME");
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  {
    SettingsManager settings;
    settings.setAudioMode("game");
    settings.setVoiceChatApp("voice");
    for (bool separate : {false, true}) {
      settings.setSeparateAudioTracks(separate);
      ReplayManager mgr;
      mgr.setSettingsManager(&settings);
      expectFallbackRouting(mgr, separate);
    }
  }
  if (oldConfig.isNull())
    qunsetenv("XDG_CONFIG_HOME");
  else
    qputenv("XDG_CONFIG_HOME", oldConfig);
}

TEST_F(ReplayManagerTest, DisablingLateAdoptedVoiceRemovesItsActualSlot) {
  QTemporaryDir config;
  ASSERT_TRUE(config.isValid());
  const QByteArray oldConfig = qgetenv("XDG_CONFIG_HOME");
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  {
    SettingsManager settings;
    settings.setAudioMode("game");
    settings.setSeparateAudioTracks(false);
    settings.setVoiceChatApp("");
    bool destroyed = false;
    ReplayManager mgr;
    prepareLateVoiceRouting(mgr, settings);
    addLateVoiceSource(mgr, destroyed);
    mgr.applyVoiceChatApp();
    waitSourceOperations(mgr);
    EXPECT_TRUE(destroyed);
  }
  if (oldConfig.isNull())
    qunsetenv("XDG_CONFIG_HOME");
  else
    qputenv("XDG_CONFIG_HOME", oldConfig);
}

TEST_F(ReplayManagerTest, AsyncFullStopRetainsPipelineAndKeepsHeartbeat) {
  gatedRetirement(true, true);
}

TEST_F(ReplayManagerTest, AsyncHandoffEnlistsOnlyCompletedOutput) {
  gatedRetirement(false, true);
}

TEST_F(ReplayManagerTest, AsyncHandoffFailureStopsOnceWithoutEnlistingOutput) {
  gatedRetirement(false, false);
}

TEST_F(ReplayManagerTest, StartupSettingsChangeRejectsOldSnapshot) {
  gatedStartup(false);
}

TEST_F(ReplayManagerTest, ReplacedCaptureRejectsLookupUntilNewGrant) {
  gatedStartup(true);
}

TEST_F(ReplayManagerTest, AsyncStopRetainsRemuxInputsUntilWorkerCompletes) {
  stopRetainsHeldRemuxInputs();
}

TEST_F(ReplayManagerTest, ExplicitStopCancelsDeferredSameGameRestart) {
  gatedRetirement(true, true, true);
}

TEST_F(ReplayManagerTest, SemanticControlsRememberIdlePreferencesAcrossPhysicalRelayout) {
  semanticPreferencesSurviveIdleAndRelayout();
}
