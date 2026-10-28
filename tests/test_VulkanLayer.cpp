/*
 * Trophy - Unit Tests
 * VulkanLayer: structural manifest writing (the regression), registration
 * validation, remove round-trip. Hermetic via temp XDG_DATA_HOME + a fake
 * source tree; never touches the real implicit_layer.d.
 */

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <gtest/gtest.h>

#include "detection/VulkanLayer.h"
#include "detection/PersistentReporterLib.h"

using namespace TrophyBackend;

class VulkanLayerTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new VulkanLayerTestEnvironment);

class VulkanLayerTest : public ::testing::Test {
protected:
  QTemporaryDir dataHome;
  QTemporaryDir srcTree;
  QByteArray oldXdgDataHome;
  QByteArray oldAppDir;
  QString fakeManifest;
  QString fakeLib;

  bool elf(const QString &path, int bits, const QByteArray &version = {}) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QByteArray bytes(20, '\0'); bytes.replace(0, 4, QByteArray("\x7f" "ELF", 4));
    bytes[4] = bits == 32 ? 1 : 2; bytes[5] = 1; bytes[6] = 1;
    bytes[16] = 3; bytes[18] = bits == 32 ? 3 : 62;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes + version) == bytes.size() + version.size();
  }
  QString companion() const { return srcTree.path() + "/reporters32/libtrophy_layer.so"; }
  QByteArray content() const {
    QFile file(VulkanLayer::destManifestPath());
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
  }

  void SetUp() override {
    ASSERT_TRUE(dataHome.isValid());
    ASSERT_TRUE(srcTree.isValid());

    fakeManifest = srcTree.path() + "/trophy_layer.json";
    fakeLib = srcTree.path() + "/libtrophy_layer.so";
    QFile mf(fakeManifest);
    ASSERT_TRUE(mf.open(QIODevice::WriteOnly | QIODevice::Text));
    // Deliberately contains a placeholder path: the writer must replace
    // it structurally, never by substring surgery.
    mf.write("{\"layer\": {\"name\": \"VK_LAYER_TROPHY_detection\", "
             "\"library_path\": \"trophy_layer.so\"}}");
    mf.close();
    QFile lib(fakeLib);
    ASSERT_TRUE(lib.open(QIODevice::WriteOnly));
    lib.write("fake");
    lib.close();

    oldXdgDataHome = qgetenv("XDG_DATA_HOME");
    oldAppDir = qgetenv("APPDIR");
    qunsetenv("APPDIR");
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
  }

  void TearDown() override {
    if (oldAppDir.isNull())
      qunsetenv("APPDIR");
    else
      qputenv("APPDIR", oldAppDir);
    if (oldXdgDataHome.isNull())
      qunsetenv("XDG_DATA_HOME");
    else
      qputenv("XDG_DATA_HOME", oldXdgDataHome);
  }
};

TEST_F(VulkanLayerTest, CompanionLookupAndStatusNeverEnroll) {
  ASSERT_TRUE(elf(fakeLib, 64)); ASSERT_TRUE(elf(companion(), 32));
  EXPECT_EQ(VulkanLayer::libraryPath32ForSource(fakeManifest), companion());
  EXPECT_FALSE(VulkanLayer::isRegistered()); EXPECT_FALSE(VulkanLayer::is32BitRegistered());
  EXPECT_FALSE(QFileInfo::exists(PersistentReporterLib::directory()));
}

TEST_F(VulkanLayerTest, VersionComesFromRegisteredFileNotCurrentPackage) {
  ASSERT_TRUE(elf(fakeLib, 64, QByteArray("TROPHY_HELPER_VERSION:vulkan:7") + '\0'));
  qputenv("APPDIR", srcTree.path().toUtf8());
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath()));
  EXPECT_EQ(VulkanLayer::helperVersion(), 7);
  ASSERT_TRUE(elf(fakeLib, 64, QByteArray("TROPHY_HELPER_VERSION:vulkan:8") + '\0'));
  EXPECT_EQ(VulkanLayer::helperVersion(), 7);
  const QString bundledDir = dataHome.path() + "/trophy/vulkan_layer";
  ASSERT_TRUE(QDir().mkpath(bundledDir));
  ASSERT_TRUE(QFile::copy(fakeManifest, bundledDir + "/trophy_layer.json"));
  ASSERT_TRUE(QFile::copy(fakeLib, bundledDir + "/libtrophy_layer.so"));
  EXPECT_EQ(VulkanLayer::bundledHelperVersion(), 8);
}

TEST_F(VulkanLayerTest, BothArchitecturesPublishInOneAtomicManifestAndOptOutTogether) {
  ASSERT_TRUE(elf(fakeLib, 64)); ASSERT_TRUE(elf(companion(), 32));
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath(), companion()));
  const auto root = QJsonDocument::fromJson(content()).object();
  EXPECT_EQ(root.value("file_format_version").toString(), "1.2.1");
  EXPECT_FALSE(root.contains("layer"));
  const auto layers = root.value("layers").toArray(); ASSERT_EQ(layers.size(), 2);
  EXPECT_EQ(layers[0].toObject().value("library_arch").toString(), "64");
  EXPECT_EQ(layers[1].toObject().value("library_arch").toString(), "32");
  EXPECT_NE(layers[0].toObject().value("name"), layers[1].toObject().value("name"));
  EXPECT_TRUE(VulkanLayer::isRegistered()); EXPECT_TRUE(VulkanLayer::is32BitRegistered());
  ASSERT_TRUE(VulkanLayer::removeRegistration());
  EXPECT_FALSE(VulkanLayer::isRegistered()); EXPECT_FALSE(VulkanLayer::is32BitRegistered());
}

TEST_F(VulkanLayerTest, BadCompanionCannotReplaceExistingManifest) {
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath()));
  const QByteArray previous = content();
  ASSERT_TRUE(elf(fakeLib, 64)); ASSERT_TRUE(elf(companion(), 64));
  EXPECT_FALSE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath(), companion()));
  EXPECT_EQ(content(), previous);
  ASSERT_TRUE(QFile::remove(companion())); ASSERT_TRUE(QFile::link(fakeLib, companion()));
  EXPECT_FALSE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath(), companion()));
  EXPECT_EQ(content(), previous);
}

TEST_F(VulkanLayerTest, BundledArchitecturePairSurvivesUnmountAndRefreshKeepsOldVersions) {
  ASSERT_TRUE(elf(fakeLib, 64)); ASSERT_TRUE(elf(companion(), 32));
  qputenv("APPDIR", srcTree.path().toUtf8());
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath(), companion()));
  const auto first = QJsonDocument::fromJson(content()).object().value("layers").toArray();
  const QString old32 = first[1].toObject().value("library_path").toString();
  ASSERT_TRUE(PersistentReporterLib::isOwned(old32, "libtrophy_layer32"));
  ASSERT_TRUE(elf(companion(), 32, "updated"));
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath(), companion()));
  const auto second = QJsonDocument::fromJson(content()).object().value("layers").toArray();
  EXPECT_NE(second[1].toObject().value("library_path").toString(), old32);
  EXPECT_TRUE(QFileInfo::exists(old32));
  ASSERT_TRUE(QFile::remove(fakeLib)); ASSERT_TRUE(QFile::remove(companion()));
  EXPECT_TRUE(VulkanLayer::isRegistered()); EXPECT_TRUE(VulkanLayer::is32BitRegistered());
}

TEST_F(VulkanLayerTest, NoCompanionRemovesOnlyTheManaged32BitEntryAndUnchangedWriteKeepsMtime) {
  ASSERT_TRUE(elf(fakeLib, 64)); ASSERT_TRUE(elf(companion(), 32));
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath(), companion()));
  QFile file(VulkanLayer::destManifestPath()); ASSERT_TRUE(file.open(QIODevice::ReadWrite));
  const auto sentinel = QDateTime::fromSecsSinceEpoch(1000000000);
  ASSERT_TRUE(file.setFileTime(sentinel, QFileDevice::FileModificationTime)); file.close();
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath(), companion()));
  EXPECT_EQ(QFileInfo(file).lastModified(), sentinel);
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib, VulkanLayer::destManifestPath()));
  EXPECT_TRUE(VulkanLayer::isRegistered()); EXPECT_FALSE(VulkanLayer::is32BitRegistered());
  EXPECT_TRUE(QJsonDocument::fromJson(content()).object().contains("layer"));
}

TEST_F(VulkanLayerTest, StartupDoesNotMigrateLegacyConsentButExplicitToggleCan) {
  ASSERT_TRUE(elf(fakeLib, 64)); ASSERT_TRUE(elf(companion(), 32));
  ASSERT_TRUE(QDir().mkpath(dataHome.path() + "/trophy"));
  ASSERT_TRUE(QFile::link(srcTree.path(), dataHome.path() + "/trophy/vulkan_layer"));
  ASSERT_TRUE(VulkanLayer::ensureRegistered());
  EXPECT_FALSE(VulkanLayer::is32BitRegistered());
  EXPECT_EQ(QJsonDocument::fromJson(content()).object().value("file_format_version").toString(), "1.0.0");
  ASSERT_TRUE(VulkanLayer::ensureRegistered(true));
  EXPECT_TRUE(VulkanLayer::is32BitRegistered());
  ASSERT_TRUE(VulkanLayer::ensureRegistered()); // Refresh already-paired consent.
  EXPECT_TRUE(VulkanLayer::is32BitRegistered());
}

TEST_F(VulkanLayerTest, WriteManifestSetsExactLibraryPath) {
  // Regression: string replacement corrupted the CMake-expanded path into
  // ".../lib/tmp/.../trophy_layer.so", silently disabling all Vulkan
  // detection. The writer must set library_path exactly.
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib,
                                         VulkanLayer::destManifestPath()));

  QFile dst(VulkanLayer::destManifestPath());
  ASSERT_TRUE(dst.open(QIODevice::ReadOnly | QIODevice::Text));
  const QString content = QString::fromUtf8(dst.readAll());
  EXPECT_TRUE(content.contains(fakeLib));
  // The old string-surgery bug doubled the path (".../lib/tmp/...").
  EXPECT_FALSE(content.contains("lib/tmp"));
  EXPECT_TRUE(VulkanLayer::isRegistered());
}

TEST_F(VulkanLayerTest, GarbageLibraryPathIsNotRegistered) {
  ASSERT_TRUE(VulkanLayer::writeManifest(
      fakeManifest, "/nonexistent/libtrophy_layer.so",
      VulkanLayer::destManifestPath()));
  // File exists but points nowhere: must NOT count as registered.
  EXPECT_FALSE(VulkanLayer::isRegistered());
}

TEST_F(VulkanLayerTest, InvalidSourceManifestFails) {
  EXPECT_FALSE(VulkanLayer::writeManifest(
      srcTree.path() + "/missing.json", fakeLib,
      VulkanLayer::destManifestPath()));
  EXPECT_FALSE(VulkanLayer::isRegistered());
}

TEST_F(VulkanLayerTest, FindsLibraryInCustomPrefix) {
  // `cmake --install --prefix ~/.local` puts the manifest under
  // <prefix>/share/trophy/vulkan_layer/ and the lib in <prefix>/lib/trophy.
  // A hardcoded /usr-only list missed that, so the layer silently stopped
  // registering once the dev build tree was gone.
  const QString prefix = srcTree.path() + "/prefix";
  const QString manifestDir = prefix + "/share/trophy/vulkan_layer";
  const QString libDir = prefix + "/lib/trophy";
  ASSERT_TRUE(QDir().mkpath(manifestDir));
  ASSERT_TRUE(QDir().mkpath(libDir));

  const QString manifest = manifestDir + "/trophy_layer.json";
  QFile mf(manifest);
  ASSERT_TRUE(mf.open(QIODevice::WriteOnly | QIODevice::Text));
  mf.write("{\"layer\": {\"name\": \"VK_LAYER_TROPHY_detection\"}}");
  mf.close();

  const QString lib = libDir + "/libtrophy_layer.so";
  QFile lf(lib);
  ASSERT_TRUE(lf.open(QIODevice::WriteOnly));
  lf.write("fake");
  lf.close();

  EXPECT_EQ(VulkanLayer::libraryPathForSource(manifest), lib);
  // Full end-to-end: the written manifest must point at the real lib.
  ASSERT_TRUE(VulkanLayer::writeManifest(manifest, lib,
                                         VulkanLayer::destManifestPath()));
  EXPECT_TRUE(VulkanLayer::isRegistered());
}

TEST_F(VulkanLayerTest, MissingLibraryIsNotResolved) {
  EXPECT_TRUE(VulkanLayer::libraryPathForSource(
                  srcTree.path() + "/prefix/share/trophy/vulkan_layer/nope.json")
                  .isEmpty());
  // And ensureRegistered must not claim success or write a broken manifest.
  EXPECT_FALSE(VulkanLayer::ensureRegistered());
  EXPECT_FALSE(VulkanLayer::isRegistered());
}

TEST_F(VulkanLayerTest, RemoveRegistrationRoundTrip) {
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib,
                                         VulkanLayer::destManifestPath()));
  ASSERT_TRUE(VulkanLayer::isRegistered());
  EXPECT_TRUE(VulkanLayer::removeRegistration());
  EXPECT_FALSE(QFile::exists(VulkanLayer::destManifestPath()));
  EXPECT_FALSE(VulkanLayer::isRegistered());
  // Removing twice is fine (opt-out is idempotent).
  EXPECT_TRUE(VulkanLayer::removeRegistration());
}

TEST_F(VulkanLayerTest, BundleManifestSurvivesUnmountAndReusesLibrary) {
  qputenv("APPDIR", srcTree.path().toUtf8());
  EXPECT_EQ(VulkanLayer::libraryPathForSource(fakeManifest), fakeLib);
  EXPECT_FALSE(QFileInfo::exists(PersistentReporterLib::directory()));
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib,
                                        VulkanLayer::destManifestPath()));
  const QString stable = PersistentReporterLib::install(fakeLib);
  ASSERT_TRUE(PersistentReporterLib::isOwned(stable, "libtrophy_layer"));
  const auto modified = QFileInfo(stable).lastModified();
  EXPECT_EQ(PersistentReporterLib::install(fakeLib), stable);
  EXPECT_EQ(QFileInfo(stable).lastModified(), modified);
  QFile manifest(VulkanLayer::destManifestPath());
  ASSERT_TRUE(manifest.open(QIODevice::ReadOnly));
  EXPECT_TRUE(manifest.readAll().contains(stable.toUtf8()));
  ASSERT_TRUE(QFile::remove(fakeLib));
  ASSERT_TRUE(QFile::remove(fakeManifest));
  EXPECT_TRUE(VulkanLayer::isRegistered());
  EXPECT_TRUE(QFile::exists(stable));
}

TEST_F(VulkanLayerTest, CorruptDurableLibraryDoesNotReplaceManifest) {
  ASSERT_TRUE(VulkanLayer::writeManifest(fakeManifest, fakeLib,
                                        VulkanLayer::destManifestPath()));
  QFile manifest(VulkanLayer::destManifestPath());
  ASSERT_TRUE(manifest.open(QIODevice::ReadOnly));
  const QByteArray original = manifest.readAll();
  manifest.close();
  qputenv("APPDIR", srcTree.path().toUtf8());
  const QString stable = PersistentReporterLib::install(fakeLib);
  ASSERT_FALSE(stable.isEmpty());
  ASSERT_TRUE(QFile::setPermissions(stable, QFileDevice::ReadOwner |
                                          QFileDevice::WriteOwner));
  QFile damaged(stable);
  ASSERT_TRUE(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate));
  damaged.write("corrupt");
  damaged.close();
  EXPECT_FALSE(VulkanLayer::writeManifest(fakeManifest, fakeLib,
                                         VulkanLayer::destManifestPath()));
  ASSERT_TRUE(manifest.open(QIODevice::ReadOnly));
  EXPECT_EQ(manifest.readAll(), original);
}

TEST_F(VulkanLayerTest, InheritedAppDirAndOutsideSymlinkStayNative) {
  qputenv("APPDIR", dataHome.path().toUtf8());
  EXPECT_EQ(PersistentReporterLib::install(fakeLib), fakeLib);
  const QString link = dataHome.path() + "/libtrophy_layer.so";
  ASSERT_TRUE(QFile::link(fakeLib, link));
  EXPECT_EQ(PersistentReporterLib::install(link), link);
  EXPECT_FALSE(QFileInfo::exists(PersistentReporterLib::directory()));
}

TEST_F(VulkanLayerTest, OptInRefreshesManifestToNewDurableVersion) {
  // Supply the normal installed-datadir fallback, pointing into a fake AppDir.
  ASSERT_TRUE(QDir().mkpath(dataHome.path() + "/trophy"));
  ASSERT_TRUE(QFile::link(srcTree.path(),
                         dataHome.path() + "/trophy/vulkan_layer"));
  qputenv("APPDIR", srcTree.path().toUtf8());
  ASSERT_EQ(VulkanLayer::sourceManifestPath(),
            dataHome.path() + "/trophy/vulkan_layer/trophy_layer.json");
  ASSERT_TRUE(VulkanLayer::ensureRegistered());
  const QString first = PersistentReporterLib::install(fakeLib);
  QFile updated(fakeLib);
  ASSERT_TRUE(updated.open(QIODevice::WriteOnly | QIODevice::Truncate));
  updated.write("new version");
  updated.close();
  EXPECT_TRUE(VulkanLayer::isRegistered());
  ASSERT_TRUE(VulkanLayer::ensureRegistered());
  const QString second = PersistentReporterLib::install(fakeLib);
  EXPECT_NE(first, second);
  EXPECT_TRUE(QFile::exists(first));
  QFile manifest(VulkanLayer::destManifestPath());
  ASSERT_TRUE(manifest.open(QIODevice::ReadOnly));
  EXPECT_TRUE(manifest.readAll().contains(second.toUtf8()));
}
