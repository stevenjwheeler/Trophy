/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/RecordingManager.h"
#include "core/RecordingSession.h"
#include "core/ReplayManager.h"
#include "core/StorageManager.h"
#include "settings/SettingsManager.h"
#include "capture/WaylandPortalCapture.h"
#include "audio/AudioMixer.h"
#include "audio/PipeWireAudioSource.h"
#include "encoding/IEncoder.h"
#include "detection/GameDetectionService.h"
#include "detection/WaylandWindowMonitor.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QElapsedTimer>
#include <gtest/gtest.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

using namespace TrophyBackend;

namespace {
constexpr std::array<std::array<uint8_t, 3>, 5> stageColors{{
    {{30, 80, 180}}, {{20, 190, 40}}, {{190, 40, 30}},
    {{80, 20, 200}}, {{80, 180, 180}}}}; // B, G, R
bool waitFor(const std::function<bool()> &condition, int timeout = 8000) {
  return QTest::qWaitFor(condition, timeout);
}

bool services(const QByteArray &command) {
  const QString directory = qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_CONTROL");
  if (directory.isEmpty()) return false;
  QFile::remove(directory + "/reply");
  QSaveFile request(directory + "/request");
  if (!request.open(QIODevice::WriteOnly) || request.write(command) != command.size() ||
      !request.commit()) return false;
  return waitFor([&] {
    QFile reply(directory + "/reply");
    return reply.open(QIODevice::ReadOnly) && reply.readAll() == command;
  }, 15000);
}

// A software SHM video source driven at 30fps, plus a tone routed through the
// private null sink. No compositor, hardware monitor, or GPU is involved.
class SyntheticMedia {
public:
  pw_thread_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_stream *video = nullptr, *audio = nullptr;
  spa_hook videoListener{}, audioListener{};
  const pw_stream_events *audioEventsTable = nullptr;
  spa_source *timer = nullptr;
  uint64_t sample = 0;
  std::atomic<int> produced{0};
  static constexpr int width = 160, height = 96;
  spa_video_info_raw videoFormat{};
  spa_audio_info_raw audioFormat{};
  int requestedStage = 0, activeStage = 0;
  int requestedWidth = width, requestedHeight = height, requestedFps = 30;
  double requestedTone = 440, activeTone = 440;

  void videoTimer(bool active) {
    timespec first{0, 1}, interval{0, 1000000000LL /
        (videoFormat.framerate.num > 0 ? videoFormat.framerate.num : 30)};
    pw_loop_update_timer(pw_thread_loop_get_loop(loop), timer,
        active ? &first : nullptr, active ? &interval : nullptr, false);
  }

  bool start() {
    pw_init(nullptr, nullptr);
    loop = pw_thread_loop_new("recording-failure-fixture", nullptr);
    if (!loop) return false;
    context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
    if (!context || pw_thread_loop_start(loop) < 0) return false;
    pw_thread_loop_lock(loop);
    core = pw_context_connect(context, nullptr, 0);
    if (!core) { pw_thread_loop_unlock(loop); return false; }
    auto *nativeLoop = pw_thread_loop_get_loop(loop);
    timer = pw_loop_add_timer(nativeLoop, [](void *data, uint64_t) {
      auto *self = static_cast<SyntheticMedia *>(data);
      pw_stream_trigger_process(self->video);
    }, this);
    video = pw_stream_new(core, "failure-video", pw_properties_new(
        PW_KEY_NODE_NAME, "failure-video", PW_KEY_MEDIA_CLASS, "Video/Source",
        "node.supports-request", "1", nullptr));
    audio = pw_stream_new(core, "failure-audio", pw_properties_new(
        PW_KEY_NODE_NAME, "failure-audio", PW_KEY_APP_NAME, "Failure fixture",
        PW_KEY_APP_PROCESS_BINARY, "failure-audio", PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_TARGET_OBJECT, "group-test-sink", nullptr));
    if (!video || !audio || !timer) { pw_thread_loop_unlock(loop); return false; }
    static const pw_stream_events videoEvents = [] {
      pw_stream_events events{};
      events.version = PW_VERSION_STREAM_EVENTS;
      events.state_changed = [](void *data, pw_stream_state, pw_stream_state state, const char *) {
        auto *self = static_cast<SyntheticMedia *>(data);
        self->videoTimer(state == PW_STREAM_STATE_STREAMING);
      };
      events.param_changed = [](void *data, uint32_t id, const spa_pod *param) {
        if (id != SPA_PARAM_Format || !param) return;
        auto *self = static_cast<SyntheticMedia *>(data);
        if (spa_format_video_raw_parse(param, &self->videoFormat) < 0) return;
        if (self->videoFormat.size.width == uint32_t(self->requestedWidth) &&
            self->videoFormat.size.height == uint32_t(self->requestedHeight) &&
            self->videoFormat.framerate.denom > 0 &&
            self->videoFormat.framerate.num == uint32_t(self->requestedFps) * self->videoFormat.framerate.denom)
          self->activeStage = self->requestedStage;
        const int stride = self->videoFormat.size.width * 4;
        uint8_t bytes[512];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
        const auto *buffers = static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
            SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * self->videoFormat.size.height),
            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride)));
        pw_stream_update_params(self->video, &buffers, 1);
        if (pw_stream_get_state(self->video, nullptr) == PW_STREAM_STATE_STREAMING)
          self->videoTimer(true);
      };
      events.process = [](void *data) {
        auto *self = static_cast<SyntheticMedia *>(data);
        auto *buffer = pw_stream_dequeue_buffer(self->video);
        if (!buffer) return;
        if (buffer->buffer && buffer->buffer->n_datas > 0) {
          auto &plane = buffer->buffer->datas[0];
          const auto count = self->videoFormat.size.width * self->videoFormat.size.height;
          if (plane.chunk) plane.chunk->size = 0;
          if (plane.data && plane.chunk && plane.maxsize >= count * 4) {
            auto *pixels = static_cast<uint8_t *>(plane.data);
            const auto &color = stageColors.at(self->activeStage);
            for (uint32_t i = 0; i < count; ++i) {
              pixels[i * 4] = color[0]; pixels[i * 4 + 1] = color[1];
              pixels[i * 4 + 2] = color[2]; pixels[i * 4 + 3] = 255;
            }
            plane.chunk->offset = 0;
            plane.chunk->stride = self->videoFormat.size.width * 4;
            plane.chunk->size = count * 4;
            ++self->produced;
          }
        }
        pw_stream_queue_buffer(self->video, buffer);
      };
      return events;
    }();
    static const pw_stream_events audioEvents = [] {
      pw_stream_events events{};
      events.version = PW_VERSION_STREAM_EVENTS;
      events.param_changed = [](void *data, uint32_t id, const spa_pod *param) {
        auto *self = static_cast<SyntheticMedia *>(data);
        if (id == SPA_PARAM_Format && param &&
            spa_format_audio_raw_parse(param, &self->audioFormat) >= 0)
          self->activeTone = self->requestedTone;
      };
      events.process = [](void *data) {
        auto *self = static_cast<SyntheticMedia *>(data);
        auto *buffer = pw_stream_dequeue_buffer(self->audio);
        if (!buffer) return;
        auto &plane = buffer->buffer->datas[0];
        const uint32_t channels = self->audioFormat.channels;
        const uint32_t rate = self->audioFormat.rate;
        if (plane.data && plane.chunk && channels > 0 && rate > 0) {
          const uint32_t frames = std::min<uint32_t>(buffer->requested, plane.maxsize / (channels * 4));
          auto *pcm = static_cast<float *>(plane.data);
          for (uint32_t i = 0; i < frames; ++i) {
            const float value = 0.2f * std::sin(2 * 3.141592653589793 * self->activeTone * self->sample++ / rate);
            std::fill_n(pcm + i * channels, channels, value);
          }
          plane.chunk->offset = 0; plane.chunk->size = frames * channels * 4;
          plane.chunk->stride = channels * 4;
          buffer->size = frames;
        }
        pw_stream_queue_buffer(self->audio, buffer);
      };
      return events;
    }();
    pw_stream_add_listener(video, &videoListener, &videoEvents, this);
    audioEventsTable = &audioEvents;
    pw_stream_add_listener(audio, &audioListener, &audioEvents, this);
    uint8_t bytes[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    spa_video_info_raw format{};
    format.format = SPA_VIDEO_FORMAT_BGRx;
    format.size = SPA_RECTANGLE(width, height);
    format.framerate = SPA_FRACTION(30, 1);
    const spa_pod *param = spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
    const int videoResult = pw_stream_connect(video, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS), &param, 1);
    spa_pod_builder_init(&builder, bytes, sizeof(bytes));
    spa_audio_info_raw audioFormat{};
    audioFormat.format = SPA_AUDIO_FORMAT_F32;
    audioFormat.rate = 48000; audioFormat.channels = 2;
    audioFormat.position[0] = SPA_AUDIO_CHANNEL_FL; audioFormat.position[1] = SPA_AUDIO_CHANNEL_FR;
    param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &audioFormat);
    const int audioResult = pw_stream_connect(audio, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), &param, 1);
    pw_thread_loop_unlock(loop);
    return videoResult >= 0 && audioResult >= 0 && waitFor([&] { return node() != PW_ID_ANY; });
  }
  uint32_t node() {
    pw_thread_loop_lock(loop);
    const auto id = pw_stream_get_node_id(video);
    pw_thread_loop_unlock(loop);
    return id;
  }
  bool changeVideo(int w, int h, int fps, int stage) {
    pw_thread_loop_lock(loop);
    requestedStage = stage;
    requestedWidth = w; requestedHeight = h; requestedFps = fps;
    uint8_t bytes[512];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    spa_video_info_raw format{};
    format.format = SPA_VIDEO_FORMAT_BGRx;
    format.size = SPA_RECTANGLE(uint32_t(w), uint32_t(h));
    format.framerate = SPA_FRACTION(uint32_t(fps), 1);
    const spa_pod *param = spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
    const int result = pw_stream_update_params(video, &param, 1);
    pw_thread_loop_unlock(loop);
    return result >= 0;
  }
  void closeVideo() {
    pw_thread_loop_lock(loop);
    videoTimer(false);
    spa_hook_remove(&videoListener);
    pw_stream_destroy(video);
    video = nullptr;
    pw_thread_loop_unlock(loop);
  }
  bool videoMatches(int w, int h, int stage) {
    pw_thread_loop_lock(loop);
    const bool matches = videoFormat.size.width == uint32_t(w) &&
        videoFormat.size.height == uint32_t(h) && activeStage == stage &&
        videoFormat.framerate.denom > 0 &&
        videoFormat.framerate.num == uint32_t(requestedFps) * videoFormat.framerate.denom;
    pw_thread_loop_unlock(loop);
    return matches;
  }
  bool changeAudio(int rate, int channels, double tone) {
    pw_thread_loop_lock(loop);
    requestedTone = tone;
    // Audio application streams fix their client format at connect time.
    // Exercise real node/port recreation and the session's normal rebinding,
    // rather than claiming EnumFormat publication changed the active format.
    spa_hook_remove(&audioListener);
    pw_stream_destroy(audio);
    audio = pw_stream_new(core, "failure-audio", pw_properties_new(
        PW_KEY_NODE_NAME, "failure-audio", PW_KEY_APP_NAME, "Failure fixture",
        PW_KEY_APP_PROCESS_BINARY, "failure-audio", PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_TARGET_OBJECT, "group-test-sink", nullptr));
    if (!audio) { pw_thread_loop_unlock(loop); return false; }
    pw_stream_add_listener(audio, &audioListener, audioEventsTable, this);
    uint8_t bytes[512];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    spa_audio_info_raw format{};
    format.format = SPA_AUDIO_FORMAT_F32; format.rate = rate; format.channels = channels;
    format.position[0] = channels == 1 ? SPA_AUDIO_CHANNEL_MONO : SPA_AUDIO_CHANNEL_FL;
    if (channels == 2) format.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
    const int result = pw_stream_connect(audio, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), &param, 1);
    pw_thread_loop_unlock(loop);
    return result >= 0;
  }
  bool audioMatches(int rate, int channels, double tone) {
    pw_thread_loop_lock(loop);
    const bool matches = audioFormat.rate == uint32_t(rate) &&
        audioFormat.channels == uint32_t(channels) && activeTone == tone;
    pw_thread_loop_unlock(loop);
    return matches;
  }
  ~SyntheticMedia() {
    if (loop) pw_thread_loop_lock(loop);
    if (audio) pw_stream_destroy(audio);
    if (video) pw_stream_destroy(video);
    if (timer) pw_loop_destroy_source(pw_thread_loop_get_loop(loop), timer);
    if (core) pw_core_disconnect(core);
    if (loop) { pw_thread_loop_unlock(loop); pw_thread_loop_stop(loop); }
    if (context) pw_context_destroy(context);
    if (loop) pw_thread_loop_destroy(loop);
  }
};
} // namespace

struct FixtureStream { uint32_t node; QVariantMap options; };
Q_DECLARE_METATYPE(FixtureStream)
Q_DECLARE_METATYPE(QList<FixtureStream>)
QDBusArgument &operator<<(QDBusArgument &arg, const FixtureStream &stream) {
  arg.beginStructure(); arg << stream.node << stream.options; arg.endStructure(); return arg;
}
const QDBusArgument &operator>>(const QDBusArgument &arg, FixtureStream &stream) {
  arg.beginStructure(); arg >> stream.node >> stream.options; arg.endStructure(); return arg;
}

class FailureSession : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
public slots:
  void Close() {
    auto signal = QDBusMessage::createSignal(message().path(), "org.freedesktop.portal.Session", "Closed");
    signal.setArguments({QVariantMap{}});
    QDBusConnection::sessionBus().send(signal);
  }
};

class FailurePortal : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")
public:
  uint32_t node = PW_ID_ANY;
  QString mode, lastSession, held;
  QStringList tokens;
  QList<FailureSession *> sessions;
  QVariantMap streams() const {
    return {{"streams", QVariant::fromValue(QList<FixtureStream>{{node,
        {{"title", "Failure fixture"}, {"app_id", "failure-audio"}}}})}};
  }
  QString request(const QVariantMap &options) const {
    QString sender = message().service(); sender.remove(':'); sender.replace('.', '_');
    return "/org/freedesktop/portal/desktop/request/" + sender + '/' + options["handle_token"].toString();
  }
  void respond(const QString &path, const QVariantMap &results, uint response = 0) {
    auto signal = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
    signal.setArguments({response, results});
    QDBusConnection::sessionBus().send(signal);
  }
public slots:
  void Configure(uint id, const QString &value) { node = id; mode = value; }
  QStringList Tokens() const { return tokens; }
  QDBusObjectPath CreateSession(const QVariantMap &options) {
    lastSession = "/test/session_" + options["session_handle_token"].toString();
    auto *session = new FailureSession;
    session->setParent(this); sessions.append(session);
    QDBusConnection::sessionBus().registerObject(lastSession, session, QDBusConnection::ExportAllSlots);
    const auto path = request(options);
    QTimer::singleShot(0, this, [this, path, sessionPath = lastSession] {
      respond(path, {{"session_handle", sessionPath}});
    });
    return QDBusObjectPath(path);
  }
  QDBusObjectPath SelectSources(const QDBusObjectPath &, const QVariantMap &options) {
    tokens.append(options.value("restore_token").toString());
    const auto path = request(options);
    const uint response = mode == "deny-select" && !tokens.last().isEmpty() ? 2 : 0;
    QTimer::singleShot(0, this, [this, path, response] { respond(path, {}, response); });
    return QDBusObjectPath(path);
  }
  QDBusObjectPath Start(const QDBusObjectPath &, const QString &, const QVariantMap &options) {
    const auto path = request(options);
    if (mode == "hold-start") held = path;
    else {
      const uint response = mode == "cancel-start" ? 1 :
          (mode == "deny-start" && !tokens.last().isEmpty() ? 2 : 0);
      auto results = streams();
      if (mode == "grant-new-token") results["restore_token"] = "fixture-new-token";
      QTimer::singleShot(0, this, [this, path, results, response] { respond(path, results, response); });
    }
    return QDBusObjectPath(path);
  }
  bool HasHeld() const { return !held.isEmpty(); }
  void ReleaseHeld() { if (!held.isEmpty()) respond(std::exchange(held, {}), streams()); }
  void Revoke() {
    auto signal = QDBusMessage::createSignal(lastSession, "org.freedesktop.portal.Session", "Closed");
    signal.setArguments({QVariantMap{}});
    QDBusConnection::sessionBus().send(signal);
  }
};

namespace {
struct FormatChecks {
  bool videoStages = false, audioStages = false;
  unsigned expectedStages = 0x1f;
};
QDBusMessage control(const QString &method, const QVariantList &args = {}) {
  auto call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop", "org.freedesktop.portal.ScreenCast", method);
  call.setAutoStartService(false);
  call.setArguments(args);
  return QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
}

struct DecoderDiagnostics {
  static thread_local DecoderDiagnostics *current;
  int errors = 0;
  DecoderDiagnostics() { current = this; av_log_set_callback(log); }
  ~DecoderDiagnostics() { av_log_set_callback(av_log_default_callback); current = nullptr; }
  static void log(void *context, int level, const char *format, va_list args) {
    if (current && level <= AV_LOG_ERROR) ++current->errors;
    av_log_default_callback(context, level, format, args);
  }
};
thread_local DecoderDiagnostics *DecoderDiagnostics::current = nullptr;

void checkMedia(const QString &path, FormatChecks checks = {}) {
  DecoderDiagnostics diagnostics;
  AVFormatContext *raw = nullptr;
  ASSERT_GE(avformat_open_input(&raw, path.toUtf8().constData(), nullptr, nullptr), 0);
  const auto closeFormat = [](AVFormatContext *value) { avformat_close_input(&value); };
  std::unique_ptr<AVFormatContext, decltype(closeFormat)> input(raw, closeFormat);
  ASSERT_GE(avformat_find_stream_info(input.get(), nullptr), 0);
  const int video = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const int audio = av_find_best_stream(input.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  ASSERT_GE(video, 0); ASSERT_GE(audio, 0);
  EXPECT_EQ(input->streams[video]->codecpar->codec_id, AV_CODEC_ID_H264);
  EXPECT_EQ(input->streams[audio]->codecpar->codec_id, AV_CODEC_ID_AAC);
  EXPECT_EQ(input->streams[audio]->codecpar->sample_rate, 48000);
  EXPECT_EQ(input->streams[audio]->codecpar->ch_layout.nb_channels, 2);
  const auto freeCodec = [](AVCodecContext *value) { avcodec_free_context(&value); };
  using Decoder = std::unique_ptr<AVCodecContext, decltype(freeCodec)>;
  auto decoder = [&](int index) -> Decoder {
    auto *codec = avcodec_find_decoder(input->streams[index]->codecpar->codec_id);
    Decoder result(avcodec_alloc_context3(codec), freeCodec);
    if (!result || avcodec_parameters_to_context(result.get(), input->streams[index]->codecpar) < 0)
      return Decoder(nullptr, freeCodec);
    result->pkt_timebase = input->streams[index]->time_base;
    result->thread_count = 1; // Keep all decoder diagnostics in this scoped check.
    if (avcodec_open2(result.get(), codec, nullptr) < 0) return Decoder(nullptr, freeCodec);
    result->err_recognition = AV_EF_EXPLODE;
    return result;
  };
  auto vd = decoder(video), ad = decoder(audio);
  ASSERT_TRUE(vd); ASSERT_TRUE(ad);
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  ASSERT_NE(packet, nullptr); ASSERT_NE(frame, nullptr);
  int videoFrames = 0, audioSamples = 0;
  double amplitude = 0;
  double videoEnd = 0, audioEnd = 0;
  std::array<int, stageColors.size()> stages{};
  std::vector<float> pcm;
  auto drain = [&](AVCodecContext *codec, bool isVideo) {
    int result;
    while ((result = avcodec_receive_frame(codec, frame)) >= 0) {
      EXPECT_EQ(frame->decode_error_flags, 0);
      EXPECT_EQ(frame->flags & AV_FRAME_FLAG_CORRUPT, 0);
      if (isVideo) {
        ++videoFrames;
        EXPECT_EQ(frame->width, SyntheticMedia::width);
        EXPECT_EQ(frame->height, SyntheticMedia::height);
        EXPECT_GT(frame->data[0][frame->height / 2 * frame->linesize[0] + frame->width / 2], 10);
        if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
          videoEnd = std::max(videoEnd, frame->best_effort_timestamp *
              av_q2d(input->streams[video]->time_base) + 1.0/30);
        if (checks.videoStages) {
          ASSERT_EQ(frame->format, AV_PIX_FMT_YUV420P);
          const int x = frame->width / 2, y = frame->height / 2;
          const std::array<double, 3> center{{double(frame->data[0][y * frame->linesize[0] + x]),
              double(frame->data[1][y / 2 * frame->linesize[1] + x / 2]),
              double(frame->data[2][y / 2 * frame->linesize[2] + x / 2])}};
          for (size_t stage = 0; stage < stageColors.size(); ++stage) {
            const auto &color = stageColors[stage];
            const double b = color[0], g = color[1], r = color[2];
            const std::array<double, 3> expected{{16 + (65.481*r + 128.553*g + 24.966*b)/255,
                128 + (-37.797*r - 74.203*g + 112*b)/255,
                128 + (112*r - 93.786*g - 18.214*b)/255}};
            if (std::abs(center[0]-expected[0]) > 6 || std::abs(center[1]-expected[1]) > 6 ||
                std::abs(center[2]-expected[2]) > 6) continue;
            ++stages[stage];
            const int left = frame->data[0][y * frame->linesize[0] + 2];
            const int top = frame->data[0][2 * frame->linesize[0] + x];
            if (stage == 1) { EXPECT_LE(left, 24); EXPECT_NEAR(top, center[0], 6); }
            if (stage == 2) { EXPECT_LE(top, 24); EXPECT_NEAR(left, center[0], 6); }
            if (stage == 0 || stage == 4) {
              EXPECT_NEAR(top, center[0], 6); EXPECT_NEAR(left, center[0], 6);
            }
          }
        }
      } else {
        audioSamples += frame->nb_samples;
        if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
          audioEnd = std::max(audioEnd, frame->best_effort_timestamp *
              av_q2d(input->streams[audio]->time_base) + double(frame->nb_samples)/frame->sample_rate);
        if (frame->format == AV_SAMPLE_FMT_FLTP) {
          const auto *samples = reinterpret_cast<const float *>(frame->extended_data[0]);
          for (int i = 0; i < frame->nb_samples; ++i) amplitude += std::abs(samples[i]);
          if (checks.audioStages) pcm.insert(pcm.end(), samples, samples + frame->nb_samples);
        }
      }
      av_frame_unref(frame);
    }
    EXPECT_TRUE(result == AVERROR(EAGAIN) || result == AVERROR_EOF);
  };
  std::vector<int64_t> lastDts(input->nb_streams, AV_NOPTS_VALUE);
  int readResult;
  while ((readResult = av_read_frame(input.get(), packet)) >= 0) {
    const int index = packet->stream_index;
    if (packet->dts != AV_NOPTS_VALUE) {
      if (lastDts[index] != AV_NOPTS_VALUE) { EXPECT_GE(packet->dts, lastDts[index]); }
      lastDts[index] = packet->dts;
    }
    if (index == video || index == audio) {
      auto *codec = index == video ? vd.get() : ad.get();
      EXPECT_GE(avcodec_send_packet(codec, packet), 0);
      drain(codec, index == video);
    }
    av_packet_unref(packet);
  }
  EXPECT_EQ(readResult, AVERROR_EOF);
  EXPECT_GE(avcodec_send_packet(vd.get(), nullptr), 0); drain(vd.get(), true);
  EXPECT_GE(avcodec_send_packet(ad.get(), nullptr), 0); drain(ad.get(), false);
  av_frame_free(&frame); av_packet_free(&packet);
  EXPECT_GE(videoFrames, 8);
  EXPECT_GT(audioSamples, 1024);
  EXPECT_GT(amplitude, audioSamples * 0.005);
  EXPECT_EQ(diagnostics.errors, 0) << "decoder errors must not silently pass";
  if (checks.videoStages) {
    for (size_t stage = 0; stage < stages.size(); ++stage)
      if (checks.expectedStages & (1u << stage))
        EXPECT_GE(stages[stage], 4) << "missing decoded resize/color stage " << stage;
  }
  if (checks.audioStages) {
    EXPECT_NEAR(audioEnd, videoEnd, 0.35) << "resumed audio must not compress source outages";
    for (double frequency : {440., 660., 880.}) {
      double peak = 0;
      constexpr size_t window = 4096;
      for (size_t start = 0; start + window <= pcm.size(); start += window / 2) {
        double real = 0, imag = 0;
        for (size_t i = 0; i < window; ++i) {
          const double phase = 2 * 3.141592653589793 * frequency * i / 48000;
          real += pcm[start+i] * std::cos(phase); imag += pcm[start+i] * std::sin(phase);
        }
        peak = std::max(peak, 2 * std::hypot(real, imag) / window);
      }
      EXPECT_GT(peak, 0.02) << "missing decoded tone after rate/channel change: " << frequency;
    }
  }
}
} // namespace

namespace TrophyBackend {
class WaylandPortalCaptureTest {
public:
  static int revokeNodeRead(WaylandPortalCapture &capture, uint32_t node) {
    if (!capture.m_pw_loop || !capture.m_pw_core) return -ENOTCONN;
    pw_thread_loop_lock(capture.m_pw_loop);
    const pw_permission permission{node, 0};
    const int result = pw_client_update_permissions(pw_core_get_client(capture.m_pw_core), 1, &permission);
    pw_thread_loop_unlock(capture.m_pw_loop);
    return result;
  }
};
// Existing friend seam: native capture/encoding/muxing paths remain real.
class ReplayManagerTest {
public:
  static void install(ReplayManager &replay) {
    replay.m_captureFactory = [] { return std::make_unique<WaylandPortalCapture>(); };
  }
  static bool idle(const ReplayManager &replay) { return replay.m_state == ReplayManager::State::Idle; }
  static qint64 frames(const ReplayManager &replay) {
    return replay.m_session ? replay.m_session->frameCount() : 0;
  }
  static void rotate(ReplayManager &replay) { replay.rotateSegment(); }
  static int retiring(const ReplayManager &replay) { return int(replay.m_retiringSegments.size()); }
  static QString finalized(const ReplayManager &replay) {
    return replay.m_segments.isEmpty() ? QString() : replay.m_segments.first().path;
  }
  static QPointer<IVideoCaptureSource> capture(const ReplayManager &replay) { return replay.m_capture.get(); }
  static RecordingSession *session(const ReplayManager &replay) { return replay.m_session.get(); }
  static AudioMixer *mixer(const ReplayManager &replay) { return replay.m_audioMixer.get(); }
  static QSize outputSize(const ReplayManager &replay) {
    return replay.m_encoder ? QSize(replay.m_encoder->width(), replay.m_encoder->height()) : QSize();
  }
};
} // namespace TrophyBackend

class RecordingManagerTest : public testing::TestWithParam<std::tuple<bool, int>> {
protected:
  QTemporaryDir root;
  QProcess portal;
  std::unique_ptr<SyntheticMedia> media;
  bool servicesStopped = false;
  void SetUp() override {
    if (qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_TEST") != "1")
      GTEST_SKIP() << "Requires private service launcher";
    ASSERT_TRUE(root.isValid());
    if (!avcodec_find_encoder_by_name("libx264") || !avcodec_find_encoder(AV_CODEC_ID_AAC))
      GTEST_SKIP() << "Requires software H.264 and AAC encoders";
    media = std::make_unique<SyntheticMedia>();
    ASSERT_TRUE(media->start());
    startPortal();
  }
  void startPortal(const QString &mode = {}) {
    ASSERT_FALSE(QDBusConnection::sessionBus().interface()->isServiceRegistered("org.freedesktop.portal.Desktop"));
    portal.start(QCoreApplication::applicationFilePath(), {"--failure-portal"});
    ASSERT_TRUE(portal.waitForStarted(3000));
    ASSERT_TRUE(waitFor([] {
      return QDBusConnection::sessionBus().interface()->isServiceRegistered("org.freedesktop.portal.Desktop").value();
    }));
    ASSERT_EQ(control("Configure", {media->node(), mode}).type(), QDBusMessage::ReplyMessage);
  }
  void TearDown() override {
    portal.terminate();
    if (!portal.waitForFinished(3000)) { portal.kill(); portal.waitForFinished(3000); }
    media.reset();
    if (servicesStopped) { EXPECT_TRUE(services("start")); }
  }
  void configure(SettingsManager &settings, RecordingManager &recorder, AudioMixer &mixer,
                 StorageManager &storage, bool separate) {
    configureSettings(settings, separate);
    recorder.setSettingsManager(&settings); recorder.setStorageManager(&storage);
    recorder.setAudioMixer(&mixer);
    recorder.m_captureFactory = [] { return std::make_unique<WaylandPortalCapture>(); };
  }
  void configureSettings(SettingsManager &settings, bool separate) {
    settings.setRecordingsPath(root.path());
    settings.setEncoder("x264"); settings.setEncoderPreset("ultrafast");
    settings.setVideoResolution("160x96"); settings.setFps(30); settings.setUseDmaBuf(false);
    settings.setAudioMode("game"); settings.setSeparateAudioTracks(separate);
    settings.setVoiceChatApp(""); settings.setMicrophoneDevice("absent-fixture-mic");
    settings.setRestoreToken("Failure fixture", "fixture-saved-token");
  }
  void fail(int failure) {
    if (failure == 0) { ASSERT_EQ(control("Revoke").type(), QDBusMessage::ReplyMessage); }
    if (failure == 1) { portal.kill(); ASSERT_TRUE(portal.waitForFinished(3000)); }
    if (failure == 2) { servicesStopped = true; ASSERT_TRUE(services("stop")); }
  }
  void recover(int failure) {
    if (failure == 2) {
      media.reset();
      ASSERT_TRUE(services("start"));
      servicesStopped = false;
      media = std::make_unique<SyntheticMedia>();
      ASSERT_TRUE(media->start());
      ASSERT_EQ(control("Configure", {media->node(), ""}).type(), QDBusMessage::ReplyMessage);
    }
    if (failure == 1) startPortal();
  }
  void exercise() {
    const auto [separate, failure] = GetParam();
    SettingsManager settings;
    AudioMixer mixer(48000, 2);
    StorageManager storage(root.path());
    RecordingManager recorder;
    configure(settings, recorder, mixer, storage, separate);
    QSignalSpy started(&recorder, &RecordingManager::recordingStarted);
    QSignalSpy saved(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy finished(&recorder, &RecordingManager::recordingFinished);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    QSignalSpy authorization(&recorder, &RecordingManager::restoreAuthNeeded);
    int ticks = 0;
    QTimer heartbeat;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
    heartbeat.start(5);
    auto begin = [&] {
      recorder.startRecording("Failure fixture");
      return waitFor([&] { return recorder.m_session && recorder.m_session->frameCount() >= 15; });
    };
    ASSERT_TRUE(begin());
    EXPECT_EQ(started.size(), 1);
    const QString partial = recorder.currentFilePath();
    const QPointer<IVideoCaptureSource> oldCapture(recorder.m_captureSource.get());
    const int before = ticks;
    fail(failure);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(waitFor([&] { return recorder.state() == RecordingManager::RecordingState::Idle; }));
    EXPECT_GT(ticks, before);
    EXPECT_TRUE(oldCapture.isNull());
    EXPECT_EQ(errors.size(), 1);
    EXPECT_EQ(finished.size(), 1);
    EXPECT_EQ(saved.size(), 0);
    EXPECT_EQ(authorization.size(), 0);
    EXPECT_EQ(settings.getRestoreToken("Failure fixture"), "fixture-saved-token");
    EXPECT_TRUE(recorder.currentFilePath().isEmpty());
    ASSERT_TRUE(QFileInfo::exists(partial));
    checkMedia(partial);
    recover(failure);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(begin());
    const QString retry = recorder.currentFilePath();
    EXPECT_NE(retry, partial);
    recorder.stopRecording();
    ASSERT_TRUE(waitFor([&] { return recorder.state() == RecordingManager::RecordingState::Idle; }));
    EXPECT_EQ(saved.size(), 1); EXPECT_EQ(finished.size(), 2); EXPECT_EQ(errors.size(), 1);
    EXPECT_EQ(authorization.size(), 0);
    checkMedia(retry);
    const auto tokens = qdbus_cast<QStringList>(control("Tokens").arguments().value(0));
    ASSERT_FALSE(tokens.isEmpty());
    for (const auto &token : tokens) EXPECT_EQ(token, "fixture-saved-token");
  }

  void startupFailure() {
    const auto [separate, failure] = GetParam();
    SettingsManager settings;
    AudioMixer mixer(48000, 2);
    StorageManager storage(root.path());
    RecordingManager recorder;
    configure(settings, recorder, mixer, storage, separate);
    ASSERT_EQ(control("Configure", {media->node(), "hold-start"}).type(), QDBusMessage::ReplyMessage);
    QSignalSpy started(&recorder, &RecordingManager::recordingStarted);
    QSignalSpy saved(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    QSignalSpy authorization(&recorder, &RecordingManager::restoreAuthNeeded);
    recorder.startRecording("Failure fixture");
    ASSERT_TRUE(waitFor([] { return control("HasHeld").arguments().value(0).toBool(); }));
    const QString abandoned = recorder.currentFilePath();
    const QPointer<IVideoCaptureSource> oldCapture(recorder.m_captureSource.get());
    fail(failure);
    ASSERT_FALSE(HasFatalFailure());
    // With no native connection yet, daemon loss is observed when the held
    // successful Start response attempts native setup, not by a fake error.
    if (failure == 2) { ASSERT_EQ(control("ReleaseHeld").type(), QDBusMessage::ReplyMessage); }
    ASSERT_TRUE(waitFor([&] { return recorder.state() == RecordingManager::RecordingState::Idle; }));
    EXPECT_TRUE(oldCapture.isNull());
    EXPECT_EQ(errors.size(), 1); EXPECT_EQ(started.size(), 0); EXPECT_EQ(saved.size(), 0);
    EXPECT_EQ(authorization.size(), 0);
    EXPECT_FALSE(QFileInfo::exists(abandoned));
    EXPECT_TRUE(recorder.currentFilePath().isEmpty());
    EXPECT_EQ(settings.getRestoreToken("Failure fixture"), "fixture-saved-token");
    recover(failure);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(control("Configure", {media->node(), ""}).type(), QDBusMessage::ReplyMessage);
    recorder.startRecording("Failure fixture");
    ASSERT_TRUE(waitFor([&] { return recorder.m_session && recorder.m_session->frameCount() >= 15; }));
    // A late reply from the abandoned attempt cannot touch the fresh capture.
    ASSERT_EQ(control("ReleaseHeld").type(), QDBusMessage::ReplyMessage);
    QTest::qWait(20);
    EXPECT_EQ(errors.size(), 1);
    EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Recording);
    const auto path = recorder.currentFilePath();
    recorder.stopRecording();
    ASSERT_TRUE(waitFor([&] { return recorder.state() == RecordingManager::RecordingState::Idle; }));
    EXPECT_EQ(started.size(), 1); EXPECT_EQ(saved.size(), 1);
    checkMedia(path);
  }

  void dvrFailure() {
    using Peer = TrophyBackend::ReplayManagerTest;
    const auto [separate, failure] = GetParam();
    SettingsManager settings;
    configureSettings(settings, separate);
    const QString cache = root.path() + "/dvr-cache";
    settings.setReplayCachePath(cache);
    settings.setInstantReplayEnabled(true);
    StorageManager storage(root.path());
    ReplayManager replay;
    replay.setSettingsManager(&settings); replay.setStorageManager(&storage);
    Peer::install(replay);
    QSignalSpy saved(&replay, &ReplayManager::replaySaved);
    QSignalSpy errors(&replay, &ReplayManager::replayFailed);
    QSignalSpy authorization(&replay, &ReplayManager::restoreAuthNeeded);
    replay.startDvr("Failure fixture");
    ASSERT_TRUE(waitFor([&] { return replay.isActive() && Peer::frames(replay) >= 15; }));
    Peer::rotate(replay);
    ASSERT_TRUE(waitFor([&] { return !Peer::finalized(replay).isEmpty(); }));
    // A settled predecessor is genuine decodable media before a later failure.
    checkMedia(Peer::finalized(replay));
    ASSERT_TRUE(waitFor([&] { return Peer::frames(replay) >= 15; }));
    const auto oldCapture = Peer::capture(replay);
    Peer::rotate(replay);
    ASSERT_GT(Peer::retiring(replay), 0); // Failure arrives during handoff ownership.
    fail(failure);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_TRUE(waitFor([&] { return Peer::idle(replay); }));
    EXPECT_TRUE(oldCapture.isNull());
    EXPECT_EQ(errors.size(), 1); EXPECT_EQ(saved.size(), 0); EXPECT_EQ(authorization.size(), 0);
    EXPECT_TRUE(QDir(cache).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
    EXPECT_EQ(settings.getRestoreToken("Failure fixture"), "fixture-saved-token");
    recover(failure);
    ASSERT_FALSE(HasFatalFailure());
    replay.startDvr("Failure fixture");
    ASSERT_TRUE(waitFor([&] { return replay.isActive() && Peer::frames(replay) >= 15; }));
    replay.saveReplay();
    ASSERT_TRUE(waitFor([&] { return saved.size() == 1; }));
    const auto path = saved.at(0).at(0).toString();
    checkMedia(path);
    EXPECT_EQ(errors.size(), 1);
    replay.stopDvr();
    ASSERT_TRUE(waitFor([&] { return Peer::idle(replay); }));
    EXPECT_TRUE(QDir(cache).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
    EXPECT_TRUE(QFileInfo::exists(path));
  }

  void nativeFormatChanges() {
    const auto [separate, mode] = GetParam();
    const bool videoChanges = mode != 1, audioChanges = mode != 0;
    SettingsManager settings;
    AudioMixer mixer(48000, 2);
    StorageManager storage(root.path());
    RecordingManager recorder;
    configure(settings, recorder, mixer, storage, separate);
    QSignalSpy started(&recorder, &RecordingManager::recordingStarted);
    QSignalSpy saved(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    recorder.startRecording("Failure fixture");
    QPointer<WaylandPortalCapture> capture = qobject_cast<WaylandPortalCapture *>(recorder.m_captureSource.get());
    ASSERT_NE(capture, nullptr);
    ASSERT_TRUE(waitFor([&] { return recorder.m_session && recorder.m_session->frameCount() >= 15; }));
    const auto path = recorder.currentFilePath();
    const std::array<QSize, 4> sizes{{{120,96}, {240,96}, {161,97}, {160,96}}};
    const std::array<int, 4> rates{{60, 15, 30, 30}};
    const std::array<int, 3> audioRates{{44100, 96000, 48000}}, channels{{1, 2, 2}};
    const std::array<double, 3> tones{{660, 880, 440}};
    for (int stage = 0; stage < (videoChanges ? 4 : 3); ++stage) {
      const int before = media->produced.load();
      if (videoChanges) {
        ASSERT_TRUE(media->changeVideo(sizes[stage].width(), sizes[stage].height(), rates[stage], stage+1));
        ASSERT_TRUE(waitFor([&] {
          return media->videoMatches(sizes[stage].width(), sizes[stage].height(), stage+1) &&
              capture && capture->nativeResolution() == sizes[stage];
        }));
      }
      if (audioChanges && stage < 3) {
        ASSERT_TRUE(media->changeAudio(audioRates[stage], channels[stage], tones[stage]));
        ASSERT_TRUE(waitFor([&] { return media->audioMatches(audioRates[stage], channels[stage], tones[stage]); }));
        QString name, serial;
        ASSERT_TRUE(waitFor([&] {
          return PipeWireAudioSource::findNodeByBinary("failure-audio", &name, &serial) != PW_ID_ANY;
        }));
        ASSERT_TRUE(waitFor([&] {
          return recorder.m_session && recorder.m_session->audioRoutingState().boundSerial[0] == serial &&
              !mixer.sourceOperationPending(0) && mixer.sourceIsCapturing(0);
        }));
      }
      QTest::qWait(450);
      ASSERT_TRUE(waitFor([&] { return media->produced.load() >= before + 16; }));
      EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Recording);
      EXPECT_EQ(errors.size(), 0);
      EXPECT_EQ(started.size(), 1);
      EXPECT_EQ(recorder.currentFilePath(), path);
    }
    recorder.stopRecording();
    ASSERT_TRUE(waitFor([&] { return recorder.state() == RecordingManager::RecordingState::Idle; }));
    EXPECT_EQ(saved.size(), 1); EXPECT_EQ(errors.size(), 0);
    checkMedia(path, {videoChanges, audioChanges});
  }

  void permissionRevocation() {
    const auto [separate, boundary] = GetParam();
    SettingsManager settings;
    AudioMixer mixer(48000, 2);
    StorageManager storage(root.path());
    RecordingManager recorder;
    configure(settings, recorder, mixer, storage, separate);
    QSignalSpy started(&recorder, &RecordingManager::recordingStarted);
    QSignalSpy saved(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy finished(&recorder, &RecordingManager::recordingFinished);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    QSignalSpy authorization(&recorder, &RecordingManager::restoreAuthNeeded);
    // Model the application's authorization handoff, not a real permission UI.
    QObject::connect(&recorder, &RecordingManager::restoreAuthNeeded, &settings,
        [&](const QString &game, const QString &identity) {
      EXPECT_EQ(recorder.state(), RecordingManager::RecordingState::Idle);
      settings.removeRestoreToken(identity.isEmpty() ? game : identity);
    });
    recorder.startRecording("Failure fixture");
    ASSERT_TRUE(waitFor([&] { return recorder.m_session && recorder.m_session->frameCount() >= 15; }));
    const QString partial = recorder.currentFilePath();
    if (boundary < 2) {
      ASSERT_EQ(control("Configure", {media->node(), boundary == 0 ? "deny-select" : "deny-start"}).type(),
          QDBusMessage::ReplyMessage);
      ASSERT_EQ(control("Revoke").type(), QDBusMessage::ReplyMessage);
    } else {
      auto *capture = qobject_cast<WaylandPortalCapture *>(recorder.m_captureSource.get());
      ASSERT_NE(capture, nullptr);
      ASSERT_GE(TrophyBackend::WaylandPortalCaptureTest::revokeNodeRead(*capture, media->node()), 0);
    }
    ASSERT_TRUE(waitFor([&] { return recorder.state() == RecordingManager::RecordingState::Idle; }));
    EXPECT_EQ(errors.size(), 1); EXPECT_EQ(saved.size(), 0); EXPECT_EQ(finished.size(), 1);
    EXPECT_EQ(authorization.size(), 0);
    EXPECT_EQ(settings.getRestoreToken("Failure fixture"), "fixture-saved-token");
    checkMedia(partial);
    if (boundary < 2) {
      recorder.startRecording("Failure fixture");
      ASSERT_TRUE(waitFor([&] {
        return authorization.size() == 1 && recorder.state() == RecordingManager::RecordingState::Idle;
      }));
      EXPECT_EQ(started.size(), 1); EXPECT_EQ(saved.size(), 0);
      EXPECT_TRUE(settings.getRestoreToken("Failure fixture").isEmpty());
      auto tokens = qdbus_cast<QStringList>(control("Tokens").arguments().value(0));
      EXPECT_EQ(tokens.size(), 2);
      QTest::qWait(100); // Rejection must not silently retry without a token.
      EXPECT_EQ(qdbus_cast<QStringList>(control("Tokens").arguments().value(0)), tokens);
      for (const auto &token : tokens) EXPECT_EQ(token, "fixture-saved-token");
      // Cancellation following an explicit authorization request stays
      // cancelled: no new token, save, or automatic retry/reauthorization.
      ASSERT_EQ(control("Configure", {media->node(), "cancel-start"}).type(), QDBusMessage::ReplyMessage);
      recorder.startRecording("Failure fixture");
      ASSERT_TRUE(waitFor([&] {
        return errors.size() == 2 && recorder.state() == RecordingManager::RecordingState::Idle;
      }));
      EXPECT_EQ(authorization.size(), 1); EXPECT_EQ(saved.size(), 0);
      EXPECT_TRUE(settings.getRestoreToken("Failure fixture").isEmpty());
      ASSERT_EQ(control("Configure", {media->node(), "grant-new-token"}).type(), QDBusMessage::ReplyMessage);
    }
    // Explicit fresh authorization/capture after the denial, never an auto-picker.
    recorder.startRecording("Failure fixture");
    ASSERT_TRUE(waitFor([&] { return recorder.m_session && recorder.m_session->frameCount() >= 15; }));
    EXPECT_EQ(settings.getRestoreToken("Failure fixture"),
        boundary < 2 ? "fixture-new-token" : "fixture-saved-token");
    const auto retry = recorder.currentFilePath();
    recorder.stopRecording();
    ASSERT_TRUE(waitFor([&] { return recorder.state() == RecordingManager::RecordingState::Idle; }));
    EXPECT_EQ(saved.size(), 1); EXPECT_EQ(errors.size(), boundary < 2 ? 2 : 1);
    EXPECT_EQ(authorization.size(), boundary < 2 ? 1 : 0);
    checkMedia(retry);
  }

  void nativeGameExit() {
    const auto [separate, timing] = GetParam();
    QProcess game;
    game.start("sleep", {"30"});
    ASSERT_TRUE(game.waitForStarted());
    WaylandWindowMonitor monitor;
    GameDetectionService detector;
    detector.setWindowMonitor(&monitor);
    detector.addToWhitelist("steam_app_42424242", "Failure fixture");
    WindowInfo window;
    window.pid = game.processId(); window.appId = "steam_app_42424242";
    window.focused = window.fullscreen = true;
    monitor.ingestSnapshot({window});
    ASSERT_EQ(detector.currentGameKey(), "steam_app_42424242");
    ASSERT_EQ(detector.currentGameName(), "Failure fixture");
    SettingsManager settings;
    AudioMixer mixer(48000, 2);
    StorageManager storage(root.path());
    RecordingManager recorder;
    configure(settings, recorder, mixer, storage, separate);
    recorder.setGameDetectionService(&detector);
    QSignalSpy saved(&recorder, &RecordingManager::recordingStopped);
    QSignalSpy errors(&recorder, &RecordingManager::recordingError);
    QSignalSpy finished(&recorder, &RecordingManager::recordingFinished);
    recorder.startRecording("Failure fixture");
    ASSERT_TRUE(waitFor([&] { return recorder.m_session && recorder.m_session->frameCount() >= 15; }));
    const auto path = recorder.currentFilePath();
    if (timing == 0) {
      game.terminate();
      ASSERT_TRUE(game.waitForFinished());
    }
    if (timing < 2) media->closeVideo();
    else {
      auto *capture = qobject_cast<WaylandPortalCapture *>(recorder.m_captureSource.get());
      ASSERT_NE(capture, nullptr);
      ASSERT_GE(TrophyBackend::WaylandPortalCaptureTest::revokeNodeRead(*capture, media->node()), 0);
    }
    if (timing == 1) QTimer::singleShot(150, &game, [&] { game.terminate(); });
    ASSERT_TRUE(waitFor([&] { return recorder.state() == RecordingManager::RecordingState::Idle; }));
    EXPECT_EQ(saved.size(), timing < 2 ? 1 : 0);
    EXPECT_EQ(errors.size(), timing < 2 ? 0 : 1);
    EXPECT_EQ(finished.size(), 1);
    checkMedia(path);
    if (game.state() != QProcess::NotRunning) {
      game.terminate();
      ASSERT_TRUE(game.waitForFinished());
    }
  }

  void dvrFormatChanges() {
    using Peer = TrophyBackend::ReplayManagerTest;
    const auto [separate, mode] = GetParam();
    const bool videoChanges = mode != 1, audioChanges = mode != 0;
    SettingsManager settings;
    configureSettings(settings, separate);
    const QString cache = root.path() + "/format-cache";
    settings.setReplayCachePath(cache); settings.setInstantReplayEnabled(true);
    StorageManager storage(root.path());
    ReplayManager replay;
    replay.setSettingsManager(&settings); replay.setStorageManager(&storage);
    Peer::install(replay);
    QSignalSpy errors(&replay, &ReplayManager::replayFailed);
    QSignalSpy saved(&replay, &ReplayManager::replaySaved);
    replay.startDvr("Failure fixture");
    ASSERT_TRUE(waitFor([&] { return replay.isActive() && Peer::frames(replay) >= 45; }));
    const std::array<QSize, 3> sizes{{{120,96}, {240,96}, {160,96}}};
    const std::array<int, 3> stages{{1, 2, 4}}, rates{{44100, 96000, 48000}}, channels{{1, 2, 2}};
    const std::array<double, 3> tones{{660, 880, 440}};
    for (int stage = 0; stage < 3; ++stage) {
      if (videoChanges) {
        ASSERT_TRUE(media->changeVideo(sizes[stage].width(), sizes[stage].height(), 30, stages[stage]));
        ASSERT_TRUE(waitFor([&] {
          const auto capture = Peer::capture(replay);
          return media->videoMatches(sizes[stage].width(), sizes[stage].height(), stages[stage]) &&
              capture && capture->nativeResolution() == sizes[stage];
        }));
      }
      if (audioChanges) {
        ASSERT_TRUE(media->changeAudio(rates[stage], channels[stage], tones[stage]));
        ASSERT_TRUE(waitFor([&] { return media->audioMatches(rates[stage], channels[stage], tones[stage]); }));
        QString name, serial;
        ASSERT_TRUE(waitFor([&] {
          return PipeWireAudioSource::findNodeByBinary("failure-audio", &name, &serial) != PW_ID_ANY;
        }));
        ASSERT_TRUE(waitFor([&] {
          return Peer::session(replay) && Peer::session(replay)->audioRoutingState().boundSerial[0] == serial &&
              Peer::mixer(replay)->sourceIsCapturing(0) && !Peer::mixer(replay)->sourceOperationPending(0);
        }));
      }
      QTest::qWait(450);
      Peer::rotate(replay);
      ASSERT_EQ(Peer::outputSize(replay), QSize(160,96));
      ASSERT_TRUE(waitFor([&] { return replay.isActive() && Peer::frames(replay) >= 15; }));
      EXPECT_EQ(errors.size(), 0);
    }
    replay.saveReplay();
    ASSERT_TRUE(waitFor([&] { return saved.size() == 1; }));
    checkMedia(saved.at(0).at(0).toString(), {videoChanges, audioChanges, 0x17});
    EXPECT_EQ(errors.size(), 0);
    replay.stopDvr();
    ASSERT_TRUE(waitFor([&] { return Peer::idle(replay); }));
    EXPECT_TRUE(QDir(cache).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
  }

  void dvrPermissionRevocation() {
    using Peer = TrophyBackend::ReplayManagerTest;
    const auto [separate, boundary] = GetParam();
    SettingsManager settings;
    configureSettings(settings, separate);
    const QString cache = root.path() + "/permission-cache";
    settings.setReplayCachePath(cache); settings.setInstantReplayEnabled(true);
    StorageManager storage(root.path());
    ReplayManager replay;
    replay.setSettingsManager(&settings); replay.setStorageManager(&storage);
    Peer::install(replay);
    QSignalSpy saved(&replay, &ReplayManager::replaySaved);
    QSignalSpy errors(&replay, &ReplayManager::replayFailed);
    QSignalSpy authorization(&replay, &ReplayManager::restoreAuthNeeded);
    QObject::connect(&replay, &ReplayManager::restoreAuthNeeded, &settings,
        [&](const QString &game, const QString &identity) {
      settings.removeRestoreToken(identity.isEmpty() ? game : identity);
    });
    replay.startDvr("Failure fixture");
    ASSERT_TRUE(waitFor([&] { return replay.isActive() && Peer::frames(replay) >= 15; }));
    if (boundary < 2) {
      ASSERT_EQ(control("Configure", {media->node(), boundary == 0 ? "deny-select" : "deny-start"}).type(),
          QDBusMessage::ReplyMessage);
      ASSERT_EQ(control("Revoke").type(), QDBusMessage::ReplyMessage);
    } else {
      auto *capture = qobject_cast<WaylandPortalCapture *>(Peer::capture(replay).data());
      ASSERT_NE(capture, nullptr);
      ASSERT_GE(TrophyBackend::WaylandPortalCaptureTest::revokeNodeRead(*capture, media->node()), 0);
    }
    ASSERT_TRUE(waitFor([&] { return Peer::idle(replay); }));
    EXPECT_EQ(errors.size(), 1); EXPECT_EQ(saved.size(), 0); EXPECT_EQ(authorization.size(), 0);
    EXPECT_TRUE(QDir(cache).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
    EXPECT_EQ(settings.getRestoreToken("Failure fixture"), "fixture-saved-token");
    if (boundary < 2) {
      replay.startDvr("Failure fixture");
      ASSERT_TRUE(waitFor([&] { return authorization.size() == 1 && Peer::idle(replay); }));
      EXPECT_TRUE(settings.getRestoreToken("Failure fixture").isEmpty());
      const auto tokens = qdbus_cast<QStringList>(control("Tokens").arguments().value(0));
      ASSERT_EQ(tokens.size(), 2);
      QTest::qWait(100);
      EXPECT_EQ(qdbus_cast<QStringList>(control("Tokens").arguments().value(0)), tokens);
      for (const auto &token : tokens) EXPECT_EQ(token, "fixture-saved-token");
      ASSERT_EQ(control("Configure", {media->node(), "cancel-start"}).type(), QDBusMessage::ReplyMessage);
      replay.startDvr("Failure fixture");
      ASSERT_TRUE(waitFor([&] { return Peer::idle(replay); }));
      EXPECT_EQ(authorization.size(), 1); EXPECT_EQ(saved.size(), 0);
      EXPECT_TRUE(settings.getRestoreToken("Failure fixture").isEmpty());
      ASSERT_EQ(control("Configure", {media->node(), "grant-new-token"}).type(), QDBusMessage::ReplyMessage);
    }
    replay.startDvr("Failure fixture");
    ASSERT_TRUE(waitFor([&] { return replay.isActive() && Peer::frames(replay) >= 15; }));
    EXPECT_EQ(settings.getRestoreToken("Failure fixture"),
        boundary < 2 ? "fixture-new-token" : "fixture-saved-token");
    replay.saveReplay();
    ASSERT_TRUE(waitFor([&] { return saved.size() == 1; }));
    checkMedia(saved.at(0).at(0).toString());
    EXPECT_EQ(errors.size(), 1);
    EXPECT_EQ(authorization.size(), boundary < 2 ? 1 : 0);
    replay.stopDvr();
    ASSERT_TRUE(waitFor([&] { return Peer::idle(replay); }));
    EXPECT_TRUE(QDir(cache).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
  }
};

TEST_P(RecordingManagerTest, NativeServiceLossFinalizesPartialAndFreshRecordingWorks) { exercise(); }
TEST_P(RecordingManagerTest, StartupServiceLossRejectsLateReplyAndFreshRecordingWorks) { startupFailure(); }
TEST_P(RecordingManagerTest, DvrHandoffServiceLossCleansCacheAndFreshClipWorks) { dvrFailure(); }
TEST_P(RecordingManagerTest, NativeFormatChangesPreserveCanvasAndDecodedAudio) { nativeFormatChanges(); }
TEST_P(RecordingManagerTest, PermissionRevocationStopsAndRequiresExplicitRetry) { permissionRevocation(); }
TEST_P(RecordingManagerTest, NativeGameExitSavesButLiveGameRevocationFails) { nativeGameExit(); }
TEST_P(RecordingManagerTest, DvrNativeFormatChangesKeepCompatibleSegmentsAndClip) { dvrFormatChanges(); }
TEST_P(RecordingManagerTest, DvrPermissionRevocationRequiresExplicitRetry) { dvrPermissionRevocation(); }
INSTANTIATE_TEST_SUITE_P(MergedAndSeparatePortalClosureOwnerExitAndDaemonExit,
    RecordingManagerTest, testing::Combine(testing::Bool(), testing::Values(0, 1, 2)));

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  av_log_set_level(AV_LOG_ERROR);
  if (qEnvironmentVariable("TROPHY_PRIVATE_PIPEWIRE_TEST") != "1" ||
      qEnvironmentVariableIsEmpty("TROPHY_PRIVATE_PIPEWIRE_CONTROL") ||
      qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS").contains("/run/user/") ||
      qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS")) return 77;
  qDBusRegisterMetaType<FixtureStream>(); qDBusRegisterMetaType<QList<FixtureStream>>();
  if (application.arguments().contains("--failure-portal")) {
    FailurePortal portal;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.freedesktop.portal.Desktop") ||
        !bus.registerObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::ExportAllSlots)) return 2;
    return application.exec();
  }
  if (!avcodec_find_encoder_by_name("libx264") || !avcodec_find_encoder(AV_CODEC_ID_AAC)) {
    fprintf(stderr, "SKIP: software H.264/AAC encoders unavailable\n");
    return 77;
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "test_RecordingFailureIntegration.moc"
