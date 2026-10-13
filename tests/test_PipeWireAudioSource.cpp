/*
 * Trophy - Unit Tests
 * Production lifecycle fault injection and real buffer/POD validation,
 * without a PipeWire daemon.
 */

#include "audio/PipeWireAudioSource.h"
#include <gtest/gtest.h>
#include <spa/buffer/buffer.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <future>
#include <memory>
#include <limits>
#include <thread>

using namespace TrophyBackend;

class PipeWireAudioSourceTest : public ::testing::Test {
protected:
  struct Lifecycle {
    QString failure;
    QStringList calls;
    int loopToken = 0, contextToken = 0, coreToken = 0, streamToken = 0;
    int propertiesToken = 0;
    bool loop = false, context = false, core = false, stream = false;
    bool started = false, locked = false, properties = false;
    const pw_stream_events *events = nullptr;
    void *userdata = nullptr;
    const pw_core_events *coreEvents = nullptr;
    void *coreUserdata = nullptr;
    pw_buffer *buffer = nullptr;
    std::recursive_mutex loopMutex;
    std::function<void()> lockGate, connectGate;
    int dequeues = 0, queues = 0;
    QStringList errors;
    int startedSignals = 0, stoppedSignals = 0;
    std::function<void()> contextGate, startGate;
    bool call(const char *name) {
      calls.append(QString::fromLatin1(name));
      return failure == QLatin1String(name);
    }
    bool empty() const {
      return !loop && !context && !core && !stream && !started && !locked &&
             !properties;
    }
  } lifecycle;
  inline static thread_local Lifecycle *active = nullptr;
  PipeWireAudioSource source{IAudioSource::SourceType::DesktopAudio};

  void installOperations(PipeWireAudioSource &target) {
    active = &lifecycle;
    target.m_operations = {
        [] { active->call("init"); },
        []() -> pw_thread_loop * {
          EXPECT_FALSE(active->loop);
          if (active->call("loop.new")) return nullptr;
          active->loop = true;
          return reinterpret_cast<pw_thread_loop *>(&active->loopToken);
        },
        [](pw_thread_loop *) -> pw_context * {
          EXPECT_TRUE(active->loop);
          EXPECT_FALSE(active->context);
          if (active->call("context.new")) return nullptr;
          if (active->contextGate) active->contextGate();
          active->context = true;
          return reinterpret_cast<pw_context *>(&active->contextToken);
        },
        [](pw_thread_loop *) {
          EXPECT_TRUE(active->loop);
          EXPECT_FALSE(active->started);
          if (active->call("loop.start")) return -EIO;
          if (active->startGate) active->startGate();
          active->started = true;
          return 0;
        },
        [](pw_thread_loop *) {
          if (active->lockGate) active->lockGate();
          active->loopMutex.lock();
          EXPECT_TRUE(active->started);
          EXPECT_FALSE(active->locked);
          active->call("loop.lock"); active->locked = true;
        },
        [](pw_thread_loop *) {
          EXPECT_TRUE(active->locked);
          active->call("loop.unlock"); active->locked = false;
          active->loopMutex.unlock();
        },
        [](pw_thread_loop *) {
          EXPECT_TRUE(active->started);
          EXPECT_FALSE(active->locked);
          EXPECT_FALSE(active->stream);
          EXPECT_FALSE(active->core);
          active->call("loop.stop"); active->started = false;
        },
        [](pw_thread_loop *) {
          EXPECT_TRUE(active->loop);
          EXPECT_FALSE(active->started);
          EXPECT_FALSE(active->locked);
          EXPECT_FALSE(active->context);
          active->call("loop.destroy"); active->loop = false;
        },
        [](pw_context *) -> pw_core * {
          EXPECT_TRUE(active->context);
          EXPECT_TRUE(active->locked);
          if (active->call("core.connect")) return nullptr;
          active->core = true;
          return reinterpret_cast<pw_core *>(&active->coreToken);
        },
        [](pw_core *) {
          EXPECT_TRUE(active->core);
          EXPECT_TRUE(active->locked);
          EXPECT_FALSE(active->stream);
          active->call("core.disconnect"); active->core = false;
          active->coreEvents = nullptr; active->coreUserdata = nullptr;
          return 0;
        },
        [](pw_context *) {
          EXPECT_TRUE(active->context);
          EXPECT_FALSE(active->started);
          EXPECT_FALSE(active->core);
          active->call("context.destroy"); active->context = false;
        },
        []() -> pw_properties * {
          EXPECT_TRUE(active->locked);
          if (active->call("properties.new")) return nullptr;
          active->properties = true;
          return reinterpret_cast<pw_properties *>(&active->propertiesToken);
        },
        [](pw_properties *, const char *key, const char *) {
          EXPECT_TRUE(active->properties);
          return active->call((QStringLiteral("properties.set:") +
                               QString::fromLatin1(key)).toLatin1().constData())
                     ? -ENOMEM : 1;
        },
        [](pw_properties *) {
          EXPECT_TRUE(active->properties);
          active->call("properties.free"); active->properties = false;
        },
        [](pw_core *, const char *, pw_properties *) -> pw_stream * {
          EXPECT_TRUE(active->locked);
          EXPECT_TRUE(active->core);
          EXPECT_TRUE(active->properties);
          active->properties = false; // PipeWire takes ownership, even on failure.
          if (active->call("stream.new")) return nullptr;
          active->stream = true;
          return reinterpret_cast<pw_stream *>(&active->streamToken);
        },
        [](pw_stream *, spa_hook *, const pw_stream_events *events, void *data) {
          EXPECT_TRUE(active->stream);
          EXPECT_TRUE(active->locked);
          active->call("stream.listen");
          active->events = events; active->userdata = data;
        },
        [](pw_stream *, uint32_t, int, const spa_pod *param) {
          EXPECT_TRUE(active->stream);
          EXPECT_TRUE(active->locked);
          EXPECT_NE(param, nullptr);
          spa_audio_info_raw info{};
          EXPECT_GE(spa_format_audio_raw_parse(param, &info), 0);
          EXPECT_EQ(info.format, SPA_AUDIO_FORMAT_F32);
          EXPECT_GT(info.channels, 0u);
          EXPECT_GT(info.rate, 0u);
          if (active->connectGate) active->connectGate();
          return active->call("stream.connect") ? -EIO : 0;
        },
        [](pw_stream *) {
          EXPECT_TRUE(active->stream);
          EXPECT_TRUE(active->locked);
          active->call("stream.destroy"); active->stream = false;
          active->events = nullptr; active->userdata = nullptr;
        },
        [](pw_core *, spa_hook *, const pw_core_events *events, void *data) {
          active->coreEvents = events; active->coreUserdata = data;
        },
        [](pw_stream *) -> pw_buffer * {
          EXPECT_TRUE(active->stream);
          ++active->dequeues;
          return active->buffer;
        },
        [](pw_stream *, pw_buffer *buffer) {
          EXPECT_TRUE(active->stream);
          EXPECT_EQ(buffer, active->buffer);
          ++active->queues;
          return 0;
        },
        [](pw_stream *) -> const pw_properties * { return nullptr; }};
    QObject::connect(&target, &IAudioSource::captureStarted, &target,
                     [this] { ++lifecycle.startedSignals; }, Qt::DirectConnection);
    QObject::connect(&target, &IAudioSource::captureStopped, &target,
                     [this] { ++lifecycle.stoppedSignals; }, Qt::DirectConnection);
    QObject::connect(&target, &IAudioSource::captureError, &target,
                     [this](const QString &message) { lifecycle.errors << message; },
                     Qt::DirectConnection);
  }
  void SetUp() override { installOperations(source); }
  void TearDown() override {
    source.stop();
    EXPECT_TRUE(lifecycle.empty());
  }
  void deliver(spa_buffer *buffer, int channels = 2, int rate = 48000) {
    source.processBuffer(buffer, channels, rate);
  }
  auto snapshot() const { return source.snapshotCallback(); }
  AudioConfig config() const { return source.m_config; }
  bool lifecycleBusy() {
    if (!source.m_lifecycleMutex.try_lock()) return true;
    source.m_lifecycleMutex.unlock();
    return false;
  }

  struct Buffer {
    float samples[4]{0.25f, -0.5f, 0.75f, -1.0f};
    spa_chunk chunk{};
    spa_data data{};
    spa_buffer buffer{};
    Buffer() {
      chunk.size = sizeof(samples);
      chunk.offset = 0;
      data.data = samples;
      data.maxsize = sizeof(samples);
      data.chunk = &chunk;
      buffer.n_datas = 1;
      buffer.datas = &data;
    }
  };
};

TEST_F(PipeWireAudioSourceTest, InvalidConfigRejectedBeforePipeWireSetup) {
  const auto original = config();
  for (const auto &invalid : {AudioConfig{48000, 0, 1024},
                              AudioConfig{48000, -1, 1024},
                              AudioConfig{0, 2, 1024},
                              AudioConfig{-1, 2, 1024},
                              AudioConfig{0, 0, 1024}}) {
    SCOPED_TRACE(::testing::Message() << "rate=" << invalid.sampleRate
                                    << " channels=" << invalid.channels);
    EXPECT_FALSE(source.initialize(invalid));
    EXPECT_EQ(config().sampleRate, original.sampleRate);
    EXPECT_EQ(config().channels, original.channels);
    EXPECT_FALSE(source.start()); // Missing loop/context: no connection attempt.
    EXPECT_FALSE(source.isCapturing());
  }
  EXPECT_TRUE(lifecycle.calls.isEmpty());
}

TEST_F(PipeWireAudioSourceTest, InitializationFailuresCleanUpAndCanRetry) {
  for (const QString &failure : {QStringLiteral("loop.new"),
                                 QStringLiteral("context.new")}) {
    SCOPED_TRACE(failure.toStdString());
    lifecycle.calls.clear();
    lifecycle.failure = failure;
    EXPECT_FALSE(source.initialize(AudioConfig{}));
    EXPECT_TRUE(lifecycle.empty());
    const QStringList expected = failure == QStringLiteral("loop.new")
        ? QStringList{"init", "loop.new"}
        : QStringList{"init", "loop.new", "context.new", "loop.destroy"};
    EXPECT_EQ(lifecycle.calls, expected);
    source.stop();
    EXPECT_EQ(lifecycle.calls, expected); // No double-destroy/stop/lock.
    EXPECT_FALSE(source.start());
    EXPECT_EQ(lifecycle.calls, expected);
    lifecycle.failure.clear();
    ASSERT_TRUE(source.initialize(AudioConfig{}));
    ASSERT_TRUE(source.start());
    source.stop();
    EXPECT_TRUE(lifecycle.empty());
  }
  EXPECT_EQ(lifecycle.startedSignals, 2);
  EXPECT_EQ(lifecycle.stoppedSignals, 2);
}

TEST_F(PipeWireAudioSourceTest, StartFailuresUnwindInOrderAndCanRetry) {
  struct Failure {
    const char *operation;
    QStringList expected;
  };
  const QStringList startPrefix{"loop.start", "loop.lock", "core.connect"};
  const QStringList cleanup{"core.disconnect", "loop.unlock", "loop.stop",
                            "context.destroy", "loop.destroy"};
  const QStringList propsPrefix = startPrefix + QStringList{
      "properties.new", "properties.set:stream.capture.sink"};
  for (const auto &failure : {
           Failure{"loop.start", {"loop.start", "context.destroy", "loop.destroy"}},
           Failure{"core.connect", startPrefix + QStringList{
               "loop.unlock", "loop.stop", "context.destroy", "loop.destroy"}},
           Failure{"properties.new", startPrefix + QStringList{"properties.new"} + cleanup},
           Failure{"properties.set:stream.capture.sink",
                   propsPrefix + QStringList{"properties.free"} + cleanup},
           Failure{"stream.new", propsPrefix + QStringList{"stream.new"} + cleanup},
           Failure{"stream.connect", propsPrefix + QStringList{
               "stream.new", "stream.listen", "stream.connect", "loop.unlock",
               "loop.lock", "stream.destroy"} + cleanup}}) {
    SCOPED_TRACE(failure.operation);
    ASSERT_TRUE(source.initialize(AudioConfig{}));
    lifecycle.calls.clear();
    lifecycle.failure = QString::fromLatin1(failure.operation);
    const int starts = lifecycle.startedSignals;
    const int stops = lifecycle.stoppedSignals;
    EXPECT_FALSE(source.start());
    EXPECT_FALSE(source.isCapturing());
    EXPECT_TRUE(lifecycle.empty());
    EXPECT_EQ(lifecycle.calls, failure.expected);
    EXPECT_EQ(lifecycle.startedSignals, starts);
    EXPECT_EQ(lifecycle.stoppedSignals, stops);
    source.stop();
    EXPECT_EQ(lifecycle.calls, failure.expected);
    EXPECT_FALSE(source.start()); // Fully unwound: retry requires initialize.
    EXPECT_EQ(lifecycle.calls, failure.expected);
    lifecycle.failure.clear();
    ASSERT_TRUE(source.initialize(AudioConfig{44100, 1, 1024}));
    ASSERT_TRUE(source.start());
    EXPECT_TRUE(source.isCapturing());
    EXPECT_EQ(lifecycle.startedSignals, starts + 1);
    source.stop();
    EXPECT_TRUE(lifecycle.empty());
    EXPECT_EQ(lifecycle.stoppedSignals, stops + 1);
  }
}

TEST_F(PipeWireAudioSourceTest, InitializedOnlyStopNeverLocksOrStopsUnstartedLoop) {
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  lifecycle.calls.clear();
  source.stop();
  EXPECT_EQ(lifecycle.calls, (QStringList{"context.destroy", "loop.destroy"}));
  source.stop();
  EXPECT_EQ(lifecycle.calls.size(), 2);
  EXPECT_EQ(lifecycle.stoppedSignals, 0);
  EXPECT_TRUE(lifecycle.empty());
}

TEST_F(PipeWireAudioSourceTest, ReinitializeRetiresInitializedAndCapturingResources) {
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  lifecycle.calls.clear();
  ASSERT_TRUE(source.initialize(AudioConfig{44100, 1, 1024}));
  EXPECT_EQ(lifecycle.calls, (QStringList{
      "context.destroy", "loop.destroy", "init", "loop.new", "context.new"}));
  ASSERT_TRUE(source.start());
  lifecycle.calls.clear();
  ASSERT_TRUE(source.initialize(AudioConfig{48000, 2, 1024}));
  EXPECT_EQ(lifecycle.calls, (QStringList{
      "loop.lock", "stream.destroy", "core.disconnect", "loop.unlock",
      "loop.stop", "context.destroy", "loop.destroy", "init", "loop.new",
      "context.new"}));
  EXPECT_FALSE(source.isCapturing());
  EXPECT_EQ(lifecycle.startedSignals, 1);
  EXPECT_EQ(lifecycle.stoppedSignals, 1);
  ASSERT_TRUE(source.start());
  lifecycle.calls.clear();
  EXPECT_FALSE(source.start());
  EXPECT_TRUE(lifecycle.calls.isEmpty());
  EXPECT_FALSE(source.initialize(AudioConfig{48000, 0, 1024}));
  EXPECT_TRUE(lifecycle.calls.isEmpty());
  EXPECT_TRUE(source.isCapturing());
  EXPECT_EQ(config().channels, 2);
  source.stop();
  EXPECT_EQ(lifecycle.stoppedSignals, 2);
  EXPECT_TRUE(lifecycle.empty());
}

TEST_F(PipeWireAudioSourceTest, ReinitializeAllocationFailureDoesNotKeepOldOwners) {
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  ASSERT_TRUE(source.start());
  lifecycle.failure = QStringLiteral("context.new");
  EXPECT_FALSE(source.initialize(AudioConfig{}));
  EXPECT_TRUE(lifecycle.empty());
  EXPECT_FALSE(source.isCapturing());
  EXPECT_EQ(lifecycle.stoppedSignals, 1);
  lifecycle.failure.clear();
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  ASSERT_TRUE(source.start());
}

TEST_F(PipeWireAudioSourceTest, TargetPropertyFailuresNeverCreateUnpinnedStream) {
  // Factory is lookup-free for application sources with a supplied identity.
  for (const QString &failure : {QStringLiteral("properties.set:target.object"),
                                 QStringLiteral("properties.set:node.target")}) {
    SCOPED_TRACE(failure.toStdString());
    auto app = PipeWireAudioSource::createApplicationSource(42, "game", "99");
    installOperations(*app);
    ASSERT_TRUE(app->initialize(AudioConfig{}));
    lifecycle.calls.clear();
    lifecycle.failure = failure;
    EXPECT_FALSE(app->start());
    EXPECT_TRUE(lifecycle.empty());
    EXPECT_FALSE(lifecycle.calls.contains("stream.new"));
    EXPECT_EQ(lifecycle.calls.count("properties.free"), 1);
    EXPECT_EQ(lifecycle.calls.mid(lifecycle.calls.indexOf("properties.free")),
              (QStringList{"properties.free", "core.disconnect", "loop.unlock",
                           "loop.stop", "context.destroy", "loop.destroy"}));
    lifecycle.failure.clear();
    ASSERT_TRUE(app->initialize(AudioConfig{}));
    ASSERT_TRUE(app->start());
    app.reset(); // The real destructor must run the same cleanup.
    EXPECT_TRUE(lifecycle.empty());
  }
}

TEST_F(PipeWireAudioSourceTest, DestructorCleansInitializedOnlyAndRunningSources) {
  for (const bool start : {false, true}) {
    auto owned = std::make_unique<PipeWireAudioSource>(
        IAudioSource::SourceType::DesktopAudio);
    installOperations(*owned);
    ASSERT_TRUE(owned->initialize(AudioConfig{}));
    if (start) {
      ASSERT_TRUE(owned->start());
    }
    lifecycle.calls.clear();
    owned.reset();
    EXPECT_TRUE(lifecycle.empty());
    const QStringList expected = start
        ? QStringList{"loop.lock", "stream.destroy", "core.disconnect",
                      "loop.unlock", "loop.stop", "context.destroy", "loop.destroy"}
        : QStringList{"context.destroy", "loop.destroy"};
    EXPECT_EQ(lifecycle.calls, expected);
  }
  EXPECT_EQ(lifecycle.startedSignals, 1);
  EXPECT_EQ(lifecycle.stoppedSignals, 1);
}

TEST_F(PipeWireAudioSourceTest, ConcurrentStopSerializesWithInFlightStart) {
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  lifecycle.calls.clear();
  std::promise<void> entered, release, stopRequested;
  auto enteredFuture = entered.get_future();
  auto releaseFuture = release.get_future().share();
  auto stopFuture = stopRequested.get_future();
  lifecycle.startGate = [&] {
    entered.set_value();
    releaseFuture.wait();
  };
  auto starter = std::async(std::launch::async, [&] {
    active = &lifecycle;
    return source.start();
  });
  enteredFuture.wait();
  EXPECT_TRUE(lifecycleBusy()); // The real start path owns the lifecycle lock.
  auto stopper = std::async(std::launch::async, [&] {
    active = &lifecycle;
    stopRequested.set_value();
    source.stop();
  });
  stopFuture.wait();
  // Callback replacement/delivery must remain independent of lifecycle waits.
  int deliveries = 0;
  source.setCallback([&](const AudioFrame &) { ++deliveries; });
  Buffer b;
  deliver(&b.buffer);
  EXPECT_EQ(deliveries, 1);
  source.setCallback({});
  release.set_value();
  EXPECT_TRUE(starter.get());
  stopper.get();
  lifecycle.startGate = {};
  EXPECT_TRUE(lifecycle.empty());
  EXPECT_FALSE(source.isCapturing());
  EXPECT_EQ(lifecycle.calls, (QStringList{
      "loop.start", "loop.lock", "core.connect", "properties.new",
      "properties.set:stream.capture.sink", "stream.new", "stream.listen",
      "stream.connect", "loop.unlock", "loop.lock", "stream.destroy",
      "core.disconnect", "loop.unlock", "loop.stop", "context.destroy",
      "loop.destroy"}));
  EXPECT_EQ(lifecycle.startedSignals, 1);
  EXPECT_EQ(lifecycle.stoppedSignals, 1);
}

TEST_F(PipeWireAudioSourceTest, ConcurrentStopSerializesWithInFlightInitialize) {
  std::promise<void> entered, release, stopRequested;
  auto enteredFuture = entered.get_future();
  auto releaseFuture = release.get_future().share();
  auto stopFuture = stopRequested.get_future();
  lifecycle.contextGate = [&] {
    entered.set_value();
    releaseFuture.wait();
  };
  auto initializer = std::async(std::launch::async, [&] {
    active = &lifecycle;
    return source.initialize(AudioConfig{});
  });
  enteredFuture.wait();
  EXPECT_TRUE(lifecycleBusy());
  auto stopper = std::async(std::launch::async, [&] {
    active = &lifecycle;
    stopRequested.set_value();
    source.stop();
  });
  stopFuture.wait();
  release.set_value();
  EXPECT_TRUE(initializer.get());
  stopper.get();
  lifecycle.contextGate = {};
  EXPECT_EQ(lifecycle.calls, (QStringList{
      "init", "loop.new", "context.new", "context.destroy", "loop.destroy"}));
  EXPECT_TRUE(lifecycle.empty());
  EXPECT_EQ(lifecycle.startedSignals, 0);
  EXPECT_EQ(lifecycle.stoppedSignals, 0);
}

TEST_F(PipeWireAudioSourceTest, AsyncStreamErrorRetiresCaptureUntilReinitialize) {
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  ASSERT_TRUE(source.start());
  Buffer b;
  pw_buffer native{};
  native.buffer = &b.buffer;
  lifecycle.buffer = &native;
  int deliveries = 0;
  source.setCallback([&](const AudioFrame &) { ++deliveries; });
  lifecycle.events->process(lifecycle.userdata);
  EXPECT_EQ(deliveries, 1);
  EXPECT_FLOAT_EQ(source.peakLevel(), 1.0f);
  lifecycle.events->state_changed(lifecycle.userdata, PW_STREAM_STATE_PAUSED,
                                 PW_STREAM_STATE_ERROR, "route failed");
  EXPECT_FALSE(source.isCapturing());
  EXPECT_FLOAT_EQ(source.peakLevel(), 0.0f);
  EXPECT_EQ(lifecycle.errors, (QStringList{"route failed"}));
  EXPECT_EQ(lifecycle.stoppedSignals, 1);
  // No loop teardown inside a listener, and no delivery after terminal error.
  EXPECT_TRUE(lifecycle.stream);
  lifecycle.events->process(lifecycle.userdata);
  EXPECT_EQ(deliveries, 1);
  EXPECT_EQ(lifecycle.dequeues, 1);
  EXPECT_EQ(lifecycle.queues, 1);
  lifecycle.events->state_changed(lifecycle.userdata, PW_STREAM_STATE_ERROR,
                                 PW_STREAM_STATE_ERROR, nullptr);
  lifecycle.events->state_changed(lifecycle.userdata, PW_STREAM_STATE_ERROR,
                                 PW_STREAM_STATE_STREAMING, nullptr);
  EXPECT_FALSE(source.isCapturing()); // A late state event cannot revive it.
  EXPECT_EQ(lifecycle.errors.size(), 1);
  EXPECT_FALSE(source.start());
  source.stop();
  EXPECT_TRUE(lifecycle.empty());
  EXPECT_EQ(lifecycle.stoppedSignals, 1);
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  ASSERT_TRUE(source.start());
  EXPECT_TRUE(source.isCapturing());
}

TEST_F(PipeWireAudioSourceTest, CoreDisconnectAndFatalErrorRetireCapture) {
  for (const int result : {-EPIPE, -EIO}) {
    ASSERT_TRUE(source.initialize(AudioConfig{}));
    ASSERT_TRUE(source.start());
    ASSERT_NE(lifecycle.coreEvents, nullptr);
    lifecycle.coreEvents->error(lifecycle.coreUserdata, PW_ID_CORE, 1, result,
                               result == -EPIPE ? "disconnected" : nullptr);
    EXPECT_FALSE(source.isCapturing());
    EXPECT_EQ(lifecycle.errors.last(), result == -EPIPE
                  ? QStringLiteral("disconnected")
                  : QStringLiteral("PipeWire core error"));
    const int errors = lifecycle.errors.size();
    lifecycle.events->state_changed(lifecycle.userdata, PW_STREAM_STATE_PAUSED,
                                   PW_STREAM_STATE_ERROR, "same failure");
    EXPECT_EQ(lifecycle.errors.size(), errors);
    EXPECT_FALSE(source.start());
    source.stop();
    EXPECT_TRUE(lifecycle.empty());
    EXPECT_EQ(lifecycle.stoppedSignals, lifecycle.startedSignals);
    EXPECT_EQ(lifecycle.coreEvents, nullptr);
  }
}

TEST_F(PipeWireAudioSourceTest, ObjectLocalCoreErrorsAndPausedStateAreNotFatal) {
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  ASSERT_TRUE(source.start());
  lifecycle.coreEvents->error(lifecycle.coreUserdata, 42, 1, -EINVAL,
                             "other object failed");
  lifecycle.coreEvents->error(lifecycle.coreUserdata, PW_ID_CORE, 1, 0, nullptr);
  lifecycle.events->state_changed(lifecycle.userdata, PW_STREAM_STATE_STREAMING,
                                 PW_STREAM_STATE_PAUSED, nullptr);
  // A stream initially disconnected (e.g. an unpinned app) is not an error.
  lifecycle.events->state_changed(lifecycle.userdata, PW_STREAM_STATE_CONNECTING,
                                 PW_STREAM_STATE_UNCONNECTED, nullptr);
  EXPECT_TRUE(source.isCapturing());
  EXPECT_TRUE(lifecycle.errors.isEmpty());
  EXPECT_EQ(lifecycle.stoppedSignals, 0);
}

TEST_F(PipeWireAudioSourceTest, ListenerFailureDuringConnectCannotBeOverwrittenByStart) {
  for (const bool coreError : {false, true}) {
    ASSERT_TRUE(source.initialize(AudioConfig{}));
    const int starts = lifecycle.startedSignals;
    lifecycle.connectGate = [&] {
      if (coreError)
        lifecycle.coreEvents->error(lifecycle.coreUserdata, PW_ID_CORE, 1,
                                   -EPIPE, "connect disconnected");
      else
        lifecycle.events->state_changed(lifecycle.userdata,
            PW_STREAM_STATE_CONNECTING, PW_STREAM_STATE_ERROR, nullptr);
    };
    EXPECT_FALSE(source.start());
    EXPECT_FALSE(source.isCapturing());
    EXPECT_TRUE(lifecycle.empty());
    EXPECT_EQ(lifecycle.startedSignals, starts);
    EXPECT_EQ(lifecycle.stoppedSignals, 0);
    EXPECT_FALSE(source.start());
    lifecycle.connectGate = {};
  }
  EXPECT_EQ(lifecycle.errors,
            (QStringList{"PipeWire error", "connect disconnected"}));
}

TEST_F(PipeWireAudioSourceTest, NativeProcessNullDequeueNeverQueues) {
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  ASSERT_TRUE(source.start());
  lifecycle.events->process(lifecycle.userdata);
  EXPECT_EQ(lifecycle.dequeues, 1);
  EXPECT_EQ(lifecycle.queues, 0);
}

TEST_F(PipeWireAudioSourceTest, StopWaitsForEnteredNativeProcessAndBufferReturn) {
  ASSERT_TRUE(source.initialize(AudioConfig{}));
  ASSERT_TRUE(source.start());
  Buffer b;
  pw_buffer native{};
  native.buffer = &b.buffer;
  lifecycle.buffer = &native;
  std::promise<void> entered, release, lockRequested;
  auto enteredFuture = entered.get_future();
  auto releaseFuture = release.get_future().share();
  auto lockFuture = lockRequested.get_future();
  source.setCallback([&](const AudioFrame &frame) {
    EXPECT_EQ(frame.samples, b.samples);
    entered.set_value();
    releaseFuture.wait();
    EXPECT_TRUE(lifecycle.stream); // Owner survives the entered native callback.
    EXPECT_TRUE(lifecycle.core);
  });
  auto processor = std::async(std::launch::async, [&] {
    active = &lifecycle;
    // PipeWire dispatches native events under its loop lock. Model precisely
    // that synchronization, not a daemon or an independent processBuffer call.
    std::lock_guard loopLock(lifecycle.loopMutex);
    lifecycle.events->process(lifecycle.userdata);
  });
  enteredFuture.wait();
  lifecycle.lockGate = [&] { lockRequested.set_value(); };
  auto stopper = std::async(std::launch::async, [&] {
    active = &lifecycle;
    source.stop();
  });
  lockFuture.wait();
  EXPECT_TRUE(lifecycleBusy());
  EXPECT_EQ(stopper.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);
  // Callback publication is independent of the lifecycle lock; clearing it
  // cannot release the registration already retained by onProcess.
  source.setCallback({});
  release.set_value();
  processor.get();
  stopper.get();
  lifecycle.lockGate = {};
  EXPECT_EQ(lifecycle.dequeues, 1);
  EXPECT_EQ(lifecycle.queues, 1); // Returned while stream was still owned.
  EXPECT_TRUE(lifecycle.empty());
  EXPECT_FALSE(source.isCapturing());
  EXPECT_EQ(lifecycle.stoppedSignals, 1);
}

TEST_F(PipeWireAudioSourceTest, NullAndEmptyBuffersDoNotDeliverOrChangePeak) {
  int calls = 0;
  source.setCallback([&](const AudioFrame &) { ++calls; });
  deliver(nullptr);
  spa_buffer empty{};
  deliver(&empty);
  empty.n_datas = 1; // Malformed buffer: nonzero count with null data array.
  deliver(&empty);

  Buffer b;
  b.data.chunk = nullptr;
  deliver(&b.buffer);
  b.data.chunk = &b.chunk;
  b.data.data = nullptr;
  deliver(&b.buffer);
  b.data.data = b.samples;
  b.chunk.size = 0;
  deliver(&b.buffer);
  EXPECT_EQ(calls, 0);
  EXPECT_FLOAT_EQ(source.peakLevel(), 0.0f);
}

TEST_F(PipeWireAudioSourceTest, NonpositiveChannelsDoNotDeliver) {
  int calls = 0;
  source.setCallback([&](const AudioFrame &) { ++calls; });
  Buffer b;
  deliver(&b.buffer, 0);
  deliver(&b.buffer, -1);
  EXPECT_EQ(calls, 0);
  EXPECT_FLOAT_EQ(source.peakLevel(), 0.0f);
}

TEST_F(PipeWireAudioSourceTest, SharedProductionPathDeliversFrameAndGain) {
  Buffer b;
  int calls = 0;
  source.setVolume(0.5f);
  source.setCallback([&](const AudioFrame &frame) {
    ++calls;
    EXPECT_EQ(frame.samples, b.samples);
    EXPECT_EQ(frame.frameCount, 2);
    EXPECT_EQ(frame.channels, 2);
    EXPECT_EQ(frame.sampleRate, 44100);
    EXPECT_EQ(frame.timestamp, 0);
    EXPECT_FLOAT_EQ(frame.samples[0], 0.125f);
    EXPECT_FLOAT_EQ(frame.samples[3], -0.5f);
  });
  deliver(&b.buffer, 2, 44100);
  EXPECT_EQ(calls, 1);
  EXPECT_FLOAT_EQ(source.peakLevel(), 1.0f); // Meter is pre-gain.
}

TEST_F(PipeWireAudioSourceTest, UnpublishedCallbackLeavesBufferUntouched) {
  Buffer b;
  source.setVolume(0.5f);
  source.setCallback([](const AudioFrame &) {});
  source.setCallback({});
  EXPECT_FALSE(snapshot());
  deliver(&b.buffer);
  EXPECT_FLOAT_EQ(b.samples[0], 0.25f);
  EXPECT_FLOAT_EQ(source.peakLevel(), 0.0f);
}

TEST_F(PipeWireAudioSourceTest, ByteOffsetSelectsSamplesWithoutMutatingPrefix) {
  Buffer b;
  b.samples[0] = 8.0f;
  b.samples[1] = -9.0f; // Prefix must not contribute to the peak either.
  b.chunk.offset = 2 * sizeof(float);
  b.chunk.size = 2 * sizeof(float); // Exact end of the mapped data is valid.
  int calls = 0;
  source.setVolume(0.5f);
  source.setCallback([&](const AudioFrame &frame) {
    ++calls;
    EXPECT_EQ(frame.samples, b.samples + 2);
    EXPECT_EQ(frame.frameCount, 1);
    EXPECT_EQ(frame.timestamp, 0);
    EXPECT_FLOAT_EQ(frame.samples[0], 0.375f);
    EXPECT_FLOAT_EQ(frame.samples[1], -0.5f);
  });
  deliver(&b.buffer);
  EXPECT_EQ(calls, 1);
  EXPECT_FLOAT_EQ(b.samples[0], 8.0f);
  EXPECT_FLOAT_EQ(b.samples[1], -9.0f);
  EXPECT_FLOAT_EQ(source.peakLevel(), 1.0f);
}

TEST_F(PipeWireAudioSourceTest, InvalidChunkBoundsAndAlignmentAreRejected) {
  int calls = 0;
  source.setVolume(0.5f);
  source.setCallback([&](const AudioFrame &) { ++calls; });
  struct InvalidChunk {
    uint32_t offset;
    uint32_t size;
    uint32_t maxsize;
  };
  for (const auto &invalid : {
           InvalidChunk{16, 8, 16}, // Offset at end, but nonempty.
           InvalidChunk{20, 8, 16}, // Offset beyond mapping.
           InvalidChunk{0, 20, 16}, // Size beyond mapping.
           InvalidChunk{8, 16, 16}, // Fits alone, not after offset.
           InvalidChunk{0, 8, 0},  // Missing mapping size.
           InvalidChunk{1, 8, 16}, // Unaligned byte offset.
           InvalidChunk{0, 7, 16}, // Partial float.
           InvalidChunk{0, 12, 16}, // Three floats: partial stereo frame.
           InvalidChunk{8, std::numeric_limits<uint32_t>::max(), 16},
           InvalidChunk{std::numeric_limits<uint32_t>::max(), 8, 16}}) {
    SCOPED_TRACE(::testing::Message() << "offset=" << invalid.offset
                                    << " size=" << invalid.size
                                    << " maxsize=" << invalid.maxsize);
    Buffer b;
    b.chunk.offset = invalid.offset;
    b.chunk.size = invalid.size;
    b.data.maxsize = invalid.maxsize;
    deliver(&b.buffer);
    EXPECT_EQ(calls, 0);
    EXPECT_FLOAT_EQ(source.peakLevel(), 0.0f);
    EXPECT_FLOAT_EQ(b.samples[0], 0.25f);
    EXPECT_FLOAT_EQ(b.samples[3], -1.0f);
  }
}

TEST_F(PipeWireAudioSourceTest, InvalidRateRejectedWithoutTouchingSamples) {
  Buffer b;
  int calls = 0;
  source.setVolume(0.5f);
  source.setCallback([&](const AudioFrame &) { ++calls; });
  deliver(&b.buffer, 2, 0);
  deliver(&b.buffer, 2, -1);
  EXPECT_EQ(calls, 0);
  EXPECT_FLOAT_EQ(source.peakLevel(), 0.0f);
  EXPECT_FLOAT_EQ(b.samples[0], 0.25f);
}

TEST_F(PipeWireAudioSourceTest, UnalignedDataPointerRejected) {
  Buffer b;
  b.data.data = reinterpret_cast<unsigned char *>(b.samples) + 1;
  b.data.maxsize -= 1;
  b.chunk.size = 2 * sizeof(float);
  int calls = 0;
  source.setCallback([&](const AudioFrame &) { ++calls; });
  deliver(&b.buffer);
  EXPECT_EQ(calls, 0);
  EXPECT_FLOAT_EQ(source.peakLevel(), 0.0f);
}

TEST_F(PipeWireAudioSourceTest, RetainedSnapshotSurvivesConcurrentReplaceAndClear) {
  // The two handshakes force replacement/clear between retaining the real
  // production snapshot and invoking it. No scheduling sleeps or PW calls.
  std::promise<void> retained;
  std::promise<void> published;
  auto ready = retained.get_future();
  auto replacement = published.get_future();
  int oldCalls = 0;
  int newCalls = 0;
  auto token = std::make_shared<int>(42);
  std::weak_ptr<int> lifetime = token;
  source.setCallback([token, &oldCalls](const AudioFrame &) { ++oldCalls; });
  token.reset();
  std::thread reader([&] {
    const auto held = snapshot();
    retained.set_value();
    replacement.wait();
    if (held && held->callback)
      held->callback(AudioFrame{});
  });
  ready.wait();
  source.setCallback([&](const AudioFrame &) { ++newCalls; });
  EXPECT_FALSE(lifetime.expired());
  Buffer b;
  deliver(&b.buffer);
  source.setCallback({});
  EXPECT_FALSE(snapshot());
  EXPECT_FALSE(lifetime.expired());
  published.set_value();
  reader.join();
  EXPECT_EQ(oldCalls, 1);
  EXPECT_EQ(newCalls, 1);
  EXPECT_TRUE(lifetime.expired());
}

TEST_F(PipeWireAudioSourceTest, CallbackCanUnpublishItselfDuringDelivery) {
  Buffer b;
  int calls = 0;
  source.setCallback([&](const AudioFrame &) {
    ++calls;
    source.setCallback({});
  });
  deliver(&b.buffer);
  deliver(&b.buffer);
  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(snapshot());
}
