/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "CrashHandler.h"

#include <QByteArray>
#include <QCoreApplication>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>

#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>

namespace TrophyBackend {

namespace {

int g_logFd = -1;
char g_version[64] = "unknown";
volatile sig_atomic_t g_inHandler = 0;

// Only async-signal-safe calls belong here: write(2), time(2), strftime is
// borderline but has been safe in practice for a one-shot crash path, and
// backtrace_symbols_fd writes without allocating a symbol table.
void writeAll(const char *data, size_t size) {
  while (size > 0 && g_logFd >= 0) {
    const ssize_t written = ::write(g_logFd, data, size);
    if (written <= 0)
      break;
    data += written;
    size -= static_cast<size_t>(written);
  }
}

void writeString(const char *text) { writeAll(text, std::strlen(text)); }

// C string form: the handler must not allocate (no QString/QByteArray).
const char *signalNameC(int signalNumber) {
  switch (signalNumber) {
  case SIGSEGV: return "SIGSEGV";
  case SIGBUS: return "SIGBUS";
  case SIGILL: return "SIGILL";
  case SIGFPE: return "SIGFPE";
  case SIGABRT: return "SIGABRT";
  case SIGTRAP: return "SIGTRAP";
  default: return "signal";
  }
}

void writeBacktrace() {
  writeString("[CRASH] backtrace (addresses; symbolise against the same build):\n");
  // The handler must not allocate, so a fixed on-stack frame buffer only.
  void *frames[64];
  const int count = ::backtrace(frames, 64);
  if (count > 0)
    ::backtrace_symbols_fd(frames, count, g_logFd);
}

void restoreAndReraise(int signalNumber) {
  ::signal(signalNumber, SIG_DFL);
  ::raise(signalNumber);
}

void crashHandler(int signalNumber, siginfo_t *info, void *) {
  if (g_inHandler) {
    // Recursive fault (e.g. inside the logger): get out immediately.
    restoreAndReraise(signalNumber);
    return;
  }
  g_inHandler = 1;

  const time_t now = ::time(nullptr);
  struct tm broken{};
  char stamp[40] = "unknown time";
  if (::gmtime_r(&now, &broken) != nullptr)
    ::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S UTC", &broken);

  char header[320];
  std::snprintf(header, sizeof(header),
                "\n[CRASH] %s (%d) at address %p | version %s | %s\n",
                signalNameC(signalNumber), signalNumber,
                info != nullptr ? info->si_addr : nullptr,
                g_version, stamp);
  writeString(header);
  writeBacktrace();
  writeString("[CRASH] re-raising with the default handler "
              "(see 'coredumpctl info trophy' for a core dump)\n");

  restoreAndReraise(signalNumber);
}

void terminateHandler() {
  if (g_inHandler)
    std::abort();
  g_inHandler = 1;
  writeString("\n[CRASH] std::terminate called "
              "(uncaught exception or forced termination)\n");
  writeBacktrace();
  writeString("[CRASH] aborting\n");
  std::abort();
}

} // namespace

QString describeSignal(int signalNumber) {
  switch (signalNumber) {
  case SIGSEGV:
    return QStringLiteral("SIGSEGV");
  case SIGBUS:
    return QStringLiteral("SIGBUS");
  case SIGILL:
    return QStringLiteral("SIGILL");
  case SIGFPE:
    return QStringLiteral("SIGFPE");
  case SIGABRT:
    return QStringLiteral("SIGABRT");
  case SIGTRAP:
    return QStringLiteral("SIGTRAP");
  default:
    return QStringLiteral("signal %1").arg(signalNumber);
  }
}

void installCrashHandlerForFd(int fd, const char *version) {
  if (fd < 0 || g_logFd >= 0)
    return;
  g_logFd = fd;
  std::snprintf(g_version, sizeof(g_version), "%s",
                version != nullptr ? version : "unknown");

  // A stack-overflow SIGSEGV cannot run on the exhausted stack.
  static char altStack[64 * 1024];
  stack_t stack{};
  stack.ss_sp = altStack;
  stack.ss_size = sizeof(altStack);
  ::sigaltstack(&stack, nullptr);

  struct sigaction action{};
  action.sa_sigaction = crashHandler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
  ::sigemptyset(&action.sa_mask);
  for (const int signalNumber : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
    ::sigaction(signalNumber, &action, nullptr);

  std::set_terminate(terminateHandler);
}

void installCrashHandler(const QString &logPath) {
  if (g_logFd >= 0)
    return;
  const int fd = ::open(logPath.toUtf8().constData(),
                        O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0)
    return;
  const QByteArray version = QCoreApplication::applicationVersion().toUtf8();
  installCrashHandlerForFd(fd, version.constData());
}

} // namespace TrophyBackend
