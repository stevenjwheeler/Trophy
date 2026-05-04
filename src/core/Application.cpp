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

Q_LOGGING_CATEGORY(appLog, "trophy.app")

namespace TrophyBackend {

Application::Application(int &argc, char **argv)
    : QGuiApplication(argc, argv) {}

Application::~Application() = default;

bool Application::initialize() {
  // 1. Force Qt to enable debug logging for our categories
  // This ensures messages actually reach our handler
  QLoggingCategory::setFilterRules("*.debug=true\n"
                                   "qt.*.debug=false\n"
                                   "qml.debug=false");

  QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/trophy";
  QDir().mkpath(configDir);
  QString logPath = configDir + "/trophy.log";

  // Open log file in append mode
  static FILE* logFile = std::fopen(logPath.toUtf8().constData(), "a");

  qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    const char *level = "UNKNOWN";
    bool isImportant = (type != QtDebugMsg);

    switch (type) {
        case QtDebugMsg:    level = "DEBUG";    break;
        case QtInfoMsg:     level = "INFO";     break;
        case QtWarningMsg:  level = "WARNING";  break;
        case QtCriticalMsg: level = "CRITICAL"; break;
        case QtFatalMsg:    level = "FATAL";    break;
    }

    // --- FILTERS ---
    QString category = QString::fromLatin1(context.category);
    QString filePath = QString::fromLatin1(context.file ? context.file : "");
    
    // Identify if the message comes from YOUR code.
    // If it's in the src directory, or uses the "default" category, we treat it as Trophy.
    bool isTrophySource = filePath.contains("/src/") || 
                          filePath.contains("src/") || 
                          (category == "default");
    
    // Identify "Noise" (Qt internals, QML engine babble)
    bool isTechnicalNoise = category.startsWith("qt.") || 
                           category.startsWith("qml") || 
                           category.startsWith("dbus.");

    // --- 1. LOG FILE LOGIC ---
    if (logFile && (isTrophySource || isImportant)) {
        QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
        std::fprintf(logFile, "[%s] [%s] [%s] %s\n", 
                     timestamp.toUtf8().constData(), 
                     level,
                     context.category,
                     msg.toUtf8().constData());
        std::fflush(logFile);
    }

    // --- 2. CONSOLE LOGIC ---
    // Usage: TROPHY_DEBUG=1 trophy
    static bool forceDebug = qEnvironmentVariableIsSet("TROPHY_DEBUG");
    
    if (isImportant || forceDebug) {
        // Keep console clean: only show noise if it's actually an error
        if (!isTechnicalNoise || isImportant) {
            std::fprintf(stderr, "[%s] %s\n", level, msg.toUtf8().constData());
            std::fflush(stderr);
        }
    }
  });

#ifdef APP_VERSION
  QString versionStr = QString::fromUtf8(TOSTRING(APP_VERSION));
  versionStr.replace("\"", "");
#else
  QString versionStr = "0.0.0-unknown";
#endif

  setApplicationVersion(versionStr);
  qCInfo(appLog) << "Initializing Trophy - Game recording and clipping" << versionStr;

  initializeServices();
  ensureVulkanLayerRegistered();
  registerQmlTypes();

  m_engine = std::make_unique<QQmlApplicationEngine>();
  m_engine->rootContext()->setContextProperty("AppVersion", versionStr);

  exposeServicesToQml();
  connectSignals();

  // --- PREPARE PROPERTIES FOR REQUIRED TAGS IN QML ---
  QVariantMap initialProperties;
  initialProperties.insert("app", QVariant::fromValue(this));
  initialProperties.insert("recordingManager", QVariant::fromValue(m_recordingManager.get()));
  initialProperties.insert("settingsManager", QVariant::fromValue(m_settingsManager.get()));
  initialProperties.insert("storageManager", QVariant::fromValue(m_storageManager.get()));
  initialProperties.insert("recordingLibrary", QVariant::fromValue(m_recordingLibrary.get()));
  initialProperties.insert("gameDetector", QVariant::fromValue(m_gameDetector.get()));

  m_engine->setInitialProperties(initialProperties);

  QUrl url;
  if (!m_mainQmlPath.isEmpty()) {
      url = QUrl::fromLocalFile(m_mainQmlPath);
      qCInfo(appLog) << "Live Reload active: Loading QML from filesystem:" << url;
  } else {
      url = QUrl(QStringLiteral("qrc:/qt/qml/TrophyBackend/qml/main.qml"));
  }

  QObject::connect(
      m_engine.get(), &QQmlApplicationEngine::objectCreationFailed, this,
      [this, url]() {
        qCCritical(appLog) << "Failed to load QML:" << url;
        emit fatalError("Failed to load user interface");
        QCoreApplication::exit(1);
      },
      Qt::QueuedConnection);

  m_engine->load(url);

  if (m_engine->rootObjects().isEmpty()) {
    qCCritical(appLog) << "No root objects created from QML";
    return false;
  }

  if (CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::X11) {
    qCCritical(appLog) << "Trophy does not support native X11 sessions.";
    emit fatalError("Trophy does not support native X11 sessions. Please switch to a Wayland session.");
    return false;
  }

  if (CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::Wayland) {
    if (!CaptureFactory::isPipeWireAvailable()) {
      qCCritical(appLog) << "PipeWire or ScreenCast portal not found!";
      emit fatalError("PipeWire or ScreenCast portal not found. Screen capture is unavailable.");
      return false;
    }
  }

  m_gameDetector->start();
  qCInfo(appLog) << "Application initialized successfully";
  return true;
}

void Application::initializeServices() {
  m_settingsManager = std::make_unique<SettingsManager>(this);
  m_settingsManager->load();
  m_storageManager = std::make_unique<StorageManager>(m_settingsManager->recordingsPath(), this);
  m_storageManager->setMaxSize(m_settingsManager->maxStorageSize());
  m_notificationService = std::make_unique<NotificationService>(this);
  m_recordingLibrary = std::make_unique<RecordingLibrary>(m_settingsManager->recordingsPath(), this);
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
  m_detectionDebounceTimer->setInterval(3000);
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
  context->setContextProperty("notificationService", m_notificationService.get());
  context->setContextProperty("audioMixer", m_audioMixer.get());
  context->setContextProperty("windowService", m_windowService.get());
  context->setContextProperty("audioDeviceEnumerator", m_audioDeviceEnumerator.get());
}

void Application::connectSignals() {
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this, &Application::recordingStateChanged);
  connect(m_recordingManager.get(), &RecordingManager::recordingStarted, this, [this](const QString &filePath) {
            m_notificationService->showNotification("Recording Started", QString("Recording to %1").arg(QFileInfo(filePath).fileName()));
            m_recordingLibrary->setActiveRecordingPath(filePath);
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingStopped, this, [this](const QString &filePath) {
            m_notificationService->showNotification("Recording Stopped", QString("Saved to %1").arg(QFileInfo(filePath).fileName()));
            m_recordingLibrary->setActiveRecordingPath("");
            m_recordingLibrary->refresh();
          });
  connect(m_gameDetector.get(), &GameDetectionService::gameDetected, this, [this](const QString &gameName, qint64 pid) {
            qCInfo(appLog) << "Game detected:" << gameName;
            emit currentGameChanged();
            if (m_settingsManager->autoRecordEnabled()) {
              QString token = m_settingsManager->getRestoreToken(gameName);
              if (!token.isEmpty()) {
                m_detectionDebounceTimer->stop();
                m_pendingAutoRecordGame.clear();
                m_recordingManager->startRecording(gameName);
              } else {
                m_pendingAutoRecordGame = gameName;
                m_detectionDebounceTimer->start();
              }
            }
          });
  connect(m_detectionDebounceTimer, &QTimer::timeout, this, [this]() {
    if (!m_pendingAutoRecordGame.isEmpty()) {
      m_notificationService->showNotification("New Game: " + m_pendingAutoRecordGame, "Click to authorize Wayland capture for this game.", "trophy", 0, 2);
    }
  });
  connect(m_gameDetector.get(), &GameDetectionService::gameClosed, this, [this](const QString &gameName) {
            emit currentGameChanged();
            if (m_pendingAutoRecordGame == gameName) {
              m_detectionDebounceTimer->stop();
              m_pendingAutoRecordGame.clear();
            }
            if (m_recordingManager->isRecording() && m_recordingManager->currentGameName() == gameName) {
              m_recordingManager->stopRecording();
            }
          });
  connect(m_settingsManager.get(), &SettingsManager::recordingsPathChanged, this, [this]() {
        m_storageManager->setStoragePath(m_settingsManager->recordingsPath());
        m_recordingLibrary->setLibraryPath(m_settingsManager->recordingsPath());
      });
  connect(m_settingsManager.get(), &SettingsManager::maxStorageSizeChanged, this, [this]() {
            m_storageManager->setMaxSize(m_settingsManager->maxStorageSize());
          });
  connect(m_storageManager.get(), &StorageManager::recordingDeleted, m_recordingLibrary.get(), &RecordingLibrary::removeRecording);
  connect(m_notificationService.get(), &NotificationService::notificationClicked, this, [this](const QString &) {
            if (!m_pendingAutoRecordGame.isEmpty()) {
              m_recordingManager->startRecording(m_pendingAutoRecordGame);
              m_pendingAutoRecordGame.clear();
            }
          });
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingAdded, m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingRemoved, m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingManager.get(), &RecordingManager::progressUpdated, this, [this](qint64, qint64) { m_storageManager->refresh(); });
}

bool Application::isRecording() const { return m_recordingManager && m_recordingManager->isRecording(); }
QString Application::currentGameName() const { return m_gameDetector ? m_gameDetector->currentGameName() : QString(); }
bool Application::isWayland() const { return CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::Wayland; }

bool Application::isEncoderAvailable(const QString &name) const {
  if (name == "auto" || name == "x264") return true;
  for (const auto &enc : EncoderFactory::detectAvailableEncoders()) { if (enc.name == name) return true; }
  return false;
}

bool Application::isControlModifierPressed() const { return (QGuiApplication::keyboardModifiers() & Qt::ControlModifier) != 0; }

void Application::ensureVulkanLayerRegistered() {
  QString configDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
  QString layerDir = configDir + "/vulkan/implicit_layer.d";
  QString layerJsonPath = layerDir + "/trophy_layer.json";
  QDir().mkpath(layerDir);
  QString appDir = QCoreApplication::applicationDirPath();
  QString srcJsonPath = appDir + "/vulkan_layer/trophy_layer.json";
  QString srcSoPath = appDir + "/vulkan_layer/trophy_layer.so";
  if (!QFile::exists(srcJsonPath)) {
    srcJsonPath = "/usr/share/trophy/vulkan_layer/trophy_layer.json";
    srcSoPath = "/usr/lib/vulkan/layer/trophy_layer.so";
  }
  if (!QFile::exists(srcJsonPath)) return;
  QFile srcFile(srcJsonPath);
  if (!srcFile.open(QIODevice::ReadOnly | QIODevice::Text)) return;
  QString content = QString::fromUtf8(srcFile.readAll());
  srcFile.close();
  content.replace("trophy_layer.so", srcSoPath);
  content.replace("@LIBRARY_PATH@", srcSoPath);
  QFile destFile(layerJsonPath);
  if (destFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
    destFile.write(content.toUtf8());
    destFile.close();
  }
}

} // namespace TrophyBackend