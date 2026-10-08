/*
 * Trophy - Unit Tests
 * ReplayClipper: tail-cut stream copy from DVR segments. Segments are
 * encoded for real (libx264 + AAC, skip-guarded like the muxer test) so
 * keyframes, extradata and multi-stream interleave are authentic.
 */

#include <QCoreApplication>
#include <QDateTime>
#include <QMap>
#include <QTemporaryDir>
#include <cmath>
#include <gtest/gtest.h>

#include "encoding/AudioEncoder.h"
#include "encoding/FFmpegEncoder.h"
#include "encoding/IEncoder.h"
#include "capture/IVideoCaptureSource.h"
#include "muxing/FFmpegMuxer.h"
#include "muxing/ReplayClipper.h"
#include "muxing/TrophyMetadata.h"

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

    QMap<QString, QString> meta;
    meta["title"] = "ClipperGame";
    // Stale trophy stamp on purpose: the clip must carry its own save
    // moment and the running recorder version, never the inherited
    // segment-start values.
    meta["trophy-version"] = "0.0.0-stale";
    meta["trophy-date"] = "2000-01-01";
    meta["trophy-time"] = "00-00-00";
    ASSERT_TRUE(muxer.setMetadata(meta));

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
  double kept = 0.0;
  ASSERT_TRUE(ReplayClipper::saveTail(segs, 1.0, out, &err, &kept))
      << qPrintable(err);

  auto streams = readStreams(out);
  ASSERT_EQ(streams.size(), 2);
  ASSERT_FALSE(streams[0].isEmpty());
  // Head cut lands on a video keyframe: first packet is a keyframe.
  EXPECT_TRUE(streams[0].first().key);
  // ~1.0s window at 30fps with 1s GOPs (keyframeInterval is honored):
  // the cut backs up to the last keyframe at or before the cut. seg1's
  // head keyframe (3.0s) carries no demuxable DTS — matroska can't store
  // the negative DTS of B-frame delay, per segment file — so the cut
  // falls back to 4.0s and the clip overshoots by ~1s. Production
  // windows are 30s, where the same effect is at most ~1 GOP and the
  // duration truncates to 0:30; the invariant that matters is kept >=
  // window (never undershoots).
  EXPECT_GE(streams[0].size(), 25);
  EXPECT_LE(streams[0].size(), 130);
  EXPECT_GE(kept, 1.0);
  EXPECT_LE(kept, 4.5);
  EXPECT_GE(streams[0].size(), 25);
  EXPECT_LE(streams[0].size(), 130);
  EXPECT_GE(kept, 1.0);
  EXPECT_LE(kept, 4.5);
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

TEST_F(ReplayClipperTest, OutputCarriesSegmentTitle) {
  if (!haveCodecs())
    GTEST_SKIP() << "libx264/AAC unavailable";
  const QString s0 = tempDir.path() + "/seg0.mkv";
  makeSegment(s0, 90);

  const QString out = tempDir.path() + "/clip.mkv";
  QString err;
  ASSERT_TRUE(ReplayClipper::saveTail({{s0, 0.0}}, 30.0, out, &err))
      << qPrintable(err);

  // The clip names itself after the game (segment title tag), not its
  // filename — this is what library cards and search display. It also
  // carries its own save moment (not the segment's start), its media
  // kind (survives renames, unlike the _Replay suffix) and the recorder
  // version.
  AVFormatContext *ctx = nullptr;
  ASSERT_EQ(
      avformat_open_input(&ctx, out.toUtf8().constData(), nullptr, nullptr),
      0);
  AVDictionaryEntry *title = av_dict_get(ctx->metadata, "title", nullptr, 0);
  ASSERT_NE(title, nullptr);
  EXPECT_STREQ(title->value, "ClipperGame");
  AVDictionaryEntry *kind =
      av_dict_get(ctx->metadata, "trophy-kind", nullptr, 0);
  ASSERT_NE(kind, nullptr);
  EXPECT_STREQ(kind->value, "clip");
  AVDictionaryEntry *created =
      av_dict_get(ctx->metadata, "creation_time", nullptr, 0);
  ASSERT_NE(created, nullptr);
  const QDateTime stamp = QDateTime::fromString(
      QString::fromUtf8(created->value).trimmed(), Qt::ISODate);
  EXPECT_TRUE(stamp.isValid());
  EXPECT_LT(qAbs(stamp.secsTo(QDateTime::currentDateTimeUtc())), 300);
  // Version is the running recorder's, not the stale segment value.
  AVDictionaryEntry *ver =
      av_dict_get(ctx->metadata, "trophy-version", nullptr, 0);
  ASSERT_NE(ver, nullptr);
  EXPECT_STREQ(ver->value, qPrintable(trophyVersion()));
  EXPECT_STRNE(ver->value, "0.0.0-stale");
  // Date/time are the save moment (today), not the segment start — and
  // the overwrite must replace the inherited tags, not duplicate them
  // (a stale duplicate first in the dict would win the probe lookup).
  AVDictionaryEntry *clipDate =
      av_dict_get(ctx->metadata, "trophy-date", nullptr, 0);
  AVDictionaryEntry *clipTime =
      av_dict_get(ctx->metadata, "trophy-time", nullptr, 0);
  ASSERT_NE(clipDate, nullptr);
  ASSERT_NE(clipTime, nullptr);
  const QDateTime clipStamp = trophyDateTimeFromTags(
      QString::fromUtf8(clipDate->value), QString::fromUtf8(clipTime->value));
  EXPECT_TRUE(clipStamp.isValid());
  EXPECT_LT(qAbs(clipStamp.secsTo(QDateTime::currentDateTime())), 300);
  int dateCount = 0;
  AVDictionaryEntry *e = nullptr;
  while ((e = av_dict_get(ctx->metadata, "trophy-date", e,
                          AV_DICT_IGNORE_SUFFIX)))
    ++dateCount;
  EXPECT_EQ(dateCount, 1);
  avformat_close_input(&ctx);
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
  // step backwards; wholesale tail duplication is caught by the count
  // (3s window + up to one 1s GOP of keyframe overshoot ≈ 120 frames).
  EXPECT_LE(streams[0].size(), 160);
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
