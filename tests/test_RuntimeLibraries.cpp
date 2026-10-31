/*
 * Trophy - Unit Tests
 * RuntimeLibraries: bundle/host classification for the startup inventory.
 */

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "core/RuntimeLibraries.h"

using namespace TrophyBackend;

namespace {

QString writeMaps(const QTemporaryDir &dir, const QString &contents) {
  const QString path = dir.path() + QStringLiteral("/maps");
  QFile file(path);
  EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
  file.write(contents.toUtf8());
  file.close();
  return path;
}

QString mapsLine(const QString &path) {
  return QStringLiteral("7f0000000000-7f0000001000 r-xp 00000000 08:01 42 ") + path +
         QLatin1Char('\n');
}

} // namespace

TEST(RuntimeLibraries, SplitsHostAndBundleAndDeduplicates) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString maps = writeMaps(
      dir,
      mapsLine(QStringLiteral("/opt/app/usr/lib/libQt6Core.so.6")) +
          mapsLine(QStringLiteral("/opt/app/usr/lib/libQt6Core.so.6")) +
          mapsLine(QStringLiteral("/usr/lib/x86_64-linux-gnu/libva.so.2.2000.0")) +
          QStringLiteral("7f--00 rw-p 00000000 00:00 0 [vdso]\n") +
          QStringLiteral("7f--00 rw-p 00000000 00:00 0 \n"));

  const auto inventory = collectLibraryInventory(maps, {QStringLiteral("/opt/app")});
  ASSERT_EQ(inventory.host.size(), 1);
  ASSERT_EQ(inventory.bundle.size(), 1);
  EXPECT_EQ(inventory.host.first(), QStringLiteral("/usr/lib/x86_64-linux-gnu/libva.so.2.2000.0"));
  EXPECT_EQ(inventory.bundle.first(), QStringLiteral("/opt/app/usr/lib/libQt6Core.so.6"));
}

TEST(RuntimeLibraries, DeletedSuffixIsStripped) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString maps = writeMaps(
      dir, mapsLine(QStringLiteral("/usr/lib/libva.so.2 (deleted)")));

  const auto inventory = collectLibraryInventory(maps, {QStringLiteral("/opt/app")});
  ASSERT_EQ(inventory.host.size(), 1);
  EXPECT_EQ(inventory.host.first(), QStringLiteral("/usr/lib/libva.so.2"));
}

TEST(RuntimeLibraries, BundleRootBoundaryIsExact) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  // /opt/application must not be classified as bundle just because the root is
  // /opt/app.
  const QString maps = writeMaps(
      dir, mapsLine(QStringLiteral("/opt/application/lib/libfoo.so.1")));

  const auto inventory = collectLibraryInventory(maps, {QStringLiteral("/opt/app")});
  EXPECT_TRUE(inventory.bundle.isEmpty());
  ASSERT_EQ(inventory.host.size(), 1);
}

TEST(RuntimeLibraries, MultipleRootsAreAllBundle) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString maps =
      writeMaps(dir, mapsLine(QStringLiteral("/prefix/lib/libTrophy.so")) +
                         mapsLine(QStringLiteral("/opt/app/usr/lib/libavcodec.so.60")));

  const auto inventory = collectLibraryInventory(
      maps, {QStringLiteral("/opt/app"), QStringLiteral("/prefix")});
  EXPECT_TRUE(inventory.host.isEmpty());
  EXPECT_EQ(inventory.bundle.size(), 2);
  // Sorted for stable logs.
  EXPECT_EQ(inventory.bundle.first(), QStringLiteral("/opt/app/usr/lib/libavcodec.so.60"));
}

TEST(RuntimeLibraries, MissingMapsFileIsEmpty) {
  const auto inventory = collectLibraryInventory(
      QStringLiteral("/nonexistent/maps"), {QStringLiteral("/opt/app")});
  EXPECT_TRUE(inventory.host.isEmpty());
  EXPECT_TRUE(inventory.bundle.isEmpty());
}

TEST(RuntimeLibraries, ReportNamesBothSectionsAndCounts) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString maps =
      writeMaps(dir, mapsLine(QStringLiteral("/opt/app/usr/lib/libQt6Core.so.6")) +
                         mapsLine(QStringLiteral("/usr/lib/libva.so.2")));

  const QString report = formatLibraryInventory(
      collectLibraryInventory(maps, {QStringLiteral("/opt/app")}));
  EXPECT_TRUE(report.contains(QStringLiteral("1 from the host, 1 from the bundle")));
  EXPECT_TRUE(report.contains(QStringLiteral("host (1):")));
  EXPECT_TRUE(report.contains(QStringLiteral("bundle under /opt/app (1):")));
  EXPECT_TRUE(report.contains(QStringLiteral("/usr/lib/libva.so.2")));
  EXPECT_TRUE(report.contains(QStringLiteral("/opt/app/usr/lib/libQt6Core.so.6")));
}

TEST(RuntimeLibraries, NonLibrariesAreIgnored) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString maps =
      writeMaps(dir, mapsLine(QStringLiteral("/etc/ld.so.cache")) +
                         mapsLine(QStringLiteral("/dev/nvidia0")) +
                         mapsLine(QStringLiteral("/memfd:JITCode:QtQml")) +
                         mapsLine(QStringLiteral("/usr/lib/gconv/gconv-modules.cache")) +
                         mapsLine(QStringLiteral("/usr/lib/libfoo.so")) +
                         mapsLine(QStringLiteral("/usr/lib/libfoo.so.1.2.3")) +
                         mapsLine(QStringLiteral("/usr/lib/libfoo.so.bak")));

  const auto inventory = collectLibraryInventory(maps, {QStringLiteral("/opt/app")});
  ASSERT_EQ(inventory.host.size(), 2);
  EXPECT_EQ(inventory.host.at(0), QStringLiteral("/usr/lib/libfoo.so"));
  EXPECT_EQ(inventory.host.at(1), QStringLiteral("/usr/lib/libfoo.so.1.2.3"));
}

TEST(RuntimeLibraries, LiveProcessInventoryIsUsable) {
  // /proc/self/maps reports a size of 0, which makes Qt's regular-file helpers
  // think the file is empty; this asserts the real process is actually read.
  const auto inventory = collectLibraryInventory(
      QStringLiteral("/proc/self/maps"), {QStringLiteral("/definitely-not-our-root")});
  EXPECT_FALSE(inventory.host.isEmpty());
  EXPECT_TRUE(inventory.bundle.isEmpty());

  const QString report = runtimeLibraryReport();
  EXPECT_TRUE(report.startsWith(QStringLiteral("Runtime libraries:")));
  EXPECT_FALSE(report.contains(QStringLiteral("0 from the host")));
}
