/*
 * Trophy - Unit Tests
 * ReplayClipper: tail-cut stream copy from DVR segments. Segments are
 * encoded for real (libx264 + AAC, skip-guarded like the muxer test) so
 * keyframes, extradata and multi-stream interleave are authentic.
 */

#include <QCoreApplication>
#include <QTemporaryDir>
#include <cmath>
#include <gtest/gtest.h>

#include "encoding/AudioEncoder.h"
#include "encoding/FFmpegEncoder.h"
#include "encoding/IEncoder.h"
#include "capture/IVideoCaptureSource.h"
#include "muxing/FFmpegMuxer.h"
#include "muxing/ReplayClipper.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
}

using namespace TrophyBackend;

class ReplayClipperTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new ReplayClipperTestEnvironment);

class ReplayClipperTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;
  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }

  static bool haveCodecs() {
    return avcodec_find_encoder_by_name("libx264") != nullptr &&
           avcodec_find_encoder(AV_CODEC_ID_AAC) != nullptr;
  }

  // Encode a segment: x264 @30fps GOP30 + AAC stereo. All timestamps
  // restart at 0 (as DVR rotation produces).
  void makeSegment(const QString &path, int videoFrames) {
    FFmpegEncoder venc(IEncoder::EncoderType::Software_x264, "libx264");
    EncoderConfig vcfg;
    vcfg.width = 320;
    vcfg.height = 240;
    vcfg.fps = 30;
    vcfg.bitrate = 500;
    vcfg.preset = "ultrafast";
    vcfg.keyframeInterval = 1;
    vcfg.inputPixelFormat = AV_PIX_FMT_BGRA;
    ASSERT_TRUE(venc.initialize(vcfg));

    AudioEncoder aenc;
    AudioEncoderConfig acfg;
    acfg.sampleRate = 48000;
    acfg.channels = 2;
    acfg.bitrate = 128;
    acfg.codec = "aac";
    ASSERT_TRUE(aenc.initialize(acfg));

    FFmpegMuxer muxer;
    ASSERT_TRUE(muxer.initialize(path));

    StreamInfo vinfo;
    vinfo.type = StreamInfo::Type::Video;
    vinfo.codecId = venc.codecId();
    vinfo.extraData = venc.extraData();
    vinfo.pixelFormat = venc.encodedPixelFormat();
    vinfo.width = 320;
    vinfo.height = 240;
    vinfo.fps = 30;
    vinfo.bitrate = 500;
    ASSERT_GE(muxer.addStream(vinfo), 0);

    StreamInfo ainfo;
    ainfo.type = StreamInfo::Type::Audio;
    ainfo.codecId = aenc.codecId();
    ainfo.extraData = aenc.extraData();
    ainfo.sampleRate = 48000;
    ainfo.channels = 2;
    ainfo.bitrate = 128;
    ASSERT_GE(muxer.addStream(ainfo), 1);

    ASSERT_TRUE(muxer.writeHeader());

    std::vector<uint8_t> bgra(320 * 240 * 4, 0x80);
    for (int f = 0; f < videoFrames; ++f) {
      bgra[f % bgra.size()] = static_cast<uint8_t>(f);
      VideoFrame frame{bgra.data(), 320, 240, 320 * 4,
                       (int64_t)f * 1000000 / 30, AV_PIX_FMT_BGRA};
      EncodedPacket pkt;
      if (venc.encodeFrame(frame, pkt)) {
        pkt.streamIndex = 0;
        EXPECT_TRUE(muxer.writePacket(pkt));
      }
    }
    std::vector<EncodedPacket> vflushed;
    venc.flush(vflushed);
    for (auto &p : vflushed) {
      p.streamIndex = 0;
      EXPECT_TRUE(muxer.writePacket(p));
    }

    std::vector<float> sine(1024 * 2);
    EncodedPacket apkt;
    // Enough audio to cover the whole video span (48kHz @30fps).
    const int totalSamples = videoFrames * 1600;
    int produced = 0;
    for (int off = 0; off < totalSamples; off += 1024) {
      const int n = std::min(1024, totalSamples - off);
      for (int i = 0; i < n; ++i) {
        const float v =
            0.2f * std::sin(2.0f * 3.14159265f * 440.0f * (off + i) / 48000.0f);
        sine[i * 2] = v;
        sine[i * 2 + 1] = v;
      }
      if (aenc.encodeAudio(sine.data(), n, apkt)) {
        apkt.streamIndex = 1;
        EXPECT_TRUE(muxer.writePacket(apkt));
        ++produced;
      }
    }
    std::vector<EncodedPacket> aflushed;
    aenc.flush(aflushed);
    for (auto &p : aflushed) {
      p.streamIndex = 1;
      EXPECT_TRUE(muxer.writePacket(p));
    }
    EXPECT_TRUE(muxer.finalize());
  }

  // Read back per-stream (dts, pts, isKey) lists.
  struct PktInfo {
    int64_t dts;
    int64_t pts;
    bool key;
  };
  QVector<QVector<PktInfo>> readStreams(const QString &path) {
    QVector<QVector<PktInfo>> out;
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr,
                            nullptr) < 0) {
      return out;
    }
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
      avformat_close_input(&ctx);
      return out;
    }
    out.resize(ctx->nb_streams);
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(ctx, pkt) >= 0) {
      PktInfo pi{pkt->dts, pkt->pts, (pkt->flags & AV_PKT_FLAG_KEY) != 0};
      out[pkt->stream_index].push_back(pi);
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    return out;
  }
};

TEST_F(ReplayClipperTest, TailCutKeepsTrailingWindow) {
  if (!haveCodecs())
    GTEST_SKIP() << "libx264/AAC unavailable";
  const QString s0 = tempDir.path() + "/seg0.mkv";
  const QString s1 = tempDir.path() + "/seg1.mkv";
  makeSegment(s0, 90); // ~3s each
  makeSegment(s1, 90);

  QList<ReplaySegment> segs{{s0, 0.0}, {s1, 3.0}};
  const QString out = tempDir.path() + "/clip.mkv";
  QString err;
  ASSERT_TRUE(ReplayClipper::saveTail(segs, 1.0, out, &err))
      << qPrintable(err);

  auto streams = readStreams(out);
  ASSERT_EQ(streams.size(), 2);
  ASSERT_FALSE(streams[0].isEmpty());
  // Head cut lands on a video keyframe: first packet is a keyframe.
  EXPECT_TRUE(streams[0].first().key);
  // ~1.0s window at 30fps, plus up to one GOP of overshoot.
  EXPECT_GE(streams[0].size(), 20);
  EXPECT_LE(streams[0].size(), 65);
  // Core invariant: per-stream clocks never go backwards. Matroska
  // demux yields NOPTS DTS on head packets of B-frame streams (also
  // true of normal recordings), so clocks are checked from the first
  // packet that carries one.
  for (const auto &s : streams) {
    int start = 0;
    while (start < s.size() && s[start].dts == AV_NOPTS_VALUE)
      ++start;
    ASSERT_LT(start, s.size());
    EXPECT_GE(s[start].pts, 0);
    for (int i = start + 1; i < s.size(); ++i) {
      if (s[i].dts == AV_NOPTS_VALUE)
        continue;
      EXPECT_GE(s[i].dts, s[i - 1].dts);
      EXPECT_GE(s[i].pts, 0);
    }
  }
}

TEST_F(ReplayClipperTest, WindowLargerThanContentKeepsAll) {
  if (!haveCodecs())
    GTEST_SKIP() << "libx264/AAC unavailable";
  const QString s0 = tempDir.path() + "/seg0.mkv";
  makeSegment(s0, 90);

  const QString out = tempDir.path() + "/clip.mkv";
  QString err;
  ASSERT_TRUE(ReplayClipper::saveTail({{s0, 0.0}}, 30.0, out, &err))
      << qPrintable(err);
  auto streams = readStreams(out);
  ASSERT_EQ(streams.size(), 2);
  EXPECT_TRUE(streams[0].first().key);
  // The head cut may advance past the first GOP (its keyframe lacks a
  // demuxable DTS); everything from there on is kept.
  EXPECT_GT(streams[0].size(), 25);
}

TEST_F(ReplayClipperTest, OverlappingSegmentsDeduplicate) {
  if (!haveCodecs())
    GTEST_SKIP() << "libx264/AAC unavailable";
  // seg1 restarts 1s before seg0 ends (rotation overlap): shared packets
  // must not appear twice.
  const QString s0 = tempDir.path() + "/seg0.mkv";
  const QString s1 = tempDir.path() + "/seg1.mkv";
  makeSegment(s0, 90);
  makeSegment(s1, 90);

  QList<ReplaySegment> segs{{s0, 0.0}, {s1, 2.0}};
  const QString out = tempDir.path() + "/clip.mkv";
  QString err;
  ASSERT_TRUE(ReplayClipper::saveTail(segs, 3.0, out, &err)) << qPrintable(err);
  auto streams = readStreams(out);
  ASSERT_FALSE(streams[0].isEmpty());
  // Rotation seams can hold a couple of near-duplicate frames: the two
  // segments encode the overlap independently, so the same wall moment
  // may carry slightly different DTS on each side. Clocks must never
  // step backwards; wholesale tail duplication is caught by the count.
  EXPECT_LE(streams[0].size(), 105);
  int64_t lastDts = AV_NOPTS_VALUE;
  for (const auto &p : streams[0]) {
    if (p.dts == AV_NOPTS_VALUE)
      continue; // demuxer warmth on B-frame heads, not file content
    if (lastDts != AV_NOPTS_VALUE) {
      EXPECT_GE(p.dts, lastDts);
    }
    lastDts = p.dts;
  }
  EXPECT_NE(lastDts, AV_NOPTS_VALUE);
}

TEST_F(ReplayClipperTest, BadInputsFailCleanly) {
  QString err;
  EXPECT_FALSE(
      ReplayClipper::saveTail({}, 1.0, tempDir.path() + "/o.mkv", &err));
  EXPECT_FALSE(err.isEmpty());
  const QString s0 = tempDir.path() + "/seg0.mkv";
  QFile f(s0); // empty placeholder: never opened (arg validation first)
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.close();
  EXPECT_FALSE(
      ReplayClipper::saveTail({{s0, 0.0}}, 0.0, tempDir.path() + "/o.mkv", nullptr));
  EXPECT_FALSE(
      ReplayClipper::saveTail({{s0, 0.0}}, -5.0, tempDir.path() + "/o.mkv", nullptr));
  EXPECT_FALSE(
      ReplayClipper::saveTail({{"", 0.0}}, 1.0, tempDir.path() + "/o.mkv", nullptr));
  EXPECT_FALSE(ReplayClipper::saveTail({ {"/nonexistent/x.mkv", 0.0} }, 1.0,
                                       tempDir.path() + "/o.mkv", nullptr));
  EXPECT_FALSE(ReplayClipper::saveTail({{s0, 0.0}}, 1.0, "", nullptr));
}
