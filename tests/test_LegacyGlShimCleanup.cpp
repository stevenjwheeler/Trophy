/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/LegacyGlShimCleanup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using TrophyBackend::LegacyGlShimCleanup::confFilePath;
using TrophyBackend::LegacyGlShimCleanup::removeOwnedEnrollment;

namespace {

void ensureApplication() {
  if (!QCoreApplication::instance()) {
    static int argc = 1;
    static char arg0[] = "test";
    static char *argv[] = {arg0, nullptr};
    new QCoreApplication(argc, argv);
  }
}

class LegacyGlShimCleanupTest : public ::testing::Test {
protected:
  QTemporaryDir config;
  QByteArray oldConfig;

  void SetUp() override {
    ensureApplication();
    ASSERT_TRUE(config.isValid());
    oldConfig = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    ASSERT_TRUE(QDir().mkpath(QFileInfo(confFilePath()).absolutePath()));
  }

  void TearDown() override {
    if (oldConfig.isNull())
      qunsetenv("XDG_CONFIG_HOME");
    else
      qputenv("XDG_CONFIG_HOME", oldConfig);
  }

  void write(const QByteArray &content) {
    QFile file(confFilePath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(content), content.size());
  }

  QByteArray read() const {
    QFile file(confFilePath());
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
  }
};

} // namespace

TEST_F(LegacyGlShimCleanupTest, RemovesOwnedEntryAndKeepsForeignOnes) {
  write("FOREIGN=/x\n"
        "LD_PRELOAD=/opt/trophy/libtrophy_gl_shim.so:/foreign.so\n"
        "OTHER=1\n");
  ASSERT_TRUE(removeOwnedEnrollment());
  EXPECT_EQ(read(), "FOREIGN=/x\nLD_PRELOAD=/foreign.so\nOTHER=1\n");
}

TEST_F(LegacyGlShimCleanupTest, DeletesFileWhenOnlyOwnedEntryRemains) {
  write("LD_PRELOAD=/opt/trophy/libtrophy_gl_shim.so\n");
  ASSERT_TRUE(removeOwnedEnrollment());
  EXPECT_FALSE(QFile::exists(confFilePath()));
}

TEST_F(LegacyGlShimCleanupTest, RemovesDurableMultilibTokenEntry) {
  write("LD_PRELOAD=/home/u/.local/share/trophy/reporters/"
        "gl-multilib-v1-abcdef/$PLATFORM/libtrophy_gl_shim.so:/keep.so\n");
  ASSERT_TRUE(removeOwnedEnrollment());
  EXPECT_EQ(read(), "LD_PRELOAD=/keep.so\n");
}

TEST_F(LegacyGlShimCleanupTest, MissingFileIsNoOp) {
  EXPECT_TRUE(removeOwnedEnrollment());
  EXPECT_FALSE(QFile::exists(confFilePath()));
}

TEST_F(LegacyGlShimCleanupTest, ForeignFileIsNotRewritten) {
  const QByteArray content = "LD_PRELOAD=/foreign.so\nFOREIGN=/x\n";
  write(content);
  ASSERT_TRUE(removeOwnedEnrollment());
  EXPECT_EQ(read(), content);
}
