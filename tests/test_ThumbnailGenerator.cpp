/*
 * Trophy - Unit Tests
 * ThumbnailGenerator: real thumbnail from a generated clip, graceful
 * failure on garbage input.
 */

#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "library/ThumbnailGenerator.h"

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
