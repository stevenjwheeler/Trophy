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
#include <cstdio>

namespace {
void signalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    QCoreApplication::quit();
  }
}
} // namespace

int main(int argc, char *argv[]) {
  std::fprintf(stderr,
               "=== Trophy - Game recording and clipping Starting ===\n");
  std::fflush(stderr);

  // Handle termination signals gracefully
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  std::fprintf(stderr, "Creating application...\n");
  std::fflush(stderr);

  // Create application
  GameRecorder::Application app(argc, argv);

  std::fprintf(stderr, "Application created\n");
  std::fflush(stderr);

  // Set application metadata
  std::fprintf(stderr, "Setting metadata...\n");
  QCoreApplication::setOrganizationName("Trophy");
  QCoreApplication::setOrganizationDomain("org.trophy.recorder");
  QCoreApplication::setApplicationName("Trophy");
  QCoreApplication::setApplicationVersion("0.1.0");
  QGuiApplication::setDesktopFileName("trophy");

  // Set application icon
  std::fprintf(stderr, "Setting icon...\n");
  app.setWindowIcon(QIcon(":/icons/trophy.svg"));

  // Use Fusion style as base for consistent look
  std::fprintf(stderr, "Setting style...\n");
  QQuickStyle::setStyle("Fusion");

  // Initialize and run
  std::fprintf(stderr, "Calling initialize()...\n");
  std::fflush(stderr);

  if (!app.initialize()) {
    std::fprintf(stderr, "ERROR: Failed to initialize application\n");
    qCritical() << "Failed to initialize application";
    return 1;
  }

  std::fprintf(stderr, "Starting event loop...\n");
  return app.exec();
}
