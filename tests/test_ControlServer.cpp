/*
 * Trophy - Unit Tests
 * ControlServer: command routing over an isolated socket path.
 * No GUI or portal needed.
 */

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "core/ControlServer.h"

using namespace TrophyBackend;

class ControlServerTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new ControlServerTestEnvironment);

class ControlServerTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;
  QString sockPath;
  void SetUp() override {
    ASSERT_TRUE(tempDir.isValid());
    sockPath = tempDir.path() + "/trophy-test.sock";
  }

  // Pump the event loop until cond or timeout.
  template <typename F> bool waitFor(F cond, int timeoutMs = 2000) {
    QElapsedTimer t;
    t.start();
    while (!cond()) {
      if (t.elapsed() > timeoutMs)
        return false;
      QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return true;
  }
};

TEST_F(ControlServerTest, CommandsRouteToSignals) {
  ControlServer server(sockPath);
  ASSERT_TRUE(server.listen());
  EXPECT_TRUE(server.isListening());

  QSignalSpy toggleSpy(&server, &ControlServer::toggleRecordingRequested);
  QSignalSpy clipSpy(&server, &ControlServer::saveClipRequested);
  QSignalSpy showSpy(&server, &ControlServer::showWindowRequested);
  QSignalSpy bookmarkSpy(&server, &ControlServer::bookmarkRequested);

  {
    QLocalSocket client;
    client.connectToServer(sockPath);
    ASSERT_TRUE(client.waitForConnected(2000));
    client.write("toggle-record\nsave-clip\nshow-window\nadd-bookmark\n");
    ASSERT_TRUE(client.waitForBytesWritten(2000));
    client.disconnectFromServer();
  }

  ASSERT_TRUE(waitFor([&] { return toggleSpy.count() == 1; }));
  EXPECT_EQ(clipSpy.count(), 1);
  EXPECT_EQ(showSpy.count(), 1);
  EXPECT_EQ(bookmarkSpy.count(), 1);
}

TEST_F(ControlServerTest, UnknownCommandsIgnored) {
  ControlServer server(sockPath);
  ASSERT_TRUE(server.listen());

  QSignalSpy toggleSpy(&server, &ControlServer::toggleRecordingRequested);
  QSignalSpy clipSpy(&server, &ControlServer::saveClipRequested);
  QSignalSpy showSpy(&server, &ControlServer::showWindowRequested);

  {
    QLocalSocket client;
    client.connectToServer(sockPath);
    ASSERT_TRUE(client.waitForConnected(2000));
    client.write("delete-everything\n\n");
    ASSERT_TRUE(client.waitForBytesWritten(2000));
    client.disconnectFromServer();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  }

  EXPECT_EQ(toggleSpy.count(), 0);
  EXPECT_EQ(clipSpy.count(), 0);
  EXPECT_EQ(showSpy.count(), 0);
}

TEST_F(ControlServerTest, SecondListenDoesNotStealLiveSocket) {
  ControlServer first(sockPath);
  ASSERT_TRUE(first.listen());

  // A rival listener on a live path must back off, not remove the socket.
  ControlServer rival(sockPath);
  EXPECT_FALSE(rival.listen());
  EXPECT_TRUE(first.isListening());

  // The original owner still serves commands afterwards.
  QSignalSpy showSpy(&first, &ControlServer::showWindowRequested);
  EXPECT_TRUE(ControlServer::sendCommandTo(sockPath, "show-window", 2000));
  EXPECT_TRUE(waitFor([&] { return showSpy.count() == 1; }));
}

TEST_F(ControlServerTest, ListenRecoversStaleSocketFile) {
  // A crashed run leaves a socket file with nobody behind it: listen()
  // must clear it and serve. Simulate with a server that is destroyed
  // without close() cleanup... QLocalServer removes on close, so instead
  // plant a plain file at the path.
  const QString stalePath = tempDir.path() + "/stale.sock";
  QFile plant(stalePath);
  ASSERT_TRUE(plant.open(QIODevice::WriteOnly));
  plant.write("dead");
  plant.close();

  ControlServer server(stalePath);
  EXPECT_TRUE(server.listen());
  EXPECT_TRUE(server.isListening());
}
TEST_F(ControlServerTest, SendCommandFailsWithoutServer) {
  // Nothing listens on the isolated path (never the default: a running
  // Trophy instance would answer there and break isolation).
  EXPECT_FALSE(ControlServer::sendCommandTo(tempDir.path() + "/empty.sock",
                                            "toggle-record", 200));
}

TEST_F(ControlServerTest, DefaultPathLivesInRuntimeDir) {
  EXPECT_TRUE(
      ControlServer::defaultSocketPath().endsWith("/trophy-control.sock"));
}
