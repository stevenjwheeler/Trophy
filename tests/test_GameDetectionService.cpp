#include <QCoreApplication>
#include <QSignalSpy>
#include <QLocalSocket>
#include <QThread>
#include <gtest/gtest.h>

#include "detection/GameDetectionService.h"

class GameDetectionTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new GameDetectionTestEnvironment);

namespace GameRecorder {

class GameDetectionServiceTest : public ::testing::Test {
protected:
  GameDetectionService service;

  void SetUp() override {
    service.start();
  }

  void TearDown() override {
    service.stop();
  }

  void simulateGameStart(qint64 pid, const QString &name) {
    QLocalSocket socket;
    socket.connectToServer("/tmp/trophy-detection.sock");
    if (socket.waitForConnected(1000)) {
        socket.write(QString("START:%1:%2\n").arg(pid).arg(name).toUtf8());
        socket.waitForBytesWritten(1000);
        // We keep the socket alive if we want to simulate a running game
        // But for a simple test, we can just wait for the service to process it
        QCoreApplication::processEvents();
    }
  }
};

TEST_F(GameDetectionServiceTest, GameDetectionViaSocket) {
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);
  
  QLocalSocket socket;
  socket.connectToServer("/tmp/trophy-detection.sock");
  ASSERT_TRUE(socket.waitForConnected(1000));
  
  socket.write("START:1234:Test Game\n");
  socket.waitForBytesWritten(1000);
  
  // Give the service time to process the message
  int retries = 0;
  while (spy.count() == 0 && retries < 10) {
      QCoreApplication::processEvents();
      QThread::msleep(50);
      retries++;
  }

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGameName(), "Test Game");
  EXPECT_EQ(service.currentGamePid(), 1234);
}

TEST_F(GameDetectionServiceTest, GameClosedOnDisconnect) {
  QSignalSpy spy(&service, &GameDetectionService::gameClosed);
  
  {
      QLocalSocket socket;
      socket.connectToServer("/tmp/trophy-detection.sock");
      ASSERT_TRUE(socket.waitForConnected(1000));
      socket.write("START:1234:Test Game\n");
      socket.waitForBytesWritten(1000);
      
      QCoreApplication::processEvents();
      QThread::msleep(100);
      QCoreApplication::processEvents();
      
      ASSERT_EQ(service.currentGamePid(), 1234);
      
      // Socket goes out of scope and disconnects
  }
  
  int retries = 0;
  while (spy.count() == 0 && retries < 10) {
      QCoreApplication::processEvents();
      QThread::msleep(50);
      retries++;
  }

  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest, WhitelistManagement) {
  QString windowClass = "steam_app_test";
  QString displayName = "Test Game";

  service.addToWhitelist(windowClass, displayName);

  QStringList whitelist = service.whitelist();
  EXPECT_TRUE(whitelist.contains(windowClass));

  service.removeFromWhitelist(windowClass);
  whitelist = service.whitelist();
  EXPECT_FALSE(whitelist.contains(windowClass));
}

} // namespace GameRecorder
