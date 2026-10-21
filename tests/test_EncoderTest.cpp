#include "capture/IVideoCaptureSource.h"
#include "encoding/FFmpegEncoder.h"
#include <QCoreApplication>
#include <QSize>
#include <gtest/gtest.h>
#include <libavutil/pixfmt.h>
#include <tuple>
#include <vector>
#include <array>
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
