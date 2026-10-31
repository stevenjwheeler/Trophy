/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/PipeWireStreamWatcher.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <functional>

#include <gtest/gtest.h>

using TrophyBackend::AudioStreamInfo;
using TrophyBackend::PipeWireStreamWatcher;

namespace {

void ensureApplication() {
  if (!QCoreApplication::instance()) {
    static int argc = 1;
    static char arg0[] = "test";
    static char *argv[] = {arg0, nullptr};
    new QCoreApplication(argc, argv);
  }
}

bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 3000) {
  QElapsedTimer timer;
  timer.start();
  while (!predicate() && timer.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  return predicate();
}

QVariantList streamSnapshot(quint32 nodeId, qint64 pid, const QString &binary) {
  QVariantMap stream;
  stream["nodeId"] = nodeId;
  stream["pid"] = pid;
  stream["binary"] = binary;
  stream["name"] = binary;
  return {stream};
}

} // namespace

TEST(PipeWireStreamWatcherTest, EmitsNewUpdatedAndRemovedStreams) {
  ensureApplication();
  std::atomic<int> phase{0};
  PipeWireStreamWatcher watcher;
  watcher.setSnapshotFunction([&phase] {
    switch (phase.load()) {
    case 0:
      return streamSnapshot(7, 123, "Game");
    case 1:
      return QVariantList{};
    default:
      return streamSnapshot(7, 456, "Game");
    }
  });

  QList<AudioStreamInfo> appeared;
  QList<quint32> removed;
  QObject::connect(&watcher, &PipeWireStreamWatcher::streamAppeared,
                   [&appeared](const AudioStreamInfo &stream) {
                     appeared.append(stream);
                   });
  QObject::connect(&watcher, &PipeWireStreamWatcher::streamRemoved,
                   [&removed](quint32 nodeId) { removed.append(nodeId); });

  watcher.start(5);
  ASSERT_TRUE(waitFor([&] { return appeared.size() == 1; }));
  EXPECT_EQ(appeared[0].nodeId, 7u);
  EXPECT_EQ(appeared[0].pid, 123);
  EXPECT_EQ(appeared[0].binary, QStringLiteral("Game"));
  ASSERT_EQ(watcher.streams().size(), 1);

  phase.store(2); // same node, new PID: re-announce for identity re-evaluation
  ASSERT_TRUE(waitFor([&] { return appeared.size() == 2; }));
  EXPECT_EQ(appeared[1].pid, 456);

  phase.store(1); // node gone
  ASSERT_TRUE(waitFor([&] { return removed.size() == 1; }));
  EXPECT_EQ(removed[0], 7u);
  EXPECT_TRUE(watcher.streams().isEmpty());

  watcher.stop();
  EXPECT_FALSE(watcher.isRunning());
}

TEST(PipeWireStreamWatcherTest, StopPreventsFurtherSnapshots) {
  ensureApplication();
  std::atomic<int> polls{0};
  PipeWireStreamWatcher watcher;
  watcher.setSnapshotFunction([&polls] {
    polls.fetch_add(1);
    return QVariantList{};
  });
  watcher.start(5);
  ASSERT_TRUE(waitFor([&] { return polls.load() >= 1; }));
  watcher.stop();
  const int afterStop = polls.load();
  QThread::msleep(40);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  EXPECT_LE(polls.load(), afterStop + 1); // at most one already-queued poll
}

TEST(PipeWireStreamWatcherTest, StartWithoutSnapshotDoesNothing) {
  ensureApplication();
  PipeWireStreamWatcher watcher;
  watcher.start(5);
  EXPECT_FALSE(watcher.isRunning());
}

TEST(PipeWireStreamWatcherTest, InvalidNodeIdsAreIgnored) {
  ensureApplication();
  PipeWireStreamWatcher watcher;
  watcher.setSnapshotFunction([] {
    return QVariantList{streamSnapshot(0, 123, "Ignored").first(),
                        streamSnapshot(9, 123, "Real").first()};
  });
  QList<AudioStreamInfo> appeared;
  QObject::connect(&watcher, &PipeWireStreamWatcher::streamAppeared,
                   [&appeared](const AudioStreamInfo &stream) {
                     appeared.append(stream);
                   });
  watcher.start(5);
  ASSERT_TRUE(waitFor([&] { return appeared.size() == 1; }));
  EXPECT_EQ(appeared[0].nodeId, 9u);
  EXPECT_EQ(appeared[0].binary, QStringLiteral("Real"));
  watcher.stop();
}
