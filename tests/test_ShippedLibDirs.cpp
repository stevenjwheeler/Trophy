/*
 * Trophy - Unit Tests
 * ShippedLibDirs: prefix-relative helper-library lookup shared by the
 * Vulkan layer registration and the GL shim enrollment.
 *
 * Locks the core contract: entries are directories — findShippedLib appends
 * the library file name itself, so passing a full file path as a search
 * dir silently matches nothing (this exact mistake broke GL shim lookup
 * in every build-tree run).
 */

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "detection/ShippedLibDirs.h"

using namespace TrophyBackend;

TEST(ShippedLibDirsTest, FindsLibInFirstTryDir) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString lib = dir.path() + "/libtrophy_probe.so";
  QFile f(lib);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write("fake");
  f.close();

  EXPECT_EQ(findShippedLib("libtrophy_probe.so", {dir.path()}), lib);
}

TEST(ShippedLibDirsTest, FilePathEntryMatchesNothing) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString lib = dir.path() + "/libtrophy_probe.so";
  QFile f(lib);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write("fake");
  f.close();

  // A file is not a search dir: "<file>/<lib>" exists nowhere.
  EXPECT_TRUE(findShippedLib("libtrophy_probe.so", {lib}).isEmpty());
  EXPECT_TRUE(findShippedLib("libtrophy_nonexistent_1234.so").isEmpty());
}

TEST(ShippedLibDirsTest, BinaryDirStripsTrailingBin) {
  // Install layouts (AppImage /usr/bin, ~/.local/bin, /usr/bin) resolve
  // to the prefix; anything else (build trees) is used as-is.
  EXPECT_EQ(prefixFromBinaryDir("/opt/trophy/bin"), "/opt/trophy");
  EXPECT_EQ(prefixFromBinaryDir("/home/u/.local/bin"), "/home/u/.local");
  EXPECT_EQ(prefixFromBinaryDir("/usr/bin"), "/usr");
  EXPECT_EQ(prefixFromBinaryDir("/tmp/opencode/trophy-build"),
            "/tmp/opencode/trophy-build");
}

TEST(ShippedLibDirsTest, AppDirRootResolvesIntoUsr) {
  // AppImage launches report the mount root (AppRun symlink), not
  // usr/bin: a root carrying usr/bin/ (and no bin/ of its own) is an
  // AppDir whose prefix is its usr/ subtree.
  QTemporaryDir appDir;
  ASSERT_TRUE(appDir.isValid());
  ASSERT_TRUE(QDir().mkpath(appDir.path() + "/usr/bin"));
  EXPECT_EQ(prefixFromBinaryDir(appDir.path()),
            QDir(appDir.path() + "/usr").absolutePath());
}

TEST(ShippedLibDirsTest, ManifestDirYieldsPrefixLibDir) {
  QTemporaryDir prefix;
  ASSERT_TRUE(prefix.isValid());
  ASSERT_TRUE(QDir().mkpath(prefix.path() + "/share/trophy/vulkan_layer"));
  ASSERT_TRUE(QDir().mkpath(prefix.path() + "/lib/trophy"));
  const QString manifest =
      prefix.path() + "/share/trophy/vulkan_layer/trophy_layer.json";
  const QString lib = prefix.path() + "/lib/trophy/libtrophy_probe.so";
  QFile mf(manifest);
  ASSERT_TRUE(mf.open(QIODevice::WriteOnly | QIODevice::Text));
  mf.write("{}");
  mf.close();
  QFile lf(lib);
  ASSERT_TRUE(lf.open(QIODevice::WriteOnly));
  lf.write("fake");
  lf.close();

  EXPECT_EQ(prefixFromManifest(manifest), prefix.path());
  const QStringList dirs = shippedLibDirs(manifest);
  ASSERT_FALSE(dirs.isEmpty());
  EXPECT_EQ(dirs.front(), prefix.path() + "/lib/trophy");
  EXPECT_EQ(findShippedLib("libtrophy_probe.so", {}, manifest), lib);
}
