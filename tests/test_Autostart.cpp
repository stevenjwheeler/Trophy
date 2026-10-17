/*
 * Trophy - Unit Tests
 * AutostartManager: desktop-entry lifecycle in an isolated config home.
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "core/AutostartManager.h"

using namespace TrophyBackend;

class AutostartTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new AutostartTestEnvironment);

class AutostartTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;
  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }
};

TEST_F(AutostartTest, DisabledByDefault) {
  AutostartManager mgr(tempDir.path());
  EXPECT_FALSE(mgr.isEnabled());
  EXPECT_FALSE(QFile::exists(mgr.filePath()));
}

TEST_F(AutostartTest, EnableWritesMinimizedEntry) {
  AutostartManager mgr(tempDir.path());
  EXPECT_TRUE(mgr.setEnabled(true, "/usr/bin/trophy"));
  EXPECT_TRUE(mgr.isEnabled());

  QFile f(mgr.filePath());
  ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
  const QString content = QString::fromUtf8(f.readAll());
  EXPECT_TRUE(content.contains("Exec=/usr/bin/trophy --minimized"));
  EXPECT_TRUE(content.contains("X-GNOME-Autostart-enabled=true"));
}

TEST_F(AutostartTest, DisableRemovesOwnEntry) {
  AutostartManager mgr(tempDir.path());
  ASSERT_TRUE(mgr.setEnabled(true, "/usr/bin/trophy"));
  EXPECT_TRUE(mgr.setEnabled(false, "/usr/bin/trophy"));
  EXPECT_FALSE(mgr.isEnabled());
  EXPECT_FALSE(QFile::exists(mgr.filePath()));
}

TEST_F(AutostartTest, DisableLeavesForeignEntryAlone) {
  AutostartManager mgr(tempDir.path());
  QDir().mkpath(QFileInfo(mgr.filePath()).absolutePath());
  QFile f(mgr.filePath());
  ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
  f.write("[Desktop Entry]\nName=UserCustom\n");
  f.close();

  EXPECT_FALSE(mgr.isEnabled()); // no marker: not ours
  EXPECT_TRUE(mgr.setEnabled(false, "/usr/bin/trophy"));
  EXPECT_TRUE(QFile::exists(mgr.filePath())); // untouched
}

TEST_F(AutostartTest, ReEnableRefreshesExecPath) {
  AutostartManager mgr(tempDir.path());
  ASSERT_TRUE(mgr.setEnabled(true, "/old/path/trophy"));
  ASSERT_TRUE(mgr.setEnabled(true, "/new/path/trophy"));

  QFile f(mgr.filePath());
  ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
  EXPECT_TRUE(QString::fromUtf8(f.readAll())
                  .contains("Exec=/new/path/trophy --minimized"));
  EXPECT_TRUE(mgr.isEnabled());
}

TEST_F(AutostartTest, ExecPathWithSpacesIsDoubleQuoted) {
  // Desktop Entry spec: single quotes are literal, so a path with spaces
  // must be wrapped in double quotes (with \, ", `, $ escaped).
  AutostartManager mgr(tempDir.path());
  ASSERT_TRUE(mgr.setEnabled(true, "/home/user/My Games/trophy"));

  QFile f(mgr.filePath());
  ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
  EXPECT_TRUE(QString::fromUtf8(f.readAll())
                  .contains("Exec=\"/home/user/My Games/trophy\" --minimized"));
}

TEST_F(AutostartTest, NativeLauncherUsesExecutablePath) {
  EXPECT_EQ(AutostartManager::launchCommand("/usr/bin/trophy"),
            QStringLiteral("/usr/bin/trophy --minimized"));
}

TEST_F(AutostartTest, AppImageLauncherUsesPersistentImagePath) {
  EXPECT_EQ(AutostartManager::launchCommand(
                "/tmp/.mount_Trophy/usr/bin/trophy",
                "/home/user/My Games/Trophy.AppImage"),
            QStringLiteral("\"/home/user/My Games/Trophy.AppImage\" --minimized"));
}

TEST_F(AutostartTest, ExecEscapesFieldCodesAndBothQuotingLayers) {
  EXPECT_EQ(AutostartManager::launchCommand(
                "/unused", QString::fromUtf8(R"(/home/user/My "$`\100%.AppImage)")),
            QString::fromUtf8(
                R"("/home/user/My \\"\\$\\`\\\\100%%.AppImage" --minimized)"));
  EXPECT_EQ(AutostartManager::launchCommand("/home/user/100%/trophy"),
            QStringLiteral("/home/user/100%%/trophy --minimized"));
}

TEST_F(AutostartTest, AppImageEntryRefreshesOldMountedPath) {
  AutostartManager mgr(tempDir.path());
  ASSERT_TRUE(mgr.setEnabled(true, "/tmp/.mount_old/usr/bin/trophy"));
  ASSERT_TRUE(mgr.setEnabled(true, "/tmp/.mount_new/usr/bin/trophy",
                             "/home/user/Trophy.AppImage"));
  QFile f(mgr.filePath());
  ASSERT_TRUE(f.open(QIODevice::ReadOnly | QIODevice::Text));
  const QString content = QString::fromUtf8(f.readAll());
  EXPECT_TRUE(content.contains("Exec=/home/user/Trophy.AppImage --minimized\n"));
  EXPECT_FALSE(content.contains(".mount_"));
  EXPECT_TRUE(mgr.isEnabled());
}

TEST_F(AutostartTest, EnableLeavesForeignEntryAlone) {
  AutostartManager mgr(tempDir.path());
  ASSERT_TRUE(QDir().mkpath(QFileInfo(mgr.filePath()).absolutePath()));
  const QByteArray original = "[Desktop Entry]\nName=UserCustom\nExec=custom\n";
  QFile f(mgr.filePath());
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  ASSERT_EQ(f.write(original), original.size());
  f.close();

  EXPECT_FALSE(mgr.setEnabled(true, "/usr/bin/trophy"));
  ASSERT_TRUE(f.open(QIODevice::ReadOnly));
  EXPECT_EQ(f.readAll(), original);
  EXPECT_FALSE(mgr.isEnabled());
}
