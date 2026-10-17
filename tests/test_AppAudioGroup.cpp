/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio/PipeWireAudioSource.h"
#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QTest>
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
  Stream *add(float value, const char *app = "Outlast fixture") {
    auto s = std::make_unique<Stream>();
    s->value = value;
    pw_thread_loop_lock(loop);
    s->stream = pw_stream_new(core, "group-playback", pw_properties_new(
        PW_KEY_NODE_NAME, app, PW_KEY_APP_NAME, app,
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
} // namespace
