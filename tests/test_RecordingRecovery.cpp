/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "library/RecordingRecovery.h"
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <csignal>
extern "C" {
#include <libavformat/avformat.h>
}
using namespace TrophyBackend;

class RecordingRecoveryTest : public testing::Test {
protected:
  QTemporaryDir dir;
  std::atomic_bool cancel{false};
  QString make(bool unfinished = true, bool trophy = true, bool emptyAudio = false) {
    const QString path = dir.filePath("game.mkv");
    QStringList args{"-nostdin", "-v", "error", "-f", "lavfi", "-i",
      "testsrc2=size=160x90:rate=30:duration=3", "-itsoffset", emptyAudio ? "10" : "0.3",
      "-f", "lavfi", "-i", "sine=frequency=440:duration=2.7", "-map", "0:v", "-map", "1:a",
      "-c:v", "libx264", "-g", "15", "-c:a", "aac", "-metadata", "title=Recovery Game"};
    if (trophy) args << "-metadata" << "trophy-kind=recording" << "-metadata" << "trophy-version=test";
    if (unfinished) args << "-live" << "1";
    if (emptyAudio) args << "-t" << "3";
    args << path;
    QProcess proc;
    proc.start("ffmpeg", args);
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0) return {};
    return path;
  }
  QByteArray bytes(const QString &path) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
  }
  void SetUp() override {
    ASSERT_TRUE(dir.isValid());
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) GTEST_SKIP();
  }
};

TEST_F(RecordingRecoveryTest, RestoresDurationSeeksAndPreservesTracksAndMetadata) {
  const auto path = make(); ASSERT_FALSE(path.isEmpty());
  const auto original = bytes(path);
  RecordingRecovery recovery;
  ASSERT_TRUE(recovery.prepare(path, cancel));
  EXPECT_EQ(bytes(path), original); // staging does not touch original
  ASSERT_TRUE(recovery.publish());
  AVFormatContext *ctx = nullptr;
  ASSERT_GE(avformat_open_input(&ctx, QFile::encodeName(path).constData(), nullptr, nullptr), 0);
  EXPECT_GT(ctx->duration, 2900000);
  EXPECT_EQ(ctx->nb_streams, 2u);
  ASSERT_NE(av_dict_get(ctx->metadata, "title", nullptr, 0), nullptr);
  EXPECT_STREQ(av_dict_get(ctx->metadata, "title", nullptr, 0)->value, "Recovery Game");
  EXPECT_GE(av_seek_frame(ctx, -1, 2000000, AVSEEK_FLAG_BACKWARD), 0);
  avformat_close_input(&ctx);
  QProcess decode;
  decode.start("ffmpeg", {"-nostdin", "-v", "error", "-ss", "2", "-i", path,
                         "-map", "0:v:0", "-frames:v", "1", "-f", "null", "-"});
  ASSERT_TRUE(decode.waitForFinished(30000));
  EXPECT_EQ(decode.exitCode(), 0);
  EXPECT_TRUE(decode.readAllStandardError().isEmpty());
}

TEST_F(RecordingRecoveryTest, ChangedSourceCannotBeReplaced) {
  const auto path = make(); ASSERT_FALSE(path.isEmpty());
  RecordingRecovery recovery; ASSERT_TRUE(recovery.prepare(path, cancel));
  QFile file(path); ASSERT_TRUE(file.open(QIODevice::Append)); file.write("changed"); file.close();
  const auto changed = bytes(path);
  EXPECT_FALSE(recovery.publish()); EXPECT_EQ(bytes(path), changed);
}

TEST_F(RecordingRecoveryTest, ReplacedInodeOrSymlinkCannotBeOverwritten) {
  const auto path = make(); ASSERT_FALSE(path.isEmpty());
  RecordingRecovery recovery; ASSERT_TRUE(recovery.prepare(path, cancel));
  const auto moved = dir.filePath("moved.mkv");
  ASSERT_TRUE(QFile::rename(path, moved));
  ASSERT_TRUE(QFile::copy(moved, path));
  const auto replacement = bytes(path);
  EXPECT_FALSE(recovery.publish());
  EXPECT_EQ(bytes(path), replacement);
  ASSERT_TRUE(QFile::remove(path)); ASSERT_TRUE(QFile::link(moved, path));
  EXPECT_FALSE(recovery.publish());
  EXPECT_TRUE(QFileInfo(path).isSymLink());
}

TEST_F(RecordingRecoveryTest, CancellationAndFailureLeaveNoTemporaryFiles) {
  const auto path = make(); ASSERT_FALSE(path.isEmpty()); const auto original = bytes(path);
  cancel = true;
  { RecordingRecovery recovery; EXPECT_FALSE(recovery.prepare(path, cancel)); EXPECT_FALSE(recovery.publish()); }
  EXPECT_EQ(bytes(path), original);
  EXPECT_TRUE(QDir(dir.path()).entryList({".trophy-recovery-*"}, QDir::Files | QDir::Hidden).isEmpty());
}

TEST_F(RecordingRecoveryTest, CompletedFileIsNeverRewritten) {
  const auto path = make(false); ASSERT_FALSE(path.isEmpty()); const auto original = bytes(path);
  RecordingRecovery recovery; EXPECT_FALSE(recovery.prepare(path, cancel)); EXPECT_FALSE(recovery.publish());
  EXPECT_EQ(bytes(path), original);
}

TEST_F(RecordingRecoveryTest, UnusedAudioTrackDoesNotPreventRecovery) {
  const auto path = make(true, true, true); ASSERT_FALSE(path.isEmpty());
  RecordingRecovery recovery; ASSERT_TRUE(recovery.prepare(path, cancel)); ASSERT_TRUE(recovery.publish());
  AVFormatContext *ctx = nullptr;
  ASSERT_GE(avformat_open_input(&ctx, QFile::encodeName(path).constData(), nullptr, nullptr), 0);
  EXPECT_EQ(ctx->nb_streams, 2u);
  EXPECT_GT(ctx->duration, 2900000);
  avformat_close_input(&ctx);
}

TEST_F(RecordingRecoveryTest, UnrelatedMatroskaIsNeverRewritten) {
  const auto path = make(true, false); ASSERT_FALSE(path.isEmpty()); const auto original = bytes(path);
  RecordingRecovery recovery; EXPECT_FALSE(recovery.prepare(path, cancel)); EXPECT_FALSE(recovery.publish());
  EXPECT_EQ(bytes(path), original);
}

TEST_F(RecordingRecoveryTest, SymlinksAndHardlinksAreRejected) {
  const auto path = make(); ASSERT_FALSE(path.isEmpty());
  const auto link = dir.filePath("alias.mkv"); ASSERT_TRUE(QFile::link(path, link));
  RecordingRecovery symlink; EXPECT_FALSE(symlink.prepare(link, cancel));
  ASSERT_EQ(::link(QFile::encodeName(path).constData(), QFile::encodeName(dir.filePath("hard.mkv")).constData()), 0);
  RecordingRecovery hardlink; EXPECT_FALSE(hardlink.prepare(path, cancel));
}

TEST_F(RecordingRecoveryTest, TruncatedFinalClusterCanBeRecoveredWithoutDroppingReadablePackets) {
  const auto path = make(); ASSERT_FALSE(path.isEmpty());
  QFile file(path); ASSERT_TRUE(file.open(QIODevice::ReadWrite)); ASSERT_TRUE(file.resize(file.size() - 100)); file.close();
  RecordingRecovery recovery; ASSERT_TRUE(recovery.prepare(path, cancel)); ASSERT_TRUE(recovery.publish());
}

TEST_F(RecordingRecoveryTest, WriteFailurePreservesOriginalAndCleansPartialReplacement) {
  const auto path = make(); ASSERT_FALSE(path.isEmpty()); const auto original = bytes(path);
  const pid_t pid = ::fork(); ASSERT_GE(pid, 0);
  if (pid == 0) {
    ::signal(SIGXFSZ, SIG_IGN);
    const rlimit limit{4096, 4096};
    if (::setrlimit(RLIMIT_FSIZE, &limit)) ::_exit(3);
    bool result;
    {
      RecordingRecovery recovery;
      result = recovery.prepare(path, cancel) || recovery.publish();
    }
    ::_exit(result ? 1 : 0);
  }
  int status = 0; ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  ASSERT_TRUE(WIFEXITED(status)); EXPECT_EQ(WEXITSTATUS(status), 0);
  EXPECT_EQ(bytes(path), original);
  EXPECT_TRUE(QDir(dir.path()).entryList({".trophy-recovery-*"}, QDir::Files | QDir::Hidden).isEmpty());
}

TEST_F(RecordingRecoveryTest, AbandonedStagingPreservesOriginalTimeAndPermissions) {
  const auto path = make(); ASSERT_FALSE(path.isEmpty());
  const auto original = bytes(path);
  const auto modified = QFileInfo(path).lastModified();
  const auto permissions = QFileInfo(path).permissions();
  {
    RecordingRecovery recovery; ASSERT_TRUE(recovery.prepare(path, cancel));
  }
  EXPECT_EQ(bytes(path), original);
  EXPECT_TRUE(QDir(dir.path()).entryList({".trophy-recovery-*"}, QDir::Files | QDir::Hidden).isEmpty());
  RecordingRecovery recovery; ASSERT_TRUE(recovery.prepare(path, cancel)); ASSERT_TRUE(recovery.publish());
  EXPECT_EQ(QFileInfo(path).lastModified(), modified);
  EXPECT_EQ(QFileInfo(path).permissions(), permissions);
}
