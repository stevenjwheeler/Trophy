/*
 * Trophy - Unit Tests
 * CrashHandler: fatal signals must leave evidence in the log.
 */

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/CrashHandler.h"

using namespace TrophyBackend;

TEST(CrashHandler, NamesKnownSignals) {
  EXPECT_EQ(describeSignal(SIGSEGV), QStringLiteral("SIGSEGV"));
  EXPECT_EQ(describeSignal(SIGABRT), QStringLiteral("SIGABRT"));
  EXPECT_EQ(describeSignal(SIGBUS), QStringLiteral("SIGBUS"));
  EXPECT_EQ(describeSignal(64), QStringLiteral("signal 64"));
}

TEST(CrashHandler, SegfaultWritesReportAndStillDiesFromTheSignal) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("crash.log"));
  const QByteArray nativePath = path.toLocal8Bit();

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // A core dump would be large, slow and pointless in a fixture.
    struct rlimit noCore {0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
    const int fd =
        ::open(nativePath.constData(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    installCrashHandlerForFd(fd, "test-build");
    ::raise(SIGSEGV);
    ::_exit(99); // not reached: the handler re-raises with SIG_DFL
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  // The default disposition must still run, so cores keep working.
  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);

  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const QString text = QString::fromUtf8(file.readAll());
  EXPECT_TRUE(text.contains(QStringLiteral("[CRASH] SIGSEGV (11)")));
  EXPECT_TRUE(text.contains(QStringLiteral("version test-build")));
  EXPECT_TRUE(text.contains(QStringLiteral("backtrace")));
  EXPECT_TRUE(text.contains(QStringLiteral("coredumpctl")));
}

TEST(CrashHandler, TerminateWritesReport) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const QString path = directory.filePath(QStringLiteral("terminate.log"));
  const QByteArray nativePath = path.toLocal8Bit();

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    struct rlimit noCore {0, 0};
    ::setrlimit(RLIMIT_CORE, &noCore);
    const int fd =
        ::open(nativePath.constData(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    installCrashHandlerForFd(fd, "test-build");
    std::terminate();
    ::_exit(99); // not reached
  }

  int status = 0;
  ASSERT_EQ(::waitpid(pid, &status, 0), pid);
  EXPECT_TRUE(WIFSIGNALED(status));

  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const QString text = QString::fromUtf8(file.readAll());
  EXPECT_TRUE(text.contains(QStringLiteral("std::terminate called")));
  EXPECT_TRUE(text.contains(QStringLiteral("backtrace")));
}
