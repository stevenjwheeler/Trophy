#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QThread>
#include <QSignalSpy>
#include <cmath>
#include <cstring>
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include "library/ShareExportEngine.h"
#include "library/ShareExporter.h"
#include "library/RecordingAnnotations.h"
#include "audio/AudioRoutingInfo.h"
#include "core/StorageManager.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
using namespace TrophyBackend;
class ShareEnvironment : public testing::Environment {
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1; static char arg[] = "test"; static char *argv[]{arg, nullptr};
      qputenv("QT_QPA_PLATFORM", "offscreen");
      new QGuiApplication(argc, argv);
    }
  }
};
static auto *environment = testing::AddGlobalTestEnvironment(new ShareEnvironment);

class OriginalCopyTest : public testing::Test {
protected:
  QTemporaryDir library, destination;
  QString source;
  QByteArray bytes = QByteArray(2 * 1024 * 1024, 'x') + QByteArray("\0#%original", 11);
  void SetUp() override {
    ASSERT_TRUE(library.isValid()); ASSERT_TRUE(destination.isValid());
    source = library.filePath(QString::fromUtf8("Original #1 % café.mkv"));
    QFile file(source);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
  }
  bool wait(ShareExporter &service) {
    QElapsedTimer timer; timer.start();
    while ((service.busy() || service.inspecting()) && timer.elapsed() < 10000) {
      QCoreApplication::processEvents(); QThread::msleep(1);
    }
    return !service.busy() && !service.inspecting();
  }
  QByteArray read(const QString &path) {
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
  }
  QStringList partials() {
    return QDir(destination.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden);
  }
};
TEST_F(OriginalCopyTest, CopiesExactBytesSequentiallyWithoutOverwritingAndReleasesPins) {
  StorageManager storage(library.path());
  ShareExporter service(&storage);
  QSignalSpy settled(&service, &ShareExporter::jobSettled);
  ASSERT_TRUE(service.copyOriginals({source, source}, destination.path()));
  ASSERT_EQ(service.jobs().size(), 2);
  EXPECT_TRUE(service.jobs()[0].toMap()["copyOriginal"].toBool());
  EXPECT_FALSE(service.jobs()[1].toMap()["active"].toBool());
  EXPECT_FALSE(storage.reserveRecordings({source}));
  ASSERT_TRUE(wait(service));
  ASSERT_EQ(settled.count(), 2);
  const auto first = settled[0][2].toString(), second = settled[1][2].toString();
  ASSERT_FALSE(first.isEmpty()); ASSERT_FALSE(second.isEmpty());
  EXPECT_NE(first, second);
  EXPECT_EQ(read(first), bytes); EXPECT_EQ(read(second), bytes); EXPECT_EQ(read(source), bytes);
  EXPECT_TRUE(settled[0][5].toBool()); EXPECT_TRUE(settled[1][5].toBool());
  EXPECT_TRUE(partials().isEmpty());
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
}
TEST_F(OriginalCopyTest, MissingBatchInputRollsBackAndCancellingKeepsOtherJobsReserved) {
  StorageManager storage(library.path());
  ShareExporter service(&storage);
  EXPECT_FALSE(service.copyOriginals({source, library.filePath("missing.mkv")}, destination.path()));
  EXPECT_TRUE(service.jobs().isEmpty());
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
  EXPECT_FALSE(service.copyOriginals({source}, library.path()));
  ASSERT_TRUE(service.copyOriginals({source, source}, destination.path()));
  const auto jobs = service.jobs();
  service.cancelJob(jobs[1].toMap()["id"].toString());
  EXPECT_EQ(service.jobs().size(), 1);
  service.cancelJob(jobs[0].toMap()["id"].toString());
  EXPECT_FALSE(storage.reserveRecordings({source})); // Active worker must settle first.
  ASSERT_TRUE(wait(service));
  EXPECT_TRUE(partials().isEmpty());
  EXPECT_EQ(read(source), bytes);
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
}
TEST_F(OriginalCopyTest, CopyFailureReportsItsIdentityAndQueueContinues) {
  StorageManager storage(library.path());
  ShareExporter service(&storage);
  QSignalSpy settled(&service, &ShareExporter::jobSettled);
  const auto removed = destination.filePath("removed");
  ASSERT_TRUE(QDir().mkdir(removed));
  ASSERT_TRUE(service.copyOriginals({source}, destination.path()));
  ASSERT_TRUE(service.copyOriginals({source}, removed));
  ASSERT_TRUE(service.copyOriginals({source}, destination.path()));
  ASSERT_TRUE(QDir().rmdir(removed)); // Queued copy has not opened its destination.
  ASSERT_TRUE(wait(service));
  ASSERT_EQ(settled.count(), 3);
  EXPECT_FALSE(settled[0][2].toString().isEmpty());
  EXPECT_TRUE(settled[1][2].toString().isEmpty());
  EXPECT_FALSE(settled[1][3].toString().isEmpty());
  EXPECT_TRUE(settled[1][5].toBool());
  EXPECT_FALSE(settled[2][2].toString().isEmpty());
  EXPECT_TRUE(partials().isEmpty());
}
TEST_F(OriginalCopyTest, DestructionJoinsCopyBeforeReleasingItsSource) {
  StorageManager storage(library.path());
  {
    ShareExporter service(&storage);
    ASSERT_TRUE(service.copyOriginals({source, source}, destination.path()));
    EXPECT_FALSE(storage.reserveRecordings({source}));
  }
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
  EXPECT_TRUE(partials().isEmpty());
  EXPECT_EQ(read(source), bytes);
}
TEST_F(OriginalCopyTest, RealWriteLimitFailurePreservesDestinationAndRemovesPartial) {
  const auto existing = destination.filePath(QFileInfo(source).fileName());
  QFile file(existing); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  ASSERT_EQ(file.write("keep"), 4); file.close();
  const auto pid = fork(); ASSERT_GE(pid, 0);
  if (pid == 0) {
    struct rlimit limit {1024, 1024};
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(77);
    signal(SIGXFSZ, SIG_IGN);
    StorageManager storage(library.path());
    ShareExporter service(&storage);
    QSignalSpy settled(&service, &ShareExporter::jobSettled);
    const bool queued = service.copyOriginals({source}, destination.path());
    const bool done = wait(service);
    _exit(queued && done && settled.count() == 1 && settled[0][2].toString().isEmpty() &&
        !settled[0][3].toString().isEmpty() && partials().isEmpty() ? 0 : 1);
  }
  int status = 0; ASSERT_EQ(waitpid(pid, &status, 0), pid); ASSERT_TRUE(WIFEXITED(status));
  if (WEXITSTATUS(status) == 77) GTEST_SKIP() << "Cannot set child write limit";
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(read(existing), "keep"); EXPECT_EQ(read(source), bytes);
  EXPECT_TRUE(partials().isEmpty());
}

class ShareExportTest : public testing::Test {
protected:
  QTemporaryDir directory;
  std::atomic_bool cancel{false};
  std::atomic<double> progress{0};
  QString source;
  void SetUp() override {
    ASSERT_TRUE(directory.isValid());
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty() ||
        !avcodec_find_encoder_by_name("libx264") || !avcodec_find_encoder_by_name("aac"))
      GTEST_SKIP() << "Optional ffmpeg fixture tool / software codecs unavailable";
    source = directory.filePath("Game #1 %.mkv");
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=30:duration=2",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=2",
        "-itsoffset", "0.4", "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=1.6",
        "-map", "0:v", "-map", "1:a", "-map", "2:a", "-c:v", "mpeg4", "-g", "12", "-c:a", "pcm_s16le",
        "-metadata:s:a:0", "title=Game", "-metadata:s:a:1", "title=Voice", source});
    ASSERT_TRUE(ffmpeg.waitForFinished(30000));
    ASSERT_EQ(ffmpeg.exitCode(), 0) << ffmpeg.readAllStandardError().toStdString();
  }
  ShareExportResult run(const QList<int> &tracks) {
    return ShareExportEngine::run(source, directory.path(), {tracks}, cancel, progress);
  }
  std::vector<float> audio(const QString &path) {
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-v", "error", "-i", path, "-map", "0:a:0", "-f", "f32le", "-ac", "2", "-ar", "48000", "-"});
    EXPECT_TRUE(ffmpeg.waitForFinished(30000));
    EXPECT_EQ(ffmpeg.exitCode(), 0) << ffmpeg.readAllStandardError().toStdString();
    const auto bytes = ffmpeg.readAllStandardOutput();
    std::vector<float> result(size_t(bytes.size()) / sizeof(float));
    std::memcpy(result.data(), bytes.constData(), result.size() * sizeof(float));
    return result;
  }
  // Payload-only digest of the video packets: a re-encode would change it.
  QPair<qint64, quint64> videoDigest(const QString &path) {
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, QFile::encodeName(path).constData(), nullptr, nullptr) < 0)
      return {-1, 0};
    avformat_find_stream_info(ctx, nullptr);
    const int v = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVPacket *p = av_packet_alloc();
    qint64 bytes = 0;
    quint64 hash = 1469598103934665603ULL;
    while (av_read_frame(ctx, p) >= 0) {
      if (p->stream_index == v) {
        bytes += p->size;
        for (int i = 0; i < p->size; ++i) {
          hash ^= p->data[i];
          hash *= 1099511628211ULL;
        }
      }
      av_packet_unref(p);
    }
    av_packet_free(&p);
    avformat_close_input(&ctx);
    return {bytes, hash};
  }
  // Payload-only digest of one stream (0-based per media type), so a copy and
  // a re-encode cannot be confused.
  QPair<qint64, quint64> digest(const QString &path, AVMediaType type, int ordinal) {
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, QFile::encodeName(path).constData(), nullptr, nullptr) < 0)
      return {-1, 0};
    avformat_find_stream_info(ctx, nullptr);
    int streamIndex = -1, seen = 0;
    for (unsigned i = 0; i < ctx->nb_streams; ++i)
      if (ctx->streams[i]->codecpar->codec_type == type && seen++ == ordinal)
        streamIndex = int(i);
    AVPacket *p = av_packet_alloc();
    qint64 bytes = 0;
    quint64 hash = 1469598103934665603ULL;
    while (av_read_frame(ctx, p) >= 0) {
      if (p->stream_index == streamIndex) {
        bytes += p->size;
        for (int i = 0; i < p->size; ++i) {
          hash ^= p->data[i];
          hash *= 1099511628211ULL;
        }
      }
      av_packet_unref(p);
    }
    av_packet_free(&p);
    avformat_close_input(&ctx);
    return {bytes, hash};
  }
  // Quietest 10 ms window in [start, start+span]: a gap at a segment join
  // shows up as a near-silent window.
  double minWindowRms(const std::vector<float> &samples, double start, double span) {
    const int window = 480;
    const int begin = int(start * 48000), end = int((start + span) * 48000);
    double quietest = 1.0;
    for (int at = begin; at + window <= end && (at + window) * 2 <= int(samples.size()); at += window / 2) {
      double sum = 0;
      for (int i = 0; i < window; ++i) {
        const double value = samples[size_t(at + i) * 2];
        sum += value * value;
      }
      quietest = std::min(quietest, std::sqrt(sum / window));
    }
    return quietest;
  }
  double tone(const std::vector<float> &samples, double frequency, double start = 0.6, double span = 0.4) {
    double sine = 0, cosine = 0;
    const int begin = int(start * 48000), count = int(span * 48000);
    if (samples.size() < size_t(begin + count) * 2) return 0;
    for (int i = 0; i < count; ++i) {
      const double phase = 2 * 3.141592653589793 * frequency * i / 48000;
      sine += samples[size_t(begin + i) * 2] * std::sin(phase);
      cosine += samples[size_t(begin + i) * 2] * std::cos(phase);
    }
    return 2 * std::hypot(sine, cosine) / count;
  }
  bool wait(ShareExporter &service) {
    QElapsedTimer timer; timer.start();
    while ((service.busy() || service.inspecting()) && timer.elapsed() < 30000) {
      QCoreApplication::processEvents(); QThread::msleep(1);
    }
    return !service.busy() && !service.inspecting();
  }
};
TEST_F(ShareExportTest, FolderUrlsPreserveSpecialCharactersAndRejectNonLocalUrls) {
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  const auto path = directory.filePath(QString::fromUtf8("Share #1 % café"));
  EXPECT_EQ(service.localFolderPath(QUrl::fromLocalFile(path)), path);
  EXPECT_TRUE(service.localFolderPath(QUrl("https://example.com/folder")).isEmpty());
  EXPECT_TRUE(service.localFolderPath(QUrl()).isEmpty());
}
TEST_F(ShareExportTest, LegacyThreeTrackNamesAreExplicitlyEstimatedAndRoutingOverridesThem) {
  const auto legacy = directory.filePath("legacy.mkv");
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-i", source, "-map", "0:v", "-map", "0:a:0", "-map", "0:a:1", "-map", "0:a:0",
      "-c", "copy", "-metadata:s:a:0", "title=", "-metadata:s:a:1", "title=", "-metadata:s:a:2", "title=", legacy});
  ASSERT_TRUE(ffmpeg.waitForFinished(30000));
  ASSERT_EQ(ffmpeg.exitCode(), 0) << ffmpeg.readAllStandardError().toStdString();
  auto info = ShareExportEngine::inspect(legacy, cancel);
  auto tracks = info.value("tracks").toList();
  ASSERT_EQ(tracks.size(), 3);
  EXPECT_EQ(tracks[0].toMap()["label"].toString(), "Game / System");
  EXPECT_EQ(tracks[1].toMap()["label"].toString(), "Voice");
  EXPECT_EQ(tracks[2].toMap()["label"].toString(), "Microphone");
  for (const auto &track : tracks) {
    EXPECT_TRUE(track.toMap()["inferred"].toBool());
    EXPECT_FALSE(track.toMap()["defaultExcluded"].toBool());
  }
  AudioRoutingInfo routing;
  routing.known = routing.system = routing.separate = true;
  routing.primary = 0; routing.voiceTrack = 1; routing.micTrack = 2;
  ASSERT_TRUE(RecordingAnnotations::setAudioRouting(legacy, routing.toJson()));
  tracks = ShareExportEngine::inspect(legacy, cancel).value("tracks").toList();
  ASSERT_EQ(tracks.size(), 3);
  EXPECT_EQ(tracks[0].toMap()["label"].toString(), "System");
  EXPECT_FALSE(tracks[0].toMap()["inferred"].toBool());
  EXPECT_TRUE(tracks[1].toMap()["defaultExcluded"].toBool());
}
TEST_F(ShareExportTest, InspectAndMixProducesFastStartCompatibleMp4) {
  const auto info = ShareExportEngine::inspect(source, cancel);
  ASSERT_FALSE(info.contains("error")) << info.value("error").toString().toStdString();
  ASSERT_EQ(info.value("tracks").toList().size(), 2);
  EXPECT_EQ(info.value("tracks").toList()[0].toMap()["label"].toString(), "Game");
  const auto output = run({1, 2});
  ASSERT_TRUE(output.error.isEmpty()) << output.error.toStdString();
  AVFormatContext *ctx = nullptr;
  ASSERT_GE(avformat_open_input(&ctx, QFile::encodeName(output.path).constData(), nullptr, nullptr), 0);
  ASSERT_GE(avformat_find_stream_info(ctx, nullptr), 0);
  ASSERT_EQ(ctx->nb_streams, 2u);
  // Stream copy: identical codec/geometry, no frame-rate retiming.
  EXPECT_EQ(ctx->streams[0]->codecpar->codec_id, AV_CODEC_ID_MPEG4);
  EXPECT_EQ(ctx->streams[0]->codecpar->width, 320);
  EXPECT_EQ(ctx->streams[0]->codecpar->height, 180);
  EXPECT_EQ(ctx->streams[0]->nb_frames, 60); // 2s at 30fps, copied 1:1.
  // The mix is MP3-in-MP4 when libmp3lame is available (it encodes ~3x
  // faster than native AAC), otherwise AAC.
  EXPECT_EQ(ctx->streams[1]->codecpar->codec_id,
            avcodec_find_encoder_by_name("libmp3lame") ? AV_CODEC_ID_MP3 : AV_CODEC_ID_AAC);
  EXPECT_EQ(ctx->streams[1]->codecpar->ch_layout.nb_channels, 2);
  avformat_close_input(&ctx);
  QFile file(output.path); ASSERT_TRUE(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll();
  EXPECT_GT(bytes.indexOf("mdat"), bytes.indexOf("moov"));
  const auto pcm = audio(output.path);
  EXPECT_GT(tone(pcm, 440), 0.02); EXPECT_GT(tone(pcm, 880), 0.02);
  EXPECT_DOUBLE_EQ(progress.load(), 1.0);
  EXPECT_TRUE(QFile::exists(source));
}
TEST_F(ShareExportTest, FriendlyCodecLabelsAndShareability) {
  const auto info = ShareExportEngine::inspect(source, cancel);
  EXPECT_EQ(info.value("videoCodec").toString(), "mpeg4");
  EXPECT_EQ(info.value("videoCodecLabel").toString(), "MPEG-4 Part 2");
  EXPECT_FALSE(info.value("shareable").toBool());
  EXPECT_TRUE(info.value("copyable").toBool());

  const QString h264 = directory.filePath("GameH264.mkv");
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
      "testsrc2=size=320x180:rate=30:duration=0.5", "-c:v", "libx264",
      "-pix_fmt", "yuv420p", h264});
  ASSERT_TRUE(ffmpeg.waitForFinished(30000));
  ASSERT_EQ(ffmpeg.exitCode(), 0) << ffmpeg.readAllStandardError().toStdString();
  const auto h = ShareExportEngine::inspect(h264, cancel);
  EXPECT_EQ(h.value("videoCodecLabel").toString(), "H.264");
  EXPECT_TRUE(h.value("shareable").toBool());
  EXPECT_TRUE(h.value("copyable").toBool());
}

TEST_F(ShareExportTest, ConvertModeProducesH264AndAac) {
  ShareExportOptions options;
  options.audioStreams = {1, 2};
  options.mode = ShareExportOptions::Mode::Convert;
  options.maxHeight = 0;
  const auto result = ShareExportEngine::run(source, directory.path(), options, cancel, progress);
  ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();

  AVFormatContext *ctx = nullptr;
  ASSERT_GE(avformat_open_input(&ctx, QFile::encodeName(result.path).constData(), nullptr, nullptr), 0);
  ASSERT_GE(avformat_find_stream_info(ctx, nullptr), 0);
  ASSERT_EQ(ctx->nb_streams, 2u);
  EXPECT_EQ(ctx->streams[0]->codecpar->codec_id, AV_CODEC_ID_H264);
  EXPECT_EQ(ctx->streams[0]->codecpar->width, 320);
  EXPECT_EQ(ctx->streams[0]->codecpar->height, 180);
  EXPECT_EQ(ctx->streams[1]->codecpar->codec_id, AV_CODEC_ID_AAC);
  EXPECT_EQ(ctx->streams[1]->codecpar->ch_layout.nb_channels, 2);
  avformat_close_input(&ctx);

  QFile file(result.path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const auto bytes = file.readAll();
  EXPECT_GT(bytes.indexOf("mdat"), bytes.indexOf("moov")); // fast start
  const auto pcm = audio(result.path);
  EXPECT_GT(tone(pcm, 440), 0.02);
  EXPECT_GT(tone(pcm, 880), 0.02);
  EXPECT_TRUE(QFile::exists(source));
}

TEST_F(ShareExportTest, ConvertModeScalesToRequestedHeight) {
  const QString big = directory.filePath("BigSource.mkv");
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
      "testsrc2=size=1440x900:rate=30:duration=1", "-f", "lavfi", "-i",
      "sine=frequency=440:sample_rate=48000:duration=1", "-map", "0:v", "-map", "1:a",
      "-c:v", "mpeg4", "-c:a", "aac", "-ac", "2", big});
  ASSERT_TRUE(ffmpeg.waitForFinished(60000));
  ASSERT_EQ(ffmpeg.exitCode(), 0) << ffmpeg.readAllStandardError().toStdString();

  ShareExportOptions options;
  options.audioStreams = {1};
  options.mode = ShareExportOptions::Mode::Convert;
  options.maxHeight = 720;
  const auto result = ShareExportEngine::run(big, directory.path(), options, cancel, progress);
  ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
  AVFormatContext *ctx = nullptr;
  ASSERT_GE(avformat_open_input(&ctx, QFile::encodeName(result.path).constData(), nullptr, nullptr), 0);
  ASSERT_GE(avformat_find_stream_info(ctx, nullptr), 0);
  EXPECT_EQ(ctx->streams[0]->codecpar->height, 720);
  EXPECT_EQ(ctx->streams[0]->codecpar->width, 1152); // 1440 * 720/900, even
  avformat_close_input(&ctx);
}

TEST_F(ShareExportTest, CopiesVideoPacketsVerbatim) {
  const auto expected = videoDigest(source);
  ASSERT_GT(expected.first, 0);
  const auto result = run({});
  ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
  // Stream copy: the same payload bytes, not a re-encode of the same scene.
  EXPECT_EQ(videoDigest(result.path), expected);
  EXPECT_NE(result.path, source);
  EXPECT_TRUE(QFile::exists(source));
}

TEST_F(ShareExportTest, SingleStereoTrackIsCopiedWithoutReencoding) {
  // Real recordings carry AAC; selecting exactly one stereo AAC track must
  // copy its packets verbatim instead of decoding, mixing and re-encoding.
  const QString aacSource = directory.filePath("AacCopy.mkv");
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
      "testsrc2=size=320x180:rate=30:duration=1", "-f", "lavfi", "-i",
      "sine=frequency=440:sample_rate=48000:duration=1", "-map", "0:v", "-map", "1:a",
      "-c:v", "mpeg4", "-c:a", "aac", "-ac", "2", aacSource});
  ASSERT_TRUE(ffmpeg.waitForFinished(30000));
  ASSERT_EQ(ffmpeg.exitCode(), 0) << ffmpeg.readAllStandardError().toStdString();

  const auto expected = digest(aacSource, AVMEDIA_TYPE_AUDIO, 0);
  ASSERT_GT(expected.first, 0);
  const auto result =
      ShareExportEngine::run(aacSource, directory.path(), {{1}}, cancel, progress);
  ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
  EXPECT_EQ(digest(result.path, AVMEDIA_TYPE_AUDIO, 0), expected);
}

TEST_F(ShareExportTest, TrackSelectionExcludesVoiceAndPreservesLateOffset) {
  auto game = run({1}); ASSERT_TRUE(game.error.isEmpty()) << game.error.toStdString();
  auto pcm = audio(game.path);
  EXPECT_GT(tone(pcm, 440), 0.04); EXPECT_LT(tone(pcm, 880), 0.005);
  auto voice = run({2}); ASSERT_TRUE(voice.error.isEmpty()) << voice.error.toStdString();
  pcm = audio(voice.path);
  EXPECT_LT(tone(pcm, 880, 0.05, 0.2), 0.001);
  EXPECT_GT(tone(pcm, 880), 0.04);
}
TEST_F(ShareExportTest, SilentCopyAndCollisionsPreserveExistingFiles) {
  const auto existing = directory.filePath("Game #1 %_Share.mp4");
  QFile sentinel(existing); ASSERT_TRUE(sentinel.open(QIODevice::WriteOnly)); sentinel.write("keep"); sentinel.close();
  auto result = run({});
  ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
  EXPECT_NE(result.path, existing);
  ASSERT_TRUE(sentinel.open(QIODevice::ReadOnly)); EXPECT_EQ(sentinel.readAll(), "keep");
  AVFormatContext *ctx = nullptr;
  ASSERT_GE(avformat_open_input(&ctx, QFile::encodeName(result.path).constData(), nullptr, nullptr), 0);
  ASSERT_GE(avformat_find_stream_info(ctx, nullptr), 0);
  EXPECT_EQ(ctx->nb_streams, 1u);
  avformat_close_input(&ctx);
}
TEST_F(ShareExportTest, CancelAndInvalidOptionsLeaveNoPartialFiles) {
  cancel.store(true);
  auto result = run({1}); EXPECT_TRUE(result.path.isEmpty()); EXPECT_FALSE(result.error.isEmpty());
  cancel.store(false);
  EXPECT_FALSE(run({0}).error.isEmpty()); // Video is not an audio track.
  EXPECT_FALSE(run({1, 1}).error.isEmpty());
  EXPECT_FALSE(run({99}).error.isEmpty()); // Not an audio stream.
  EXPECT_TRUE(QDir(directory.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
  EXPECT_TRUE(QFile::exists(source));
}
TEST_F(ShareExportTest, ServicePinsSourceAcrossProbeAndExportAndReleasesOnClose) {
  QTemporaryDir output;
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  ASSERT_TRUE(service.inspect(source));
  EXPECT_FALSE(storage.deleteRecording(source));
  ASSERT_TRUE(wait(service)); ASSERT_TRUE(service.error().isEmpty()) << service.error().toStdString();
  EXPECT_FALSE(storage.reserveRecordings({source}));
  EXPECT_FALSE(service.start(directory.path(), {1, 2}));
  ASSERT_TRUE(service.start(output.path(), {1, 2}));
  EXPECT_TRUE(storage.renameRecording(source, "Other").isEmpty());
  ASSERT_TRUE(wait(service));
  EXPECT_TRUE(service.error().isEmpty()) << service.error().toStdString();
  EXPECT_FALSE(service.resultPath().isEmpty());
  EXPECT_EQ(service.resultFolder().toLocalFile(), output.path());
  service.release();
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
}

TEST_F(ShareExportTest, BatchQueuesDifferentCapturesWithIndependentAudioAndReservations) {
  const auto second = directory.filePath("Second.mkv");
  ASSERT_TRUE(QFile::copy(source, second));
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  QTemporaryDir output;
  QSignalSpy completed(&service, &ShareExporter::finished);
  ASSERT_TRUE(service.inspect(second)); // The batch form retains its last probe.
  ASSERT_TRUE(wait(service));
  ASSERT_TRUE(service.startBatch(output.path(), {
      QVariantMap{{"sourcePath", source}, {"audioStreams", QVariantList{1}}},
      QVariantMap{{"sourcePath", second}, {"audioStreams", QVariantList{}}}}));
  ASSERT_EQ(service.jobs().size(), 2);
  EXPECT_TRUE(service.jobs()[0].toMap()["active"].toBool());
  EXPECT_FALSE(service.jobs()[1].toMap()["active"].toBool());
  EXPECT_FALSE(storage.reserveRecordings({source}));
  EXPECT_FALSE(storage.reserveRecordings({second}));
  service.release();
  ASSERT_TRUE(wait(service));
  ASSERT_EQ(completed.size(), 2);
  for (int i = 0; i < 2; ++i) {
    AVFormatContext *ctx = nullptr;
    const auto path = completed[i][0].toString();
    ASSERT_GE(avformat_open_input(&ctx, QFile::encodeName(path).constData(), nullptr, nullptr), 0);
    ASSERT_GE(avformat_find_stream_info(ctx, nullptr), 0);
    EXPECT_EQ(ctx->nb_streams, i == 0 ? 2u : 1u);
    avformat_close_input(&ctx);
  }
  EXPECT_TRUE(storage.reserveRecordings({source, second}));
  storage.releaseRecordings({source, second});
}
TEST_F(ShareExportTest, BatchFailureRollsBackReservationsWithoutPublishingPartialJobs) {
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  QTemporaryDir output;
  EXPECT_FALSE(service.startBatch(output.path(), {
      QVariantMap{{"sourcePath", source}, {"audioStreams", QVariantList{1}}},
      QVariantMap{{"sourcePath", directory.filePath("missing.mkv")}, {"audioStreams", QVariantList{}}}}));
  EXPECT_TRUE(service.jobs().isEmpty());
  EXPECT_TRUE(storage.reserveRecordings({source}));
  storage.releaseRecordings({source});
  EXPECT_FALSE(service.startBatch(directory.path(), {
      QVariantMap{{"sourcePath", source}, {"audioStreams", QVariantList{1}}}}));
  EXPECT_FALSE(service.startBatch(output.path(), {
      QVariantMap{{"sourcePath", source}, {"audioStreams", QVariantList{1, 1}}}}));
  EXPECT_TRUE(service.jobs().isEmpty());
}
TEST_F(ShareExportTest, RejectedBatchKeepsExistingQueueAndOtherOwnersReservations) {
  const auto second = directory.filePath("Reserved.mkv");
  ASSERT_TRUE(QFile::copy(source, second));
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  QTemporaryDir output;
  ASSERT_TRUE(service.inspect(source));
  ASSERT_TRUE(wait(service));
  ASSERT_TRUE(service.start(output.path(), {1}));
  ASSERT_TRUE(storage.reserveRecordings({second}));
  EXPECT_FALSE(service.startBatch(output.path(), {
      QVariantMap{{"sourcePath", source}, {"audioStreams", QVariantList{1}}},
      QVariantMap{{"sourcePath", second}, {"audioStreams", QVariantList{1}}}}));
  EXPECT_EQ(service.jobs().size(), 1);
  EXPECT_TRUE(storage.recordingInUse(source));
  EXPECT_TRUE(storage.recordingInUse(second));
  service.release();
  ASSERT_TRUE(wait(service));
  EXPECT_FALSE(storage.recordingInUse(source));
  EXPECT_TRUE(storage.recordingInUse(second));
  storage.releaseRecordings({second});
}
TEST_F(ShareExportTest, QueueRunsOneAtATimeInOrderAndKeepsSharedPins) {
  QTemporaryDir output;
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  ASSERT_TRUE(service.inspect(source)); ASSERT_TRUE(wait(service));
  QSignalSpy completed(&service, &ShareExporter::finished);
  QSignalSpy settled(&service, &ShareExporter::jobSettled);
  ASSERT_TRUE(service.start(output.path(), {1}));
  ASSERT_TRUE(service.start(output.path(), {2}));
  ASSERT_TRUE(service.start(output.path(), {}));
  auto jobs = service.jobs();
  ASSERT_EQ(jobs.size(), 3);
  EXPECT_TRUE(jobs[0].toMap()["active"].toBool());
  EXPECT_FALSE(jobs[1].toMap()["active"].toBool());
  EXPECT_FALSE(jobs[2].toMap()["active"].toBool());
  const auto secondId = jobs[1].toMap()["id"].toString();
  service.cancelJob(secondId);
  EXPECT_EQ(service.jobs().size(), 2);
  service.release(); // Closing the form must not cancel or release queued jobs.
  EXPECT_FALSE(storage.reserveRecordings({source}));
  ASSERT_TRUE(wait(service));
  ASSERT_EQ(completed.count(), 2);
  ASSERT_EQ(settled.count(), 2);
  EXPECT_EQ(settled[0][0].toString(), jobs[0].toMap()["id"].toString());
  EXPECT_EQ(settled[1][0].toString(), jobs[2].toMap()["id"].toString());
  EXPECT_EQ(settled[0][1].toString(), QFileInfo(source).canonicalFilePath());
  EXPECT_EQ(settled[0][2].toString(), completed[0][0].toString());
  EXPECT_TRUE(settled[0][3].toString().isEmpty());
  EXPECT_FALSE(settled[0][4].toBool());
  const auto first = completed[0][0].toString();
  const auto last = completed[1][0].toString();
  EXPECT_NE(first, last);
  std::atomic_bool stop{false};
  EXPECT_EQ(ShareExportEngine::inspect(first, stop)["tracks"].toList().size(), 1);
  EXPECT_TRUE(ShareExportEngine::inspect(last, stop)["tracks"].toList().isEmpty());
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
}
TEST_F(ShareExportTest, QueueContinuesAfterFailureAndActiveCancellation) {
  QTemporaryDir output;
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  ASSERT_TRUE(service.inspect(source)); ASSERT_TRUE(wait(service));
  QSignalSpy completed(&service, &ShareExporter::finished);
  QSignalSpy failures(&service, &ShareExporter::failed);
  QSignalSpy settled(&service, &ShareExporter::jobSettled);
  ASSERT_TRUE(service.start(output.filePath("missing"), {}));
  ASSERT_TRUE(service.start(output.path(), {}));
  service.release();
  ASSERT_TRUE(wait(service));
  EXPECT_EQ(failures.count(), 1);
  EXPECT_EQ(completed.count(), 1);
  ASSERT_EQ(settled.count(), 2);
  EXPECT_TRUE(settled[0][2].toString().isEmpty());
  EXPECT_FALSE(settled[0][3].toString().isEmpty());
  ASSERT_TRUE(service.inspect(source)); ASSERT_TRUE(wait(service));
  ASSERT_TRUE(service.start(output.path(), {1}));
  ASSERT_TRUE(service.start(output.path(), {}));
  service.cancelJob(service.jobs()[0].toMap()["id"].toString());
  service.release();
  ASSERT_TRUE(wait(service));
  EXPECT_EQ(failures.count(), 1); // Cancellation does not report a failure.
  ASSERT_EQ(settled.count(), 4);
  EXPECT_TRUE(settled[2][4].toBool());
  EXPECT_GE(completed.count(), 2);
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
}
TEST_F(ShareExportTest, QueueAllowsIndependentProbeAndJoinsBeforeReleasingOnDestruction) {
  QTemporaryDir output;
  StorageManager storage(directory.path());
  {
    ShareExporter service(&storage);
    ASSERT_TRUE(service.inspect(source)); ASSERT_TRUE(wait(service));
    ASSERT_TRUE(service.start(output.path(), {1}));
    ASSERT_TRUE(service.start(output.path(), {2}));
    service.release();
    ASSERT_TRUE(service.inspect(source)); // Another form may inspect a pinned capture.
    EXPECT_TRUE(service.inspecting());
    EXPECT_EQ(service.jobs().size(), 2);
    EXPECT_FALSE(storage.reserveRecordings({source}));
  }
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
  EXPECT_TRUE(QDir(output.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
}
TEST_F(ShareExportTest, AacResamplingPreservesLateAudioAndAudibleTail) {
  source = directory.filePath("LateAAC.mkv");
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=30:duration=2",
      "-itsoffset", "0.4", "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=44100:duration=1.6",
      "-map", "0:v", "-map", "1:a", "-c:v", "mpeg4", "-c:a", "aac", source});
  ASSERT_TRUE(ffmpeg.waitForFinished(30000)); ASSERT_EQ(ffmpeg.exitCode(), 0);
  const auto result = run({1});
  ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
  const auto pcm = audio(result.path);
  EXPECT_LT(tone(pcm, 880, 0.05, 0.2), 0.001);
  EXPECT_GT(tone(pcm, 880), 0.04);
  EXPECT_GT(tone(pcm, 880, 1.6, 0.2), 0.04);
}

TEST_F(ShareExportTest, RealOutputWriteLimitFailureLeavesNoPartialOrDeletedSource) {
  const auto pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    struct rlimit limit {1024, 1024};
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(77);
    signal(SIGXFSZ, SIG_IGN);
    const auto result = run({1, 2});
    _exit(result.path.isEmpty() && !result.error.isEmpty() ? 0 : 1);
  }
  int status = 0; ASSERT_EQ(waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status));
  if (WEXITSTATUS(status) == 77) GTEST_SKIP() << "Cannot install child-only file size limit";
  EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_TRUE(QFile::exists(source));
  EXPECT_TRUE(QDir(directory.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
}

TEST_F(ShareExportTest, CancellingBackgroundExportRetainsPinsUntilSettlement) {
  QTemporaryDir output;
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  QSignalSpy failures(&service, &ShareExporter::failed);
  ASSERT_TRUE(service.inspect(source)); ASSERT_TRUE(wait(service));
  ASSERT_TRUE(service.start(output.path(), {1, 2}));
  service.cancel(); service.release();
  EXPECT_FALSE(storage.reserveRecordings({source}));
  ASSERT_TRUE(wait(service));
  EXPECT_TRUE(QFile::exists(source));
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
  EXPECT_TRUE(QDir(output.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
  EXPECT_TRUE(failures.isEmpty());
}

TEST_F(ShareExportTest, BackgroundFailureReportsOnceAndReleasesSource) {
  QTemporaryDir output;
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  ASSERT_TRUE(service.inspect(source)); ASSERT_TRUE(wait(service));
  QSignalSpy failures(&service, &ShareExporter::failed);
  ASSERT_TRUE(service.start(output.filePath("missing-directory"), {1}));
  ASSERT_TRUE(wait(service));
  EXPECT_EQ(failures.count(), 1);
  EXPECT_FALSE(service.error().isEmpty()); EXPECT_TRUE(service.resultPath().isEmpty());
  EXPECT_TRUE(QFile::exists(source));
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
}
