/*
 * Trophy - Unit Tests
 * WaveformCache: peak extraction, normalization, disk cache,
 * track counting and lane labels.
 */

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <cstring>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
}

#include "library/WaveformCache.h"

using namespace TrophyBackend;

namespace TrophyBackend {
class WaveformCacheTestPeer {
public:
  using Stats = WaveformCache::DecodeStats;
  static void observe(WaveformCache &cache,
                      std::function<void(const Stats &)> observer) {
    cache.m_decodeObserver = std::move(observer);
  }
  static void overrideRead(WaveformCache &cache,
                          std::function<int(int, qint64)> override) {
    cache.m_readResultOverride = std::move(override);
  }
};
}

class WaveformCacheTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new WaveformCacheTestEnvironment);

class WaveformCacheTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;

  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }

  void TearDown() override {
    // Peak extraction runs on the global thread pool; join
    // before the temp dir (and its files) goes away.
    QThreadPool::globalInstance()->waitForDone(10000);
  }

  // Render with the system ffmpeg. Empty string when ffmpeg is
  // unavailable (callers should SKIP).
  QString render(const QString &name, const QStringList &args) {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
      return QString();
    const QString path = tempDir.path() + "/" + name;
    QProcess proc;
    proc.start("ffmpeg", QStringList({"-y", "-v", "error"}) + args +
                             QStringList({path}));
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0 ||
        !QFile::exists(path))
      return QString();
    return path;
  }

  QString renderTone(const QString &name) {
    return render(name, {"-f", "lavfi", "-i",
                         "sine=frequency=440:duration=1", "-c:a", "aac"});
  }

  QString renderSilence(const QString &name) {
    return render(name, {"-f", "lavfi", "-i",
                         "anullsrc=channel_layout=mono:sample_rate=44100:d=1",
                         "-c:a", "aac"});
  }

  QString renderTwoTracks(const QString &name) {
    return render(name, {"-f", "lavfi", "-i",
                         "sine=frequency=440:duration=1", "-f", "lavfi",
                         "-i", "sine=frequency=880:duration=1", "-map",
                         "0:a", "-map", "1:a", "-c:a", "aac"});
  }

  static double listMax(const QVariantList &peaks) {
    double m = 0.0;
    for (const QVariant &v : peaks)
      m = std::max(m, v.toDouble());
    return m;
  }

  // Independent CLI decodes for continuous-track fixtures. Place PCM at each
  // stream's recorded start and reduce every lane against a shared file span.
  // The discontinuous fixture below separately verifies internal gaps.
  QVariantList reference(const QString &path, int track, qint64 *sampleCount,
                         qint64 *positionedBlocks = nullptr) {
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) < 0)
      return {};
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
      avformat_close_input(&ctx);
      return {};
    }
    QVector<QByteArray> decoded;
    QVector<qint64> starts;
    qint64 timeline = ctx->duration > 0 ? av_rescale_q(ctx->duration,
        AV_TIME_BASE_Q, AVRational{1, 8000}) : 0;
    const auto origin = ctx->start_time == AV_NOPTS_VALUE ? 0 : ctx->start_time;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
      const auto *stream = ctx->streams[i];
      if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
      QProcess proc;
      proc.start("ffmpeg", {"-v", "error", "-i", path, "-map",
                           QString("0:%1").arg(i), "-ac", "1", "-ar",
                           "8000", "-f", "f32le", "pipe:1"});
      if (!proc.waitForFinished(30000) || proc.exitCode() != 0) {
        avformat_close_input(&ctx);
        return {};
      }
      decoded.append(proc.readAllStandardOutput());
      const qint64 offset = stream->start_time == AV_NOPTS_VALUE ? 0 :
          av_rescale_q(av_rescale_q(stream->start_time, stream->time_base,
                                    AV_TIME_BASE_Q) - origin,
                       AV_TIME_BASE_Q, AVRational{1, 8000});
      starts.append(offset);
      timeline = std::max(timeline, offset + qint64(decoded.last().size() / sizeof(float)));
    }
    avformat_close_input(&ctx);
    const auto &raw = decoded[track];
    *sampleCount = raw.size() / sizeof(float);
    QVector<float> blocks;
    const auto end = starts[track] + *sampleCount;
    blocks.resize((end + 255) / 256);
    if (positionedBlocks) *positionedBlocks = blocks.size();
    for (qint64 i = 0; i < *sampleCount; ++i) {
      const auto at = starts[track] + i;
      if (at < 0) continue;
      float value;
      std::memcpy(&value, raw.constData() + i * sizeof(float), sizeof(float));
      blocks[at / 256] = std::max(blocks[at / 256], std::fabs(value));
    }
    const auto totalBlocks = (timeline + 255) / 256;
    QVariantList result;
    double maximum = 0;
    for (int b = 0; b < WaveformCache::kBuckets; ++b) {
      float value = 0;
      for (qsizetype i = qint64(b) * totalBlocks / WaveformCache::kBuckets;
           i < std::min<qint64>(blocks.size(),
               qint64(b + 1) * totalBlocks / WaveformCache::kBuckets); ++i)
        value = std::max(value, blocks[i]);
      result.append(double(value));
      maximum = std::max(maximum, double(value));
    }
    for (QVariant &value : result)
      value = value.toDouble() / std::max(maximum, 0.1);
    return result;
  }

  static qint64 directReadCount(const QString &path) {
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, path.toUtf8().constData(), nullptr, nullptr) < 0)
      return -1;
    AVPacket *packet = av_packet_alloc();
    qint64 reads = 0;
    int result;
    do {
      result = av_read_frame(ctx, packet);
      ++reads;
      av_packet_unref(packet);
    } while (result >= 0);
    av_packet_free(&packet);
    avformat_close_input(&ctx);
    return result == AVERROR_EOF ? reads : -1;
  }
};

TEST_F(WaveformCacheTest, TrackLabels) {
  EXPECT_EQ(WaveformCache::trackLabel(0, 1), "Audio");
  EXPECT_EQ(WaveformCache::trackLabel(0, 3), "Game");
  EXPECT_EQ(WaveformCache::trackLabel(1, 3), "Voice");
  EXPECT_EQ(WaveformCache::trackLabel(2, 3), "Mic");
  EXPECT_EQ(WaveformCache::trackLabel(0, 2), "Game");
  EXPECT_EQ(WaveformCache::trackLabel(1, 2), "Voice");
  EXPECT_EQ(WaveformCache::trackLabel(5, 6), "Track 6");
}

TEST_F(WaveformCacheTest, MissingFileGivesNothing) {
  WaveformCache cache;
  const QString ghost = tempDir.path() + "/ghost.mkv";
  // Missing/unopenable is unknown (-1), not a definitive "no audio" (0).
  EXPECT_EQ(cache.audioTrackCount(ghost), -1);

  // Still emits (with an empty list) so QML never waits forever.
  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(ghost, 0);
  ASSERT_TRUE(spy.wait(5000));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.first().at(0).toString(), ghost);
  EXPECT_TRUE(spy.first().at(2).toList().isEmpty());
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/.thumbnails/ghost.peaks"));
}

TEST_F(WaveformCacheTest, AudioTrackProbeDeliversOperationIdAsynchronously) {
  WaveformCache cache;
  const QString ghost = tempDir.path() + "/ghost-probe.mkv";
  QSignalSpy spy(&cache, &WaveformCache::audioTrackCountReady);
  cache.requestAudioTrackCount(ghost, 17);
  EXPECT_EQ(spy.count(), 0);
  ASSERT_TRUE(spy.wait(5000));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.first()[0].toString(), ghost);
  EXPECT_EQ(spy.first()[1].toInt(), 17);
  EXPECT_EQ(spy.first()[2].toInt(), -1);
}

TEST_F(WaveformCacheTest, DestroyWhileMissingFileJobIsPending) {
  const QString ghost = tempDir.path() + "/missing.mkv";
  auto *cache = new WaveformCache;
  cache->requestPeaks(ghost, 0);
  delete cache;
  ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
  QCoreApplication::processEvents();
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/.thumbnails/missing.peaks"));
}

TEST_F(WaveformCacheTest, OrdinaryTonePeaksRemainLegible) {
  const QString path = renderTone("tone.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  WaveformCache cache;
  ASSERT_EQ(cache.audioTrackCount(path), 1);

  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  ASSERT_TRUE(spy.wait(10000));
  ASSERT_EQ(spy.count(), 1);
  const QVariantList peaks = spy.first().at(2).toList();

  ASSERT_EQ(peaks.size(), WaveformCache::kBuckets);
  for (const QVariant &v : peaks) {
    EXPECT_GE(v.toDouble(), 0.0);
    EXPECT_LE(v.toDouble(), 1.0);
  }
  // The ordinary test sine stays legible with bounded normalization.
  EXPECT_GT(listMax(peaks), 0.5);
}

TEST_F(WaveformCacheTest, SilencePeaksAllZero) {
  const QString path = renderSilence("silence.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  WaveformCache cache;
  ASSERT_EQ(cache.audioTrackCount(path), 1);

  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  ASSERT_TRUE(spy.wait(10000));
  ASSERT_EQ(spy.count(), 1);
  const QVariantList peaks = spy.first().at(2).toList();

  ASSERT_EQ(peaks.size(), WaveformCache::kBuckets);
  EXPECT_EQ(listMax(peaks), 0.0);
}

TEST_F(WaveformCacheTest, QuietTracksCannotNormalizeNoiseToFullHeight) {
  // Float PCM preserves tiny nonzero signals, including the roughly -140 dB
  // residue observed on an otherwise silent recorded microphone track.
  for (const double amplitude : {1e-7, 0.001, 0.01, 0.1, 0.5}) {
    SCOPED_TRACE(amplitude);
    const QString path = render(QString("quiet-%1.mkv").arg(amplitude), {
        "-f", "lavfi", "-i",
        QString("aevalsrc=%1*sin(2*PI*440*t):s=48000:d=1").arg(amplitude),
        "-c:a", "pcm_f32le"});
    if (path.isEmpty())
      GTEST_SKIP() << "ffmpeg unavailable or render failed";
    WaveformCache cache;
    QSignalSpy spy(&cache, &WaveformCache::peaksReady);
    cache.requestPeaks(path, 0);
    ASSERT_TRUE(spy.wait(10000));
    ASSERT_EQ(spy.size(), 1);
    const QVariantList peaks = spy.first()[2].toList();
    ASSERT_EQ(peaks.size(), WaveformCache::kBuckets);
    const double maximum = listMax(peaks);
    EXPECT_NEAR(maximum, std::min(1.0, amplitude * 10),
                std::min(1.0, amplitude * 10) * 0.02);

    // A fresh cache object must retain the corrected scale on disk reuse.
    WaveformCache reopened;
    std::atomic<int> jobs{0};
    WaveformCacheTestPeer::observe(reopened, [&](const auto &) { ++jobs; });
    QSignalSpy cached(&reopened, &WaveformCache::peaksReady);
    reopened.requestPeaks(path, 0);
    ASSERT_TRUE(cached.wait(5000));
    ASSERT_EQ(cached.size(), 1);
    EXPECT_EQ(cached.first()[2].toList(), peaks);
    EXPECT_EQ(jobs.load(), 0);
  }
}

TEST_F(WaveformCacheTest, LegacyNormalizedCacheIsRegenerated) {
  const QString path = render("legacy.mkv", {
      "-f", "lavfi", "-i", "aevalsrc=0.0000001*sin(2*PI*440*t):s=48000:d=1",
      "-c:a", "pcm_f32le"});
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";
  for (quint32 legacyVersion : {1u, 2u}) {
  SCOPED_TRACE(legacyVersion);
  const QString sidecar = tempDir.path() + "/.thumbnails/legacy.peaks";
  ASSERT_TRUE(QDir().mkpath(QFileInfo(sidecar).absolutePath()));
  QFile file(sidecar);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  QDataStream data(&file);
  data.setVersion(QDataStream::Qt_6_0);
  data.setFloatingPointPrecision(QDataStream::SinglePrecision);
  const QFileInfo source(path);
  data << quint32(0x54504b31) << legacyVersion << source.size()
       << source.lastModified().toSecsSinceEpoch() << qint32(1)
       << qint32(WaveformCache::kBuckets);
  for (int b = 0; b < WaveformCache::kBuckets; ++b)
    data << 1.0f;
  file.close();

  WaveformCache cache;
  std::atomic<int> jobs{0};
  WaveformCacheTestPeer::observe(cache, [&](const auto &) { ++jobs; });
  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  ASSERT_TRUE(spy.wait(10000));
  ASSERT_EQ(spy.size(), 1);
  EXPECT_LT(listMax(spy.first()[2].toList()), 0.000002);
  EXPECT_EQ(jobs.load(), 1);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  QDataStream updated(&file);
  quint32 magic = 0, version = 0;
  updated >> magic >> version;
  EXPECT_EQ(magic, 0x54504b31u);
  EXPECT_EQ(version, 3u);
  }
}

TEST_F(WaveformCacheTest, DelayedShortTrackAndInternalGapsUseSharedVideoTimeline) {
  const QString path = render("timeline.mkv", {
      "-f", "lavfi", "-i", "color=c=black:s=32x32:r=10:d=8",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=8",
      "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=44100:duration=4",
      "-filter_complex", "[2:a]aselect='lt(t,1)+gte(t,3)',asetpts=PTS+2/TB[late]",
      "-map", "0:v", "-map", "1:a", "-map", "[late]",
      "-c:v", "ffv1", "-c:a", "pcm_f32le"});
  if (path.isEmpty()) GTEST_SKIP() << "ffmpeg unavailable or render failed";
  WaveformCache cache;
  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  cache.requestPeaks(path, 1);
  QElapsedTimer timer;
  timer.start();
  while (spy.size() < 2 && timer.elapsed() < 10000) spy.wait(500);
  ASSERT_EQ(spy.size(), 2);
  QVariantList primary, late;
  for (const auto &args : spy)
    (args[1].toInt() == 0 ? primary : late) = args[2].toList();
  ASSERT_EQ(late.size(), WaveformCache::kBuckets);
  ASSERT_EQ(primary.size(), late.size());
  const auto maximum = [&](double start, double end) {
    double value = 0;
    for (int i = int(start / 8 * late.size()); i < int(end / 8 * late.size()); ++i)
      value = std::max(value, late[i].toDouble());
    return value;
  };
  EXPECT_EQ(maximum(0, 1.8), 0.0); // Leading silence, not stretched to t=0.
  EXPECT_GT(maximum(2.2, 2.8), 0.5);
  EXPECT_EQ(maximum(3.2, 4.8), 0.0); // Preserve the discontinuity's silence.
  EXPECT_GT(maximum(5.2, 5.8), 0.5);
  EXPECT_EQ(maximum(6.2, 7.8), 0.0); // Ends early, not stretched to file EOF.
}

TEST_F(WaveformCacheTest, CacheWrittenAndReused) {
  const QString path = renderTone("cached.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  WaveformCache cache;
  std::atomic<int> jobs{0};
  WaveformCacheTestPeer::observe(cache, [&](const auto &) { ++jobs; });
  {
    QSignalSpy spy(&cache, &WaveformCache::peaksReady);
    cache.requestPeaks(path, 0);
    ASSERT_TRUE(spy.wait(10000));
  }
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/.thumbnails/cached.peaks"));

  // Second request serves the sidecar (same values, no decode).
  QSignalSpy spy2(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  ASSERT_TRUE(spy2.wait(5000));
  ASSERT_EQ(spy2.count(), 1);
  EXPECT_EQ(spy2.first().at(2).toList().size(), WaveformCache::kBuckets);
  EXPECT_GT(listMax(spy2.first().at(2).toList()), 0.5);
  EXPECT_EQ(jobs.load(), 1);
}

TEST_F(WaveformCacheTest, TwoTracksPeaksSeparately) {
  const QString path = renderTwoTracks("stereo2.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  WaveformCache cache;
  ASSERT_EQ(cache.audioTrackCount(path), 2);

  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  cache.requestPeaks(path, 1);
  // Both tracks are owed an emit; they may arrive together.
  QElapsedTimer timer;
  timer.start();
  while (spy.count() < 2 && timer.elapsed() < 10000)
    spy.wait(500);
  ASSERT_EQ(spy.count(), 2);
  for (const auto &args : spy) {
    const QVariantList peaks = args.at(2).toList();
    ASSERT_EQ(peaks.size(), WaveformCache::kBuckets);
    EXPECT_GT(listMax(peaks), 0.5);
  }
}

TEST_F(WaveformCacheTest, NativeSidecarFailureStillServesReentrantRequest) {
  const QString path = renderTone("blocked.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";
  const QString sidecar = tempDir.path() + "/.thumbnails/blocked.peaks";
  ASSERT_TRUE(QDir().mkpath(sidecar)); // native EISDIR on cache write
  WaveformCache cache;
  std::atomic<int> jobs{0};
  WaveformCacheTestPeer::observe(cache, [&](const auto &) { ++jobs; });
  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  bool requestedAgain = false;
  QObject::connect(&cache, &WaveformCache::peaksReady, &cache,
                   [&](const QString &file, int track, const QVariantList &) {
    if (!requestedAgain) {
      requestedAgain = true;
      cache.requestPeaks(file, track);
    }
  });
  cache.requestPeaks(path, 0);
  QElapsedTimer timer;
  timer.start();
  while (spy.size() < 2 && timer.elapsed() < 10000)
    spy.wait(500);
  ASSERT_EQ(spy.size(), 2);
  for (const auto &args : spy) {
    ASSERT_EQ(args[2].toList().size(), WaveformCache::kBuckets);
    EXPECT_GT(listMax(args[2].toList()), 0.5);
  }
  ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
  QCoreApplication::processEvents();
  EXPECT_EQ(spy.size(), 2);
  EXPECT_EQ(jobs.load(), 1) << "reentrant request must reuse settled job memory";
  ASSERT_TRUE(QDir().rmdir(sidecar));
  spy.clear();
  cache.requestPeaks(path, 0);
  ASSERT_TRUE(spy.wait(10000));
  ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
  EXPECT_TRUE(QFileInfo(sidecar).isFile());
  EXPECT_EQ(jobs.load(), 2) << "failed sidecar is retried on a later job";
}

TEST_F(WaveformCacheTest, BoundedMalformedPeakCacheCorpusRegeneratesFinitePeaks) {
  const QString path = renderTone("malformed-cache.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";
  const QString sidecar = tempDir.path() + "/.thumbnails/malformed-cache.peaks";
  ASSERT_TRUE(QDir().mkpath(QFileInfo(sidecar).absolutePath()));
  const QFileInfo source(path);
  const QList<float> invalid = {std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -0.5f, 2.0f};
  WaveformCache cache;
  for (int variant = 0; variant < 10; ++variant) {
    SCOPED_TRACE(variant);
    QByteArray bytes;
    QDataStream data(&bytes, QIODevice::WriteOnly);
    data.setVersion(QDataStream::Qt_6_0);
    data.setFloatingPointPrecision(QDataStream::SinglePrecision);
    const qint32 count = variant == 0 ? 0 : variant == 1 ? -1 :
                         variant == 2 ? 33 : 1;
    data << quint32(0x54504b31) << quint32(3) << source.size()
         << source.lastModified().toSecsSinceEpoch() << count
         << qint32(WaveformCache::kBuckets);
    for (int b = 0; b < WaveformCache::kBuckets; ++b)
      data << ((variant >= 6 && b == 0) ? invalid[variant - 6] : 0.25f);
    if (variant >= 3 && variant <= 5)
      bytes.truncate(variant == 3 ? 1 : variant == 4 ? 31 : bytes.size() - 1);
    QFile file(sidecar);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
    QSignalSpy spy(&cache, &WaveformCache::peaksReady);
    cache.requestPeaks(path, 0);
    ASSERT_TRUE(spy.wait(10000));
    ASSERT_EQ(spy.size(), 1);
    const QVariantList peaks = spy.first()[2].toList();
    ASSERT_EQ(peaks.size(), WaveformCache::kBuckets);
    for (const QVariant &peak : peaks) {
      EXPECT_TRUE(std::isfinite(peak.toDouble()));
      EXPECT_GE(peak.toDouble(), 0.0);
      EXPECT_LE(peak.toDouble(), 1.0);
    }
    EXPECT_GT(listMax(peaks), 0.5) << "bad cache must be decoded, not reused";
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
    QCoreApplication::processEvents();
  }
}

TEST_F(WaveformCacheTest, SinglePassMixedRatesDelayedShortAndSilentStreams) {
  const QString path = render("mixed.mkv", {
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1",
      "-itsoffset", "0.3", "-f", "lavfi", "-i",
      "sine=frequency=880:sample_rate=22050:duration=0.03",
      "-f", "lavfi", "-i", "anullsrc=r=44100:cl=mono:d=0.1",
      "-map", "0:a", "-map", "1:a", "-map", "2:a", "-c:a", "pcm_s16le"});
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";
  WaveformCache cache;
  WaveformCacheTestPeer::Stats stats;
  std::atomic<int> jobs{0};
  WaveformCacheTestPeer::observe(cache, [&](const auto &value) {
    stats = value;
    ++jobs;
  });
  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  // The first emitted lane must observe complete demux/drains, not partial
  // normalization published before later packets for that lane arrive.
  QObject::connect(&cache, &WaveformCache::peaksReady, &cache,
                   [&](const QString &, int, const QVariantList &) {
    EXPECT_EQ(jobs.load(), 1);
    EXPECT_TRUE(stats.cleanEof);
    EXPECT_EQ(stats.decoderDrains, 3);
    EXPECT_EQ(stats.resamplerDrains, 3);
  });
  for (int t = 0; t < 4; ++t)
    cache.requestPeaks(path, t); // includes an invalid track, settled once
  QElapsedTimer timer;
  timer.start();
  while (spy.size() < 4 && timer.elapsed() < 10000)
    spy.wait(500);
  ASSERT_EQ(spy.size(), 4);
  ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
  QCoreApplication::processEvents();
  EXPECT_EQ(spy.size(), 4);
  EXPECT_EQ(stats.passes, 1);
  EXPECT_EQ(stats.tracks, 3);
  EXPECT_EQ(stats.reads, directReadCount(path));
  EXPECT_EQ(stats.packets + 1, stats.reads);
  EXPECT_GT(stats.drainSamples, 0); // real resampler delay was recovered
  EXPECT_LE(stats.maxOutputCapacity, 4096);
  qint64 totalSamples = 0;
  qint64 expectedBlocks = 0;
  QSet<int> seen;
  for (const auto &args : spy) {
    const int track = args[1].toInt();
    EXPECT_FALSE(seen.contains(track));
    seen.insert(track);
    const QVariantList actual = args[2].toList();
    if (track == 3) {
      EXPECT_TRUE(actual.isEmpty());
      continue;
    }
    qint64 samples = 0;
    qint64 positionedBlocks = 0;
    const QVariantList expected = reference(path, track, &samples, &positionedBlocks);
    ASSERT_EQ(actual.size(), WaveformCache::kBuckets);
    ASSERT_EQ(expected.size(), actual.size());
    totalSamples += samples;
    expectedBlocks += positionedBlocks;
    for (int b = 0; b < actual.size(); ++b)
      EXPECT_NEAR(actual[b].toDouble(), expected[b].toDouble(), 0.0001)
          << "track=" << track << " bucket=" << b;
  }
  EXPECT_EQ(stats.samples, totalSamples);
  EXPECT_EQ(stats.blocks, expectedBlocks);
  // History scales with the shared timeline (including leading silence), not
  // full-rate PCM or retained packets. This fixture spans one second.
  EXPECT_LE(stats.blocks, stats.tracks * ((8000 + 255) / 256));
  EXPECT_LE(stats.blockCapacity, stats.blocks * 2 + stats.tracks * 16);
  EXPECT_LE(stats.outputCapacity, qint64(4096) * stats.tracks);
  RecordProperty("demux_reads", int(stats.reads));
  RecordProperty("decoded_mono_samples", int(stats.samples));
  RecordProperty("resampler_tail_samples", int(stats.drainSamples));
  RecordProperty("history_blocks", int(stats.blocks));
  RecordProperty("history_capacity_floats", int(stats.blockCapacity));
  RecordProperty("output_capacity_floats", int(stats.outputCapacity));
  RecordProperty("largest_output_buffer_samples", int(stats.maxOutputCapacity));
}

TEST_F(WaveformCacheTest, EncodedMultiTrackReferenceIncludesFinalResamplerTail) {
  const QString path = render("encoded.mkv", {
      "-f", "lavfi", "-i", "sine=frequency=330:sample_rate=48000:duration=0.4",
      "-itsoffset", "0.1", "-f", "lavfi", "-i",
      "sine=frequency=1100:sample_rate=32000:duration=0.01",
      "-map", "0:a", "-map", "1:a", "-c:a", "aac"});
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";
  WaveformCache cache;
  WaveformCacheTestPeer::Stats stats;
  WaveformCacheTestPeer::observe(cache, [&](const auto &value) { stats = value; });
  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  cache.requestPeaks(path, 1);
  QElapsedTimer timer;
  timer.start();
  while (spy.size() < 2 && timer.elapsed() < 10000)
    spy.wait(500);
  ASSERT_EQ(spy.size(), 2);
  ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
  EXPECT_EQ(stats.passes, 1);
  EXPECT_EQ(stats.reads, directReadCount(path));
  EXPECT_TRUE(stats.cleanEof);
  EXPECT_EQ(stats.decoderDrains, 2);
  EXPECT_EQ(stats.resamplerDrains, 2);
  EXPECT_GT(stats.drainSamples, 0);
  qint64 total = 0;
  for (const auto &args : spy) {
    qint64 samples = 0;
    const QVariantList expected = reference(path, args[1].toInt(), &samples);
    const QVariantList actual = args[2].toList();
    ASSERT_EQ(actual.size(), WaveformCache::kBuckets);
    ASSERT_EQ(expected.size(), actual.size());
    total += samples;
    for (int b = 0; b < actual.size(); ++b)
      EXPECT_NEAR(actual[b].toDouble(), expected[b].toDouble(), 0.0001)
          << "track=" << args[1].toInt() << " bucket=" << b;
  }
  EXPECT_EQ(stats.samples, total);
}

TEST_F(WaveformCacheTest, UpsamplingUsesCheckedLargerOutputBudgetWithoutLosingSamples) {
  const QString path = render("upsampled.mkv", {
      "-f", "lavfi", "-i", "sine=frequency=100:sample_rate=1000:duration=1",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=1",
      "-map", "0:a", "-map", "1:a", "-c:a", "pcm_s16le"});
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";
  WaveformCache cache;
  WaveformCacheTestPeer::Stats stats;
  WaveformCacheTestPeer::observe(cache, [&](const auto &value) { stats = value; });
  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  cache.requestPeaks(path, 1);
  QElapsedTimer timer;
  timer.start();
  while (spy.size() < 2 && timer.elapsed() < 10000)
    spy.wait(500);
  ASSERT_EQ(spy.size(), 2);
  ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
  EXPECT_EQ(stats.passes, 1);
  EXPECT_EQ(stats.reads, directReadCount(path));
  EXPECT_GT(stats.maxOutputCapacity, 4096);
  EXPECT_LE(stats.maxOutputCapacity, 1 << 20);
  qint64 total = 0;
  for (const auto &args : spy) {
    qint64 samples = 0;
    const QVariantList expected = reference(path, args[1].toInt(), &samples);
    const QVariantList actual = args[2].toList();
    ASSERT_EQ(actual.size(), WaveformCache::kBuckets);
    ASSERT_EQ(expected.size(), actual.size());
    total += samples;
    for (int b = 0; b < actual.size(); ++b)
      EXPECT_NEAR(actual[b].toDouble(), expected[b].toDouble(), 0.0001)
          << "track=" << args[1].toInt() << " bucket=" << b;
  }
  EXPECT_EQ(stats.samples, total);
}

TEST_F(WaveformCacheTest, NonEofReadFailureSettlesAllTracksWithoutDrainOrCache) {
  const QString path = renderTwoTracks("read-error.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";
  for (const int error : {AVERROR(EIO), AVERROR_INVALIDDATA}) {
    SCOPED_TRACE(error);
    WaveformCache cache;
    WaveformCacheTestPeer::Stats stats;
    WaveformCacheTestPeer::observe(cache, [&](const auto &value) { stats = value; });
    WaveformCacheTestPeer::overrideRead(cache, [error](int result, qint64 reads) {
      return reads == 5 ? error : result;
    });
    QSignalSpy spy(&cache, &WaveformCache::peaksReady);
    for (int t = 0; t < 3; ++t)
      cache.requestPeaks(path, t);
    QElapsedTimer timer;
    timer.start();
    while (spy.size() < 3 && timer.elapsed() < 10000)
      spy.wait(500);
    ASSERT_EQ(spy.size(), 3);
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
    QCoreApplication::processEvents();
    EXPECT_EQ(spy.size(), 3);
    for (const auto &args : spy)
      EXPECT_TRUE(args[2].toList().isEmpty());
    EXPECT_EQ(stats.passes, 1);
    EXPECT_EQ(stats.reads, 5);
    EXPECT_GT(stats.packets, 0);
    EXPECT_FALSE(stats.cleanEof);
    EXPECT_EQ(stats.decoderDrains, 0);
    EXPECT_EQ(stats.resamplerDrains, 0);
    EXPECT_FALSE(QFile::exists(tempDir.path() + "/.thumbnails/read-error.peaks"));
  }
  WaveformCache retry;
  QSignalSpy spy(&retry, &WaveformCache::peaksReady);
  retry.requestPeaks(path, 0);
  ASSERT_TRUE(spy.wait(10000));
  EXPECT_EQ(spy.first()[2].toList().size(), WaveformCache::kBuckets);
}
