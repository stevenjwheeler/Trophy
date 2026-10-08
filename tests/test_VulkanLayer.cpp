/*
 * Trophy - Unit Tests
 * VulkanLayer: structural manifest writing (the regression), registration
 * validation, remove round-trip. Hermetic via temp XDG_DATA_HOME + a fake
 * source tree; never touches the real implicit_layer.d.
 */

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "detection/VulkanLayer.h"

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
  QString fakeManifest;
  QString fakeLib;

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
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
  }

  void TearDown() override {
    if (oldXdgDataHome.isNull())
      qunsetenv("XDG_DATA_HOME");
    else
      qputenv("XDG_DATA_HOME", oldXdgDataHome);
  }
};

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
