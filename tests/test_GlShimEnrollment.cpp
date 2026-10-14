/*
 * Trophy - Unit Tests
 * GlShimEnrollment: library lookup, conf-file enroll/remove round-trip,
 * foreign LD_PRELOAD merge. Hermetic via TROPHY_GL_SHIM_LIB + temp
 * XDG_CONFIG_HOME. Never touches the real session environment.
 */

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "detection/GlShimEnrollment.h"
#include "detection/ShippedLibDirs.h"
#include "detection/PersistentReporterLib.h"

using namespace TrophyBackend;

class GlShimEnrollmentTestEnvironment : public ::testing::Environment {
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
    new GlShimEnrollmentTestEnvironment);

class GlShimEnrollmentTest : public ::testing::Test {
protected:
  QTemporaryDir configHome;
  QTemporaryDir libDir;
  QTemporaryDir dataHome;
  QByteArray oldDataHome;
  QByteArray oldAppDir;
  QByteArray oldXdgConfigHome;
  QByteArray oldOverride;
  QString fakeLib;

  void SetUp() override {
    ASSERT_TRUE(configHome.isValid());
    ASSERT_TRUE(libDir.isValid());

    // Fake shim library (presence is all the lookup checks).
    fakeLib = libDir.path() + "/libtrophy_gl_shim.so";
    QFile f(fakeLib);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("fake");
    f.close();

    oldXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    oldOverride = qgetenv("TROPHY_GL_SHIM_LIB");
    oldDataHome = qgetenv("XDG_DATA_HOME");
    oldAppDir = qgetenv("APPDIR");
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    qunsetenv("APPDIR");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    qputenv("TROPHY_GL_SHIM_LIB", fakeLib.toUtf8());
  }

  void TearDown() override {
    if (oldDataHome.isNull())
      qunsetenv("XDG_DATA_HOME");
    else
      qputenv("XDG_DATA_HOME", oldDataHome);
    if (oldAppDir.isNull())
      qunsetenv("APPDIR");
    else
      qputenv("APPDIR", oldAppDir);
    if (oldXdgConfigHome.isNull())
      qunsetenv("XDG_CONFIG_HOME");
    else
      qputenv("XDG_CONFIG_HOME", oldXdgConfigHome);
    if (oldOverride.isNull())
      qunsetenv("TROPHY_GL_SHIM_LIB");
    else
      qputenv("TROPHY_GL_SHIM_LIB", oldOverride);
  }
};

TEST_F(GlShimEnrollmentTest, LibraryOverrideResolves) {
  EXPECT_EQ(GlShimEnrollment::libraryPath(), fakeLib);
}

TEST_F(GlShimEnrollmentTest, BundleRefreshNeverEnrollsWithoutPriorConsent) {
  qputenv("APPDIR", libDir.path().toUtf8());
  EXPECT_TRUE(GlShimEnrollment::refreshEnrolledBundle());
  EXPECT_FALSE(QFile::exists(GlShimEnrollment::confFilePath()));
  EXPECT_FALSE(QDir(dataHome.path() + "/trophy/reporters").exists());
}

TEST_F(GlShimEnrollmentTest, BundleRefreshMigratesVanishedLegacyMountPath) {
  qputenv("APPDIR", libDir.path().toUtf8());
  const QString confPath = GlShimEnrollment::confFilePath();
  ASSERT_TRUE(QDir().mkpath(QFileInfo(confPath).absolutePath()));
  QFile conf(confPath);
  ASSERT_TRUE(conf.open(QIODevice::WriteOnly));
  conf.write("LD_PRELOAD=/tmp/.mount_old/usr/lib/trophy/libtrophy_gl_shim.so:/foreign.so\n");
  conf.close();
  EXPECT_FALSE(GlShimEnrollment::isEnrolled());
  ASSERT_TRUE(GlShimEnrollment::refreshEnrolledBundle());
  EXPECT_TRUE(GlShimEnrollment::isEnrolled());
  ASSERT_TRUE(conf.open(QIODevice::ReadOnly));
  const QByteArray updated = conf.readAll();
  EXPECT_FALSE(updated.contains(".mount_old"));
  EXPECT_TRUE(updated.contains(dataHome.path().toUtf8()));
  EXPECT_TRUE(updated.contains(":/foreign.so"));
}

TEST_F(GlShimEnrollmentTest, BundleRefreshDoesNotKeepTransientOverrideAlias) {
  const QString alias = libDir.path() + "/custom-shim.so";
  ASSERT_TRUE(QFile::copy(fakeLib, alias));
  qputenv("TROPHY_GL_SHIM_LIB", alias.toUtf8());
  qputenv("APPDIR", libDir.path().toUtf8());
  const QString path = GlShimEnrollment::confFilePath();
  ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write("LD_PRELOAD=" + alias.toUtf8() + ":/foreign.so\n");
  file.close();
  ASSERT_TRUE(GlShimEnrollment::refreshEnrolledBundle());
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const QByteArray content = file.readAll();
  EXPECT_FALSE(content.contains(alias.toUtf8()));
  EXPECT_TRUE(content.contains(":/foreign.so"));
}

TEST_F(GlShimEnrollmentTest, UnchangedBundleRefreshPreservesConfigMtime) {
  qputenv("APPDIR", libDir.path().toUtf8());
  ASSERT_TRUE(GlShimEnrollment::setEnrolled(true));
  QFile file(GlShimEnrollment::confFilePath());
  ASSERT_TRUE(file.open(QIODevice::ReadWrite));
  const QDateTime sentinel = QDateTime::fromSecsSinceEpoch(1000000000);
  ASSERT_TRUE(file.setFileTime(sentinel, QFileDevice::FileModificationTime));
  file.close();
  ASSERT_TRUE(GlShimEnrollment::refreshEnrolledBundle());
  EXPECT_EQ(QFileInfo(file).lastModified(), sentinel);
}

TEST_F(GlShimEnrollmentTest, EnrollRemoveRoundTrip) {
  ASSERT_FALSE(GlShimEnrollment::isEnrolled());

  ASSERT_TRUE(GlShimEnrollment::setEnrolled(true));
  EXPECT_TRUE(GlShimEnrollment::isEnrolled());

  QFile conf(GlShimEnrollment::confFilePath());
  ASSERT_TRUE(conf.open(QIODevice::ReadOnly | QIODevice::Text));
  EXPECT_TRUE(QString::fromUtf8(conf.readAll()).contains(fakeLib));

  EXPECT_TRUE(GlShimEnrollment::setEnrolled(false));
  EXPECT_FALSE(GlShimEnrollment::isEnrolled());
  EXPECT_FALSE(QFile::exists(GlShimEnrollment::confFilePath()));
}

TEST_F(GlShimEnrollmentTest, ForeignPreloadEntryPreserved) {
  // A pre-existing unrelated LD_PRELOAD in our conf file must survive.
  QDir().mkpath(QFileInfo(GlShimEnrollment::confFilePath()).absolutePath());
  QFile pre(GlShimEnrollment::confFilePath());
  ASSERT_TRUE(pre.open(QIODevice::WriteOnly | QIODevice::Text));
  pre.write("LD_PRELOAD=/opt/other/libother.so\n");
  pre.close();

  ASSERT_TRUE(GlShimEnrollment::setEnrolled(true));
  QFile conf(GlShimEnrollment::confFilePath());
  ASSERT_TRUE(conf.open(QIODevice::ReadOnly | QIODevice::Text));
  const QString content = QString::fromUtf8(conf.readAll());
  EXPECT_TRUE(content.contains(fakeLib));
  EXPECT_TRUE(content.contains("/opt/other/libother.so"));
  EXPECT_TRUE(GlShimEnrollment::isEnrolled());
}

TEST_F(GlShimEnrollmentTest, FallsBackToCustomPrefixLibDir) {
  // No override: the lookup must find <prefix>/lib/trophy next to a
  // <prefix>/bin executable, which is where --prefix ~/.local installs it.
  qunsetenv("TROPHY_GL_SHIM_LIB");
  const QString found = findShippedLib(
      "libtrophy_gl_shim.so",
      {libDir.path()}); // stands in for <prefix>/lib/trophy
  EXPECT_EQ(found, libDir.path() + "/libtrophy_gl_shim.so");
  // And a name present nowhere resolves empty rather than inventing a path.
  EXPECT_TRUE(findShippedLib("libtrophy_nope_1234.so").isEmpty());
}

TEST_F(GlShimEnrollmentTest, FindsLibraryInPrefixMultilibLayout) {
  // Installed layout (AppImage included): <prefix>/bin/trophy with the
  // lib under <prefix>/lib/<multiarch>/trophy. This is what failed in
  // the wild — the lookup only ever consulted the dev gl_shim dir.
  QTemporaryDir prefix;
  ASSERT_TRUE(prefix.isValid());
  ASSERT_TRUE(QDir().mkpath(prefix.path() + "/bin"));
  ASSERT_TRUE(QDir().mkpath(prefix.path() + "/lib/x86_64-linux-gnu/trophy"));
  const QString lib =
      prefix.path() + "/lib/x86_64-linux-gnu/trophy/libtrophy_gl_shim.so";
  QFile f(lib);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write("fake");
  f.close();
  EXPECT_EQ(GlShimEnrollment::libraryPathForAppDir(prefix.path() + "/bin"),
            lib);
}

TEST_F(GlShimEnrollmentTest, FindsLibraryInDevTreeLayout) {
  // The dev build tree keeps the lib in <appDir>/gl_shim/. A regression
  // passed the full file path as a search dir, which silently matched
  // nothing (findShippedLib appends the file name itself).
  QTemporaryDir appDir;
  ASSERT_TRUE(appDir.isValid());
  ASSERT_TRUE(QDir().mkpath(appDir.path() + "/gl_shim"));
  const QString lib = appDir.path() + "/gl_shim/libtrophy_gl_shim.so";
  QFile f(lib);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write("fake");
  f.close();
  EXPECT_EQ(GlShimEnrollment::libraryPathForAppDir(appDir.path()), lib);
  EXPECT_TRUE(
      GlShimEnrollment::libraryPathForAppDir(appDir.path() + "/nowhere")
          .isEmpty());
}

TEST_F(GlShimEnrollmentTest, EnrollFailsWithoutLibrary) {
  qputenv("TROPHY_GL_SHIM_LIB", "/nonexistent/libtrophy_gl_shim.so");
  EXPECT_TRUE(GlShimEnrollment::libraryPath().isEmpty());
  EXPECT_FALSE(GlShimEnrollment::setEnrolled(true));
  EXPECT_FALSE(GlShimEnrollment::isEnrolled());
}

TEST_F(GlShimEnrollmentTest, InactiveWithoutPreload) {
  // This test process itself must not run preloaded; documents the API.
  if (GlShimEnrollment::isActiveInCurrentSession())
    GTEST_SKIP() << "test process runs with LD_PRELOAD (unusual)";
  EXPECT_FALSE(GlShimEnrollment::isActiveInCurrentSession());
}

TEST_F(GlShimEnrollmentTest, BundleEnrollmentSurvivesUnmountAndOldVersion) {
  qputenv("APPDIR", libDir.path().toUtf8());
  EXPECT_EQ(GlShimEnrollment::libraryPath(), fakeLib);
  EXPECT_FALSE(GlShimEnrollment::isEnrolled());
  EXPECT_FALSE(QFileInfo::exists(PersistentReporterLib::directory()));
  ASSERT_TRUE(GlShimEnrollment::setEnrolled(true));
  QFile conf(GlShimEnrollment::confFilePath());
  ASSERT_TRUE(conf.open(QIODevice::ReadOnly));
  const QByteArray original = conf.readAll();
  conf.close();
  const QString stable = PersistentReporterLib::install(fakeLib);
  ASSERT_TRUE(PersistentReporterLib::isOwned(stable, "libtrophy_gl_shim"));
  EXPECT_TRUE(original.contains(stable.toUtf8()));
  const auto modified = QFileInfo(stable).lastModified();
  ASSERT_TRUE(GlShimEnrollment::setEnrolled(true));
  EXPECT_EQ(QFileInfo(stable).lastModified(), modified);

  // A new bundle is available, but the old valid enrollment is still active.
  QFile next(fakeLib);
  ASSERT_TRUE(next.open(QIODevice::WriteOnly | QIODevice::Truncate));
  next.write("new version");
  next.close();
  EXPECT_TRUE(GlShimEnrollment::isEnrolled());
  ASSERT_TRUE(GlShimEnrollment::setEnrolled(true));
  const QString second = PersistentReporterLib::install(fakeLib);
  EXPECT_NE(stable, second);
  ASSERT_TRUE(conf.open(QIODevice::ReadOnly));
  EXPECT_TRUE(conf.readAll().contains(second.toUtf8()));
  conf.close();
  ASSERT_TRUE(QFile::remove(fakeLib));
  EXPECT_TRUE(GlShimEnrollment::isEnrolled());
  EXPECT_TRUE(QFile::exists(stable));
}

TEST_F(GlShimEnrollmentTest, PersistenceFailureKeepsEnrollment) {
  ASSERT_TRUE(GlShimEnrollment::setEnrolled(true));
  QFile conf(GlShimEnrollment::confFilePath());
  ASSERT_TRUE(conf.open(QIODevice::ReadOnly));
  const QByteArray original = conf.readAll();
  conf.close();
  qputenv("APPDIR", libDir.path().toUtf8());
  ASSERT_TRUE(QDir().mkpath(dataHome.path() + "/trophy"));
  QFile obstruction(PersistentReporterLib::directory());
  ASSERT_TRUE(obstruction.open(QIODevice::WriteOnly));
  obstruction.write("not a directory");
  obstruction.close();
  EXPECT_FALSE(GlShimEnrollment::setEnrolled(true));
  ASSERT_TRUE(conf.open(QIODevice::ReadOnly));
  EXPECT_EQ(conf.readAll(), original);
}

TEST_F(GlShimEnrollmentTest, InheritedAppDirDoesNotCopyNativeOverride) {
  qputenv("APPDIR", configHome.path().toUtf8());
  ASSERT_TRUE(GlShimEnrollment::setEnrolled(true));
  EXPECT_FALSE(QFileInfo::exists(PersistentReporterLib::directory()));
  EXPECT_TRUE(GlShimEnrollment::isEnrolled());
}
