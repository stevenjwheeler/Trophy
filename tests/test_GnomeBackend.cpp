/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "detection/GnomeBackend.h"
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <gtest/gtest.h>

class FakeGnomeHelper : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.trophy.WindowHelper1")
public:
  QString payload = "[]";
  int calls = 0;
  bool hold = false;
  QDBusMessage held;
public slots:
  QString GetWindows() {
    ++calls;
    if (hold) {
      setDelayedReply(true);
      held = message();
    }
    return payload;
  }
};

namespace TrophyBackend {
class GnomeBackendTest : public ::testing::Test {
protected:
  FakeGnomeHelper helper;
  QDBusConnection bus = QDBusConnection::sessionBus();
  bool registered = false;
  void SetUp() override {
    if (qEnvironmentVariable("TROPHY_PRIVATE_GNOME_TEST") != "1")
      GTEST_SKIP() << "Requires the private D-Bus launcher";
    ASSERT_FALSE(bus.interface()->isServiceRegistered(GnomeBackend::kService));
    ASSERT_TRUE(bus.registerService(GnomeBackend::kService));
    registered = true;
    ASSERT_TRUE(bus.registerObject(GnomeBackend::kPath, &helper, QDBusConnection::ExportAllSlots));
  }
  void TearDown() override {
    if (registered) {
      bus.unregisterObject(GnomeBackend::kPath);
      bus.unregisterService(GnomeBackend::kService);
    }
  }
  static void poll(GnomeBackend &backend) {
    auto work = std::async(std::launch::async, [&] { backend.refresh(); });
    QElapsedTimer timer;
    timer.start();
    while (work.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && timer.elapsed() < 5000) {
      QCoreApplication::processEvents();
      QThread::msleep(1);
    }
    ASSERT_EQ(work.wait_for(std::chrono::milliseconds(0)), std::future_status::ready);
    work.get();
    QCoreApplication::processEvents();
  }
  static QString install(const QString &directory) { return GnomeBackend::installHelper(directory); }
};

TEST_F(GnomeBackendTest, NormalStartupOnlyReadsAndEmptySnapshotIsReady) {
  GnomeBackend backend;
  const bool installedBefore = GnomeBackend::isInstalled();
  ASSERT_TRUE(backend.start());
  EXPECT_FALSE(backend.isActive());
  QSignalSpy changed(&backend, &GnomeBackend::windowsUpdated);
  poll(backend);
  EXPECT_TRUE(backend.isActive());
  EXPECT_TRUE(backend.windows().isEmpty());
  EXPECT_EQ(changed.count(), 1);
  poll(backend);
  EXPECT_EQ(changed.count(), 1);
  EXPECT_EQ(helper.calls, 2);
  EXPECT_EQ(GnomeBackend::isInstalled(), installedBefore);
}

TEST_F(GnomeBackendTest, WindowSnapshotAndMinimizeChangesReachConsumers) {
  GnomeBackend backend;
  ASSERT_TRUE(backend.start());
  helper.payload = R"([{"id":42,"pid":123,"appId":"game","title":"Game","focused":true,"fullscreen":true,"minimized":false,"width":1920,"height":1080,"outputWidth":1920,"outputHeight":1080}])";
  poll(backend);
  ASSERT_EQ(backend.windows().size(), 1);
  EXPECT_EQ(backend.windows().first().pid, 123);
  EXPECT_TRUE(backend.windows().first().focused);
  QSignalSpy changed(&backend, &GnomeBackend::windowsUpdated);
  helper.payload.replace("\"minimized\":false", "\"minimized\":true");
  poll(backend);
  EXPECT_EQ(changed.count(), 1);
  EXPECT_TRUE(backend.windows().first().minimized);
}

TEST_F(GnomeBackendTest, DisabledExtensionClearsWindowsAndRecovers) {
  GnomeBackend backend;
  ASSERT_TRUE(backend.start());
  helper.payload = R"([{"id":42,"title":"Game"}])";
  poll(backend);
  ASSERT_TRUE(backend.isActive());
  bus.unregisterObject(GnomeBackend::kPath);
  poll(backend);
  EXPECT_FALSE(backend.isActive());
  EXPECT_TRUE(backend.windows().isEmpty());
  ASSERT_TRUE(bus.registerObject(GnomeBackend::kPath, &helper, QDBusConnection::ExportAllSlots));
  poll(backend);
  EXPECT_TRUE(backend.isActive());
  EXPECT_EQ(backend.windows().size(), 1);
}

TEST_F(GnomeBackendTest, MalformedAndOversizedRepliesAreNotHealthyEmptySnapshots) {
  GnomeBackend backend;
  ASSERT_TRUE(backend.start());
  for (const QString &payload : {QString("null"), QString("{}"), QString("[1]"), QString("[{}]"), QString("broken"), QString(1024 * 1024 + 1, ' ')}) {
    helper.payload = payload;
    poll(backend);
    EXPECT_FALSE(backend.isActive());
  }
  helper.payload = "[]";
  poll(backend);
  EXPECT_TRUE(backend.isActive());
  backend.stop();
  const int calls = helper.calls;
  poll(backend);
  EXPECT_EQ(helper.calls, calls);
  EXPECT_FALSE(backend.isActive());
}

TEST_F(GnomeBackendTest, ExplicitInstallCopiesPayloadAndRunsOnlyEnableCommand) {
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const QByteArray oldPath = qgetenv("PATH");
  QFile command(temp.filePath("gnome-extensions"));
  ASSERT_TRUE(command.open(QIODevice::WriteOnly));
  command.write("#!/bin/sh\n[ \"$1\" = enable ] && [ \"$2\" = trophy-monitor@trophy ]\n");
  command.close();
  ASSERT_TRUE(command.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  qputenv("PATH", temp.path().toUtf8());
  const QString target = temp.filePath("extension");
  const QString result = install(target);
  qputenv("PATH", oldPath);
  EXPECT_TRUE(result.contains("enabled")) << result.toStdString();
  for (const QString &name : {QString("metadata.json"), QString("extension.js")}) {
    QFile actual(QDir(target).filePath(name));
    QFile bundled(":/gnome/" + name);
    ASSERT_TRUE(actual.open(QIODevice::ReadOnly));
    ASSERT_TRUE(bundled.open(QIODevice::ReadOnly));
    EXPECT_EQ(actual.readAll(), bundled.readAll());
  }
}

TEST_F(GnomeBackendTest, StopRejectsInFlightSnapshot) {
  GnomeBackend backend;
  ASSERT_TRUE(backend.start());
  helper.hold = true;
  auto work = std::async(std::launch::async, [&] { backend.refresh(); });
  QElapsedTimer timer;
  timer.start();
  while (helper.calls == 0 && timer.elapsed() < 3000) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  ASSERT_EQ(helper.calls, 1);
  backend.stop();
  ASSERT_TRUE(bus.send(helper.held.createReply(QVariantList{QString(R"([{"id":42,"title":"Late"}])")})));
  work.get();
  EXPECT_FALSE(backend.isActive());
  EXPECT_TRUE(backend.windows().isEmpty());
}

TEST_F(GnomeBackendTest, RestartRejectsReplyFromPreviousGeneration) {
  GnomeBackend backend;
  ASSERT_TRUE(backend.start());
  helper.hold = true;
  auto work = std::async(std::launch::async, [&] { backend.refresh(); });
  QElapsedTimer timer;
  timer.start();
  while (helper.calls == 0 && timer.elapsed() < 3000) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  ASSERT_EQ(helper.calls, 1);
  backend.stop();
  ASSERT_TRUE(backend.start());
  ASSERT_TRUE(bus.send(helper.held.createReply(QVariantList{QString(R"([{"id":42,"title":"Old"}])")})));
  work.get();
  EXPECT_FALSE(backend.isActive());
  EXPECT_TRUE(backend.windows().isEmpty());
  helper.hold = false;
  poll(backend);
  EXPECT_TRUE(backend.isActive());
}

TEST_F(GnomeBackendTest, MissingEnableToolExplainsLoginAndManualEnable) {
  QTemporaryDir temp;
  const QByteArray oldPath = qgetenv("PATH");
  qputenv("PATH", temp.path().toUtf8());
  const QString result = install(temp.filePath("extension"));
  qputenv("PATH", oldPath);
  EXPECT_TRUE(result.contains("Log out"));
  EXPECT_TRUE(result.contains("enable Trophy Window Helper"));
  EXPECT_TRUE(QFile::exists(temp.filePath("extension/extension.js")));
}

TEST_F(GnomeBackendTest, InstallRejectsForeignDirectoryAndSymlink) {
  QTemporaryDir temp;
  const QString foreign = temp.filePath("foreign");
  ASSERT_TRUE(QDir().mkpath(foreign));
  QFile metadata(QDir(foreign).filePath("metadata.json"));
  ASSERT_TRUE(metadata.open(QIODevice::WriteOnly));
  metadata.write("{\"uuid\":\"someone-else\"}");
  metadata.close();
  EXPECT_TRUE(install(foreign).contains("unrecognized"));
  EXPECT_FALSE(QFile::exists(QDir(foreign).filePath("extension.js")));
  ASSERT_TRUE(QFile::link(foreign, temp.filePath("link")));
  EXPECT_TRUE(install(temp.filePath("link")).contains("regular directory"));
}
} // namespace TrophyBackend

int main(int argc, char **argv) {
  QTemporaryDir isolation("/tmp/opencode/trophy-gnome-tests-XXXXXX");
  if (!isolation.isValid()) return 1;
  qputenv("XDG_DATA_HOME", isolation.filePath("data").toUtf8());
  qputenv("XDG_CONFIG_HOME", isolation.filePath("config").toUtf8());
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "test_GnomeBackend.moc"
