/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "library/PreviewCache.h"
#include "library/RecordingSidecars.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QQuickImageProvider>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>
extern "C" {
#include <libavformat/avformat.h>
}
using namespace TrophyBackend;

class PreviewCacheTest : public ::testing::Test {
protected:
  QTemporaryDir temp;
  QString render(const QString &name = "Game # %.mkv", int seconds = 2) {
    const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) return {};
    const QString path = temp.filePath(name);
    QProcess process;
    process.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i",
        "testsrc2=size=640x360:rate=30", "-f", "lavfi", "-i", "sine=frequency=440",
        "-t", QString::number(seconds), "-c:v", "mpeg4", "-g", "15", "-c:a", "pcm_s16le", path});
    if (!process.waitForFinished(20000) || process.exitCode() != 0) return {};
    return path;
  }
  QString request(PreviewCache &cache, const QString &source, int ticket = 1) {
    QSignalSpy ready(&cache, &PreviewCache::previewReady);
    cache.requestPreview(QUrl::fromLocalFile(source), ticket);
    if (!QTest::qWaitFor([&] { return !ready.isEmpty(); }, 20000)) return {};
    EXPECT_EQ(ready.first().at(0).toUrl().toLocalFile(), source);
    EXPECT_EQ(ready.first().at(1).toInt(), ticket);
    return ready.first().at(2).toUrl().toLocalFile();
  }
};

TEST_F(PreviewCacheTest, GeneratesSilentSmallVideoAndReusesIt) {
  const QString source = render("Long.mkv", 9);
  if (source.isEmpty()) GTEST_SKIP() << "ffmpeg fixture unavailable";
  PreviewCache cache;
  const QString preview = request(cache, source);
  ASSERT_FALSE(preview.isEmpty());
  EXPECT_EQ(QFileInfo(preview).absolutePath(), temp.filePath(".thumbnails"));
  EXPECT_TRUE(QFileInfo(preview).fileName().startsWith("Long.preview-v1-mkv-"));
  AVFormatContext *ctx = nullptr;
  ASSERT_GE(avformat_open_input(&ctx, QFile::encodeName(preview).constData(), nullptr, nullptr), 0);
  ASSERT_GE(avformat_find_stream_info(ctx, nullptr), 0);
  ASSERT_EQ(ctx->nb_streams, 1u);
  EXPECT_EQ(ctx->streams[0]->codecpar->codec_type, AVMEDIA_TYPE_VIDEO);
  EXPECT_LE(ctx->streams[0]->codecpar->width, 320);
  EXPECT_LE(ctx->streams[0]->codecpar->height, 180);
  EXPECT_GT(ctx->duration, 5000000);
  EXPECT_LE(ctx->duration, 6100000);
  avformat_close_input(&ctx);
  const auto stamp = QFileInfo(preview).lastModified();
  EXPECT_EQ(request(cache, source, 2), preview);
  EXPECT_EQ(QFileInfo(preview).lastModified(), stamp);
  QProcess decode;
  decode.start("ffmpeg", {"-v", "error", "-i", preview, "-f", "null", "-"});
  ASSERT_TRUE(decode.waitForFinished(10000));
  EXPECT_EQ(decode.exitCode(), 0) << decode.readAllStandardError().toStdString();
  EXPECT_TRUE(QDir(temp.filePath(".thumbnails")).entryList({"*.part"}, QDir::Files).isEmpty());
}

TEST_F(PreviewCacheTest, FingerprintInvalidatesAndCorruptCacheRegenerates) {
  const QString source = render();
  if (source.isEmpty()) GTEST_SKIP();
  PreviewCache cache;
  const QString first = request(cache, source);
  ASSERT_FALSE(first.isEmpty());
  QFile broken(first);
  ASSERT_TRUE(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
  broken.write("broken"); broken.close();
  EXPECT_EQ(request(cache, source, 2), first);
  EXPECT_GT(QFileInfo(first).size(), 6);
  QFile input(source);
  ASSERT_TRUE(input.open(QIODevice::ReadWrite));
  ASSERT_TRUE(input.setFileTime(QFileInfo(source).lastModified().addSecs(2), QFileDevice::FileModificationTime));
  input.close();
  const QString second = request(cache, source, 3);
  ASSERT_FALSE(second.isEmpty());
  EXPECT_NE(second, first);
  EXPECT_FALSE(QFileInfo::exists(first));
}

TEST_F(PreviewCacheTest, DuplicateRequestsJoinAndCancelledQueuedWorkDoesNotRun) {
  const QString first = render("First.mkv");
  if (first.isEmpty()) GTEST_SKIP();
  const QString second = temp.filePath("Second.mkv");
  ASSERT_TRUE(QFile::copy(first, second));
  PreviewCache cache;
  QSignalSpy ready(&cache, &PreviewCache::previewReady);
  cache.preparePreview(first);
  cache.requestPreview(QUrl::fromLocalFile(first), 1);
  cache.requestPreview(QUrl::fromLocalFile(first), 2);
  cache.cancelPreview(2);
  cache.requestPreview(QUrl::fromLocalFile(second), 3);
  cache.cancelPreview(3);
  ASSERT_TRUE(QTest::qWaitFor([&] { return ready.count() == 2; }, 20000));
  EXPECT_EQ(ready.at(0).at(2), ready.at(1).at(2));
  for (const auto &result : ready) {
    EXPECT_NE(result.at(1).toInt(), 2);
    EXPECT_NE(result.at(1).toInt(), 3);
    EXPECT_FALSE(result.at(2).toUrl().isEmpty());
  }
  QTest::qWait(100);
  EXPECT_TRUE(QDir(temp.filePath(".thumbnails")).entryList({"Second.preview-*"}, QDir::Files).isEmpty());
}

TEST_F(PreviewCacheTest, MissingMalformedAndNonLocalInputsFailWithoutPartialFiles) {
  const QString broken = temp.filePath("Bad.mkv");
  QFile file(broken);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly)); file.write("bad"); file.close();
  PreviewCache cache;
  EXPECT_TRUE(request(cache, broken).isEmpty());
  EXPECT_TRUE(request(cache, temp.filePath("Missing.mkv"), 2).isEmpty());
  QSignalSpy ready(&cache, &PreviewCache::previewReady);
  cache.requestPreview(QUrl("https://example.invalid/video"), 3);
  ASSERT_EQ(ready.count(), 1);
  EXPECT_TRUE(ready.first().at(2).toUrl().isEmpty());
  EXPECT_TRUE(QDir(temp.filePath(".thumbnails")).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
}

TEST_F(PreviewCacheTest, SymlinkCacheDirectoryIsNotFollowed) {
  const QString source = render();
  if (source.isEmpty()) GTEST_SKIP();
  QTemporaryDir outside;
  ASSERT_TRUE(QFile::link(outside.path(), temp.filePath(".thumbnails")));
  PreviewCache cache;
  EXPECT_TRUE(request(cache, source).isEmpty());
  EXPECT_TRUE(QDir(outside.path()).entryList(QDir::Files).isEmpty());
}

TEST_F(PreviewCacheTest, RenameAndDeleteFollowExistingSidecarLifecycle) {
  const QString source = render("Old.mkv");
  if (source.isEmpty()) GTEST_SKIP();
  PreviewCache cache;
  const QString oldPreview = request(cache, source);
  ASSERT_FALSE(oldPreview.isEmpty());
  const auto stamp = QFileInfo(oldPreview).lastModified();
  const QString renamed = temp.filePath("New.mkv");
  ASSERT_TRUE(QFile::rename(source, renamed));
  RecordingSidecars::renameForVideo(source, renamed);
  const QString newPreview = request(cache, renamed, 2);
  EXPECT_FALSE(newPreview.isEmpty());
  EXPECT_EQ(QFileInfo(newPreview).lastModified(), stamp);
  EXPECT_FALSE(QFileInfo::exists(oldPreview));
  ASSERT_TRUE(QFile::remove(renamed));
  RecordingSidecars::removeForVideo(renamed);
  EXPECT_FALSE(QFileInfo::exists(newPreview));
}

TEST_F(PreviewCacheTest, TeardownSettlesWorkerBeforeInputRemoval) {
  const QString source = render();
  if (source.isEmpty()) GTEST_SKIP();
  { PreviewCache cache; cache.requestPreview(QUrl::fromLocalFile(source), 1); }
  EXPECT_TRUE(QDir(temp.filePath(".thumbnails")).entryList({"*.part"}, QDir::Files).isEmpty());
  EXPECT_TRUE(QFile::remove(source));
}

TEST_F(PreviewCacheTest, FramesArePredecodedAndServedWithoutReadingVideo) {
  const QString source = render();
  if (source.isEmpty()) GTEST_SKIP();
  PreviewCache cache;
  std::unique_ptr<QQuickImageProvider> provider(cache.createImageProvider());
  QSignalSpy frames(&cache, &PreviewCache::previewFramesReady);
  cache.requestPreview(QUrl::fromLocalFile(source), 1);
  ASSERT_TRUE(QTest::qWaitFor([&] { return frames.count() == 1; }, 20000));
  const auto reply = frames.first();
  const QString key = reply.at(3).toString();
  const int count = reply.at(4).toInt();
  EXPECT_GT(count, 1);
  EXPECT_LE(count, 90);
  EXPECT_EQ(reply.at(5).toInt(), 15);
  cache.retainFrames(key);
  ASSERT_TRUE(QFile::remove(reply.at(2).toUrl().toLocalFile()));
  QSize size;
  const QImage first = provider->requestImage(key + "/0", &size, {});
  const QImage last = provider->requestImage(key + '/' + QString::number(count - 1), nullptr, {});
  EXPECT_FALSE(first.isNull());
  EXPECT_FALSE(last.isNull());
  EXPECT_LE(size.width(), 320);
  EXPECT_LE(size.height(), 180);
  EXPECT_TRUE(provider->requestImage(key + "/-1", nullptr, {}).isNull());
  EXPECT_TRUE(provider->requestImage(key + '/' + QString::number(count), nullptr, {}).isNull());
  cache.releaseFrames(key);
}

TEST_F(PreviewCacheTest, MemoryCacheEvictsOldFramesButKeepsActivePreviewPinned) {
  const QString first = render("First.mkv", 9);
  if (first.isEmpty()) GTEST_SKIP();
  PreviewCache cache;
  std::unique_ptr<QQuickImageProvider> provider(cache.createImageProvider());
  QSignalSpy frames(&cache, &PreviewCache::previewFramesReady);
  auto load = [&](const QString &path, int ticket) {
    frames.clear();
    cache.requestPreview(QUrl::fromLocalFile(path), ticket);
    EXPECT_TRUE(QTest::qWaitFor([&] { return frames.count() == 1; }, 20000));
    return frames.isEmpty() ? QString() : frames.first().at(3).toString();
  };
  const QString pinned = load(first, 1);
  ASSERT_FALSE(pinned.isEmpty());
  cache.retainFrames(pinned);
  QString old;
  for (int i = 2; i <= 4; ++i) {
    const QString path = temp.filePath(QString("Game%1.mkv").arg(i));
    ASSERT_TRUE(QFile::copy(first, path));
    const QString key = load(path, i);
    ASSERT_FALSE(key.isEmpty());
    if (i == 2) old = key;
  }
  EXPECT_FALSE(provider->requestImage(pinned + "/0", nullptr, {}).isNull());
  EXPECT_TRUE(provider->requestImage(old + "/0", nullptr, {}).isNull());
  cache.releaseFrames(pinned);
  const QString path = temp.filePath("Last.mkv");
  ASSERT_TRUE(QFile::copy(first, path));
  ASSERT_FALSE(load(path, 5).isEmpty());
  EXPECT_TRUE(provider->requestImage(pinned + "/0", nullptr, {}).isNull());
}

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
