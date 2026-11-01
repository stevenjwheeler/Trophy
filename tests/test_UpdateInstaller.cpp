/*
 * Trophy - Unit Tests
 * UpdateInstaller: generated update helper and post-update notice text.
 */

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include "core/UpdateInstaller.h"

using namespace TrophyBackend;

namespace {

QString writeFile(const QDir &dir, const QString &name, const QByteArray &contents,
                  bool executable = false) {
  const QString path = dir.filePath(name);
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
  file.write(contents);
  file.close();
  if (executable)
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner);
  return path;
}

QByteArray readFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}

} // namespace

TEST(UpdateInstallerHelper, PassesReplacedVersionToRelaunchedApp) {
  const QString script = UpdateInstaller::buildHelperScript(
      QStringLiteral("/tmp/new.AppImage"), QStringLiteral("/home/u/Trophy.AppImage"),
      1234, QStringLiteral("0.9.7"));

  EXPECT_TRUE(script.startsWith(QStringLiteral("#!/bin/sh\n")));
  // The parent is awaited so the file swap never races the running binary.
  EXPECT_TRUE(script.contains(QStringLiteral("while kill -0 \"$PARENT\"")));
  EXPECT_TRUE(script.contains(QStringLiteral("PARENT=1234")));
  // Relaunch carries the previous version so the new instance can announce it.
  EXPECT_TRUE(script.contains(
      QStringLiteral("\"$TARGET\" --updated-from '0.9.7' &")));
}

TEST(UpdateInstallerHelper, OmitsTheFlagWithoutAVersion) {
  const QString script = UpdateInstaller::buildHelperScript(
      QStringLiteral("/tmp/new.AppImage"), QStringLiteral("/home/u/Trophy.AppImage"),
      7, QString());
  EXPECT_TRUE(script.contains(QStringLiteral("\"$TARGET\" &")));
  EXPECT_FALSE(script.contains(QStringLiteral("--updated-from")));
}

TEST(UpdateInstallerHelper, QuotingSurvivesAwkwardPaths) {
  const QString script = UpdateInstaller::buildHelperScript(
      QStringLiteral("/tmp/a b/it's new.AppImage"),
      QStringLiteral("/home/u/My Trophy's Dir/Trophy.AppImage"), 9,
      QStringLiteral("0.9.7"));

  // Single quotes are closed/reopened, so neither path can break out.
  EXPECT_TRUE(script.contains(QStringLiteral("NEW='/tmp/a b/it'\\''s new.AppImage'")));
  EXPECT_TRUE(script.contains(
      QStringLiteral("TARGET='/home/u/My Trophy'\\''s Dir/Trophy.AppImage'")));
  EXPECT_TRUE(script.contains(QStringLiteral("--updated-from '0.9.7'")));
}

TEST(UpdateInstallerHelper, SwapsTheFileAndRelaunchesWithTheFlag) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QDir work(dir.path());

  const QString capture = work.filePath("launch-args.txt");
  const QString target = work.filePath("Trophy.AppImage");
  const QByteArray relauncher =
      QStringLiteral("#!/bin/sh\nprintf '%s\\n' \"$*\" > '%1'\n").arg(capture).toUtf8();

  // Stands in for the downloaded AppImage: after the swap this is what the
  // helper executes, and it records the arguments it was given.
  const QString newFile = writeFile(work, "downloaded.AppImage", relauncher, true);
  writeFile(work, "Trophy.AppImage", "#!/bin/sh\nexit 1\n", true);

  // A non-existent parent makes the wait loop finish immediately.
  const QString scriptPath =
      writeFile(work, "helper.sh",
                UpdateInstaller::buildHelperScript(newFile, target, 999999,
                                                   QStringLiteral("0.9.7"))
                    .toUtf8(),
                true);

  EXPECT_EQ(QProcess::execute(QStringLiteral("/bin/sh"), {scriptPath}), 0);

  // The relaunch runs in the background; give it a moment to record its argv.
  for (int i = 0; i < 100 && !QFile::exists(capture); ++i)
    QThread::msleep(50);
  ASSERT_TRUE(QFile::exists(capture));
  EXPECT_EQ(readFile(capture).trimmed(), QByteArray("--updated-from 0.9.7"));
  EXPECT_FALSE(QFile::exists(scriptPath));       // helper cleaned itself up
  EXPECT_FALSE(QFile::exists(target + QStringLiteral(".old")));
  EXPECT_EQ(readFile(target), relauncher); // target was replaced
}

TEST(UpdateInstallerNotice, OnlyRealVersionChangesAreAnnounced) {
  EXPECT_TRUE(UpdateInstaller::shouldAnnounceUpdate(QStringLiteral("0.9.7"),
                                                    QStringLiteral("0.9.8")));
  EXPECT_FALSE(UpdateInstaller::shouldAnnounceUpdate(QStringLiteral("0.9.8"),
                                                     QStringLiteral("0.9.8")));
  EXPECT_FALSE(UpdateInstaller::shouldAnnounceUpdate(QString(),
                                                     QStringLiteral("0.9.8")));
}

TEST(UpdateInstallerNotice, DescribesTheVersionTransition) {
  EXPECT_EQ(UpdateInstaller::updateNoticeBody(QStringLiteral("0.9.7"),
                                              QStringLiteral("0.9.8")),
            QStringLiteral("Trophy was updated from 0.9.7 to 0.9.8."));
  EXPECT_EQ(UpdateInstaller::updateNoticeBody(QString(), QStringLiteral("0.9.8")),
            QStringLiteral("Trophy is now version 0.9.8."));
}
