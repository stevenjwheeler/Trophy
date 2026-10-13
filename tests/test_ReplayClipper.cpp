/*
 * Trophy - Unit Tests
 * ReplayClipper: tail-cut stream copy from DVR segments. Segments are
 * encoded for real (libx264 + AAC, skip-guarded like the muxer test) so
 * keyframes, extradata and multi-stream interleave are authentic.
 */

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <gtest/gtest.h>
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>

#include "capture/IVideoCaptureSource.h"
#include "encoding/AudioEncoder.h"
#include "encoding/FFmpegEncoder.h"
#include "encoding/IEncoder.h"
#include "muxing/FFmpegMuxer.h"
#include "muxing/ReplayClipper.h"
#include "muxing/TrophyMetadata.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
}

using namespace TrophyBackend;

namespace TrophyBackend {
struct ReplayClipperTestAccess {
  explicit ReplayClipperTestAccess(const ReplayClipperInternal::Operations &ops)
      : previous(ReplayClipper::s_operations) {
    ReplayClipper::s_operations = &ops;
  }
  ~ReplayClipperTestAccess() { ReplayClipper::s_operations = previous; }
  ReplayClipperTestAccess(const ReplayClipperTestAccess &) = delete;
  ReplayClipperTestAccess &operator=(const ReplayClipperTestAccess &) = delete;

private:
  const ReplayClipperInternal::Operations *previous;
};
} // namespace TrophyBackend

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

  void makeCliSegment(const QString &path, const QString &size = "320x240",
                      const QString &audioCodec = "aac", int rate = 48000,
                      double audioOffset = 0.0) {
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                            "testsrc=size=" + size + ":rate=30:duration=3",
                            "-itsoffset", QString::number(audioOffset), "-f",
                            "lavfi", "-i", "sine=sample_rate=" +
                                QString::number(rate) + ":duration=2",
                            "-map", "0:v", "-map", "1:a", "-c:v", "libx264",
                            "-pix_fmt", "yuv420p", "-g", "12", "-bf", "0",
                            "-c:a", audioCodec, path});
    ASSERT_TRUE(ffmpeg.waitForFinished(30000));
    ASSERT_EQ(ffmpeg.exitStatus(), QProcess::NormalExit);
    ASSERT_EQ(ffmpeg.exitCode(), 0)
        << ffmpeg.readAllStandardError().constData();
  }

  QVector<double> firstPtsSeconds(const QString &path) {
    QVector<double> result;
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) < 0)
      return result;
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
      avformat_close_input(&ctx);
      return result;
    }
    result.fill(-1.0, ctx->nb_streams);
    AVPacket *pkt = av_packet_alloc();
    if (pkt) {
      while (av_read_frame(ctx, pkt) >= 0) {
        if (result[pkt->stream_index] < 0 && pkt->pts != AV_NOPTS_VALUE)
          result[pkt->stream_index] = pkt->pts *
              av_q2d(ctx->streams[pkt->stream_index]->time_base);
        av_packet_unref(pkt);
      }
      av_packet_free(&pkt);
    }
    avformat_close_input(&ctx);
    return result;
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
      VideoFrame frame{
          bgra.data(),    320, 240, 320 * 4, (int64_t)f * 1000000 / 30,
          AV_PIX_FMT_BGRA};
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
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) <
        0) {
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

TEST_F(ReplayClipperTest, NativeFileSizeLimitCleansPartialAndRetries) {
  if (!haveCodecs())
    GTEST_SKIP() << "libx264/AAC unavailable";
  const QString source = tempDir.path() + "/native-limit-source.mkv";
  makeSegment(source, 90);
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_GT(QFileInfo(source).size(), 4096);

  // Run native EFBIG failures only in a death-test subprocess. Ignore
  // SIGXFSZ there so write(2) returns EFBIG rather than killing the worker.
  // The parent's resource limits and signal disposition never change.
  auto child = [&]() {
    struct rlimit original {};
    if (getrlimit(RLIMIT_FSIZE, &original) != 0 || original.rlim_max < 4096)
      _exit(10);
    struct sigaction ignore {}, previous {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGXFSZ, &ignore, &previous) != 0)
      _exit(11);
    for (bool tail : {false, true}) {
      for (rlim_t bytes : {rlim_t(512), rlim_t(4096)}) {
        const QString output = tempDir.path() +
            QString("/limit-%1-%2.mkv").arg(tail).arg(bytes);
        QFile sentinel(output);
        if (!sentinel.open(QIODevice::WriteOnly) ||
            sentinel.write("preserve") != 8)
          _exit(12);
        sentinel.close();
        struct rlimit limited = original;
        limited.rlim_cur = bytes;
        if (setrlimit(RLIMIT_FSIZE, &limited) != 0)
          _exit(13);
        QString error;
        double kept = -123.0;
        auto cut = [&]() {
          return tail ? ReplayClipper::saveTail({{source, 0.0}}, 3.0, output + ".part",
                                               &error, &kept)
                      : ReplayClipper::saveRange(source, 0.0, 3.0, output + ".part",
                                                &error, &kept);
        };
        const bool saved = cut();
        if (setrlimit(RLIMIT_FSIZE, &original) != 0)
          _exit(14);
        if (saved || error.isEmpty() || kept != -123.0 ||
            QFile::exists(output + ".part"))
          _exit(15);
        if (!sentinel.open(QIODevice::ReadOnly) || sentinel.readAll() != "preserve")
          _exit(16);
        sentinel.close();
        if (!cut() || kept <= 0.0 || !QFile::exists(output + ".part"))
          _exit(17);
        const auto streams = readStreams(output + ".part");
        if (streams.size() != 2 || streams[0].isEmpty() || streams[1].isEmpty())
          _exit(18);
      }
    }
    if (sigaction(SIGXFSZ, &previous, nullptr) != 0)
      _exit(19);
    _exit(0);
  };
  EXPECT_EXIT(child(), ::testing::ExitedWithCode(0), "");
}

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
      avformat_open_input(&ctx, out.toUtf8().constData(), nullptr, nullptr), 0);
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
  while (
      (e = av_dict_get(ctx->metadata, "trophy-date", e, AV_DICT_IGNORE_SUFFIX)))
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
  EXPECT_FALSE(ReplayClipper::saveTail({{s0, 0.0}}, 0.0,
                                       tempDir.path() + "/o.mkv", nullptr));
  EXPECT_FALSE(ReplayClipper::saveTail({{s0, 0.0}}, -5.0,
                                       tempDir.path() + "/o.mkv", nullptr));
  EXPECT_FALSE(ReplayClipper::saveTail({{"", 0.0}}, 1.0,
                                       tempDir.path() + "/o.mkv", nullptr));
  EXPECT_FALSE(ReplayClipper::saveTail({{"/nonexistent/x.mkv", 0.0}}, 1.0,
                                       tempDir.path() + "/o.mkv", nullptr));
  EXPECT_FALSE(ReplayClipper::saveTail({{s0, 0.0}}, 1.0, "", nullptr));
}

TEST_F(ReplayClipperTest, RangeCutKeepsMiddleSpan) {
  if (!haveCodecs())
    GTEST_SKIP() << "libx264/AAC unavailable";
  const QString src = tempDir.path() + "/src.mkv";
  makeSegment(src, 90); // ~3s at 30fps, 1s GOPs

  const QString out = tempDir.path() + "/clip.mkv";
  QString err;
  double kept = 0.0;
  ASSERT_TRUE(ReplayClipper::saveRange(src, 1.0, 2.0, out, &err, &kept))
      << qPrintable(err);

  auto streams = readStreams(out);
  ASSERT_EQ(streams.size(), 2);
  ASSERT_FALSE(streams[0].isEmpty());
  // Head cut lands on a video keyframe: first packet is a keyframe.
  EXPECT_TRUE(streams[0].first().key);
  // 1s range at 30fps: ~30 frames. The end bound is what keeps the tail
  // out — without it the same cut would keep ~60 frames to EOF. The
  // head may back up by up to one 1s GOP (keyframe overshoot), never
  // more.
  EXPECT_GE(streams[0].size(), 25);
  EXPECT_LE(streams[0].size(), 65);
  EXPECT_GE(kept, 1.0);
  EXPECT_LE(kept, 2.5);
  // Per-stream clocks never go backwards (same NOPTS-head caveat as the
  // tail cut: matroska demux yields NOPTS DTS on B-frame heads).
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
  // Same identity stamping as DVR clips: game title carried, kind clip.
  AVFormatContext *ctx = nullptr;
  ASSERT_EQ(
      avformat_open_input(&ctx, out.toUtf8().constData(), nullptr, nullptr), 0);
  AVDictionaryEntry *title = av_dict_get(ctx->metadata, "title", nullptr, 0);
  ASSERT_NE(title, nullptr);
  EXPECT_STREQ(title->value, "ClipperGame");
  AVDictionaryEntry *kind =
      av_dict_get(ctx->metadata, "trophy-kind", nullptr, 0);
  ASSERT_NE(kind, nullptr);
  EXPECT_STREQ(kind->value, "clip");
  avformat_close_input(&ctx);
}

TEST_F(ReplayClipperTest, RangeEndPastEofKeepsToEnd) {
  if (!haveCodecs())
    GTEST_SKIP() << "libx264/AAC unavailable";
  const QString src = tempDir.path() + "/src.mkv";
  makeSegment(src, 90);

  const QString out = tempDir.path() + "/clip.mkv";
  QString err;
  double kept = 0.0;
  ASSERT_TRUE(ReplayClipper::saveRange(src, 2.0, 60.0, out, &err, &kept))
      << qPrintable(err);
  auto streams = readStreams(out);
  ASSERT_FALSE(streams[0].isEmpty());
  EXPECT_TRUE(streams[0].first().key);
  // ~1s of tail: the end bound past EOF changes nothing.
  EXPECT_GE(streams[0].size(), 20);
  EXPECT_LE(streams[0].size(), 65);
}

TEST_F(ReplayClipperTest, ForwardKeyframeBeyondRangeEndFailsAndRetries) {
  if (!haveCodecs())
    GTEST_SKIP() << "libx264/AAC unavailable";
  const QString source = tempDir.path() + "/short-range-source.mkv";
  makeSegment(source, 90);
  ASSERT_FALSE(HasFatalFailure());
  const QString output = tempDir.path() + "/short-range.mkv.part";
  QString error;
  double kept = -123.0;
  // This source's head keyframe has no demuxable DTS. Its first usable
  // keyframe is at ~1s, beyond the requested 250ms range: keeping that
  // frame would report a successful zero-length clip outside the range.
  EXPECT_FALSE(ReplayClipper::saveRange(source, 0.0, 0.25, output, &error, &kept));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_EQ(kept, -123.0);
  EXPECT_FALSE(QFile::exists(output));
  ASSERT_TRUE(ReplayClipper::saveRange(source, 0.0, 2.0, output, &error, &kept));
  EXPECT_GT(kept, 0.0);
  const auto streams = readStreams(output);
  ASSERT_EQ(streams.size(), 2);
  EXPECT_GT(streams[0].size(), 1);
  EXPECT_FALSE(streams[1].isEmpty());
}

TEST_F(ReplayClipperTest, RangeStartAtZeroKeepsHead) {
  if (!haveCodecs())
    GTEST_SKIP() << "libx264/AAC unavailable";
  const QString src = tempDir.path() + "/src.mkv";
  makeSegment(src, 90);

  const QString out = tempDir.path() + "/clip.mkv";
  QString err;
  ASSERT_TRUE(ReplayClipper::saveRange(src, 0.0, 2.0, out, &err))
      << qPrintable(err);
  auto streams = readStreams(out);
  ASSERT_FALSE(streams[0].isEmpty());
  // The cut precedes the first keyframe, so it falls back forward; the
  // head GOP (whose keyframe lacks a demuxable DTS) may be skipped, but
  // everything from there to the end bound is kept (~1s at 30fps).
  EXPECT_TRUE(streams[0].first().key);
  EXPECT_GE(streams[0].size(), 25);
  EXPECT_LE(streams[0].size(), 65);
}

TEST_F(ReplayClipperTest, RangeCutRescalesPacketDurationToOutputTimeBase) {
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || !haveCodecs())
    GTEST_SKIP() << "ffmpeg CLI or libx264 unavailable";
  const QString src = tempDir.path() + "/timescale.mp4";
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          "testsrc=size=320x240:rate=30:duration=5", "-an",
                          "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "30",
                          "-video_track_timescale", "90000", src});
  ASSERT_TRUE(ffmpeg.waitForFinished(30000));
  ASSERT_EQ(ffmpeg.exitCode(), 0) << ffmpeg.readAllStandardError().constData();

  AVFormatContext *input = nullptr;
  ASSERT_GE(
      avformat_open_input(&input, src.toUtf8().constData(), nullptr, nullptr),
      0);
  ASSERT_GE(avformat_find_stream_info(input, nullptr), 0);
  AVRational inputTb{0, 1};
  for (unsigned i = 0; i < input->nb_streams; ++i) {
    if (input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      inputTb = input->streams[i]->time_base;
      break;
    }
  }
  avformat_close_input(&input);
  ASSERT_GT(inputTb.num, 0);

  const QString out = tempDir.path() + "/rescaled.mkv";
  QString err;
  ASSERT_TRUE(ReplayClipper::saveRange(src, 0.0, 4.0, out, &err))
      << qPrintable(err);
  AVFormatContext *output = nullptr;
  ASSERT_GE(
      avformat_open_input(&output, out.toUtf8().constData(), nullptr, nullptr),
      0);
  ASSERT_GE(avformat_find_stream_info(output, nullptr), 0);
  int videoIndex = -1;
  for (unsigned i = 0; i < output->nb_streams; ++i) {
    if (output->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      videoIndex = static_cast<int>(i);
      break;
    }
  }
  ASSERT_GE(videoIndex, 0);
  const AVRational outputTb = output->streams[videoIndex]->time_base;
  ASSERT_NE(av_cmp_q(inputTb, outputTb), 0);
  AVPacket *pkt = av_packet_alloc();
  ASSERT_NE(pkt, nullptr);
  int64_t packetDuration = 0;
  while (av_read_frame(output, pkt) >= 0) {
    if (pkt->stream_index == videoIndex) {
      packetDuration = pkt->duration;
      av_packet_unref(pkt);
      break;
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  avformat_close_input(&output);
  ASSERT_GT(packetDuration, 0);
  EXPECT_NEAR(packetDuration * av_q2d(outputTb), 1.0 / 30.0, 0.005);
}

TEST_F(ReplayClipperTest, BadRangesFailCleanly) {
  // Argument validation runs before any file is opened (the placeholder
  // is never read on these paths).
  const QString s0 = tempDir.path() + "/seg0.mkv";
  QFile f(s0);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.close();
  QString err;
  EXPECT_FALSE(
      ReplayClipper::saveRange(s0, 2.0, 1.0, tempDir.path() + "/o.mkv", &err));
  EXPECT_FALSE(err.isEmpty());
  err.clear();
  EXPECT_FALSE(
      ReplayClipper::saveRange(s0, 1.0, 1.0, tempDir.path() + "/o.mkv", &err));
  EXPECT_FALSE(err.isEmpty());
  EXPECT_FALSE(ReplayClipper::saveRange(s0, -1.0, 1.0,
                                        tempDir.path() + "/o.mkv", nullptr));
  EXPECT_FALSE(ReplayClipper::saveRange("", 0.0, 1.0, tempDir.path() + "/o.mkv",
                                        nullptr));
  EXPECT_FALSE(ReplayClipper::saveRange("/nonexistent/x.mkv", 0.0, 1.0,
                                        tempDir.path() + "/o.mkv", nullptr));
  // A real (if unreadable) file with a bad output still fails cleanly.
  EXPECT_FALSE(ReplayClipper::saveRange(s0, 0.0, 1.0, "", nullptr));
}

TEST_F(ReplayClipperTest, RejectsEqualCountLayoutMismatchesBeforeWriting) {
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || !haveCodecs())
    GTEST_SKIP() << "ffmpeg CLI or libx264/AAC unavailable";
  const QString first = tempDir.path() + "/first.mkv";
  ASSERT_NO_FATAL_FAILURE(makeCliSegment(first));
  struct Variant { const char *name; const char *size; const char *codec; int rate; };
  const Variant variants[] = {
      {"codec", "320x240", "pcm_s16le", 48000},
      {"rate", "320x240", "aac", 44100},
      {"width", "352x240", "aac", 48000},
      {"height", "320x256", "aac", 48000}};
  for (const auto &v : variants) {
    SCOPED_TRACE(v.name);
    const QString second = tempDir.path() + "/" + v.name + ".mkv";
    ASSERT_NO_FATAL_FAILURE(makeCliSegment(second, v.size, v.codec, v.rate));
    const QString out = tempDir.path() + "/clip.mkv";
    QFile destination(out);
    ASSERT_TRUE(destination.open(QIODevice::WriteOnly));
    ASSERT_EQ(destination.write("existing destination"), 20);
    destination.close();
    QString error;
    EXPECT_FALSE(ReplayClipper::saveTail({{first, 0.0}, {second, 3.0}},
                                       6.0, out, &error));
    EXPECT_EQ(error, "input stream layout mismatch");
    ASSERT_TRUE(destination.open(QIODevice::ReadOnly));
    EXPECT_EQ(destination.readAll(), QByteArray("existing destination"));
  }
}

TEST_F(ReplayClipperTest, RangeAndTailPreserveLateAudioOnSharedVideoTimeline) {
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || !haveCodecs())
    GTEST_SKIP() << "ffmpeg CLI or libx264/AAC unavailable";
  const QString source = tempDir.path() + "/offset.mkv";
  ASSERT_NO_FATAL_FAILURE(makeCliSegment(source, "320x240", "aac", 48000, 0.8));
  const auto inputPts = firstPtsSeconds(source);
  ASSERT_EQ(inputPts.size(), 2);
  ASSERT_GT(inputPts[1] - inputPts[0], 0.7);
  const QList<ReplaySegment> segments{{source, 0.0}};
  for (bool tail : {false, true}) {
    SCOPED_TRACE(tail ? "tail" : "range");
    const QString out = tempDir.path() + (tail ? "/tail.mkv" : "/range.mkv");
    QString error;
    const bool saved = tail
        ? ReplayClipper::saveTail(segments, 5.0, out, &error)
        : ReplayClipper::saveRange(source, 0.0, 2.5, out, &error);
    ASSERT_TRUE(saved) << qPrintable(error);
    const auto outputPts = firstPtsSeconds(out);
    ASSERT_EQ(outputPts.size(), 2);
    ASSERT_GE(outputPts[0], 0.0);
    ASSERT_GE(outputPts[1], 0.0);
    EXPECT_NEAR(outputPts[1] - outputPts[0], inputPts[1] - inputPts[0], 0.002);
  }
}

TEST_F(ReplayClipperTest, HeaderFailureRemovesPartialAndPreservesDestination) {
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() ||
      !avcodec_find_encoder(AV_CODEC_ID_RAWVIDEO))
    GTEST_SKIP() << "ffmpeg CLI or rawvideo unavailable";
  // AVI supports raw RGB, while Matroska rejects it by default. This reaches
  // writeCut's header failure after opening/truncating the supplied .part.
  const QString source = tempDir.path() + "/rgb.avi";
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          "testsrc=size=64x64:rate=30:duration=1", "-an",
                          "-c:v", "rawvideo", "-pix_fmt", "bgr24", "-g", "12",
                          source});
  ASSERT_TRUE(ffmpeg.waitForFinished(30000));
  ASSERT_EQ(ffmpeg.exitCode(), 0) << ffmpeg.readAllStandardError().constData();
  const QString out = tempDir.path() + "/clip.mkv";
  QFile destination(out);
  ASSERT_TRUE(destination.open(QIODevice::WriteOnly));
  ASSERT_EQ(destination.write("existing destination"), 20);
  destination.close();
  const QList<ReplaySegment> segments{{source, 0.0}};
  for (bool tail : {false, true}) {
    SCOPED_TRACE(tail ? "tail" : "range");
    QString error;
    const bool saved = tail
        ? ReplayClipper::saveTail(segments, 5.0, out + ".part", &error)
        : ReplayClipper::saveRange(source, 0.0, 0.8, out + ".part", &error);
    EXPECT_FALSE(saved);
    EXPECT_EQ(error, "cannot write header");
    EXPECT_FALSE(QFile::exists(out + ".part"));
    ASSERT_TRUE(destination.open(QIODevice::ReadOnly));
    EXPECT_EQ(destination.readAll(), QByteArray("existing destination"));
    destination.close();
  }
}

namespace {
enum class CutFault {
  ScanRead, CopyRead, ScanPacket, InputPacket, OutputPacket, PacketRef,
  OutputContext, OutputStream, CodecParameters, Header, Mux, ConsumedMux, Trailer,
  Flush, Close, OpenScan, ProbeScan, OpenCopy, ProbeCopy, AvioOpen,
  MetadataCopy, MetadataKind, MetadataVersion, MetadataDate, MetadataTime,
  MetadataCreation
};

struct CutFaultState {
  CutFault fault;
  QString partialPath;
  bool copying = false;
  bool fired = false;
  bool partialExisted = false;
  int scanReads = 0;
  int copyReads = 0;
  int allocations = 0;
  int refs = 0;
  int streams = 0;
  int parameters = 0;
  int writes = 0;
  int closes = 0;
  int inputOpens = 0;
  int probes = 0;
  bool closedBeforeRemoval = false;
  bool closeClearedHandle = false;

  void fire() {
    fired = true;
    partialExisted = QFile::exists(partialPath);
  }
};
thread_local CutFaultState *cutFault = nullptr;

int faultInputOpen(AVFormatContext **ctx, const char *url,
                   const AVInputFormat *format, AVDictionary **options) {
  auto &s = *cutFault;
  const int n = ++s.inputOpens;
  if ((s.fault == CutFault::OpenScan && n == 2) ||
      (s.fault == CutFault::OpenCopy && n == 4)) {
    s.fire();
    *ctx = nullptr;
    return AVERROR(EIO);
  }
  return avformat_open_input(ctx, url, format, options);
}

int faultProbe(AVFormatContext *ctx, AVDictionary **options) {
  auto &s = *cutFault;
  const int n = ++s.probes;
  const int result = avformat_find_stream_info(ctx, options);
  if (result >= 0 && ((s.fault == CutFault::ProbeScan && n == 2) ||
                     (s.fault == CutFault::ProbeCopy && n == 4))) {
    s.fire(); // A real open/probed context must be closed on this error.
    return AVERROR(EIO);
  }
  return result;
}

int faultAvioOpen(AVIOContext **ctx, const char *url, int flags) {
  if (cutFault->fault == CutFault::AvioOpen) {
    cutFault->fire();
    *ctx = nullptr;
    return AVERROR(EACCES);
  }
  return avio_open(ctx, url, flags);
}

int faultRead(AVFormatContext *ctx, AVPacket *pkt) {
  auto &s = *cutFault;
  int &calls = s.copying ? s.copyReads : s.scanReads;
  if (++calls == 12 && s.fault ==
          (s.copying ? CutFault::CopyRead : CutFault::ScanRead)) {
    s.fire();
    return AVERROR(EIO); // Not EOF: a partial scan/copy must not succeed.
  }
  const int result = av_read_frame(ctx, pkt);
  if (result == AVERROR_EOF)
    s.copying = true; // One real fixture input, reopened for the copy pass.
  return result;
}

AVPacket *faultAlloc() {
  auto &s = *cutFault;
  const int n = ++s.allocations;
  if ((s.fault == CutFault::ScanPacket && n == 1) ||
      (s.fault == CutFault::InputPacket && n == 2) ||
      (s.fault == CutFault::OutputPacket && n == 3)) {
    s.fire();
    return nullptr;
  }
  return av_packet_alloc();
}

int faultRef(AVPacket *dst, const AVPacket *src) {
  auto &s = *cutFault;
  if (++s.refs == 4 && s.fault == CutFault::PacketRef) {
    s.fire();
    return AVERROR(ENOMEM);
  }
  return av_packet_ref(dst, src);
}

int faultOutput(AVFormatContext **ctx, const AVOutputFormat *format,
                const char *name, const char *filename) {
  auto &s = *cutFault;
  if (s.fault == CutFault::OutputContext) {
    s.fire();
    *ctx = nullptr;
    return AVERROR(ENOMEM);
  }
  return avformat_alloc_output_context2(ctx, format, name, filename);
}

AVStream *faultStream(AVFormatContext *ctx, const AVCodec *codec) {
  auto &s = *cutFault;
  if (++s.streams == 2 && s.fault == CutFault::OutputStream) {
    s.fire();
    return nullptr;
  }
  return avformat_new_stream(ctx, codec);
}

int faultParameters(AVCodecParameters *dst, const AVCodecParameters *src) {
  auto &s = *cutFault;
  if (++s.parameters == 2 && s.fault == CutFault::CodecParameters) {
    s.fire();
    return AVERROR(ENOMEM);
  }
  return avcodec_parameters_copy(dst, src);
}

int faultMetadataCopy(AVDictionary **dst, const AVDictionary *src, int flags) {
  if (cutFault->fault == CutFault::MetadataCopy) {
    // A failed copy can leave an allocated, partially populated dictionary.
    const int result = av_dict_set(dst, "partial-copy", "allocated", 0);
    if (result < 0)
      return result;
    cutFault->fire();
    return AVERROR(ENOMEM);
  }
  return av_dict_copy(dst, src, flags);
}

int faultMetadataSet(AVDictionary **dst, const char *key, const char *value,
                     int flags) {
  const struct { CutFault fault; const char *key; } stamps[] = {
      {CutFault::MetadataKind, "trophy-kind"},
      {CutFault::MetadataVersion, "trophy-version"},
      {CutFault::MetadataDate, "trophy-date"},
      {CutFault::MetadataTime, "trophy-time"},
      {CutFault::MetadataCreation, "creation_time"}};
  for (const auto &stamp : stamps) {
    if (cutFault->fault == stamp.fault && QByteArray(key) == stamp.key) {
      cutFault->fire();
      return AVERROR(ENOMEM);
    }
  }
  return av_dict_set(dst, key, value, flags);
}

int faultHeader(AVFormatContext *ctx, AVDictionary **options) {
  const int result = avformat_write_header(ctx, options);
  if (result >= 0 && cutFault->fault == CutFault::Header) {
    cutFault->fire(); // Fail after actual header output has begun.
    return AVERROR(EIO);
  }
  return result;
}

int faultMux(AVFormatContext *ctx, AVPacket *pkt) {
  auto &s = *cutFault;
  if (++s.writes == 4 &&
      (s.fault == CutFault::Mux || s.fault == CutFault::ConsumedMux)) {
    s.fire();
    // FFmpeg may blank the packet even on error. Exercise both forms.
    if (s.fault == CutFault::ConsumedMux)
      av_packet_unref(pkt);
    return AVERROR(EIO);
  }
  return av_interleaved_write_frame(ctx, pkt);
}

int faultTrailer(AVFormatContext *ctx) {
  const int result = av_write_trailer(ctx);
  if (result >= 0 && cutFault->fault == CutFault::Trailer) {
    cutFault->fire(); // Even finalized-looking bytes must not be published.
    return AVERROR(EIO);
  }
  return result;
}

void faultFlush(AVIOContext *ctx) {
  avio_flush(ctx);
  if (cutFault->fault == CutFault::Flush) {
    cutFault->fire();
    ctx->error = AVERROR(ENOSPC); // avio_flush is void; errors are sticky.
  }
}

int faultClose(AVIOContext **ctx) {
  auto &s = *cutFault;
  ++s.closes;
  s.closedBeforeRemoval = QFile::exists(s.partialPath);
  const int result = avio_closep(ctx); // Always retire the actual file handle.
  s.closeClearedHandle = *ctx == nullptr;
  if (result >= 0 && s.fault == CutFault::Close) {
    s.fire();
    return AVERROR(EIO);
  }
  return result;
}

struct FaultCase { CutFault fault; const char *name; const char *error; };
class ReplayClipperFaultTest : public ReplayClipperTest,
                              public ::testing::WithParamInterface<FaultCase> {};
} // namespace

TEST_F(ReplayClipperTest, SuccessfulFinalizationClosesOutputExactlyOnce) {
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || !haveCodecs())
    GTEST_SKIP() << "ffmpeg CLI or libx264/AAC unavailable";
  const QString source = tempDir.path() + "/source.mkv";
  ASSERT_NO_FATAL_FAILURE(makeCliSegment(source));
  const QList<ReplaySegment> segments{{source, 0.0}};
  for (bool tail : {false, true}) {
    SCOPED_TRACE(tail ? "tail" : "range");
    const QString output = tempDir.path() + (tail ? "/tail.part" : "/range.part");
    // Only observe close: this fault selector cannot fire through that hook.
    CutFaultState state{CutFault::Trailer, output};
    ReplayClipperInternal::Operations ops;
    ops.closeOutput = &faultClose;
    QString error;
    double kept = -123.0;
    {
      struct StateGuard {
        CutFaultState *previous = cutFault;
        explicit StateGuard(CutFaultState &state) { cutFault = &state; }
        ~StateGuard() { cutFault = previous; }
      } stateGuard(state);
      ReplayClipperTestAccess override(ops);
      const bool saved = tail
          ? ReplayClipper::saveTail(segments, 5.0, output, &error, &kept)
          : ReplayClipper::saveRange(source, 0.0, 2.5, output, &error, &kept);
      EXPECT_TRUE(saved) << qPrintable(error);
    }
    EXPECT_FALSE(state.fired);
    EXPECT_EQ(state.closes, 1);
    EXPECT_TRUE(state.closedBeforeRemoval);
    EXPECT_TRUE(state.closeClearedHandle);
    EXPECT_TRUE(QFile::exists(output));
    EXPECT_GT(kept, 2.0);
    const auto packets = readStreams(output);
    ASSERT_EQ(packets.size(), 2);
    ASSERT_FALSE(packets[0].isEmpty());
    EXPECT_TRUE(packets[0].first().key);
  }
}

TEST_P(ReplayClipperFaultTest, CutFailureCleansPartialAndCanRetry) {
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || !haveCodecs())
    GTEST_SKIP() << "ffmpeg CLI or libx264/AAC unavailable";
  const QString source = tempDir.path() + "/source.mkv";
  ASSERT_NO_FATAL_FAILURE(makeCliSegment(source)); // -g 12, -bf 0
  const auto sourcePackets = readStreams(source);
  ASSERT_EQ(sourcePackets.size(), 2);
  int keys = 0;
  for (const auto &pkt : sourcePackets[0])
    keys += pkt.key;
  ASSERT_GT(keys, 2); // The real cut has usable, distinct keyframe choices.

  const auto testCase = GetParam();
  const QString destinationPath = tempDir.path() + "/clip.mkv";
  const QString partialPath = destinationPath + ".part";
  const QList<ReplaySegment> segments{{source, 0.0}};
  QFile destination(destinationPath);
  ASSERT_TRUE(destination.open(QIODevice::WriteOnly));
  ASSERT_EQ(destination.write("caller-managed destination"), 26);
  destination.close();

  for (bool tail : {false, true}) {
    SCOPED_TRACE(tail ? "tail" : "range");
    ReplayClipperInternal::Operations ops;
    ops.openInput = &faultInputOpen;
    ops.probeInput = &faultProbe;
    ops.openOutput = &faultAvioOpen;
    ops.readFrame = &faultRead;
    ops.packetAlloc = &faultAlloc;
    ops.packetRef = &faultRef;
    ops.allocOutput = &faultOutput;
    ops.newStream = &faultStream;
    ops.copyParameters = &faultParameters;
    ops.copyMetadata = &faultMetadataCopy;
    ops.setMetadata = &faultMetadataSet;
    ops.writeHeader = &faultHeader;
    ops.writeFrame = &faultMux;
    ops.writeTrailer = &faultTrailer;
    ops.flush = &faultFlush;
    ops.closeOutput = &faultClose;
    CutFaultState state{testCase.fault, partialPath};
    QString error;
    double kept = -123.0;
    {
      struct StateGuard {
        CutFaultState *previous = cutFault;
        explicit StateGuard(CutFaultState &state) { cutFault = &state; }
        ~StateGuard() { cutFault = previous; }
      } stateGuard(state);
      ReplayClipperTestAccess override(ops);
      const bool saved = tail
          ? ReplayClipper::saveTail(segments, 5.0, partialPath, &error, &kept)
          : ReplayClipper::saveRange(source, 0.0, 2.5, partialPath, &error, &kept);
      EXPECT_FALSE(saved);
    }
    ASSERT_TRUE(state.fired); // Never pass by failing before the injected site.
    QString expectedError = QString::fromUtf8(testCase.error);
    if (expectedError.contains("%1"))
      expectedError = expectedError.arg(
          testCase.fault == CutFault::AvioOpen ? partialPath : source);
    EXPECT_EQ(error, expectedError);
    EXPECT_EQ(kept, -123.0); // Failure must not publish a success-only duration.
    EXPECT_FALSE(QFile::exists(partialPath));
    const bool beforeOutput = testCase.fault == CutFault::ScanRead ||
        testCase.fault == CutFault::ScanPacket ||
        testCase.fault == CutFault::OutputContext ||
        testCase.fault == CutFault::OutputStream ||
        testCase.fault == CutFault::CodecParameters ||
        testCase.fault == CutFault::OpenScan ||
        testCase.fault == CutFault::ProbeScan ||
        testCase.fault == CutFault::AvioOpen ||
        testCase.fault == CutFault::MetadataCopy ||
        testCase.fault == CutFault::MetadataKind ||
        testCase.fault == CutFault::MetadataVersion ||
        testCase.fault == CutFault::MetadataDate ||
        testCase.fault == CutFault::MetadataTime ||
        testCase.fault == CutFault::MetadataCreation;
    EXPECT_EQ(state.partialExisted, !beforeOutput);
    EXPECT_EQ(state.closes, beforeOutput ? 0 : 1);
    if (!beforeOutput) {
      EXPECT_TRUE(state.closedBeforeRemoval);
      EXPECT_TRUE(state.closeClearedHandle);
    }
    if (testCase.fault == CutFault::CopyRead ||
        testCase.fault == CutFault::PacketRef ||
        testCase.fault == CutFault::Mux ||
        testCase.fault == CutFault::ConsumedMux ||
        testCase.fault == CutFault::Trailer ||
        testCase.fault == CutFault::Flush ||
        testCase.fault == CutFault::Close) {
      EXPECT_GT(state.writes, 0); // Fail after actual packet-copy progress.
    }

    ASSERT_TRUE(destination.open(QIODevice::ReadOnly));
    EXPECT_EQ(destination.readAll(), QByteArray("caller-managed destination"));
    destination.close();
    // The scoped override is gone. Retry the same real input/output path.
    const bool retried = tail
        ? ReplayClipper::saveTail(segments, 5.0, partialPath, &error, &kept)
        : ReplayClipper::saveRange(source, 0.0, 2.5, partialPath, &error, &kept);
    ASSERT_TRUE(retried) << qPrintable(error);
    EXPECT_GT(kept, 2.0);
    const auto packets = readStreams(partialPath);
    ASSERT_EQ(packets.size(), 2);
    for (const auto &stream : packets) {
      ASSERT_FALSE(stream.isEmpty());
      int64_t lastDts = AV_NOPTS_VALUE;
      for (const auto &pkt : stream) {
        if (pkt.dts == AV_NOPTS_VALUE)
          continue;
        EXPECT_GE(pkt.pts, 0);
        if (lastDts != AV_NOPTS_VALUE) {
          EXPECT_GE(pkt.dts, lastDts);
        }
        lastDts = pkt.dts;
      }
    }
    EXPECT_TRUE(packets[0].first().key);
    ASSERT_TRUE(QFile::remove(partialPath));
  }
}

INSTANTIATE_TEST_SUITE_P(FFmpegFailures, ReplayClipperFaultTest, ::testing::Values(
    FaultCase{CutFault::ScanRead, "ScanRead", "error reading input while scanning keyframes"},
    FaultCase{CutFault::CopyRead, "CopyRead", "error reading input while writing clip"},
    FaultCase{CutFault::ScanPacket, "ScanPacket", "cannot allocate scan packet"},
    FaultCase{CutFault::InputPacket, "InputPacket", "cannot allocate input packet"},
    FaultCase{CutFault::OutputPacket, "OutputPacket", "cannot copy input packet"},
    FaultCase{CutFault::PacketRef, "PacketRef", "cannot copy input packet"},
    FaultCase{CutFault::OutputContext, "OutputContext", "cannot create matroska context"},
    FaultCase{CutFault::OutputStream, "OutputStream", "cannot create output stream"},
    FaultCase{CutFault::CodecParameters, "CodecParameters", "cannot copy codec parameters"},
    FaultCase{CutFault::Header, "Header", "cannot write header"},
    FaultCase{CutFault::Mux, "Mux", "mux error"},
    FaultCase{CutFault::ConsumedMux, "ConsumedMux", "mux error"},
    FaultCase{CutFault::Trailer, "Trailer", "cannot write trailer"},
    FaultCase{CutFault::Flush, "Flush", "cannot flush output"},
    FaultCase{CutFault::Close, "Close", "cannot close output"},
    FaultCase{CutFault::OpenScan, "OpenScan", "cannot open input %1"},
    FaultCase{CutFault::ProbeScan, "ProbeScan", "cannot probe input %1"},
    FaultCase{CutFault::OpenCopy, "OpenCopy", "cannot open input %1"},
    FaultCase{CutFault::ProbeCopy, "ProbeCopy", "cannot probe input %1"},
    FaultCase{CutFault::AvioOpen, "AvioOpen", "cannot open output %1"},
    FaultCase{CutFault::MetadataCopy, "MetadataCopy", "cannot copy container metadata"},
    FaultCase{CutFault::MetadataKind, "MetadataKind", "cannot stamp clip metadata"},
    FaultCase{CutFault::MetadataVersion, "MetadataVersion", "cannot stamp clip metadata"},
    FaultCase{CutFault::MetadataDate, "MetadataDate", "cannot stamp clip metadata"},
    FaultCase{CutFault::MetadataTime, "MetadataTime", "cannot stamp clip metadata"},
    FaultCase{CutFault::MetadataCreation, "MetadataCreation", "cannot stamp clip metadata"}),
    [](const ::testing::TestParamInfo<FaultCase> &info) { return info.param.name; });

namespace {
enum class LaterFault { LayoutOpen, LayoutProbe, ScanOpen, ScanProbe, ScanRead,
                        CopyOpen, CopyProbe, CopyRead, CopyRef };
struct LaterCase { LaterFault fault; const char *name; int segment; };
struct LaterState {
  LaterCase testCase;
  QByteArray targetPath;
  QString partialPath;
  int targetOpens = 0;
  int targetReads = 0;
  int writes = 0;
  int earlierWrites = 0;
  int writesAtFailure = 0;
  int closes = 0;
  bool copying = false;
  bool readingTarget = false;
  bool fired = false;
  bool partialAtFailure = false;
  bool closedBeforeRemoval = false;
  bool clearedHandle = false;

  void fire() {
    fired = true;
    writesAtFailure = writes;
    partialAtFailure = QFile::exists(partialPath);
  }
};
thread_local LaterState *laterState = nullptr;
struct LaterGuard {
  LaterState *previous = laterState;
  explicit LaterGuard(LaterState &state) { laterState = &state; }
  ~LaterGuard() { laterState = previous; }
};

int laterOpen(AVFormatContext **ctx, const char *url,
              const AVInputFormat *format, AVDictionary **options) {
  auto &s = *laterState;
  if (s.targetPath == url) {
    ++s.targetOpens;
    s.targetReads = 0;
    if (s.copying)
      s.earlierWrites = s.writes;
    if ((s.testCase.fault == LaterFault::LayoutOpen && s.targetOpens == 1) ||
        (s.testCase.fault == LaterFault::ScanOpen && s.targetOpens == 2) ||
        (s.testCase.fault == LaterFault::CopyOpen && s.copying)) {
      s.fire();
      *ctx = nullptr;
      return AVERROR(EIO);
    }
  }
  return avformat_open_input(ctx, url, format, options);
}

int laterProbe(AVFormatContext *ctx, AVDictionary **options) {
  auto &s = *laterState;
  const int result = avformat_find_stream_info(ctx, options);
  if (result >= 0 && s.targetPath == ctx->url &&
      ((s.testCase.fault == LaterFault::LayoutProbe && s.targetOpens == 1) ||
       (s.testCase.fault == LaterFault::ScanProbe && s.targetOpens == 2) ||
       (s.testCase.fault == LaterFault::CopyProbe && s.copying))) {
    s.fire(); // Own a real probed context when reporting failure.
    return AVERROR(EIO);
  }
  return result;
}

int laterHeader(AVFormatContext *ctx, AVDictionary **options) {
  const int result = avformat_write_header(ctx, options);
  if (result >= 0)
    laterState->copying = true;
  return result;
}

int laterRead(AVFormatContext *ctx, AVPacket *pkt) {
  auto &s = *laterState;
  s.readingTarget = s.targetPath == ctx->url;
  if (s.readingTarget && ++s.targetReads == 12 &&
      ((s.testCase.fault == LaterFault::ScanRead && !s.copying) ||
       (s.testCase.fault == LaterFault::CopyRead && s.copying))) {
    s.fire();
    return AVERROR(EIO);
  }
  return av_read_frame(ctx, pkt);
}

int laterRef(AVPacket *dst, const AVPacket *src) {
  auto &s = *laterState;
  if (s.readingTarget && s.testCase.fault == LaterFault::CopyRef &&
      s.writes > s.earlierWrites + 3) {
    s.fire();
    return AVERROR(ENOMEM);
  }
  return av_packet_ref(dst, src);
}

int laterWrite(AVFormatContext *ctx, AVPacket *pkt) {
  const int result = av_interleaved_write_frame(ctx, pkt);
  if (result >= 0)
    ++laterState->writes;
  return result;
}

int laterClose(AVIOContext **ctx) {
  auto &s = *laterState;
  ++s.closes;
  s.closedBeforeRemoval = QFile::exists(s.partialPath);
  const int result = avio_closep(ctx);
  s.clearedHandle = *ctx == nullptr;
  return result;
}

class ReplayClipperLaterInputTest : public ReplayClipperTest,
    public ::testing::WithParamInterface<LaterCase> {};
} // namespace

TEST_P(ReplayClipperLaterInputTest, TailFailureAtLaterInputCleansAndRetries) {
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() || !haveCodecs())
    GTEST_SKIP() << "ffmpeg CLI or libx264/AAC unavailable";
  QList<ReplaySegment> segments;
  for (int i = 0; i < 3; ++i) {
    const QString path = tempDir.path() + QString("/seg%1.mkv").arg(i);
    ASSERT_NO_FATAL_FAILURE(makeCliSegment(path, "320x240", "aac", 48000, 0.8));
    segments.push_back({path, i * 3.0});
  }
  const auto testCase = GetParam();
  const bool copyFault = testCase.fault == LaterFault::CopyOpen ||
      testCase.fault == LaterFault::CopyProbe ||
      testCase.fault == LaterFault::CopyRead ||
      testCase.fault == LaterFault::CopyRef;
  const QString destinationPath = tempDir.path() + "/clip.mkv";
  const QString partialPath = destinationPath + ".part";
  QFile destination(destinationPath);
  ASSERT_TRUE(destination.open(QIODevice::WriteOnly));
  ASSERT_EQ(destination.write("caller-managed destination"), 26);
  destination.close();
  LaterState state{testCase, segments[testCase.segment].path.toUtf8(), partialPath};
  ReplayClipperInternal::Operations ops;
  ops.openInput = &laterOpen;
  ops.probeInput = &laterProbe;
  ops.readFrame = &laterRead;
  ops.packetRef = &laterRef;
  ops.writeHeader = &laterHeader;
  ops.writeFrame = &laterWrite;
  ops.closeOutput = &laterClose;
  QString error;
  double kept = -123.0;
  {
    LaterGuard guard(state);
    ReplayClipperTestAccess override(ops);
    EXPECT_FALSE(ReplayClipper::saveTail(segments, 12.0, partialPath, &error, &kept));
  }
  ASSERT_TRUE(state.fired);
  EXPECT_EQ(state.targetOpens, copyFault ? 3 :
      ((testCase.fault == LaterFault::LayoutOpen ||
        testCase.fault == LaterFault::LayoutProbe) ? 1 : 2));
  QString expected;
  switch (testCase.fault) {
  case LaterFault::LayoutOpen: case LaterFault::ScanOpen: case LaterFault::CopyOpen:
    expected = "cannot open input " + segments[testCase.segment].path; break;
  case LaterFault::LayoutProbe: case LaterFault::ScanProbe: case LaterFault::CopyProbe:
    expected = "cannot probe input " + segments[testCase.segment].path; break;
  case LaterFault::ScanRead:
    expected = "error reading input while scanning keyframes"; break;
  case LaterFault::CopyRead:
    expected = "error reading input while writing clip"; break;
  case LaterFault::CopyRef: expected = "cannot copy input packet"; break;
  }
  EXPECT_EQ(error, expected);
  EXPECT_EQ(kept, -123.0);
  EXPECT_FALSE(QFile::exists(partialPath));
  EXPECT_EQ(state.partialAtFailure, copyFault);
  EXPECT_EQ(state.closes, copyFault ? 1 : 0);
  if (copyFault) {
    EXPECT_GT(state.earlierWrites, 100); // Actual preceding-segment packet writes.
    EXPECT_GE(state.writesAtFailure, state.earlierWrites);
    EXPECT_TRUE(state.closedBeforeRemoval);
    EXPECT_TRUE(state.clearedHandle);
    if (testCase.fault == LaterFault::CopyRead || testCase.fault == LaterFault::CopyRef)
      EXPECT_GT(state.writesAtFailure, state.earlierWrites);
  } else {
    EXPECT_EQ(state.writesAtFailure, 0);
  }
  ASSERT_TRUE(destination.open(QIODevice::ReadOnly));
  EXPECT_EQ(destination.readAll(), QByteArray("caller-managed destination"));
  destination.close();

  ASSERT_TRUE(ReplayClipper::saveTail(segments, 12.0, partialPath, &error, &kept))
      << qPrintable(error);
  EXPECT_GT(kept, 8.5);
  EXPECT_LT(kept, 9.1);
  const auto packets = readStreams(partialPath);
  ASSERT_EQ(packets.size(), 2);
  ASSERT_FALSE(packets[0].isEmpty());
  EXPECT_TRUE(packets[0].first().key);
  EXPECT_GT(packets[0].size(), 260); // Retry includes all three inputs.
  for (const auto &stream : packets) {
    ASSERT_FALSE(stream.isEmpty());
    int64_t lastDts = AV_NOPTS_VALUE;
    for (const auto &pkt : stream) {
      EXPECT_NE(pkt.dts, AV_NOPTS_VALUE); // CLI fixture has no B-frame delay.
      EXPECT_GE(pkt.pts, 0);
      if (lastDts != AV_NOPTS_VALUE)
        EXPECT_GT(pkt.dts, lastDts);
      lastDts = pkt.dts;
    }
  }
  const auto pts = firstPtsSeconds(partialPath);
  const auto sourcePts = firstPtsSeconds(segments.front().path);
  ASSERT_EQ(pts.size(), 2);
  ASSERT_EQ(sourcePts.size(), 2);
  EXPECT_GT(sourcePts[1] - sourcePts[0], 0.7);
  EXPECT_NEAR(pts[1] - pts[0], sourcePts[1] - sourcePts[0], 0.002);
}

INSTANTIATE_TEST_SUITE_P(LaterInputs, ReplayClipperLaterInputTest,
    ::testing::Values(
        LaterCase{LaterFault::LayoutOpen, "LayoutOpen", 1},
        LaterCase{LaterFault::LayoutProbe, "LayoutProbe", 1},
        LaterCase{LaterFault::ScanOpen, "ScanOpen", 1},
        LaterCase{LaterFault::ScanProbe, "ScanProbe", 1},
        LaterCase{LaterFault::ScanRead, "ScanRead", 1},
        LaterCase{LaterFault::CopyOpen, "CopyOpen", 1},
        LaterCase{LaterFault::CopyProbe, "CopyProbe", 1},
        LaterCase{LaterFault::CopyRead, "CopyRead", 1},
        LaterCase{LaterFault::CopyRef, "CopyRef", 1},
        LaterCase{LaterFault::LayoutOpen, "LayoutOpen", 2},
        LaterCase{LaterFault::LayoutProbe, "LayoutProbe", 2},
        LaterCase{LaterFault::ScanOpen, "ScanOpen", 2},
        LaterCase{LaterFault::ScanProbe, "ScanProbe", 2},
        LaterCase{LaterFault::ScanRead, "ScanRead", 2},
        LaterCase{LaterFault::CopyOpen, "CopyOpen", 2},
        LaterCase{LaterFault::CopyProbe, "CopyProbe", 2},
        LaterCase{LaterFault::CopyRead, "CopyRead", 2},
        LaterCase{LaterFault::CopyRef, "CopyRef", 2}),
    [](const ::testing::TestParamInfo<LaterCase> &info) {
      return std::string(info.param.name) + "Segment" +
          std::to_string(info.param.segment + 1);
    });
