/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio/PipeWireAudioSource.h"
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QTest>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QSaveFile>
#include <QElapsedTimer>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>

using namespace TrophyBackend;

// Test-only negative control: reproduce the old single-target capture path.
class PipeWireAudioSourceTest {
public:
  static void singleTarget(PipeWireAudioSource &source) {
    source.m_operations.attachAppGroup = nullptr;
  }
};

namespace {
bool waitFor(const std::function<bool()> &predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    QTest::qWait(10);
  }
  return predicate();
}

uint32_t privateCaptureNode(const QString &name) {
  QProcess dump;
  dump.start("pw-dump", {});
  if (!dump.waitForFinished(5000) || dump.exitCode() != 0) return PW_ID_ANY;
  for (const auto &value : QJsonDocument::fromJson(dump.readAllStandardOutput()).array()) {
    const auto object = value.toObject();
    const auto props = object["info"].toObject()["props"].toObject();
    if (object["type"].toString() == QLatin1String(PW_TYPE_INTERFACE_Node) &&
        (props["node.name"].toString() == name ||
         props["media.name"].toString() == name))
      return object["id"].toInt();
  }
  return PW_ID_ANY;
}

uint32_t privatePort(uint32_t node, const QString &direction) {
  QProcess dump;
  dump.start("pw-dump", {});
  if (!dump.waitForFinished(5000) || dump.exitCode() != 0) return PW_ID_ANY;
  for (const auto &value : QJsonDocument::fromJson(dump.readAllStandardOutput()).array()) {
    const auto object = value.toObject();
    const auto props = object["info"].toObject()["props"].toObject();
    if (object["type"].toString() == QLatin1String(PW_TYPE_INTERFACE_Port) &&
        props["node.id"].toVariant().toUInt() == node &&
        props["port.direction"].toString() == direction)
      return object["id"].toInt();
  }
  return PW_ID_ANY;
}

bool privateServices(const QByteArray &command) {
  const QString directory = qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_CONTROL");
  if (qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_TEST") != "1" || directory.isEmpty())
    return false;
  QFile::remove(directory + "/reply");
  QSaveFile request(directory + "/request");
  if (!request.open(QIODevice::WriteOnly) || request.write(command) != command.size() ||
      !request.commit())
    return false;
  // The launcher only controls its owned private children, never a host PID.
  QElapsedTimer deadline;
  deadline.start();
  while (deadline.elapsed() < 15000) {
    QFile reply(directory + "/reply");
    if (reply.open(QIODevice::ReadOnly) && reply.readAll() == command) return true;
    QTest::qWait(10);
  }
  return false;
}

struct PlaybackClient {
  struct Stream {
    pw_stream *stream = nullptr;
    spa_hook listener{};
    std::atomic<float> value{0};
    static void process(void *userdata) {
      auto &s = *static_cast<Stream *>(userdata);
      auto *b = pw_stream_dequeue_buffer(s.stream);
      if (!b) return;
      auto &d = b->buffer->datas[0];
      const uint32_t frames = std::min<uint32_t>(b->requested, d.maxsize / 8);
      std::fill_n(static_cast<float *>(d.data), frames * 2, s.value.load());
      d.chunk->offset = 0;
      d.chunk->size = frames * 8;
      d.chunk->stride = 8;
      b->size = frames;
      pw_stream_queue_buffer(s.stream, b);
    }
  };
  pw_thread_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  std::vector<std::unique_ptr<Stream>> streams;
  PlaybackClient() {
    pw_init(nullptr, nullptr);
    loop = pw_thread_loop_new("group-test", nullptr);
    context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
    core = pw_context_connect(context, nullptr, 0);
    pw_thread_loop_start(loop);
  }
  ~PlaybackClient() {
    pw_thread_loop_lock(loop);
    for (auto &s : streams) if (s->stream) pw_stream_destroy(s->stream);
    pw_core_disconnect(core);
    pw_thread_loop_unlock(loop);
    pw_thread_loop_stop(loop);
    pw_context_destroy(context);
    pw_thread_loop_destroy(loop);
  }
  Stream *add(float value, const char *app = "Outlast fixture",
              const char *binary = "game-fixture") {
    auto s = std::make_unique<Stream>();
    s->value = value;
    pw_thread_loop_lock(loop);
    s->stream = pw_stream_new(core, "group-playback", pw_properties_new(
        PW_KEY_NODE_NAME, app, PW_KEY_APP_NAME, app,
        PW_KEY_APP_PROCESS_BINARY, binary,
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_TARGET_OBJECT, "group-test-sink", nullptr));
    static const pw_stream_events events = [] {
      pw_stream_events e{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.process = Stream::process;
      return e;
    }();
    pw_stream_add_listener(s->stream, &s->listener, &events, s.get());
    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = 48000;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
    const int result = pw_stream_connect(s->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        &param, 1);
    EXPECT_GE(result, 0);
    pw_thread_loop_unlock(loop);
    auto *out = s.get();
    streams.push_back(std::move(s));
    EXPECT_TRUE(waitFor([&] {
      pw_thread_loop_lock(loop);
      const bool ready = pw_stream_get_state(out->stream, nullptr) == PW_STREAM_STATE_STREAMING;
      pw_thread_loop_unlock(loop);
      return ready;
    }));
    return out;
  }
  void remove(Stream *stream) {
    pw_thread_loop_lock(loop);
    pw_stream_destroy(stream->stream);
    stream->stream = nullptr;
    pw_thread_loop_unlock(loop);
  }
};

TEST(AppAudioGroupTest, SilentFirstStreamIncludesLateSiblingAndMixesOnOneClock) {
  if (qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_TEST") != "1")
    GTEST_SKIP() << "Requires the private PipeWire launcher";
  int argc = 1;
  char name[] = "audio-group-test";
  char *argv[] = {name, nullptr};
  QCoreApplication application(argc, argv);
  PlaybackClient game, unrelated;
  auto *first = game.add(0.0f);
  // Same title in another client must not be captured, nor another app in the
  // original client. This also catches accidentally capturing the sink mix.
  unrelated.add(0.7f);
  game.add(0.6f, "Not the game");
  QString nodeName, serial;
  const uint32_t id = PipeWireAudioSource::findNodeByProcessName(
      "Outlast fixture", &nodeName, &serial);
  ASSERT_NE(id, PW_ID_ANY);
  auto capture = PipeWireAudioSource::createApplicationSource(id, nodeName, serial);
  if (qEnvironmentVariableIsSet("TROPHY_TEST_SINGLE_APP_STREAM"))
    PipeWireAudioSourceTest::singleTarget(*capture);
  std::atomic<float> latest{-1};
  std::atomic<int64_t> frames{0};
  capture->setCallback([&](const AudioFrame &f) {
    latest = f.samples[0];
    frames += f.frameCount;
  });
  ASSERT_TRUE(capture->initialize({}));
  ASSERT_TRUE(capture->start());
  ASSERT_TRUE(waitFor([&] { return latest.load() == 0.0f; }));
  auto *second = game.add(0.2f);
  ASSERT_TRUE(waitFor([&] { return std::abs(latest.load() - 0.2f) < 0.001f; }));
  first->value = 0.1f;
  ASSERT_TRUE(waitFor([&] { return std::abs(latest.load() - 0.3f) < 0.001f; }));
  const auto startFrames = frames.load();
  QTest::qWait(500);
  // Summing must not concatenate packets / run the audio timeline twice.
  EXPECT_GT(frames.load() - startFrames, 12000);
  EXPECT_LT(frames.load() - startFrames, 36000);
  game.remove(second);
  ASSERT_TRUE(waitFor([&] { return std::abs(latest.load() - 0.1f) < 0.001f; }));
  game.add(0.25f);
  ASSERT_TRUE(waitFor([&] { return std::abs(latest.load() - 0.35f) < 0.001f; }));
  capture->stop();
}

TEST(AppAudioGroupTest, VoiceUsesExactBinaryAndNeverGroupsUnrelatedChromium) {
  if (qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_TEST") != "1")
    GTEST_SKIP() << "Requires the private PipeWire launcher";
  int argc = 1;
  char name[] = "voice-group-test";
  char *argv[] = {name, nullptr};
  QCoreApplication application(argc, argv);
  PlaybackClient playback;
  auto *discord = playback.add(0.1f, "Chromium", "Discord");
  playback.add(0.7f, "Chromium", "VRChat.exe");
  playback.add(0.6f, "Discord", "DiscordHelper");
  QString nodeName, serial;
  const auto id = PipeWireAudioSource::findNodeByBinary("discord", &nodeName, &serial);
  ASSERT_NE(id, PW_ID_ANY);
  auto capture = PipeWireAudioSource::createApplicationSource(id, nodeName, serial, "Discord");
  std::atomic<float> latest{-1};
  std::atomic<bool> unrelated{false};
  capture->setCallback([&](const AudioFrame &frame) {
    latest = frame.samples[0];
    if (frame.samples[0] > 0.4f) unrelated = true;
  });
  ASSERT_TRUE(capture->initialize({}));
  ASSERT_TRUE(capture->start());
  ASSERT_TRUE(waitFor([&] { return std::abs(latest.load() - 0.1f) < 0.001f; }));
  playback.add(0.2f, "Chromium", "dIsCoRd");
  ASSERT_TRUE(waitFor([&] { return std::abs(latest.load() - 0.3f) < 0.001f; }));
  playback.remove(discord);
  QTest::qWait(200);
  EXPECT_FALSE(unrelated.load());
  capture->stop();
  EXPECT_EQ(PipeWireAudioSource::findNodeByBinary("NotDiscord"), PW_ID_ANY);
}

TEST(AppAudioGroupTest, ActualLinkFromSecondGameNodeIntoVoiceIsRejected) {
  if (qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_TEST") != "1")
    GTEST_SKIP() << "Requires the private PipeWire launcher";
  int argc = 1;
  char name[] = "voice-wrong-link-test";
  char *argv[] = {name, nullptr};
  QCoreApplication application(argc, argv);
  PlaybackClient playback;
  playback.add(0.1f, "Chromium", "Discord");
  playback.add(0.0f, "VRChat.exe", "VRChat.exe");
  auto *secondGame = playback.add(0.7f, "VRChat.exe", "VRChat.exe");
  QString nodeName, serial;
  const auto id = PipeWireAudioSource::findNodeByBinary("Discord", &nodeName, &serial);
  ASSERT_NE(id, PW_ID_ANY);
  auto capture = PipeWireAudioSource::createApplicationSource(id, nodeName, serial, "Discord");
  capture->setStreamName("trophy-test-voice");
  std::atomic<int> callbacks{0};
  std::atomic<bool> wrongAudio{false};
  capture->setCallback([&](const AudioFrame &frame) {
    ++callbacks;
    if (frame.samples[0] > 0.4f) wrongAudio = true;
  });
  ASSERT_TRUE(capture->initialize({}));
  ASSERT_TRUE(capture->start());
  ASSERT_TRUE(waitFor([&] { return callbacks.load() > 0; }));
  const auto input = privateCaptureNode("trophy-test-voice");
  ASSERT_NE(input, PW_ID_ANY);
  pw_thread_loop_lock(playback.loop);
  const auto output = pw_stream_get_node_id(secondGame->stream);
  pw_thread_loop_unlock(playback.loop);
  const auto outputPort = privatePort(output, "out");
  const auto inputPort = privatePort(input, "in");
  ASSERT_NE(outputPort, PW_ID_ANY);
  ASSERT_NE(inputPort, PW_ID_ANY);
  // Emulate the observed qpwgraph arrangement, not just target properties:
  // the second VRChat playback node is linked directly into Trophy Voice.
  pw_thread_loop_lock(playback.loop);
  auto *props = pw_properties_new(nullptr, nullptr);
  pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", output);
  pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", outputPort);
  pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", input);
  pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", inputPort);
  auto *link = static_cast<pw_proxy *>(pw_core_create_object(playback.core,
      "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
  pw_properties_free(props);
  pw_thread_loop_unlock(playback.loop);
  ASSERT_NE(link, nullptr);
  EXPECT_TRUE(waitFor([&] { return !capture->isCapturing(); }));
  const auto stopped = callbacks.load();
  QTest::qWait(100);
  EXPECT_EQ(callbacks.load(), stopped);
  EXPECT_FALSE(wrongAudio.load());
  pw_thread_loop_lock(playback.loop);
  pw_proxy_destroy(link);
  pw_thread_loop_unlock(playback.loop);
  capture->stop();
}

TEST(AppAudioGroupTest, WrongBinaryOrStaleSerialNeverDeliversGameAudioToVoice) {
  if (qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_TEST") != "1")
    GTEST_SKIP() << "Requires the private PipeWire launcher";
  int argc = 1;
  char name[] = "voice-identity-test";
  char *argv[] = {name, nullptr};
  QCoreApplication application(argc, argv);
  PlaybackClient playback;
  playback.add(0.7f, "Chromium", "VRChat.exe");
  QString nodeName, serial;
  const auto id = PipeWireAudioSource::findNodeByBinary("VRChat.exe", &nodeName, &serial);
  ASSERT_NE(id, PW_ID_ANY);
  for (bool staleSerial : {false, true}) {
    SCOPED_TRACE(staleSerial);
    auto capture = PipeWireAudioSource::createApplicationSource(id, nodeName,
        staleSerial ? QStringLiteral("999999999") : serial,
        staleSerial ? QString() : QStringLiteral("Discord"));
    std::atomic<int> callbacks{0};
    capture->setCallback([&](const AudioFrame &) { ++callbacks; });
    ASSERT_TRUE(capture->initialize({}));
    if (capture->start())
      EXPECT_TRUE(waitFor([&] { return !capture->isCapturing(); }));
    EXPECT_EQ(callbacks.load(), 0);
    capture->stop();
  }
}

TEST(AppAudioGroupTest, RemovedPrimaryCannotAdoptReplacementWithoutFreshIdentity) {
  if (qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_TEST") != "1")
    GTEST_SKIP() << "Requires the private PipeWire launcher";
  int argc = 1;
  char name[] = "source-replacement-test";
  char *argv[] = {name, nullptr};
  QCoreApplication application(argc, argv);
  PlaybackClient playback;
  auto *original = playback.add(0.1f, "Replacement fixture", "replacement-fixture");
  QString nodeName, serial;
  const auto id = PipeWireAudioSource::findNodeByBinary("replacement-fixture", &nodeName, &serial);
  ASSERT_NE(id, PW_ID_ANY);
  auto capture = PipeWireAudioSource::createApplicationSource(id, nodeName, serial, "replacement-fixture");
  std::atomic<int> callbacks{0};
  std::atomic<bool> replacementLeaked{false};
  capture->setCallback([&](const AudioFrame &frame) {
    ++callbacks;
    if (frame.samples[0] > 0.2f) replacementLeaked = true;
  });
  ASSERT_TRUE(capture->initialize({}));
  ASSERT_TRUE(capture->start());
  ASSERT_TRUE(waitFor([&] { return callbacks.load() > 0; }));
  playback.remove(original);
  playback.add(0.4f, "Replacement fixture", "replacement-fixture");
  QTest::qWait(200);
  capture->stop();
  const auto stopped = callbacks.load();
  QTest::qWait(100);
  EXPECT_EQ(callbacks.load(), stopped);
  EXPECT_FALSE(replacementLeaked.load());
  QString newName, newSerial;
  const auto newId = PipeWireAudioSource::findNodeByBinary("replacement-fixture", &newName, &newSerial);
  ASSERT_NE(newId, PW_ID_ANY);
  ASSERT_NE(newSerial, serial);
  auto recovered = PipeWireAudioSource::createApplicationSource(newId, newName, newSerial, "replacement-fixture");
  std::atomic<float> latest{-1};
  recovered->setCallback([&](const AudioFrame &frame) { latest = frame.samples[0]; });
  ASSERT_TRUE(recovered->initialize({}));
  ASSERT_TRUE(recovered->start());
  ASSERT_TRUE(waitFor([&] { return std::abs(latest.load() - 0.4f) < 0.001f; }));
  recovered->stop();
}

TEST(AppAudioGroupTest, PrivateDaemonExitRetiresCaptureAndFreshDaemonCanCaptureAgain) {
  if (qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_TEST") != "1" ||
      qEnvironmentVariableIsEmpty("TROPHY_PRIVATE_PIPEWIRE_CONTROL"))
    GTEST_SKIP() << "Requires the private PipeWire control launcher";
  int argc = 1;
  char name[] = "daemon-recovery-test";
  char *argv[] = {name, nullptr};
  QCoreApplication application(argc, argv);
  auto playback = std::make_unique<PlaybackClient>();
  playback->add(0.15f, "Daemon fixture", "daemon-fixture");
  QString nodeName, serial;
  const auto id = PipeWireAudioSource::findNodeByBinary("daemon-fixture", &nodeName, &serial);
  ASSERT_NE(id, PW_ID_ANY);
  auto capture = PipeWireAudioSource::createApplicationSource(id, nodeName, serial, "daemon-fixture");
  std::atomic<int> callbacks{0}, errors{0};
  QObject::connect(capture.get(), &PipeWireAudioSource::captureError,
      capture.get(), [&](const QString &) { ++errors; }, Qt::DirectConnection);
  capture->setCallback([&](const AudioFrame &) { ++callbacks; });
  ASSERT_TRUE(capture->initialize({}));
  ASSERT_TRUE(capture->start());
  ASSERT_TRUE(waitFor([&] { return callbacks.load() > 0; }));
  ASSERT_TRUE(privateServices("stop"));
  ASSERT_TRUE(waitFor([&] { return !capture->isCapturing() && errors.load() > 0; }));
  capture->stop();
  const int stopped = callbacks.load();
  QTest::qWait(100);
  EXPECT_EQ(callbacks.load(), stopped);
  EXPECT_EQ(errors.load(), 1); // Stream/core notifications coalesce.
  capture.reset();
  playback.reset();
  ASSERT_TRUE(privateServices("start"));
  PlaybackClient freshPlayback;
  freshPlayback.add(0.25f, "Daemon fixture", "daemon-fixture");
  const auto freshId = PipeWireAudioSource::findNodeByBinary("daemon-fixture", &nodeName, &serial);
  ASSERT_NE(freshId, PW_ID_ANY);
  auto recovered = PipeWireAudioSource::createApplicationSource(freshId, nodeName, serial, "daemon-fixture");
  std::atomic<float> latest{-1};
  recovered->setCallback([&](const AudioFrame &frame) { latest = frame.samples[0]; });
  ASSERT_TRUE(recovered->initialize({}));
  ASSERT_TRUE(recovered->start());
  ASSERT_TRUE(waitFor([&] { return std::abs(latest.load() - 0.25f) < 0.001f; }));
  recovered->stop();
}
} // namespace
