/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Application.h"
#include "NotificationService.h"
#include "RecordingManager.h"
#include "StorageManager.h"
#include "WindowService.h"
#include "audio/AudioMixer.h"
#include "detection/GameDetectionService.h"
#include "library/RecordingLibrary.h"
#include "settings/SettingsManager.h"

#include <QDebug> // Added
#include <QDir>
#include <QQmlContext>
#include <QQuickWindow>
#include <QStandardPaths>
#include <chrono> // Added
#include <cstdio>

namespace GameRecorder {

Application::Application(int &argc, char **argv)
    : QGuiApplication(argc, argv) {}

Application::~Application() = default;

bool Application::initialize() {
  // Install message handler to capture all Qt messages
  qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context,
                            const QString &msg) {
    const char *level = "UNKNOWN";
    switch (type) {
    case QtDebugMsg:
      level = "DEBUG";
      break;
    case QtInfoMsg:
      level = "INFO";
      break;
    case QtWarningMsg:
      level = "WARNING";
      break;
    case QtCriticalMsg:
      level = "CRITICAL";
      break;
    case QtFatalMsg:
      level = "FATAL";
      break;
    }
    std::fprintf(stderr, "[Qt %s] %s\n", level, msg.toUtf8().constData());
    std::fflush(stderr);
  });

  std::fprintf(stderr,
               "[App] Initializing Trophy - Game recording and clipping %s\n",
               applicationVersion().toUtf8().constData());
  qInfo() << "Initializing Trophy - Game recording and clipping"
          << applicationVersion();

  // Initialize services in dependency order
  std::fprintf(stderr, "[App] Initializing services...\n");
  initializeServices();

  // Register QML types
  std::fprintf(stderr, "[App] Registering QML types...\n");
  registerQmlTypes();

  // Create QML engine
  std::fprintf(stderr, "[App] Creating QML engine...\n");
  m_engine = std::make_unique<QQmlApplicationEngine>();

  // Expose services to QML
  std::fprintf(stderr, "[App] Exposing services to QML...\n");
  exposeServicesToQml();

  // Connect signals between services
  std::fprintf(stderr, "[App] Connecting signals...\n");
  connectSignals();

  // Load main QML file
  // In debug mode, try to load from filesystem for live reload
  QUrl url;
#ifdef QT_DEBUG
  QString devPath = QCoreApplication::applicationDirPath() + "/../qml/main.qml";
  QFileInfo devFile(devPath);
  if (devFile.exists()) {
    url = QUrl::fromLocalFile(devFile.absoluteFilePath());
    std::fprintf(
        stderr,
        "[App] DEV MODE: Loading QML from filesystem for live reload\n");
  } else {
    url = QUrl(QStringLiteral("qrc:/qt/qml/GameRecorder/qml/main.qml"));
  }
#else
  url = QUrl(QStringLiteral("qrc:/qt/qml/GameRecorder/qml/main.qml"));
#endif
  std::fprintf(stderr, "[App] Loading QML from: %s\n",
               url.toString().toUtf8().constData());

  QObject::connect(
      m_engine.get(), &QQmlApplicationEngine::objectCreationFailed, this,
      [this, url]() {
        std::fprintf(stderr,
                     "[App] ERROR: QML object creation failed for: %s\n",
                     url.toString().toUtf8().constData());
        qCritical() << "Failed to load QML:" << url;
        emit fatalError("Failed to load user interface");
        QCoreApplication::exit(1);
      },
      Qt::QueuedConnection);

  m_engine->load(url);
  std::fprintf(stderr, "[App] QML load() completed\n");

  if (m_engine->rootObjects().isEmpty()) {
    std::fprintf(stderr, "[App] ERROR: No root objects created from QML\n");
    qCritical() << "No root objects created from QML";
    return false;
  }

  std::fprintf(stderr, "[App] Root objects created: %d\n",
               static_cast<int>(m_engine->rootObjects().size()));

  // Start game detection service
  std::fprintf(stderr, "[App] Starting game detection...\n");
  m_gameDetector->start();

  std::fprintf(stderr, "[App] Application initialized successfully!\n");
  qInfo() << "Application initialized successfully";
  return true;
}

void Application::initializeServices() {
  // Settings must be created first as other services depend on it
  m_settingsManager = std::make_unique<SettingsManager>(this);
  m_settingsManager->load();

  // Storage manager for disk space management
  m_storageManager = std::make_unique<StorageManager>(
      m_settingsManager->recordingsPath(), this);
  m_storageManager->setMaxSize(m_settingsManager->maxStorageSize());

  // Notification service for system notifications
  m_notificationService = std::make_unique<NotificationService>(this);

  // Recording library for managing recorded videos
  m_recordingLibrary = std::make_unique<RecordingLibrary>(
      m_settingsManager->recordingsPath(), this);

  // Audio mixer for multi-track audio
  m_audioMixer = std::make_unique<AudioMixer>(48000, 2, this);

  // Game detection service
  m_gameDetector = std::make_unique<GameDetectionService>(this);

  // Recording manager (orchestrates recording)
  m_recordingManager = std::make_unique<RecordingManager>(this);
  m_recordingManager->setSettingsManager(m_settingsManager.get());
  m_recordingManager->setStorageManager(m_storageManager.get());
  m_recordingManager->setAudioMixer(m_audioMixer.get());

  // Window service for manual selection
  m_windowService = std::make_unique<WindowService>(this);
}

void Application::registerQmlTypes() {
  // Types are automatically registered by qt_add_qml_module in CMakeLists.txt
  // No manual registration needed
}

void Application::exposeServicesToQml() {
  QQmlContext *context = m_engine->rootContext();

  // Expose application instance
  context->setContextProperty("app", this);

  // Expose all services
  context->setContextProperty("recordingManager", m_recordingManager.get());
  context->setContextProperty("settingsManager", m_settingsManager.get());
  context->setContextProperty("storageManager", m_storageManager.get());
  context->setContextProperty("recordingLibrary", m_recordingLibrary.get());
  context->setContextProperty("gameDetector", m_gameDetector.get());
  context->setContextProperty("notificationService",
                              m_notificationService.get());
  context->setContextProperty("audioMixer", m_audioMixer.get());
  context->setContextProperty("windowService", m_windowService.get());
}

void Application::connectSignals() {
  // Connect recording manager signals
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this,
          &Application::recordingStateChanged);

  connect(m_recordingManager.get(), &RecordingManager::recordingStarted, this,
          [this](const QString &filePath) {
            m_notificationService->showNotification(
                "Recording Started",
                QString("Recording to %1").arg(QFileInfo(filePath).fileName()));
            m_recordingLibrary->setActiveRecordingPath(filePath);
          });

  connect(m_recordingManager.get(), &RecordingManager::recordingStopped, this,
          [this](const QString &filePath) {
            m_notificationService->showNotification(
                "Recording Stopped",
                QString("Saved to %1").arg(QFileInfo(filePath).fileName()));
            m_recordingLibrary->setActiveRecordingPath("");
            // Refresh library
            m_recordingLibrary->refresh();
          });

  // Connect game detection to recording
  connect(m_gameDetector.get(), &GameDetectionService::gameDetected, this,
          [this](const QString &gameName, qint64 pid) {
            qInfo() << "Game detected:" << gameName << "PID:" << pid;
            emit currentGameChanged();

            // Auto-record if enabled
            if (m_settingsManager->autoRecordEnabled()) {
              m_recordingManager->startRecording(gameName);
            }
          });

  connect(m_gameDetector.get(), &GameDetectionService::gameClosed, this,
          [this](const QString &gameName) {
            qInfo() << "Game closed:" << gameName;
            emit currentGameChanged();

            // Stop recording if game closes
            if (m_recordingManager->isRecording()) {
              m_recordingManager->stopRecording();
            }
          });

  // Connect settings changes
  connect(
      m_settingsManager.get(), &SettingsManager::recordingsPathChanged, this,
      [this]() {
        m_storageManager->setStoragePath(m_settingsManager->recordingsPath());
        m_recordingLibrary->setLibraryPath(m_settingsManager->recordingsPath());
      });

  connect(m_settingsManager.get(), &SettingsManager::maxStorageSizeChanged,
          this, [this]() {
            m_storageManager->setMaxSize(m_settingsManager->maxStorageSize());
          });

  connect(m_storageManager.get(), &StorageManager::recordingDeleted,
          m_recordingLibrary.get(), &RecordingLibrary::removeRecording);
}

void Application::setupSystemTray() {
  // TODO: Implement system tray icon for background operation
}

bool Application::isRecording() const {
  return m_recordingManager && m_recordingManager->isRecording();
}

QString Application::currentGameName() const {
  return m_gameDetector ? m_gameDetector->currentGameName() : QString();
}

} // namespace GameRecorder
