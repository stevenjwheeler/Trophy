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
  ShareExportResult run(const QList<int> &tracks, int height = 1080, qint64 bytes = 0, bool watermark = true) {
    return ShareExportEngine::run(source, directory.path(), {height, bytes, tracks, watermark}, cancel, progress);
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
TEST_F(ShareExportTest, AnimatedWatermarkIsBurnedInOnlyAtOpeningAndQueueRetainsEachChoice) {
  QProcess fixture;
  fixture.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
      "color=black:size=320x180:rate=10:duration=7", "-c:v", "mpeg4", source});
  ASSERT_TRUE(fixture.waitForFinished(30000));
  ASSERT_EQ(fixture.exitCode(), 0);
  QFile original(source); ASSERT_TRUE(original.open(QIODevice::ReadOnly));
  const auto originalBytes = original.readAll(); original.close();
  QTemporaryDir destination;
  StorageManager storage(directory.path());
  ShareExporter service(&storage);
  QSignalSpy settled(&service, &ShareExporter::jobSettled);
  const QVariantList captures{QVariantMap{{"sourcePath", source}, {"audioStreams", QVariantList{}}}};
  ASSERT_TRUE(service.startBatch(destination.path(), 1080, 0, captures, false));
  ASSERT_TRUE(service.inspect(source));
  // The first queued job may finish while inspecting; the probe must settle.
  QElapsedTimer timer; timer.start();
  while (service.inspecting() && timer.elapsed() < 10000) {
    QCoreApplication::processEvents(); QThread::msleep(1);
  }
  ASSERT_FALSE(service.inspecting());
  ASSERT_TRUE(service.start(destination.path(), 1080, 0, {})); // default on
  service.release();
  ASSERT_TRUE(wait(service));
  ASSERT_EQ(settled.size(), 2);
  auto pixels = [](const QString &path) {
    QProcess decoder;
    decoder.start("ffmpeg", {"-v", "error", "-ignore_editlist", "1", "-i", path, "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "gray", "-f", "rawvideo", "-"});
    EXPECT_TRUE(decoder.waitForFinished(30000));
    EXPECT_EQ(decoder.exitCode(), 0);
    return decoder.readAllStandardOutput();
  };
  const auto plain = pixels(settled[0][2].toString());
  const auto markedFrames = pixels(settled[1][2].toString());
  constexpr int size = 320 * 180;
  ASSERT_EQ(plain.size(), 70 * size);
  ASSERT_EQ(markedFrames.size(), 420 * size);
  QByteArray marked;
  for (int index = 0; index < 70; ++index)
    marked.append(markedFrames.constData() + index * 6 * size, size);
  auto difference = [&](int index, int x0, int y0, int x1, int y1) {
    double sum = 0;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
      const int at = index * size + y * 320 + x;
      sum += std::abs(int(uint8_t(marked[at])) - int(uint8_t(plain[at])));
    }
    return sum / ((x1 - x0) * (y1 - y0));
  };
  EXPECT_LT(difference(0, 0, 0, 320, 180), 0.5);
  for (int index = 1; index <= 5; ++index)
    EXPECT_LT(difference(index, 0, 0, 320, 180), 0.5); // clean first 500ms
  EXPECT_GT(difference(25, 0, 125, 140, 180), 3.0);
  EXPECT_GT(difference(25, 0, 140, 8, 170), 3.0); // attached to the left edge
  EXPECT_LT(difference(25, 180, 0, 320, 180), 0.5);
  EXPECT_LT(difference(25, 0, 0, 320, 110), 0.5);
  EXPECT_LT(difference(65, 0, 0, 320, 180), 0.5);
  auto rightEdge = [&](int index) {
    int edge = -1;
    for (int y = 125; y < 180; ++y) for (int x = 0; x < 160; ++x)
      if (uint8_t(marked[index * size + y * 320 + x]) > 35) edge = std::max(edge, x);
    return edge;
  };
  EXPECT_GT(difference(6, 0, 125, 140, 180), 0.5); // starts after 500ms, not one second
  EXPECT_LT(rightEdge(6), rightEdge(25));
  EXPECT_GT(rightEdge(9), rightEdge(12)); // whole-tab overshoot settles gently
  EXPECT_LT(rightEdge(53), rightEdge(45)); // entire tab slides back out
  EXPECT_LT(rightEdge(54), rightEdge(53));
  QProcess color;
  color.start("ffmpeg", {"-v", "error", "-ignore_editlist", "1", "-i", settled[1][2].toString(),
      "-vf", "select=eq(n\\,150)", "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "-"});
  ASSERT_TRUE(color.waitForFinished(30000));
  ASSERT_EQ(color.exitCode(), 0);
  const auto rgb = color.readAllStandardOutput();
  ASSERT_EQ(rgb.size(), size * 3);
  int logoPixels = 0;
  for (int y = 125; y < 180; ++y) for (int x = 0; x < 40; ++x) {
    const int at = (y * 320 + x) * 3;
    if (uint8_t(rgb[at]) > 100 && int(uint8_t(rgb[at])) > int(uint8_t(rgb[at + 1])) + 50)
      ++logoPixels;
  }
  EXPECT_GT(logoPixels, 20); // bundled red Trophy logo, not a neutral placeholder
  int redTextPixels = 0;
  for (int y = 125; y < 180; ++y) for (int x = 50; x < 140; ++x) {
    const int at = (y * 320 + x) * 3;
    if (uint8_t(rgb[at]) > 100 && int(uint8_t(rgb[at])) > int(uint8_t(rgb[at + 1])) + 50)
      ++redTextPixels;
  }
  EXPECT_EQ(redTextPixels, 0); // no red underline beneath the lettering
  ASSERT_TRUE(original.open(QIODevice::ReadOnly));
  EXPECT_EQ(original.readAll(), originalBytes);
}
TEST_F(ShareExportTest, WholeTabSpringKeepsLetteringIntactAtThirtyAndSixtyFps) {
  for (const int fps : {30, 60}) {
    SCOPED_TRACE(fps);
    QProcess fixture;
    fixture.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
        QString("color=black:size=320x180:rate=%1:duration=2").arg(fps), "-c:v", "mpeg4", source});
    ASSERT_TRUE(fixture.waitForFinished(30000));
    ASSERT_EQ(fixture.exitCode(), 0);
    const auto output = run({});
    ASSERT_TRUE(output.error.isEmpty()) << output.error.toStdString();
    QProcess decoder;
    decoder.start("ffmpeg", {"-v", "error", "-ignore_editlist", "1", "-i", output.path,
        "-map", "0:v:0", "-fps_mode", "passthrough", "-pix_fmt", "gray", "-f", "rawvideo", "-"});
    ASSERT_TRUE(decoder.waitForFinished(30000));
    ASSERT_EQ(decoder.exitCode(), 0);
    const auto frames = decoder.readAllStandardOutput();
    constexpr int size = 320 * 180;
    constexpr int outputFps = 60;
    ASSERT_EQ(frames.size(), 2 * outputFps * size);
    auto ink = [&](int index) {
      double sum = 0, moment = 0;
      for (int y = 110; y < 180; ++y) for (int x = 0; x < 160; ++x) {
        const double value = std::max(0, int(uint8_t(frames[index * size + y * 320 + x])) - 50);
        sum += value;
        moment += value * x;
      }
      return std::pair{sum, sum ? moment / sum : 0};
    };
    const auto reference = ink(outputFps + outputFps / 5);
    ASSERT_GT(reference.first, 1000);
    double minimum = reference.second, maximum = reference.second;
    double previous = -1;
    // The tab is fully onscreen by 0.8s. During spring settling, lettering
    // translates intact rather than being repeatedly clipped by a reveal edge.
    for (int index = int(std::ceil(0.8 * outputFps)); index <= int(1.1 * outputFps); ++index) {
      const auto current = ink(index);
      EXPECT_NEAR(current.first, reference.first, reference.first * 0.08);
      minimum = std::min(minimum, current.second);
      maximum = std::max(maximum, current.second);
      if (previous >= 0) EXPECT_LT(std::abs(current.second - previous), 3.0);
      previous = current.second;
    }
    EXPECT_GT(maximum - minimum, 1.0); // genuine whole-artwork spring motion
  }
}
TEST_F(ShareExportTest, SparseNominalSixtyFpsSourceGetsSteadyWatermarkWithoutRetimingAudioOrScene) {
  QProcess fixture;
  fixture.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
      "color=black:size=320x180:rate=60:duration=2,drawbox=x=200:y=0:w=120:h=110:color=white:t=fill:enable='gte(t,1)'",
      "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=2",
      "-itsoffset", "0.4", "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000:duration=1.6",
      "-map", "0:v", "-map", "1:a", "-map", "2:a", "-vf", "select='not(mod(n,5))+eq(n,119)'",
      "-fps_mode", "passthrough", "-enc_time_base:v", "1:60", "-c:v", "mpeg4", "-c:a", "pcm_s16le", source});
  ASSERT_TRUE(fixture.waitForFinished(30000));
  ASSERT_EQ(fixture.exitCode(), 0) << fixture.readAllStandardError().toStdString();
  const auto marked = run({2});
  ASSERT_TRUE(marked.error.isEmpty()) << marked.error.toStdString();
  const auto plain = run({}, 1080, 0, false);
  ASSERT_TRUE(plain.error.isEmpty()) << plain.error.toStdString();
  auto timestamps = [](const QString &path) {
    std::vector<double> times;
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, QFile::encodeName(path).constData(), nullptr, nullptr) < 0) return times;
    if (avformat_find_stream_info(ctx, nullptr) < 0) { avformat_close_input(&ctx); return times; }
    const int video = av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVPacket *p = av_packet_alloc();
    if (video >= 0 && p) while (av_read_frame(ctx, p) >= 0) {
      if (p->stream_index == video && p->pts != AV_NOPTS_VALUE)
        times.push_back(p->pts * av_q2d(ctx->streams[video]->time_base));
      av_packet_unref(p);
    }
    av_packet_free(&p); avformat_close_input(&ctx);
    return times;
  };
  const auto inputTimes = timestamps(source), plainTimes = timestamps(plain.path), markedTimes = timestamps(marked.path);
  ASSERT_EQ(inputTimes.size(), 25u);
  ASSERT_EQ(plainTimes.size(), inputTimes.size());
  for (size_t i = 0; i < inputTimes.size(); ++i) EXPECT_NEAR(plainTimes[i], inputTimes[i], 0.0001);
  ASSERT_EQ(markedTimes.size(), 120u);
  for (size_t i = 0; i < markedTimes.size(); ++i) EXPECT_NEAR(markedTimes[i], double(i) / 60, 0.00002);
  QProcess decoder;
  decoder.start("ffmpeg", {"-v", "error", "-i", marked.path, "-map", "0:v:0", "-fps_mode", "passthrough",
      "-pix_fmt", "gray", "-f", "rawvideo", "-"});
  ASSERT_TRUE(decoder.waitForFinished(30000));
  ASSERT_EQ(decoder.exitCode(), 0);
  const auto frames = decoder.readAllStandardOutput();
  constexpr int size = 320 * 180;
  ASSERT_EQ(frames.size(), 120 * size); // including the final frame in ordinary playback
  for (int index = 0; index < 120; ++index) {
    const int scenePixel = uint8_t(frames[index * size + 30 * 320 + 250]);
    if (index < 60) EXPECT_LT(scenePixel, 10);
    else EXPECT_GT(scenePixel, 240); // scene change stays at 1.0s, not pulled forward
  }
  int movingFrames = 0;
  for (int index = 34; index < 46; ++index) {
    bool changed = false;
    for (int y = 125; y < 180 && !changed; ++y) for (int x = 0; x < 150; ++x) {
      const int at = index * size + y * 320 + x;
      if (std::abs(int(uint8_t(frames[at])) - int(uint8_t(frames[at + size]))) > 15) { changed = true; break; }
    }
    movingFrames += changed;
  }
  EXPECT_GE(movingFrames, 10); // animation advances between the sparse input pictures
  const auto pcm = audio(marked.path);
  EXPECT_LT(tone(pcm, 880, 0.05, 0.2), 0.001);
  EXPECT_GT(tone(pcm, 880), 0.04);
  EXPECT_NEAR(double(pcm.size()) / (48000 * 2), 2.0, 0.03);
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
  EXPECT_EQ(ctx->streams[0]->codecpar->codec_id, AV_CODEC_ID_H264);
  EXPECT_EQ(ctx->streams[0]->codecpar->format, AV_PIX_FMT_YUV420P);
  EXPECT_EQ(ctx->streams[0]->codecpar->width, 320); // Never upscale.
  EXPECT_EQ(ctx->streams[0]->nb_frames, 120); // 30fps source is held on the watermark's 60fps timeline.
  EXPECT_EQ(ctx->streams[1]->codecpar->codec_id, AV_CODEC_ID_AAC);
  EXPECT_EQ(ctx->streams[1]->codecpar->ch_layout.nb_channels, 2);
  avformat_close_input(&ctx);
  QFile file(output.path); ASSERT_TRUE(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll();
  EXPECT_GT(bytes.indexOf("mdat"), bytes.indexOf("moov"));
  const auto pcm = audio(output.path);
  EXPECT_GT(tone(pcm, 440), 0.02); EXPECT_GT(tone(pcm, 880), 0.02);
  EXPECT_DOUBLE_EQ(progress.load(), 1.0);
  EXPECT_TRUE(QFile::exists(source));
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
TEST_F(ShareExportTest, SilentSizePresetAndCollisionsPreserveExistingFiles) {
  const auto existing = directory.filePath("Game #1 %_Share.mp4");
  QFile sentinel(existing); ASSERT_TRUE(sentinel.open(QIODevice::WriteOnly)); sentinel.write("keep"); sentinel.close();
  auto result = run({}, 720, 256 * 1024);
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
  EXPECT_FALSE(run({1}, 123).error.isEmpty());
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
  EXPECT_FALSE(service.start(directory.path(), 720, 0, {1, 2}));
  ASSERT_TRUE(service.start(output.path(), 720, 0, {1, 2}));
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
  ASSERT_TRUE(service.startBatch(output.path(), 720, 0, {
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
  EXPECT_FALSE(service.startBatch(output.path(), 720, 0, {
      QVariantMap{{"sourcePath", source}, {"audioStreams", QVariantList{1}}},
      QVariantMap{{"sourcePath", directory.filePath("missing.mkv")}, {"audioStreams", QVariantList{}}}}));
  EXPECT_TRUE(service.jobs().isEmpty());
  EXPECT_TRUE(storage.reserveRecordings({source}));
  storage.releaseRecordings({source});
  EXPECT_FALSE(service.startBatch(directory.path(), 720, 0, {
      QVariantMap{{"sourcePath", source}, {"audioStreams", QVariantList{1}}}}));
  EXPECT_FALSE(service.startBatch(output.path(), 720, 0, {
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
  ASSERT_TRUE(service.start(output.path(), 720, 0, {1}));
  ASSERT_TRUE(storage.reserveRecordings({second}));
  EXPECT_FALSE(service.startBatch(output.path(), 720, 0, {
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
  ASSERT_TRUE(service.start(output.path(), 720, 0, {1}));
  ASSERT_TRUE(service.start(output.path(), 720, 0, {2}));
  ASSERT_TRUE(service.start(output.path(), 720, 0, {}));
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
  ASSERT_TRUE(service.start(output.filePath("missing"), 720, 0, {}));
  ASSERT_TRUE(service.start(output.path(), 720, 0, {}));
  service.release();
  ASSERT_TRUE(wait(service));
  EXPECT_EQ(failures.count(), 1);
  EXPECT_EQ(completed.count(), 1);
  ASSERT_EQ(settled.count(), 2);
  EXPECT_TRUE(settled[0][2].toString().isEmpty());
  EXPECT_FALSE(settled[0][3].toString().isEmpty());
  ASSERT_TRUE(service.inspect(source)); ASSERT_TRUE(wait(service));
  ASSERT_TRUE(service.start(output.path(), 720, 0, {1}));
  ASSERT_TRUE(service.start(output.path(), 720, 0, {}));
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
    ASSERT_TRUE(service.start(output.path(), 720, 0, {1}));
    ASSERT_TRUE(service.start(output.path(), 720, 0, {2}));
    service.release();
    ASSERT_TRUE(service.inspect(source)); // Another form may inspect a pinned capture.
    EXPECT_TRUE(service.inspecting());
    EXPECT_EQ(service.jobs().size(), 2);
    EXPECT_FALSE(storage.reserveRecordings({source}));
  }
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
  EXPECT_TRUE(QDir(output.path()).entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
}
TEST_F(ShareExportTest, ScalesDownAndCapsHighFrameRate) {
  source = directory.filePath("HighRate.mkv");
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
      "testsrc2=size=1440x900:rate=120:duration=0.25", "-c:v", "mpeg4", "-threads", "2", source});
  ASSERT_TRUE(ffmpeg.waitForFinished(30000)); ASSERT_EQ(ffmpeg.exitCode(), 0);
  const auto result = run({}, 720);
  ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
  AVFormatContext *ctx = nullptr;
  ASSERT_GE(avformat_open_input(&ctx, QFile::encodeName(result.path).constData(), nullptr, nullptr), 0);
  ASSERT_GE(avformat_find_stream_info(ctx, nullptr), 0);
  EXPECT_EQ(ctx->streams[0]->codecpar->width, 1152);
  EXPECT_EQ(ctx->streams[0]->codecpar->height, 720);
  EXPECT_GE(ctx->streams[0]->nb_frames, 14);
  EXPECT_LE(ctx->streams[0]->nb_frames, 16);
  avformat_close_input(&ctx);
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
  ASSERT_TRUE(service.start(output.path(), 720, 0, {1, 2}));
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
  ASSERT_TRUE(service.start(output.filePath("missing-directory"), 720, 0, {1}));
  ASSERT_TRUE(wait(service));
  EXPECT_EQ(failures.count(), 1);
  EXPECT_FALSE(service.error().isEmpty()); EXPECT_TRUE(service.resultPath().isEmpty());
  EXPECT_TRUE(QFile::exists(source));
  EXPECT_TRUE(storage.reserveRecordings({source})); storage.releaseRecordings({source});
}
