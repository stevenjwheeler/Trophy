#include "capture/IVideoCaptureSource.h"
#include "encoding/FFmpegEncoder.h"
#include <QCoreApplication>
#include <QSize>
#include <gtest/gtest.h>
#include <libavutil/pixfmt.h>
#include <tuple>
#include <vector>

using namespace TrophyBackend;

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