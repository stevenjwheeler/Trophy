/* Regression: "Start minimized" did nothing on GNOME.
 * The startup-hide decision used to live inside the tray-host branch, so a
 * desktop without a legacy tray (GNOME, unless the AppIndicator extension is
 * installed) always showed the window. GNOME supports hiding without a tray
 * because the single-instance guard restores the window on the next launch.
 *
 * Application is constructed but never initialized: no services, portal,
 * socket or QML engine are started, and the absent root window makes
 * setMainWindowVisible() a no-op after the decision is taken.
 */
#include "core/Application.h"
#include "settings/SettingsManager.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace TrophyBackend;

namespace TrophyBackend {
class ApplicationStartupVisibilityTest : public ::testing::Test {
protected:
  Application &app = *static_cast<Application *>(QApplication::instance());

  void SetUp() override {
    app.m_settingsManager = std::make_unique<SettingsManager>(&app);
    app.setForceMinimized(false);
    m_desktop = qgetenv("XDG_CURRENT_DESKTOP");
    m_sessionDesktop = qgetenv("XDG_SESSION_DESKTOP");
  }

  void TearDown() override {
    qputenv("XDG_CURRENT_DESKTOP", m_desktop);
    qputenv("XDG_SESSION_DESKTOP", m_sessionDesktop);
    app.m_settingsManager.reset();
    app.setForceMinimized(false);
  }

  // applyStartupVisibility() is private; friendship is not inherited by
  // TEST_F bodies, so route through the fixture.
  bool apply() { return app.applyStartupVisibility(); }
  void setDesktop(const char *current, const char *session) {
    qputenv("XDG_CURRENT_DESKTOP", current);
    qputenv("XDG_SESSION_DESKTOP", session);
  }
  // No SystemTrayManager exists on an uninitialized Application, so
  // isTrayAvailable() is false: every case below has no tray host.
  void requestStartMinimized(bool enabled) {
    app.m_settingsManager->setStartMinimized(enabled);
  }

private:
  QByteArray m_desktop;
  QByteArray m_sessionDesktop;
};
} // namespace TrophyBackend

TEST_F(ApplicationStartupVisibilityTest, NoRequestKeepsWindowVisible) {
  setDesktop("GNOME", "gnome");
  requestStartMinimized(false);
  EXPECT_FALSE(apply());
}

TEST_F(ApplicationStartupVisibilityTest, GnomeWithoutTrayStartsHidden) {
  setDesktop("GNOME", "gnome");
  requestStartMinimized(true);
  EXPECT_TRUE(apply());
}

TEST_F(ApplicationStartupVisibilityTest, GnomeAutostartFlagStartsHidden) {
  setDesktop("GNOME", "gnome");
  requestStartMinimized(false);
  app.setForceMinimized(true);
  EXPECT_TRUE(apply());
}

TEST_F(ApplicationStartupVisibilityTest, UnsupportedDesktopKeepsWindowVisible) {
  setDesktop("KDE", "KDE");
  requestStartMinimized(true);
  EXPECT_FALSE(apply());
}

TEST_F(ApplicationStartupVisibilityTest, UnsupportedDesktopAutostartKeepsWindowVisible) {
  setDesktop("Hyprland", "hyprland");
  requestStartMinimized(false);
  app.setForceMinimized(true);
  EXPECT_FALSE(apply());
}

int main(int argc, char **argv) {
  const QString tempRoot = QDir("/tmp/opencode").exists()
                               ? QStringLiteral("/tmp/opencode") : QDir::tempPath();
  QTemporaryDir isolation(tempRoot + "/trophy-app-startup-XXXXXX");
  if (!isolation.isValid())
    return 2;
  for (const char *key : {"XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME",
                          "XDG_RUNTIME_DIR"}) {
    const QString path = isolation.path() + "/" + key;
    if (!QDir().mkpath(path))
      return 2;
    if (!QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
      return 2;
    qputenv(key, path.toUtf8());
  }
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  Application app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
