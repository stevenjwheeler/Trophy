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
#include <QIcon>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStandardPaths>
#include <csignal>

namespace {
void signalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    QCoreApplication::quit();
  }
}
} // namespace

int main(int argc, char *argv[]) {
  // Handle termination signals gracefully
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  // Set application metadata BEFORE creating the application object
  QCoreApplication::setOrganizationName("Trophy");
  QCoreApplication::setOrganizationDomain("org.trophy.recorder");
  QCoreApplication::setApplicationName("Trophy");
  QCoreApplication::setApplicationVersion("0.1.0");
  QApplication::setDesktopFileName("trophy");

  // Create application
  TrophyBackend::Application app(argc, argv);

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