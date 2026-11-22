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
  EXPECT_TRUE(report.contains(QStringLiteral("/usr/lib: libva.so.2")));
  EXPECT_TRUE(report.contains(QStringLiteral("/opt/app/usr/lib: libQt6Core.so.6")));
}

TEST(RuntimeLibraries, ReportGroupsExactDirectoriesAndSortsLibraryNamesWithinEachOrigin) {
  LibraryInventory inventory;
  inventory.host = {"/usr/lib/libz.so.1", "/usr/lib/qt6/plugins/libsame.so",
                    "/opt/driver libs/libsame.so", "/usr/lib/liba.so.2"};
  inventory.bundle = {"/opt/Trophy bundle/lib/libz.so", "/opt/Trophy bundle/lib/liba.so"};
  inventory.bundleRoots = {"/opt/Trophy bundle"};
  const QString expected =
      "Runtime libraries: 4 from the host, 2 from the bundle\n"
      "  host (4):\n"
      "    /opt/driver libs: libsame.so\n"
      "    /usr/lib: liba.so.2, libz.so.1\n"
      "    /usr/lib/qt6/plugins: libsame.so\n"
      "  bundle under /opt/Trophy bundle (2):\n"
      "    /opt/Trophy bundle/lib: liba.so, libz.so";
  EXPECT_EQ(formatLibraryInventory(inventory), expected);
  inventory.host = {"/usr/lib/liba.so.2", "/opt/driver libs/libsame.so",
                    "/usr/lib/qt6/plugins/libsame.so", "/usr/lib/libz.so.1"};
  EXPECT_EQ(formatLibraryInventory(inventory), expected);
}

TEST(RuntimeLibraries, GroupedReportRetainsEntryLimitAndFullCounts) {
  LibraryInventory inventory;
  for (int i = 0; i < 405; ++i)
    inventory.host.append(QString("/usr/lib/libfixture%1.so").arg(i, 3, 10, QLatin1Char('0')));
  const auto report = formatLibraryInventory(inventory);
  EXPECT_TRUE(report.startsWith("Runtime libraries: 405 from the host, 0 from the bundle\n"));
  EXPECT_TRUE(report.contains("  host (405):\n"));
  EXPECT_TRUE(report.contains("/usr/lib: libfixture000.so, libfixture001.so"));
  EXPECT_TRUE(report.contains("libfixture399.so"));
  EXPECT_FALSE(report.contains("libfixture400.so"));
  EXPECT_TRUE(report.contains("    ... 5 more\n"));
  EXPECT_TRUE(report.endsWith("  bundle (0):"));
  EXPECT_EQ(report.count("/usr/lib"), 1);
}

TEST(RuntimeLibraries, EmptyGroupedReportKeepsBothOriginSections) {
  EXPECT_EQ(formatLibraryInventory({}),
      "Runtime libraries: 0 from the host, 0 from the bundle\n"
      "  host (0):\n"
      "  bundle (0):");
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

TEST(RuntimeLibraries, NestedBundleRootsAreCollapsed) {
  EXPECT_EQ(mergeBundleRoots({QStringLiteral("/opt/app"),
                              QStringLiteral("/opt/app/usr"),
                              QStringLiteral("/prefix")}),
            (QStringList{QStringLiteral("/opt/app"), QStringLiteral("/prefix")}));
}

TEST(RuntimeLibraries, MappedLibraryPathFindsLiveMappings) {
  // The VA-API diagnostic uses this; /proc reports size 0, so a QFile-based
  // implementation that trusts atEnd() silently returns nothing.
  const QString libc = mappedLibraryPath(QStringLiteral("libc.so.6"));
  EXPECT_FALSE(libc.isEmpty());
  EXPECT_TRUE(libc.startsWith(QStringLiteral("/")));
  EXPECT_TRUE(mappedLibraryPath(QStringLiteral("libtrophy-does-not-exist.so.9")).isEmpty());
}

TEST(RuntimeLibraries, OriginUsesAppDir) {
  const QByteArray previous = qgetenv("APPDIR");
  qputenv("APPDIR", "/opt/trophy");
  EXPECT_EQ(libraryOrigin(QStringLiteral("/opt/trophy/usr/lib/libQt6Core.so.6")),
            QStringLiteral("bundled"));
  EXPECT_EQ(libraryOrigin(QStringLiteral("/usr/lib/libva.so.2")), QStringLiteral("host"));
  EXPECT_EQ(libraryOrigin(QString()), QStringLiteral("unknown"));
  if (previous.isEmpty())
    qunsetenv("APPDIR");
  else
    qputenv("APPDIR", previous);
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
