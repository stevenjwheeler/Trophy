/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/ToplevelModel.h"

#include <gtest/gtest.h>

using TrophyBackend::ToplevelInfo;
using TrophyBackend::ToplevelModel;

namespace {

quintptr openToplevel(ToplevelModel &model, const QString &identifier,
                      const QString &appId, const QString &title) {
  const quintptr handle = model.add();
  model.setIdentifier(handle, identifier);
  model.setAppId(handle, appId);
  model.setTitle(handle, title);
  EXPECT_EQ(model.commit(handle), ToplevelModel::Change::Opened);
  return handle;
}

} // namespace

TEST(ToplevelModelTest, CommitWaitsForIdentifier) {
  ToplevelModel model;
  const quintptr handle = model.add();
  model.setAppId(handle, "steam_app_1");
  model.setTitle(handle, "Game");
  EXPECT_EQ(model.commit(handle), ToplevelModel::Change::None);
  EXPECT_TRUE(model.snapshot().isEmpty());

  model.setIdentifier(handle, "toplevel-1");
  EXPECT_EQ(model.commit(handle), ToplevelModel::Change::Opened);
  const auto snapshot = model.snapshot();
  ASSERT_EQ(snapshot.size(), 1);
  EXPECT_EQ(snapshot.first().identifier, QStringLiteral("toplevel-1"));
  EXPECT_EQ(snapshot.first().appId, QStringLiteral("steam_app_1"));
  EXPECT_EQ(snapshot.first().title, QStringLiteral("Game"));
}

TEST(ToplevelModelTest, OnlyRealChangesProduceUpdates) {
  ToplevelModel model;
  const quintptr handle = openToplevel(model, "a", "app", "Game");

  EXPECT_EQ(model.commit(handle), ToplevelModel::Change::None);
  model.setTitle(handle, "Game");
  EXPECT_EQ(model.commit(handle), ToplevelModel::Change::None);

  model.setTitle(handle, "Renamed");
  EXPECT_EQ(model.commit(handle), ToplevelModel::Change::Updated);
  EXPECT_EQ(model.info(handle).title, QStringLiteral("Renamed"));
}

TEST(ToplevelModelTest, ChangesBeforeOpenAreCoalesced) {
  ToplevelModel model;
  const quintptr handle = model.add();
  model.setIdentifier(handle, "a");
  model.setTitle(handle, "Old");
  model.setTitle(handle, "New");
  EXPECT_EQ(model.commit(handle), ToplevelModel::Change::Opened);
  EXPECT_EQ(model.info(handle).title, QStringLiteral("New"));
}

TEST(ToplevelModelTest, CloseOnlyReportsAnnouncedToplevels) {
  ToplevelModel model;
  const quintptr pending = model.add();
  EXPECT_EQ(model.remove(pending), ToplevelModel::Change::None);

  const quintptr announced = openToplevel(model, "b", "app", "Game");
  EXPECT_EQ(model.remove(announced), ToplevelModel::Change::Closed);
  EXPECT_TRUE(model.snapshot().isEmpty());
  EXPECT_FALSE(model.contains(announced));
}

TEST(ToplevelModelTest, SnapshotIsSortedAndOnlyContainsAnnouncedRows) {
  ToplevelModel model;
  openToplevel(model, "zeta", "app", "Zeta");
  openToplevel(model, "alpha", "app", "Alpha");
  const quintptr pending = model.add();
  model.setIdentifier(pending, "middle");

  const auto snapshot = model.snapshot();
  ASSERT_EQ(snapshot.size(), 2);
  EXPECT_EQ(snapshot[0].identifier, QStringLiteral("alpha"));
  EXPECT_EQ(snapshot[1].identifier, QStringLiteral("zeta"));

  // A late commit after the identifier finally arrives publishes it.
  EXPECT_EQ(model.commit(pending), ToplevelModel::Change::Opened);
  EXPECT_EQ(model.snapshot().size(), 3);
}

TEST(ToplevelModelTest, UnknownHandlesAreIgnored) {
  ToplevelModel model;
  EXPECT_FALSE(model.contains(42));
  EXPECT_EQ(model.info(42), ToplevelInfo{});
  EXPECT_EQ(model.commit(42), ToplevelModel::Change::None);
  EXPECT_EQ(model.remove(42), ToplevelModel::Change::None);
  EXPECT_EQ(model.setTitle(42, "x"), ToplevelModel::Change::None);
}
