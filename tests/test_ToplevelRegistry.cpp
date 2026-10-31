/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/ToplevelRegistry.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <gtest/gtest.h>

using TrophyBackend::ToplevelRegistry;

namespace {

void ensureApplication() {
  if (!QCoreApplication::instance()) {
    static int argc = 1;
    static char arg0[] = "test";
    static char *argv[] = {arg0, nullptr};
    new QCoreApplication(argc, argv);
  }
}

void pump(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
}

} // namespace

TEST(ToplevelRegistryTest, ListsLiveToplevelsWhenCompositorSupportsIt) {
  ensureApplication();
  if (!ToplevelRegistry::isAvailable())
    GTEST_SKIP() << "No Wayland session in the environment";

  ToplevelRegistry registry;
  QString error;
  if (!registry.start(&error))
    GTEST_SKIP() << error.toStdString();

  // start() already roundtrips the initial list; a short pump also covers a
  // compositor that delivers a few handles just after the roundtrip.
  pump(300);
  const auto toplevels = registry.toplevels();
  for (const auto &toplevel : toplevels) {
    EXPECT_FALSE(toplevel.identifier.isEmpty());
  }
  // An empty desktop is a valid result; the important part is that binding,
  // parsing and identifier delivery did not fail.
  EXPECT_TRUE(registry.isActive());
  registry.stop();
  EXPECT_FALSE(registry.isActive());
}

TEST(ToplevelRegistryTest, StopAndRestartAreSafe) {
  ensureApplication();
  if (!ToplevelRegistry::isAvailable())
    GTEST_SKIP() << "No Wayland session in the environment";

  ToplevelRegistry registry;
  QString error;
  if (!registry.start(&error))
    GTEST_SKIP() << error.toStdString();
  registry.stop();
  registry.stop(); // idempotent
  EXPECT_TRUE(registry.toplevels().isEmpty());
  EXPECT_TRUE(registry.start(&error)) << error.toStdString();
  registry.stop();
}
