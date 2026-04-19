/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <qdebug.h>
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#include "Application.h"
#include "NotificationService.h"
#include "RecordingManager.h"
#include "StorageManager.h"
#include "WindowService.h"
#include "audio/AudioDeviceEnumerator.h"
#include "audio/AudioMixer.h"
#include "capture/CaptureFactory.h"
#include "detection/GameDetectionService.h"
#include "encoding/EncoderFactory.h"
#include "library/RecordingLibrary.h"
#include "settings/SettingsManager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QLoggingCategory>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <cstdio>

namespace GameRecorder {

Application::Application(int &argc, char **argv)
    : QGuiApplication(argc, argv) {}

Application::~Application() = default;

bool Application::initialize() {
  QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/trophy";
  QDir().mkpath(configDir);
  QString logPath = configDir + "/trophy.log";

  // Open log file in WriteOnly mode to truncate it each session
  static FILE* logFile = std::fopen(logPath.toUtf8().constData(), "w");

  // Install message handler to capture all Qt messages
  qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context,
                             const QString &msg) {
    const char *level = "UNKNOWN";
    bool shouldLogToConsole = true;

    switch (type) {
    case QtDebugMsg:
      level = "DEBUG";
      shouldLogToConsole = false; // Silence debug on console by default
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

    // Always write to the .log file with a timestamp
    // Filter out extremely noisy Qt internal logs
    QString category = QString::fromLatin1(context.category);
    QString filePath = QString::fromLatin1(context.file ? context.file : "");
    
    // Noise: anything from qt/qml/dbus categories, OR anything in the 'default' category 
    // that doesn't originate from our 'src/' directory.
    bool isTrophySource = filePath.contains("/src/") || filePath.contains("src/");
    bool isTechnicalNoise = category.startsWith("qt.") || 
                           category.startsWith("qml") || 
                           category.startsWith("dbus.") ||
                           (category == "default" && !isTrophySource);

    if (logFile && (!isTechnicalNoise || type != QtDebugMsg)) {
      QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
      std::fprintf(logFile, "[%s] [%s] [%s] %s\n", 
                   timestamp.toUtf8().constData(), 
                   level,
                   context.category,
                   msg.toUtf8().constData());
      std::fflush(logFile);
    }

    // Allow environment variables to override console silence
    static bool forceDebug = qEnvironmentVariableIsSet("TROPHY_DEBUG") || 
                             qgetenv("QT_LOGGING_RULES").contains("debug=true");
    
    if (shouldLogToConsole || forceDebug) {
      std::fprintf(stderr, "[Qt %s] %s\n", level, msg.toUtf8().constData());
      std::fflush(stderr);
    }
  });

  // Enable all debug logs but keep Qt internals quiet
  QLoggingCategory::setFilterRules("qt.*.debug=false\n"
                                   "*.debug=true");

  // Safely extract the version from the macro
#ifdef APP_VERSION
  QString versionStr = QString::fromUtf8(TOSTRING(APP_VERSION));
  // Remove any escaped quotes that might be passed from CMake
  versionStr.replace("\"", "");
#else
  QString versionStr = "0.0.0-unknown";
#endif

  // Set the version globally for Qt
  setApplicationVersion(versionStr);

  qInfo() << "Initializing Trophy - Game recording and clipping" << versionStr;

  // Initialize services in dependency order
  initializeServices();

  // Register Vulkan layer for the current user
  ensureVulkanLayerRegistered();

  // Register QML types
  registerQmlTypes();

  // Create QML engine
  m_engine = std::make_unique<QQmlApplicationEngine>();

  // Set app version in QML context
  m_engine->rootContext()->setContextProperty("AppVersion", versionStr);

  // Expose services to QML
  exposeServicesToQml();

  // Connect signals between services
  connectSignals();

  // Load main QML file
  QUrl url;
  url = QUrl(QStringLiteral("qrc:/qt/qml/GameRecorder/qml/main.qml"));

  QObject::connect(
      m_engine.get(), &QQmlApplicationEngine::objectCreationFailed, this,
      [this, url]() {
        qCritical() << "Failed to load QML:" << url;
        emit fatalError("Failed to load user interface");
        QCoreApplication::exit(1);
      },
      Qt::QueuedConnection);

  m_engine->load(url);

  if (m_engine->rootObjects().isEmpty()) {
    qCritical() << "No root objects created from QML";
    return false;
  }

  // Check for X11 session
  if (CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::X11) {
    qCritical() << "Trophy does not support native X11 sessions.";
    qCritical() << "Please run Trophy in a Wayland session.";
    emit fatalError("Trophy does not support native X11 sessions. Please switch to a Wayland session.");
    return false;
  }

  // Check for PipeWire availability on Wayland
  if (CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::Wayland) {
    if (!CaptureFactory::isPipeWireAvailable()) {
      qCritical() << "PipeWire or ScreenCast portal not available!";
      qCritical() << "Screen capture will not work.";
      emit fatalError("PipeWire or ScreenCast portal not found. Screen capture is unavailable.");
      return false;
    }
  }

  // Start game detection service
  m_gameDetector->start();

  qInfo() << "Application initialized successfully";
  return true;
}

void Application::initializeServices() {
  m_settingsManager = std::make_unique<SettingsManager>(this);
  m_settingsManager->load();

  m_storageManager = std::make_unique<StorageManager>(
      m_settingsManager->recordingsPath(), this);
  m_storageManager->setMaxSize(m_settingsManager->maxStorageSize());

  m_notificationService = std::make_unique<NotificationService>(this);

  m_recordingLibrary = std::make_unique<RecordingLibrary>(
      m_settingsManager->recordingsPath(), this);

  m_audioMixer = std::make_unique<AudioMixer>(48000, 2, this);

  m_audioDeviceEnumerator = std::make_unique<AudioDeviceEnumerator>(this);

  m_gameDetector = std::make_unique<GameDetectionService>(this);

  m_windowService = std::make_unique<WindowService>(this);

  m_recordingManager = std::make_unique<RecordingManager>(this);
  m_recordingManager->setSettingsManager(m_settingsManager.get());
  m_recordingManager->setStorageManager(m_storageManager.get());
  m_recordingManager->setAudioMixer(m_audioMixer.get());
  m_recordingManager->setWindowService(m_windowService.get());
  m_recordingManager->setGameDetectionService(m_gameDetector.get());

  m_detectionDebounceTimer = new QTimer(this);
  m_detectionDebounceTimer->setSingleShot(true);
  m_detectionDebounceTimer->setInterval(3000); // 3 seconds debounce
}

void Application::registerQmlTypes() {
  qRegisterMetaType<AudioDeviceInfo>("AudioDeviceInfo");
}

void Application::exposeServicesToQml() {
  QQmlContext *context = m_engine->rootContext();

  context->setContextProperty("app", this);
  context->setContextProperty("recordingManager", m_recordingManager.get());
  context->setContextProperty("settingsManager", m_settingsManager.get());
  context->setContextProperty("storageManager", m_storageManager.get());
  context->setContextProperty("recordingLibrary", m_recordingLibrary.get());
  context->setContextProperty("gameDetector", m_gameDetector.get());
  context->setContextProperty("notificationService",
                              m_notificationService.get());
  context->setContextProperty("audioMixer", m_audioMixer.get());
  context->setContextProperty("windowService", m_windowService.get());
  context->setContextProperty("audioDeviceEnumerator",
                              m_audioDeviceEnumerator.get());
}

void Application::connectSignals() {
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
            m_recordingLibrary->refresh();
          });

  connect(m_gameDetector.get(), &GameDetectionService::gameDetected, this,
          [this](const QString &gameName, qint64 pid) {
            qInfo() << "Game detected:" << gameName;
            emit currentGameChanged();

            if (m_settingsManager->autoRecordEnabled()) {
              QString token = m_settingsManager->getRestoreToken(gameName);
              if (!token.isEmpty()) {
                // If we have a token, we can just start immediately.
                // Cancel any pending debounce since we found a definitive match
                m_detectionDebounceTimer->stop();
                m_pendingAutoRecordGame.clear();

                qInfo() << "Auto-recording game with saved token:" << gameName;
                m_recordingManager->startRecording(gameName);
              } else {
                // No token: debounce the notification to let detection settle
                // (prevents helper processes like 'wine64' from triggering
                // before the actual game)
                m_pendingAutoRecordGame = gameName;
                m_detectionDebounceTimer->start();
                qDebug() << "Debouncing authorization notification for:"
                        << gameName;
              }
            }
          });

  connect(m_detectionDebounceTimer, &QTimer::timeout, this, [this]() {
    if (!m_pendingAutoRecordGame.isEmpty()) {
      qDebug() << "Debounce complete, notifying user for:"
              << m_pendingAutoRecordGame;
      m_notificationService->showNotification(
          "New Game: " + m_pendingAutoRecordGame,
          "Click to authorize Wayland capture for this game.", "trophy",
          0, // Never expire
          2  // Critical urgency
      );
    }
  });

  connect(m_gameDetector.get(), &GameDetectionService::gameClosed, this,
          [this](const QString &gameName) {
            qInfo() << "Game closed:" << gameName;
            emit currentGameChanged();

            if (m_pendingAutoRecordGame == gameName) {
              qDebug() << "Cancelling pending notification for closed game:" << gameName;
              m_detectionDebounceTimer->stop();
              m_pendingAutoRecordGame.clear();
            }

            if (m_recordingManager->isRecording() && m_recordingManager->currentGameName() == gameName) {
              m_recordingManager->stopRecording();
            }
          });

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

  connect(m_notificationService.get(), &NotificationService::notificationClicked,
          this, [this](const QString &/*action*/) {
            if (!m_pendingAutoRecordGame.isEmpty()) {
              qInfo() << "User authorized capture for:"
                       << m_pendingAutoRecordGame;
              m_recordingManager->startRecording(m_pendingAutoRecordGame);
              m_pendingAutoRecordGame.clear();
            }
          });

  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingAdded,
          m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingRemoved,
          m_storageManager.get(), &StorageManager::refresh);

  // Keep storage bar live while recording by refreshing on each progress tick
  connect(m_recordingManager.get(), &RecordingManager::progressUpdated, this,
          [this](qint64, qint64) { m_storageManager->refresh(); });
}

void Application::setupSystemTray() {}

bool Application::isRecording() const {
  return m_recordingManager && m_recordingManager->isRecording();
}

QString Application::currentGameName() const {
  return m_gameDetector ? m_gameDetector->currentGameName() : QString();
}

bool Application::isWayland() const {
  return CaptureFactory::detectDisplayServer() ==
         CaptureFactory::DisplayServer::Wayland;
}

bool Application::isEncoderAvailable(const QString &name) const {
  if (name == "auto" || name == "x264")
    return true;

  for (const auto &enc : EncoderFactory::detectAvailableEncoders()) {
    if (enc.name == name)
      return true;
  }
  return false;
}

bool Application::isControlModifierPressed() const {
  return (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) != 0;
}

void Application::ensureVulkanLayerRegistered() {
  QString configDir =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
  QString layerDir = configDir + "/vulkan/implicit_layer.d";
  QString layerJsonPath = layerDir + "/trophy_layer.json";

  // Create directory if it doesn't exist
  QDir().mkpath(layerDir);

  // Find source files
  // 1. Check relative to app binary (development/portable)
  QString appDir = QCoreApplication::applicationDirPath();
  QString srcJsonPath = appDir + "/vulkan_layer/trophy_layer.json";
  QString srcSoPath = appDir + "/vulkan_layer/trophy_layer.so";

  // 2. Fallback to system path if not found (installed)
  if (!QFile::exists(srcJsonPath)) {
    srcJsonPath = "/usr/share/trophy/vulkan_layer/trophy_layer.json";
    srcSoPath = "/usr/lib/vulkan/layer/trophy_layer.so";
  }

  if (!QFile::exists(srcJsonPath)) {
    qWarning() << "Could not find Vulkan layer manifest at" << srcJsonPath;
    return;
  }

  // Read source JSON
  QFile srcFile(srcJsonPath);
  if (!srcFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "Failed to open source Vulkan layer manifest";
    return;
  }

  QString content = QString::fromUtf8(srcFile.readAll());
  srcFile.close();

  // Replace relative/placeholder path with absolute path to the .so
  content.replace("trophy_layer.so", srcSoPath);
  content.replace("@LIBRARY_PATH@", srcSoPath);

  // Write to destination
  QFile destFile(layerJsonPath);
  if (destFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
    destFile.write(content.toUtf8());
    destFile.close();
    qInfo() << "Successfully registered Vulkan detection layer at"
            << layerJsonPath;
  } else {
    qWarning() << "Failed to write Vulkan layer manifest to" << layerJsonPath;
  }
}

} // namespace GameRecorder