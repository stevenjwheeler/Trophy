/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "core/Application.h"
#include "core/ControlServer.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QIcon>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <csignal>
#include <unistd.h>

namespace {
// Self-pipe for termination signals: a handler runs in async-signal
// context where QCoreApplication::quit() is unsafe, so it only
// writes a byte; the GUI thread quits from the event loop via the
// QSocketNotifier below.
int g_signalPipe[2] = {-1, -1};

void signalHandler(int signal) {
  if ((signal == SIGINT || signal == SIGTERM) && g_signalPipe[1] != -1) {
    const char b = 1;
    (void)::write(g_signalPipe[1], &b, sizeof(b));
  }
}
} // namespace

int main(int argc, char *argv[]) {
  // Set application metadata BEFORE creating the application object.
  // The version here is overwritten by Application's ctor (same APP_VERSION
  // logic); this pre-QApplication value only feeds early arg-parser output.
#ifdef APP_VERSION
#define TROPHY_MAIN_STRINGIFY(x) #x
#define TROPHY_MAIN_TOSTRING(x) TROPHY_MAIN_STRINGIFY(x)
  QCoreApplication::setApplicationVersion(
      QString::fromUtf8(TROPHY_MAIN_TOSTRING(APP_VERSION)).replace("\"", ""));
#else
  QCoreApplication::setApplicationVersion("0.0.0-unknown");
#endif
  QCoreApplication::setOrganizationName("Trophy");
  QCoreApplication::setOrganizationDomain("org.trophy.recorder");
  QCoreApplication::setApplicationName("Trophy");
  // In a Flatpak sandbox the desktop file is renamed to the app id, and
  // notifications/tray/portal associations key on the desktop id: resolve
  // it at runtime instead of hardcoding "trophy".
  {
    const QString flatpakId =
        QString::fromUtf8(qgetenv("FLATPAK_ID"));
    if (!flatpakId.isEmpty() || QFile::exists(QStringLiteral("/.flatpak-info")))
      QApplication::setDesktopFileName(
          !flatpakId.isEmpty() ? flatpakId
                               : QStringLiteral("io.github.stevenjwheeler.Trophy"));
    else
      QApplication::setDesktopFileName(QStringLiteral("trophy"));
  }

  // Create application
  TrophyBackend::Application app(argc, argv);

  // Handle termination signals gracefully via a self-pipe: the
  // handler only writes a byte (async-signal-safe) and the GUI
  // thread quits from the event loop.
  if (::pipe(g_signalPipe) == 0) {
    auto *signalNotifier = new QSocketNotifier(
        g_signalPipe[0], QSocketNotifier::Read, &app);
    // String-based connect: QSocketNotifier::activated carries a private
    // QPrivateSignal parameter, so its address cannot be taken here; moc
    // generates a default-arg form that makes the QSocketDescriptor-only
    // signature connectable. Draining the pipe is unnecessary — quit()
    // ends the event loop and the process exits with it.
    QObject::connect(signalNotifier, SIGNAL(activated(QSocketDescriptor)),
                     &app, SLOT(quit()));
  }
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  QCommandLineParser parser;
  parser.setApplicationDescription("Trophy - Game recording and clipping");
  parser.addHelpOption();
  parser.addOptions({
      {{"m", "minimized"}, "Start minimized to the system tray."},
      {"toggle-record",
       "Toggle recording on the running instance, then exit."},
      {"save-clip",
       "Save an instant-replay clip on the running instance, then exit."},
      {"show-window", "Show the running instance's window, then exit."},
  });
  parser.process(app);
  if (parser.isSet("minimized")) {
    app.setForceMinimized(true);
  }

  // Single-shot remote commands for native compositor keybinds: if an
  // instance is already listening, forward and exit. Otherwise fall
  // through and start normally (the command is dropped).
  const QStringList remoteCommands = {"toggle-record", "save-clip",
                                      "show-window"};
  for (const QString &cmd : remoteCommands) {
    if (parser.isSet(cmd)) {
      if (TrophyBackend::ControlServer::sendCommand(cmd)) {
        return 0;
      }
      break;
    }
  }

  // Single-instance guard: a second plain launch raises the running
  // instance instead of starting a second GUI (which double-recorded).
  // Held for the process lifetime; a crash leaves a stale lock that
  // QLockFile clears via PID detection.
  QLockFile instanceLock(QStandardPaths::writableLocation(
                             QStandardPaths::RuntimeLocation) +
                         QStringLiteral("/trophy.lock"));
  if (!instanceLock.tryLock()) {
    TrophyBackend::ControlServer::sendCommand(QStringLiteral("show-window")); // best effort
    qInfo() << "Trophy is already running; showing the existing window.";
    return 0;
  }

  // Set application icon
  app.setWindowIcon(QIcon(":/icons/trophy.svg"));

  // Use Fusion style as base for consistent look
  QQuickStyle::setStyle("Fusion");

  // Initialize and run
  if (!app.initialize()) {
    qCritical() << "Failed to initialize application";
    return 1;
  }

  return app.exec();
}