/*
 * Trophy - Unit Tests
 * HardwareInfo: PCI vendor mapping + gather smoke test (never fatal,
 * degrades gracefully headless/driverless).
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "core/HardwareInfo.h"
#include "encoding/EncoderFactory.h"

using namespace TrophyBackend;

class HardwareInfoTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new HardwareInfoTestEnvironment);

TEST(HardwareInfoTest, VendorMapping) {
  EXPECT_EQ(HardwareInfo::vendorNameForId(0x10DE), "NVIDIA");
  EXPECT_EQ(HardwareInfo::vendorNameForId(0x1002), "AMD");
  EXPECT_EQ(HardwareInfo::vendorNameForId(0x8086), "Intel");
  EXPECT_TRUE(HardwareInfo::vendorNameForId(0x1234).startsWith("Unknown"));
  // Full PCI value with extra bits still maps on the low 16.
  EXPECT_EQ(HardwareInfo::vendorNameForId(0x110DE), "NVIDIA");
}

TEST(HardwareInfoTest, MarketingNamePrefersBracket) {
  EXPECT_EQ(HardwareInfo::marketingNameForChip("AD107 [GeForce RTX 4060]"),
            "GeForce RTX 4060");
  EXPECT_EQ(HardwareInfo::marketingNameForChip("Navi 31 [Radeon RX 7900 XTX]"),
            "Radeon RX 7900 XTX");
  EXPECT_EQ(HardwareInfo::marketingNameForChip(
                "Raptor Lake-S GT1 [UHD Graphics 770]"),
            "UHD Graphics 770");
  // No brackets: pass through (older pci.ids entries, plain names).
  EXPECT_EQ(
      HardwareInfo::marketingNameForChip("Rembrandt Radeon High Definition"),
      "Rembrandt Radeon High Definition");
  EXPECT_EQ(HardwareInfo::marketingNameForChip(""), "");
  EXPECT_EQ(HardwareInfo::marketingNameForChip("AD107 []"), "AD107 []");
  EXPECT_EQ(HardwareInfo::marketingNameForChip("[unclosed"), "[unclosed");
}

TEST(HardwareInfoTest, FakeSysfsSkipsVendorlessEntries) {  // Regression: simpledrm/efifb leftovers have no device/vendor file and
  // showed up as bare "Unknown" rows. Build a fake sysfs: one NVIDIA GPU
  // plus two vendorless leftovers.
  QTemporaryDir sysfs;
  ASSERT_TRUE(sysfs.isValid());
  auto mkCard = [&](const QString &card, const QString &vendor) {
    QDir().mkpath(sysfs.path() + "/" + card + "/device");
    if (!vendor.isEmpty()) {
      QFile f(sysfs.path() + "/" + card + "/device/vendor");
      EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
      f.write(vendor.toUtf8());
      f.close();
    }
  };
  mkCard("card0", "0x10de");
  mkCard("card1", "");
  mkCard("card2", "");

  const QByteArray oldEnv = qgetenv("TROPHY_DRM_SYSFS");
  qputenv("TROPHY_DRM_SYSFS", sysfs.path().toUtf8());
  HardwareInfo info;
  if (oldEnv.isNull())
    qunsetenv("TROPHY_DRM_SYSFS");
  else
    qputenv("TROPHY_DRM_SYSFS", oldEnv);

  ASSERT_EQ(info.gpuDescriptions().size(), 1);
  EXPECT_TRUE(info.gpuDescriptions().first().startsWith("NVIDIA"));
  EXPECT_TRUE(info.hasNvidiaGpu());
  EXPECT_FALSE(info.hasAmdGpu());
  EXPECT_FALSE(info.hasIntelGpu());
}

TEST(HardwareInfoTest, GatherNeverFatal) {
  EXPECT_NO_THROW({
    HardwareInfo info;
    // gpuDescriptions may be empty headless; the point is no crash and
    // sane, self-consistent state.
    for (const QString &gpu : info.gpuDescriptions())
      EXPECT_FALSE(gpu.isEmpty());
    if (!info.gpuDescriptions().isEmpty()) {
      EXPECT_TRUE(info.hasNvidiaGpu() || info.hasAmdGpu() ||
                  info.hasIntelGpu() ||
                  info.gpuDescriptions().first().startsWith("Unknown"));
    } else {
      EXPECT_FALSE(info.hasNvidiaGpu());
      EXPECT_FALSE(info.hasAmdGpu());
      EXPECT_FALSE(info.hasIntelGpu());
      EXPECT_FALSE(info.dmaBufAvailable());
    }
    // Without GPUs there is nothing for NVENC/VA-API to run on.
    if (!info.hasNvidiaGpu())
      EXPECT_FALSE(info.nvencAvailable());
    if (!info.hasAmdGpu() && !info.hasIntelGpu() && !info.hasNvidiaGpu())
      EXPECT_FALSE(info.vaapiAvailable());
  });
}

TEST(HardwareInfoTest, RecommendationAlwaysPresent) {
  if (EncoderFactory::createBest(true) == nullptr)
    GTEST_SKIP() << "no usable encoder in this FFmpeg build";
  HardwareInfo info;
  EXPECT_FALSE(info.recommendedEncoderName().isEmpty());
  EXPECT_FALSE(info.recommendedEncoderReason().isEmpty());
  // Display name is human-readable, never a raw id with underscores.
  EXPECT_FALSE(info.recommendedEncoderDisplayName().isEmpty());
  EXPECT_FALSE(info.recommendedEncoderDisplayName().contains('_'));
}

TEST(HardwareInfoTest, EncoderFlagsMatchFactory) {
  HardwareInfo info;
  // Flags must never claim more than the factory probes confirm.
  if (info.nvencAvailable())
    EXPECT_TRUE(EncoderFactory::isAvailable(
        IEncoder::EncoderType::NVENC_H264));
  if (info.vaapiAvailable())
    EXPECT_TRUE(EncoderFactory::isAvailable(
                    IEncoder::EncoderType::VAAPI_H264) ||
                EncoderFactory::isAvailable(
                    IEncoder::EncoderType::VAAPI_HEVC) ||
                EncoderFactory::isAvailable(
                    IEncoder::EncoderType::VAAPI_AV1));
}

TEST(HardwareInfoTest, DeviceNameResolvesFromFakeDb) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString db = dir.path() + "/pci.ids";
  QFile f(db);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
  f.write("# fake db\n"
          "10de  NVIDIA Corporation\n"
          "\t2882  AD107 [GeForce RTX 4060]\n"
          "\t1234  Some Future Card\n"
          "1002  Advanced Micro Devices, Inc. [AMD/ATI]\n"
          "\t744c  Navi 31 [Radeon RX 7900 XT]\n");
  f.close();

  EXPECT_EQ(HardwareInfo::deviceNameForIds(0x10DE, "0x2882", db),
            "AD107 [GeForce RTX 4060]");
  EXPECT_EQ(HardwareInfo::deviceNameForIds(0x1002, "744C", db),
            "Navi 31 [Radeon RX 7900 XT]");
  // Unknown device / vendor / missing db -> empty (caller falls back).
  EXPECT_TRUE(HardwareInfo::deviceNameForIds(0x10DE, "0xffff", db).isEmpty());
  EXPECT_TRUE(HardwareInfo::deviceNameForIds(0x1234, "0x2882", db).isEmpty());
  EXPECT_TRUE(
      HardwareInfo::deviceNameForIds(0x10DE, "0x2882", db + ".missing")
          .isEmpty());
  EXPECT_TRUE(HardwareInfo::deviceNameForIds(0x10DE, "bogus", db).isEmpty());
}
