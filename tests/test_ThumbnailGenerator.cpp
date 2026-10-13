/*
 * Trophy - Unit Tests
 * ThumbnailGenerator: real thumbnail from a generated clip, graceful
 * invalid scaling dimensions and recovery after input/output failures.
 */

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <limits>

#include "library/ThumbnailGenerator.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

using namespace TrophyBackend;

class ThumbnailGeneratorTestEnvironment : public ::testing::Environment {
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

static auto *env = testing::AddGlobalTestEnvironment(
    new ThumbnailGeneratorTestEnvironment);

class ThumbnailGeneratorTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;
  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }

  static QSize thumbnailSize(int width, int height) {
    return ThumbnailGenerator::thumbnailSize(width, height);
  }

  static bool generateSync(ThumbnailGenerator &gen, const QString &input,
                           const QString &output) {
    return gen.generateThumbnailSync(input, output);
  }

  static void settleQueue(ThumbnailGenerator &gen) {
    gen.m_future.waitForFinished();
  }

  static void setReceiver(ThumbnailGenerator &gen,
                          int (*receiver)(AVCodecContext *, AVFrame *)) {
    gen.m_receiveFrame = receiver;
  }

  static bool waitForCount(QSignalSpy &spy, int count) {
    QElapsedTimer timer;
    timer.start();
    while (spy.count() < count && timer.elapsed() < 15000)
      spy.wait(15000 - static_cast<int>(timer.elapsed()));
    return spy.count() == count;
  }

  // Renders a 1s 320x240 test clip with the system ffmpeg. Empty string
  // when ffmpeg is unavailable (callers should SKIP).
  QString makeClip(const QString &name = "clip.mp4") {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
      return QString();
    const QString path = tempDir.path() + "/" + name;
    QProcess proc;
    proc.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          "testsrc=size=320x240:rate=30:duration=1", "-pix_fmt",
                          "yuv420p", path});
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0 ||
        !QFile::exists(path))
      return QString();
    return path;
  }
};

namespace {
int receiveZeroWidth(AVCodecContext *context, AVFrame *frame) {
  const int result = avcodec_receive_frame(context, frame);
  if (result >= 0)
    frame->width = 0;
  return result;
}
int receiveZeroHeight(AVCodecContext *context, AVFrame *frame) {
  const int result = avcodec_receive_frame(context, frame);
  if (result >= 0)
    frame->height = 0;
  return result;
}
enum class DecodedFault { MissingLuma, MissingChroma, ShortStride, OversizedStride,
                          InvalidFormat, OversizedWidth, StaleCodecShape,
                          NegativeStrides };
thread_local DecodedFault decodedFault;
int receiveMalformedFrame(AVCodecContext *context, AVFrame *frame) {
  const int result = avcodec_receive_frame(context, frame);
  if (result < 0)
    return result;
  switch (decodedFault) {
  case DecodedFault::MissingLuma: frame->data[0] = nullptr; break;
  case DecodedFault::MissingChroma: frame->data[1] = nullptr; break;
  case DecodedFault::ShortStride: frame->linesize[0] = 1; break;
  case DecodedFault::OversizedStride:
    frame->linesize[0] = std::numeric_limits<int>::max(); break;
  case DecodedFault::InvalidFormat: frame->format = AV_PIX_FMT_NONE; break;
  case DecodedFault::OversizedWidth:
    frame->width = std::numeric_limits<int>::max(); break;
  case DecodedFault::StaleCodecShape:
    context->width = 160;
    context->height = 120;
    context->pix_fmt = AV_PIX_FMT_RGB24;
    break;
  case DecodedFault::NegativeStrides:
    // The CLI fixture is YUV420P. Reverse each plane within its owned buffer.
    for (int plane = 0; plane < 3; ++plane) {
      const int rows = plane == 0 ? frame->height : (frame->height + 1) / 2;
      frame->data[plane] += frame->linesize[plane] * (rows - 1);
      frame->linesize[plane] = -frame->linesize[plane];
    }
    break;
  }
  return result;
}
}

TEST_F(ThumbnailGeneratorTest, MalformedDecodedDimensionsFailAndAllowReuse) {
  const QString clip = makeClip();
  if (clip.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";
  ThumbnailGenerator gen;
  const QString output = tempDir.path() + "/decoded.jpg";
  for (auto receiver : {receiveZeroWidth, receiveZeroHeight}) {
    setReceiver(gen, receiver);
    EXPECT_FALSE(generateSync(gen, clip, output));
    EXPECT_FALSE(QFile::exists(output));
    QFile::remove(output);
    setReceiver(gen, avcodec_receive_frame);
    ASSERT_TRUE(generateSync(gen, clip, output));
    EXPECT_EQ(QImage(output).size(), QSize(320, 240));
    ASSERT_TRUE(QFile::remove(output));
  }
}

TEST_F(ThumbnailGeneratorTest, MalformedDecodedPlanesAndFormatFailAndAllowReuse) {
  const QString clip = makeClip();
  if (clip.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";
  ThumbnailGenerator gen;
  const QString output = tempDir.path() + "/malformed.jpg";
  for (auto fault : {DecodedFault::MissingLuma, DecodedFault::MissingChroma,
                     DecodedFault::ShortStride, DecodedFault::OversizedStride,
                     DecodedFault::InvalidFormat,
                     DecodedFault::OversizedWidth}) {
    SCOPED_TRACE(static_cast<int>(fault));
    decodedFault = fault;
    setReceiver(gen, receiveMalformedFrame);
    EXPECT_FALSE(generateSync(gen, clip, output));
    EXPECT_FALSE(QFile::exists(output));
    QFile::remove(output);
    setReceiver(gen, avcodec_receive_frame);
    ASSERT_TRUE(generateSync(gen, clip, output));
    EXPECT_EQ(QImage(output).size(), QSize(320, 240));
    ASSERT_TRUE(QFile::remove(output));
  }
}

TEST_F(ThumbnailGeneratorTest, ConversionUsesDecodedFrameRatherThanCodecShape) {
  const QString clip = makeClip();
  if (clip.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";
  ThumbnailGenerator gen;
  const QString expected = tempDir.path() + "/expected.jpg";
  const QString output = tempDir.path() + "/stale-codec.jpg";
  ASSERT_TRUE(generateSync(gen, clip, expected));
  decodedFault = DecodedFault::StaleCodecShape;
  setReceiver(gen, receiveMalformedFrame);
  ASSERT_TRUE(generateSync(gen, clip, output));
  EXPECT_EQ(QImage(output), QImage(expected));
}

TEST_F(ThumbnailGeneratorTest, ValidNegativeStridesRemainSupported) {
  const QString clip = makeClip();
  if (clip.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";
  ThumbnailGenerator gen;
  const QString output = tempDir.path() + "/negative-strides.jpg";
  decodedFault = DecodedFault::NegativeStrides;
  setReceiver(gen, receiveMalformedFrame);
  ASSERT_TRUE(generateSync(gen, clip, output));
  EXPECT_EQ(QImage(output).size(), QSize(320, 240));
}

TEST_F(ThumbnailGeneratorTest, SingleDelayedFrameIsDrainedAtEof) {
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty() ||
      !avcodec_find_encoder_by_name("libx264"))
    GTEST_SKIP() << "ffmpeg CLI or libx264 unavailable";
  const QString clip = tempDir.path() + "/single.mp4";
  QProcess process;
  process.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                            "testsrc=size=320x240:rate=30", "-frames:v", "1",
                            "-c:v", "libx264", "-bf", "3", "-pix_fmt",
                            "yuv420p", clip});
  ASSERT_TRUE(process.waitForFinished(30000));
  ASSERT_EQ(process.exitCode(), 0) << process.readAllStandardError().constData();
  ThumbnailGenerator gen;
  const QString output = tempDir.path() + "/single.jpg";
  ASSERT_TRUE(generateSync(gen, clip, output));
  EXPECT_EQ(QImage(output).size(), QSize(320, 240));
}

// These exercise the exact production guard, not garbage demux failure (which
// normally exits before a decoder can return a zero-dimension frame).
TEST_F(ThumbnailGeneratorTest, RejectsZeroAndNegativeDimensions) {
  for (const QSize &size : {QSize(0, 240), QSize(320, 0), QSize(0, 0),
                           QSize(-1, 240), QSize(320, -1), QSize(-1, -1)}) {
    EXPECT_TRUE(thumbnailSize(size.width(), size.height()).isEmpty())
        << size.width() << "x" << size.height();
  }
}

TEST_F(ThumbnailGeneratorTest, AspectScalingIsOverflowSafeAndNonzero) {
  const int max = std::numeric_limits<int>::max();
  EXPECT_EQ(thumbnailSize(1920, 1080), QSize(320, 180));
  EXPECT_EQ(thumbnailSize(240, 320), QSize(320, 426));
  EXPECT_EQ(thumbnailSize(max, 1), QSize(320, 1));
  EXPECT_EQ(thumbnailSize(max, max), QSize(320, 320));
  EXPECT_EQ(thumbnailSize(320, max), QSize(320, max));
  EXPECT_TRUE(thumbnailSize(1, max).isEmpty());
}

TEST_F(ThumbnailGeneratorTest, RealClipProducesThumbnail) {
  const QString clip = makeClip();
  if (clip.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";

  ThumbnailGenerator gen;
  QSignalSpy readySpy(&gen, &ThumbnailGenerator::thumbnailReady);
  QSignalSpy failedSpy(&gen, &ThumbnailGenerator::thumbnailFailed);

  gen.generateThumbnail(clip);
  ASSERT_TRUE(readySpy.wait(15000)) << "no thumbnailReady within timeout";
  EXPECT_EQ(failedSpy.count(), 0);

  ASSERT_EQ(readySpy.count(), 1);
  const QString thumbPath = readySpy.at(0).at(1).toString();
  EXPECT_TRUE(QFile::exists(thumbPath));
  EXPECT_GT(QFile(thumbPath).size(), 0);
  QImage img(thumbPath);
  EXPECT_FALSE(img.isNull());
  EXPECT_EQ(img.width(), 320); // generator scales to 320px wide
  EXPECT_EQ(img.height(), 240);
}

TEST_F(ThumbnailGeneratorTest, GarbageFileFailsGracefully) {
  const QString bad = tempDir.path() + "/garbage.mkv";
  QFile f(bad);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write(QByteArray(8192, '\0'));
  f.close();

  ThumbnailGenerator gen;
  QSignalSpy failedSpy(&gen, &ThumbnailGenerator::thumbnailFailed);

  gen.generateThumbnail(bad);
  ASSERT_TRUE(failedSpy.wait(15000)) << "no thumbnailFailed within timeout";
  EXPECT_EQ(failedSpy.at(0).at(0).toString(), bad);
}

TEST_F(ThumbnailGeneratorTest, InputAndOutputFailuresAllowRepeatedReuse) {
  const QString clip = makeClip();
  if (clip.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";

  const QString bad = tempDir.path() + "/garbage.mkv";
  QFile garbage(bad);
  ASSERT_TRUE(garbage.open(QIODevice::WriteOnly));
  ASSERT_EQ(garbage.write(QByteArray(8192, '\0')), 8192);
  garbage.close();

  ThumbnailGenerator gen;
  for (int i = 0; i < 3; ++i) {
    const QString output = tempDir.path() + QString("/thumb-%1.jpg").arg(i);
    EXPECT_FALSE(generateSync(gen, bad, output));
    EXPECT_FALSE(QFile::exists(output));
    // An existing directory cannot be saved as JPEG. This fails after frame
    // decoding, RGB allocation and sws conversion, exercising late cleanup.
    EXPECT_FALSE(generateSync(gen, clip, tempDir.path()));
    ASSERT_TRUE(generateSync(gen, clip, output));
    const QImage image(output);
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.size(), QSize(320, 240));
  }
}

TEST_F(ThumbnailGeneratorTest, QueueContinuesAfterFailureAndCanRestart) {
  const QString clip = makeClip();
  if (clip.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";

  ThumbnailGenerator gen;
  QSignalSpy ready(&gen, &ThumbnailGenerator::thumbnailReady);
  QSignalSpy failed(&gen, &ThumbnailGenerator::thumbnailFailed);
  const QString missing = tempDir.path() + "/missing.mkv";
  for (int i = 1; i <= 3; ++i) {
    gen.generateThumbnail(missing);
    gen.generateThumbnail(clip);
    ASSERT_TRUE(waitForCount(ready, i));
    ASSERT_TRUE(waitForCount(failed, i));
    EXPECT_EQ(failed.at(i - 1).at(0).toString(), missing);
    EXPECT_EQ(ready.at(i - 1).at(0).toString(), clip);
    EXPECT_EQ(QImage(ready.at(i - 1).at(1).toString()).size(), QSize(320, 240));
    EXPECT_FALSE(QFile::exists(tempDir.path() + "/.thumbnails/missing.jpg"));
    // Ensure the worker exited, so the next pair exercises a fresh queue run.
    settleQueue(gen);
  }
}
