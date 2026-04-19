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
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
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
  // (Some backend initializations rely on these being set early)
  QCoreApplication::setOrganizationName("Trophy");
  QCoreApplication::setOrganizationDomain("org.trophy.recorder");
  QCoreApplication::setApplicationName("Trophy");
  QCoreApplication::setApplicationVersion("0.1.0");
  QGuiApplication::setDesktopFileName("trophy");

  // Create application
  GameRecorder::Application app(argc, argv);

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
