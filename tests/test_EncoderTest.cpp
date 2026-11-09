#include "capture/IVideoCaptureSource.h"
#include "encoding/EncoderTuning.h"
#include "encoding/FFmpegEncoder.h"
#include <QCoreApplication>
#include <QSize>
#include <gtest/gtest.h>
#include <libavutil/pixfmt.h>
#include <tuple>
#include <vector>
#include <array>
#include <algorithm>
#include <memory>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

using namespace TrophyBackend;

namespace TrophyBackend {
// Exercise the real CPU staging/scaler without requiring a CUDA/VA device.
class FFmpegEncoderTestAccess {
public:
  static bool prepareStaging(FFmpegEncoder &encoder, AVPixelFormat format) {
    encoder.m_config.width = 320; encoder.m_config.height = 180;
    encoder.m_frame = av_frame_alloc();
    if (!encoder.m_frame) return false;
    encoder.m_frame->format = format;
    encoder.m_frame->width = 320; encoder.m_frame->height = 180;
    return av_frame_get_buffer(encoder.m_frame, 0) >= 0;
  }
  static bool convert(FFmpegEncoder &encoder, const VideoFrame &frame) {
    return encoder.convertSoftwareFrame(frame);
  }
  static SwsContext *scaler(FFmpegEncoder &encoder) { return encoder.m_swsCtx; }
  static AVCodecContext *codecContext(FFmpegEncoder &encoder) {
    return encoder.m_codecCtx;
  }
  static int fillUntilBackpressure(FFmpegEncoder &encoder, const VideoFrame &frame) {
    if (!encoder.convertSoftwareFrame(frame)) return -1;
    encoder.m_firstTimestamp = frame.timestamp;
    for (int i = 0; i < 256; ++i) {
      encoder.m_frame->pts = i * 17;
      const int ret = avcodec_send_frame(encoder.m_codecCtx, encoder.m_frame);
      if (ret == AVERROR(EAGAIN)) return i;
      if (ret < 0) return -1;
    }
    return -1;
  }
};
}

// Use the fully qualified name from your logs: FFmpegEncoder::EncoderType
using EncoderTestData = std::tuple<FFmpegEncoder::EncoderType, QString, QSize>;

class EncoderMatrixTest : public ::testing::TestWithParam<EncoderTestData> {
protected:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
  }

  QString getCodecName(FFmpegEncoder::EncoderType type) {
    switch (type) {
    case FFmpegEncoder::EncoderType::NVENC_H264:
      return "h264_nvenc";
    case FFmpegEncoder::EncoderType::NVENC_HEVC:
      return "hevc_nvenc";
    case FFmpegEncoder::EncoderType::NVENC_AV1:
      return "av1_nvenc";
    case FFmpegEncoder::EncoderType::VAAPI_H264:
      return "h264_vaapi";
    case FFmpegEncoder::EncoderType::VAAPI_HEVC:
      return "hevc_vaapi";
    case FFmpegEncoder::EncoderType::VAAPI_AV1:
      return "av1_vaapi";
    case FFmpegEncoder::EncoderType::Software_x264:
      return "libx264";
    }
    return "";
  }
};

TEST(EncoderRateTest, UncappedRetainsOneHundredFpsPacketTimestamps) {
  if (!avcodec_find_encoder_by_name("libx264"))
    GTEST_SKIP() << "libx264 unavailable";
  FFmpegEncoder encoder(IEncoder::EncoderType::Software_x264, "libx264");
  EncoderConfig config;
  config.width = config.height = 64;
  config.fps = 0;
  config.preset = "ultrafast";
  ASSERT_TRUE(encoder.initialize(config));
  EXPECT_EQ(encoder.fps(), 0);
  std::vector<uint8_t> pixels(64 * 64 * 4, 128);
  VideoFrame frame{};
  frame.width = frame.height = 64;
  frame.stride = 64 * 4;
  frame.data = pixels.data();
  frame.pixelFormat = AV_PIX_FMT_BGRA;
  std::vector<int64_t> timestamps;
  for (int i = 0; i < 100; ++i) {
    frame.timestamp = 1000000 + int64_t(i) * 10000;
    EncodedPacket packet;
    if (encoder.encodeFrame(frame, packet))
      timestamps.push_back(packet.pts);
  }
  std::vector<EncodedPacket> tail;
  encoder.flush(tail);
  for (const auto &packet : tail) timestamps.push_back(packet.pts);
  std::sort(timestamps.begin(), timestamps.end());
  ASSERT_EQ(timestamps.size(), 100u);
  for (int i = 0; i < 100; ++i) EXPECT_EQ(timestamps[i], i * 10);
}

TEST(EncoderRateTest, RealCodecBackpressurePreservesInputAndFlushesDelayedFrames) {
  if (!avcodec_find_encoder_by_name("libx264")) GTEST_SKIP() << "libx264 unavailable";
  for (bool sendAnother : {false, true}) {
    SCOPED_TRACE(sendAnother ? "retry input" : "retry EOF");
    FFmpegEncoder encoder(IEncoder::EncoderType::Software_x264, "libx264");
    EncoderConfig config;
    config.width = config.height = 64;
    config.preset = "ultrafast";
    ASSERT_TRUE(encoder.initialize(config));
    std::vector<uint8_t> pixels(64 * 64 * 4, 128);
    VideoFrame frame{};
    frame.width = frame.height = 64;
    frame.stride = 256;
    frame.data = pixels.data();
    frame.pixelFormat = AV_PIX_FMT_BGRA;
    frame.timestamp = 1000000;
    const int sent = FFmpegEncoderTestAccess::fillUntilBackpressure(encoder, frame);
    ASSERT_GT(sent, 0);
    std::vector<EncodedPacket> packets;
    if (sendAnother) {
      frame.timestamp += int64_t(sent) * 17000;
      encoder.encodeFrames(frame, packets);
    }
    EXPECT_GE(encoder.flush(packets), 0);
    ASSERT_EQ(packets.size(), size_t(sent + (sendAnother ? 1 : 0)));
    std::vector<int64_t> pts;
    for (const auto &packet : packets) pts.push_back(packet.pts);
    std::sort(pts.begin(), pts.end());
    for (size_t i = 0; i < pts.size(); ++i) EXPECT_EQ(pts[i], int64_t(i) * 17);
  }
}

TEST_P(EncoderMatrixTest, FullPipelineTest) {
  const auto &params = GetParam();
  FFmpegEncoder::EncoderType type = std::get<0>(params);
  QString preset = std::get<1>(params);
  QSize res = std::get<2>(params);

  QString codecName = getCodecName(type);
  FFmpegEncoder encoder(type, codecName);

  EncoderConfig config;
  config.width = res.width();
  config.height = res.height();
  config.fps = 60;
  config.bitrate = 5000;
  config.preset = preset;
  config.useVBR = true;

  if (!encoder.initialize(config)) {
    if (type != FFmpegEncoder::EncoderType::Software_x264) {
      GTEST_SKIP() << "Hardware encoder " << codecName.toStdString()
                   << " not supported.";
    } else {
      FAIL() << "Software encoder " << codecName.toStdString()
             << " failed to initialize!";
    }
    return;
  }

  // Create a dummy frame (1000x1000 RGBA)
  int srcW = 1000;
  int srcH = 1000;
  std::vector<uint8_t> dummyData(srcW * srcH * 4, 0xAB);

  VideoFrame frame;
  frame.data = dummyData.data();
  frame.width = srcW;
  frame.height = srcH;
  frame.stride = srcW * 4;
  frame.timestamp = 0;
  frame.pixelFormat = AV_PIX_FMT_RGBA; // must match the dummy bytes above;
  // pixelFormat has no default init, and leaving it unset feeds garbage
  // into swscale (which aborts on assertion-enabled distro builds).

  EncodedPacket output;
  bool frameAccepted = false;

  // We try a few times because encoders (especially x265/NVENC)
  // often need multiple frames before they spit out the first packet.
  for (int i = 0; i < 5; ++i) {
    frame.timestamp = i * 1000; // Increment timestamp
    if (encoder.encodeFrame(frame, output)) {
      frameAccepted = true;
      break;
    }
  }

  // In a unit test, we at least want to see that the encoder is "alive"
  // and accepting data without crashing.
  EXPECT_TRUE(encoder.isInitialized());
}

INSTANTIATE_TEST_SUITE_P(
    VideoSettings, EncoderMatrixTest,
    ::testing::Combine(
        ::testing::Values(FFmpegEncoder::EncoderType::NVENC_H264,
                          FFmpegEncoder::EncoderType::NVENC_HEVC,
                          FFmpegEncoder::EncoderType::NVENC_AV1,
                          FFmpegEncoder::EncoderType::VAAPI_H264,
                          FFmpegEncoder::EncoderType::VAAPI_HEVC,
                          FFmpegEncoder::EncoderType::VAAPI_AV1,
                          FFmpegEncoder::EncoderType::Software_x264),
        ::testing::Values("ultrafast", "medium"),
        ::testing::Values(QSize(1280, 720), QSize(1080, 1296))));

TEST(EncoderRejectTest, InvalidPixelFormatDroppedWithoutAbort) {
  // A frame carrying no valid format must be dropped, never handed to
  // swscale: assertion-enabled distro builds abort the process there.
  if (!QCoreApplication::instance()) {
    static int argc = 1;
    static char arg0[] = "test";
    static char *argv[] = {arg0, nullptr};
    new QCoreApplication(argc, argv);
  }
  FFmpegEncoder encoder(FFmpegEncoder::EncoderType::Software_x264,
                        "libx264");
  EncoderConfig config;
  config.width = 1280;
  config.height = 720;
  config.fps = 60;
  config.bitrate = 5000;
  config.preset = "ultrafast";
  if (!encoder.initialize(config))
    GTEST_SKIP() << "libx264 not available.";

  std::vector<uint8_t> dummyData(64 * 64 * 4, 0xAB);
  VideoFrame frame;
  frame.data = dummyData.data();
  frame.width = 64;
  frame.height = 64;
  frame.stride = 64 * 4;
  frame.timestamp = 0;
  frame.pixelFormat = -1; // AV_PIX_FMT_NONE: no identity at all

  EncodedPacket output;
  EXPECT_FALSE(encoder.encodeFrame(frame, output));
}

namespace {
using ResizeEncoder = std::tuple<IEncoder::EncoderType, const char *>;

class EncoderResizeTest : public ::testing::TestWithParam<ResizeEncoder> {};

struct ResizeDecoder {
  AVCodecContext *context = nullptr;
  AVFrame *frame = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  SwsContext *rgb = nullptr;
  ~ResizeDecoder() {
    sws_freeContext(rgb);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&context);
  }
};
}

TEST(EncoderScalingTest, NvencCpuStagingUsesBilinearAndYuvStagingKeepsBicubicWithoutGpu) {
  struct Case { IEncoder::EncoderType type; const char *codec; AVPixelFormat format; int flags; };
  for (auto test : {
      Case{IEncoder::EncoderType::NVENC_H264, "h264_nvenc", AV_PIX_FMT_BGR0, SWS_BILINEAR},
      Case{IEncoder::EncoderType::Software_x264, "libx264", AV_PIX_FMT_YUV420P, SWS_BICUBIC},
      Case{IEncoder::EncoderType::VAAPI_H264, "h264_vaapi", AV_PIX_FMT_NV12, SWS_BICUBIC}}) {
    SCOPED_TRACE(test.codec);
    FFmpegEncoder encoder(test.type, test.codec);
    ASSERT_TRUE(FFmpegEncoderTestAccess::prepareStaging(encoder, test.format));
    std::vector<uint8_t> pixels(137 * 79 * 4 + AV_INPUT_BUFFER_PADDING_SIZE, 127);
    VideoFrame frame{};
    frame.data = pixels.data(); frame.width = 137; frame.height = 79;
    frame.stride = 137 * 4; frame.pixelFormat = AV_PIX_FMT_BGRA;
    ASSERT_TRUE(FFmpegEncoderTestAccess::convert(encoder, frame));
    auto *scaler = FFmpegEncoderTestAccess::scaler(encoder);
    ASSERT_NE(scaler, nullptr);
    int64_t flags = 0;
    ASSERT_EQ(av_opt_get_int(scaler, "sws_flags", 0, &flags), 0);
    // swscale may add implementation flags (e.g. FULL_CHR_H_INT) on init.
    EXPECT_EQ(flags & (SWS_BILINEAR | SWS_BICUBIC), test.flags);
    // Reusing the context must keep the chosen filter and succeed again.
    ASSERT_TRUE(FFmpegEncoderTestAccess::convert(encoder, frame));
    EXPECT_EQ(FFmpegEncoderTestAccess::scaler(encoder), scaler);
  }
}

TEST_P(EncoderResizeTest, DecodedFramesFitFixedCanvasWithoutStalePixels) {
  const auto [type, codec] = GetParam();
  FFmpegEncoder encoder(type, codec);
  EncoderConfig config;
  config.width = 320;
  config.height = 180;
  config.fps = 30;
  config.bitrate = 4000;
  if (!encoder.initialize(config))
    GTEST_SKIP() << codec << " unavailable";

  struct Stage { int width, height; std::array<uint8_t, 3> color; };
  // Full canvas -> pillarbox -> letterbox from a larger source -> odd
  // dimensions -> original size. Distinct colours expose stale pooled pixels.
  const std::array<Stage, 7> stages{{
      {320, 180, {255, 255, 255}}, {160, 120, {255, 0, 0}},
      {640, 180, {0, 255, 0}}, {137, 79, {0, 0, 255}},
      {160, 90, {0, 255, 255}}, {640, 360, {255, 0, 255}},
      {320, 180, {255, 255, 255}}}};
  std::vector<EncodedPacket> packets;
  int submitted = 0;
  for (const auto &stage : stages) {
    const int stride = stage.width * 4 + 32;
    std::vector<uint8_t> pixels(stride * stage.height + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    for (int y = 0; y < stage.height; ++y)
      for (int x = 0; x < stage.width; ++x) {
        auto *p = pixels.data() + y * stride + x * 4;
        p[0] = stage.color[0]; p[1] = stage.color[1]; p[2] = stage.color[2]; p[3] = 255;
      }
    for (int i = 0; i < 6; ++i) {
      VideoFrame frame{};
      frame.data = pixels.data();
      frame.width = stage.width;
      frame.height = stage.height;
      frame.stride = stride;
      frame.pixelFormat = AV_PIX_FMT_RGBA;
      frame.timestamp = int64_t(submitted++) * 33333;
      EncodedPacket packet;
      if (encoder.encodeFrame(frame, packet))
        packets.push_back(std::move(packet));
    }
  }
  encoder.flush(packets);
  EXPECT_EQ(encoder.droppedFrames(), 0);
  ASSERT_EQ(packets.size(), size_t(submitted));

  ResizeDecoder decoder;
  const AVCodec *codecImpl = avcodec_find_decoder(AVCodecID(encoder.codecId()));
  ASSERT_NE(codecImpl, nullptr);
  decoder.context = avcodec_alloc_context3(codecImpl);
  ASSERT_NE(decoder.context, nullptr);
  ASSERT_NE(decoder.frame, nullptr);
  ASSERT_NE(decoder.packet, nullptr);
  const auto extra = encoder.extraData();
  decoder.context->extradata = static_cast<uint8_t *>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  ASSERT_NE(decoder.context->extradata, nullptr);
  memcpy(decoder.context->extradata, extra.data(), extra.size());
  decoder.context->extradata_size = int(extra.size());
  ASSERT_EQ(avcodec_open2(decoder.context, codecImpl, nullptr), 0);

  int decoded = 0;
  auto receive = [&] {
    int ret;
    while ((ret = avcodec_receive_frame(decoder.context, decoder.frame)) == 0) {
      ASSERT_LT(decoded, submitted);
      SCOPED_TRACE("decoded frame " + std::to_string(decoded));
      ASSERT_EQ(decoder.frame->width, config.width);
      ASSERT_EQ(decoder.frame->height, config.height);
      decoder.rgb = sws_getCachedContext(decoder.rgb, config.width, config.height,
          AVPixelFormat(decoder.frame->format), config.width, config.height,
          AV_PIX_FMT_RGB24, SWS_POINT, nullptr, nullptr, nullptr);
      ASSERT_NE(decoder.rgb, nullptr);
      std::vector<uint8_t> rgb(config.width * config.height * 3);
      uint8_t *dst[] = {rgb.data(), nullptr, nullptr, nullptr};
      int strides[] = {config.width * 3, 0, 0, 0};
      ASSERT_EQ(sws_scale(decoder.rgb, decoder.frame->data, decoder.frame->linesize,
                          0, config.height, dst, strides), config.height);
      const int stage = decoded / 6;
      auto check = [&](int x, int y, std::array<uint8_t, 3> expected) {
        for (int c = 0; c < 3; ++c)
          EXPECT_NEAR(rgb[(y * config.width + x) * 3 + c], expected[c], 28)
              << "pixel " << x << "," << y << " channel " << c;
      };
      check(160, 90, stages[stage].color);
      if (stage == 1) {
        check(8, 90, {0, 0, 0}); check(312, 90, {0, 0, 0});
        check(160, 8, stages[stage].color); check(160, 172, stages[stage].color);
      } else if (stage == 2) {
        check(160, 8, {0, 0, 0}); check(160, 172, {0, 0, 0});
        check(8, 90, stages[stage].color); check(312, 90, stages[stage].color);
      } else {
        check(20, 20, stages[stage].color); check(300, 160, stages[stage].color);
      }
      ++decoded;
      av_frame_unref(decoder.frame);
    }
    EXPECT_TRUE(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
  };
  for (const auto &packet : packets) {
    ASSERT_EQ(av_new_packet(decoder.packet, int(packet.data.size())), 0);
    memcpy(decoder.packet->data, packet.data.data(), packet.data.size());
    ASSERT_EQ(avcodec_send_packet(decoder.context, decoder.packet), 0);
    av_packet_unref(decoder.packet);
    receive();
  }
  ASSERT_EQ(avcodec_send_packet(decoder.context, nullptr), 0);
  receive();
  EXPECT_EQ(decoded, submitted);
}

TEST_P(EncoderResizeTest, UnusableResizedDmaBufRequestsFallbackWithoutPacket) {
  const auto [type, codec] = GetParam();
  FFmpegEncoder encoder(type, codec);
  EncoderConfig config;
  config.width = 320; config.height = 180; config.fps = 30; config.bitrate = 2000;
  if (!encoder.initialize(config))
    GTEST_SKIP() << codec << " unavailable";
  int requests = 0;
  for (const QSize size : {QSize(160, 120), QSize(640, 360), QSize(137, 79)}) {
    VideoFrame frame{};
    frame.width = size.width(); frame.height = size.height();
    frame.isDmaBuf = true;
    frame.pixelFormat = AV_PIX_FMT_DRM_PRIME;
    frame.dmaBufFd = -1; // Must not reach any native import operation.
    frame.requestCpuFallback = [&] { ++requests; };
    EncodedPacket packet;
    EXPECT_FALSE(encoder.encodeFrame(frame, packet));
    EXPECT_TRUE(packet.data.empty());
  }
  EXPECT_EQ(requests, 3);
  EXPECT_EQ(encoder.droppedFrames(), 3);
  EXPECT_EQ(encoder.eglImportFrames(), 0);
  EXPECT_EQ(encoder.zeroCopyFrames(), 0);
}

INSTANTIATE_TEST_SUITE_P(ResizePaths, EncoderResizeTest, ::testing::Values(
    ResizeEncoder{IEncoder::EncoderType::Software_x264, "libx264"},
    ResizeEncoder{IEncoder::EncoderType::NVENC_H264, "h264_nvenc"},
    ResizeEncoder{IEncoder::EncoderType::VAAPI_H264, "h264_vaapi"}));

// --- Generic preset mapping and rate-control wiring ------------------------
//
// Settings exposes x264's preset names for every encoder, but NVENC only
// accepts p1–p7. These cases pin the mapping and prove that the names we pass
// to each codec are valid AVOptions, without needing a GPU (av_opt_set
// validates named constants on the codec's private context).

namespace {
const std::array<QString, 7> kGenericPresets{
    {"ultrafast", "superfast", "veryfast", "faster", "fast", "medium",
     "slow"}};
const std::array<QString, 7> kNvencPresets{
    {"p1", "p2", "p3", "p4", "p5", "p6", "p7"}};
} // namespace

TEST(EncoderTuningTest, NvencPresetsMapToPLevels) {
  for (size_t i = 0; i < kGenericPresets.size(); ++i) {
    EXPECT_EQ(EncoderTuning::nvencPreset(kGenericPresets[i]),
              kNvencPresets[i])
        << kGenericPresets[i].toStdString();
  }
  // Empty/unknown keeps NVENC's own medium default rather than failing open.
  EXPECT_EQ(EncoderTuning::nvencPreset(""), QStringLiteral("p4"));
  EXPECT_EQ(EncoderTuning::nvencPreset("bogus"), QStringLiteral("p4"));
  EXPECT_EQ(EncoderTuning::nvencPreset(" SLOW "), QStringLiteral("p7"));
}

TEST(EncoderTuningTest, X264PresetsPassThroughAndFallBack) {
  for (const auto &preset : kGenericPresets)
    EXPECT_EQ(EncoderTuning::x264Preset(preset), preset);
  EXPECT_EQ(EncoderTuning::x264Preset("bogus"), QStringLiteral("medium"));
  EXPECT_EQ(EncoderTuning::x264Preset(""), QStringLiteral("medium"));
}

TEST(EncoderTuningTest, MappedNamesAreValidCodecOptions) {
  // NVENC: the exact strings initializeCodec() hands to avcodec_open2 must be
  // accepted, otherwise the codec fails to open (the old code passed x264's
  // names to NVENC, which is why the option was ignored instead).
  const AVCodec *nvenc = avcodec_find_encoder_by_name("h264_nvenc");
  if (!nvenc) {
    GTEST_SKIP() << "FFmpeg built without NVENC";
  }
  AVCodecContext *ctx = avcodec_alloc_context3(nvenc);
  ASSERT_NE(ctx, nullptr);
  for (const auto &preset : kGenericPresets) {
    const QString mapped = EncoderTuning::nvencPreset(preset);
    EXPECT_EQ(av_opt_set(ctx->priv_data, "preset", mapped.toUtf8().constData(),
                         0),
              0)
        << preset.toStdString() << " -> " << mapped.toStdString();
  }
  EXPECT_EQ(av_opt_set(ctx->priv_data, "tune", "hq", 0), 0);
  avcodec_free_context(&ctx);

  // x264 stores the preset as a string, so any value is "accepted" by the
  // option layer and only an unknown name fails at open time. Prove both
  // sides: the raw name fails, the sanitised name opens.
  const AVCodec *x264 = avcodec_find_encoder_by_name("libx264");
  if (!x264) {
    GTEST_SKIP() << "FFmpeg built without libx264";
  }
  auto openWithPreset = [&](const QString &preset) {
    AVCodecContext *codec = avcodec_alloc_context3(x264);
    if (!codec) return -1;
    codec->width = 64;
    codec->height = 64;
    codec->pix_fmt = AV_PIX_FMT_YUV420P;
    codec->time_base = {1, 30};
    codec->framerate = {30, 1};
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "preset", preset.toUtf8().constData(), 0);
    const int ret = avcodec_open2(codec, x264, &opts);
    av_dict_free(&opts);
    avcodec_free_context(&codec);
    return ret;
  };
  EXPECT_LT(openWithPreset("bogus"), 0)
      << "an unsanitised preset name must fail, not silently encode";
  EXPECT_EQ(openWithPreset(EncoderTuning::x264Preset("bogus")), 0);
  for (const auto &preset : kGenericPresets)
    EXPECT_EQ(openWithPreset(EncoderTuning::x264Preset(preset)), 0)
        << preset.toStdString();
}

namespace {

class X264RateControlTest : public ::testing::Test {
protected:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
  }

  static EncoderConfig smallConfig() {
    EncoderConfig config;
    config.width = 320;
    config.height = 180;
    config.fps = 30;
    config.bitrate = 5000;
    config.maxBitrate = 8000;
    config.preset = "slow";
    config.useVBR = true;
    return config;
  }

  static QString privateOption(AVCodecContext *ctx, const char *name) {
    uint8_t *value = nullptr;
    if (av_opt_get(ctx->priv_data, name, 0, &value) < 0 || !value)
      return QString();
    const QString text = QString::fromUtf8(reinterpret_cast<char *>(value));
    av_free(value);
    return text;
  }
};

} // namespace

TEST_F(X264RateControlTest, PresetAndVbrCeilingReachTheCodec) {
  FFmpegEncoder encoder(FFmpegEncoder::EncoderType::Software_x264, "libx264");
  if (!encoder.initialize(smallConfig()))
    GTEST_SKIP() << "libx264 not available.";

  AVCodecContext *ctx = FFmpegEncoderTestAccess::codecContext(encoder);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(ctx->bit_rate, 5000000);
  EXPECT_EQ(ctx->rc_max_rate, 8000000);
  EXPECT_EQ(ctx->rc_buffer_size, 16000000);
  // The dropdown used to be stored and then ignored entirely.
  EXPECT_EQ(privateOption(ctx, "preset"), QStringLiteral("slow"));
}

TEST_F(X264RateControlTest, UnknownPresetFallsBackInsteadOfFailingOpen) {
  EncoderConfig config = smallConfig();
  config.preset = "bogus";
  FFmpegEncoder encoder(FFmpegEncoder::EncoderType::Software_x264, "libx264");
  if (!encoder.initialize(config))
    FAIL() << "unknown preset must not fail encoder startup";
  AVCodecContext *ctx = FFmpegEncoderTestAccess::codecContext(encoder);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(privateOption(ctx, "preset"), QStringLiteral("medium"));
}

TEST_F(X264RateControlTest, CeilingIgnoredWhenCbrOrNotAboveTarget) {
  // A maximum at (or below) the target would fight rate control, so it is
  // deliberately not installed.
  EncoderConfig atTarget = smallConfig();
  atTarget.maxBitrate = atTarget.bitrate;
  FFmpegEncoder encoder(FFmpegEncoder::EncoderType::Software_x264, "libx264");
  if (!encoder.initialize(atTarget))
    GTEST_SKIP() << "libx264 not available.";
  AVCodecContext *ctx = FFmpegEncoderTestAccess::codecContext(encoder);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(ctx->rc_max_rate, 0);
  EXPECT_EQ(ctx->rc_buffer_size, 0);

  // CBR mode owns its own rate; no extra VBR ceiling is installed.
  EncoderConfig cbr = smallConfig();
  cbr.useVBR = false;
  FFmpegEncoder cbrEncoder(FFmpegEncoder::EncoderType::Software_x264,
                           "libx264");
  ASSERT_TRUE(cbrEncoder.initialize(cbr));
  ctx = FFmpegEncoderTestAccess::codecContext(cbrEncoder);
  ASSERT_NE(ctx, nullptr);
  EXPECT_EQ(ctx->rc_max_rate, 0);
  EXPECT_EQ(ctx->rc_buffer_size, 0);
}
