#include <QCoreApplication>
#include <gtest/gtest.h>

#include "core/InstallInfo.h"

using namespace TrophyBackend;

class InstallInfoTestEnvironment : public ::testing::Environment {
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
static auto *env = testing::AddGlobalTestEnvironment(new InstallInfoTestEnvironment);

TEST(InstallInfoTest, NamesAndCommands) {
  EXPECT_EQ(InstallInfo::name(InstallKind::AppImage), "AppImage");
  EXPECT_EQ(InstallInfo::name(InstallKind::DebianPackage), "Debian package");
  EXPECT_EQ(InstallInfo::name(InstallKind::RpmPackage), "RPM package");
  EXPECT_EQ(InstallInfo::name(InstallKind::PortablePrefix), "Portable install");
  EXPECT_EQ(InstallInfo::name(InstallKind::Unknown), "Unknown");

  EXPECT_EQ(InstallInfo::manualCommand(InstallKind::DebianPackage, "Trophy-1.0.0-amd64.deb"),
            "sudo apt install ./Trophy-1.0.0-amd64.deb");
  EXPECT_EQ(InstallInfo::manualCommand(InstallKind::RpmPackage, "Trophy-1.0.0-x86_64.rpm"),
            "sudo dnf install ./Trophy-1.0.0-x86_64.rpm");
  EXPECT_TRUE(InstallInfo::manualCommand(InstallKind::AppImage, "x").isEmpty());
  EXPECT_TRUE(InstallInfo::manualCommand(InstallKind::Unknown, "x").isEmpty());
}