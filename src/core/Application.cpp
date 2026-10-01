/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <qdebug.h>
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#include <unistd.h> // isatty(STDIN_FILENO) for launch-context logging

#include "Application.h"
#include "AutostartManager.h"
#include "ControlServer.h"
#include "GlobalShortcutManager.h"
#include "NotificationService.h"
#include "ReplayManager.h"
#include "HardwareInfo.h"
#include "RecordingManager.h"
#include "StorageManager.h"
#include "SystemTrayManager.h"
#include "WindowService.h"
#include "audio/AudioDeviceEnumerator.h"
#include "audio/AudioMixer.h"
#include "capture/CaptureFactory.h"
#include "detection/GameDetectionService.h"
#include "detection/GlShimEnrollment.h"
#include "detection/VulkanLayer.h"
#include "detection/WaylandWindowMonitor.h"
#include "encoding/EncoderFactory.h"
#include "library/RecordingLibrary.h"
#include "settings/SettingsManager.h"

#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QLoggingCategory>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstdio>

Q_LOGGING_CATEGORY(appLog, "trophy.app")

namespace TrophyBackend {

Application::Application(int &argc, char **argv)
    : QApplication(argc, argv) {}

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
  // The Vulkan layer is optional: only (re)register when enabled. A user
  // who opted out keeps no layer JSON behind.
  if (m_settingsManager->vulkanLayerEnabled()) {
    m_vulkanLayerRegistered = ensureVulkanLayerRegistered();
  } else {
    removeVulkanLayerRegistration();
    m_vulkanLayerRegistered = false;
  }
  emit detectionStatusChanged();
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

  if (m_trayManager && m_trayManager->isAvailable()) {
    m_trayManager->setTrayVisible(true);
    m_trayManager->setRecording(m_recordingManager->isRecording(),
                                 m_recordingManager->currentGameName());
    emit trayAvailabilityChanged();
    if (m_settingsManager->startMinimized() || m_forceMinimized) {
      qCInfo(appLog) << "Starting minimized to system tray";
      setMainWindowVisible(false);
    }
  } else {
    qCInfo(appLog) << "No system tray host; running with plain window";
  }

  syncAutostartFile();

  // Control socket for native compositor keybinds (best effort).
  if (m_controlServer && !m_controlServer->listen()) {
    qCWarning(appLog) << "Control socket unavailable; native keybinds disabled";
  }

  // Global shortcuts register once the event loop spins (portal D-Bus) —
  // but only after first-run setup: the wizard's Shortcuts step asks
  // first, so the portal approval dialog never ambushes a new user.
  QTimer::singleShot(0, this, [this]() {
    if (m_shortcutManager && m_settingsManager &&
        m_settingsManager->setupCompleted() &&
        m_settingsManager->globalShortcutsEnabled())
      m_shortcutManager->requestShortcuts();
  });

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
  // Launch context: portal restore tokens are attributed per app identity,
  // so a token saved by a terminal run may not restore in a launcher run
  // (and vice versa). GIO_LAUNCHED_DESKTOP_FILE only exists for gio/GNOME
  // launches, so stdin being a tty is the more reliable terminal signal
  // on KDE — log the raw facts plus the guess.
  const bool stdinTty = ::isatty(STDIN_FILENO);
  const QByteArray gioEnv = qgetenv("GIO_LAUNCHED_DESKTOP_FILE");
  qCInfo(appLog) << "Launched from:" << QCoreApplication::applicationFilePath()
                 << "stdin-tty:" << stdinTty << "desktop-file-env:"
                 << (gioEnv.isEmpty() ? QStringLiteral("none")
                                     : QString::fromLocal8Bit(gioEnv))
                 << (stdinTty ? "(likely terminal)" : "(likely launcher)");
  qCInfo(appLog) << "Application initialized successfully";
  return true;
}

void Application::initializeServices() {
  m_settingsManager = std::make_unique<SettingsManager>(this);
  m_settingsManager->load();
  m_storageManager = std::make_unique<StorageManager>(m_settingsManager->recordingsPath(), this);
  m_storageManager->setMaxSize(m_settingsManager->maxStorageSize());
  m_storageManager->setAutoDeleteOldest(m_settingsManager->autoDeleteOldest());
  m_notificationService = std::make_unique<NotificationService>(this);
  m_hardwareInfo = std::make_unique<HardwareInfo>(this);
  m_recordingLibrary = std::make_unique<RecordingLibrary>(m_settingsManager->recordingsPath(), this);
  m_audioMixer = std::make_unique<AudioMixer>(48000, 2, this);
  m_audioDeviceEnumerator = std::make_unique<AudioDeviceEnumerator>(this);
  m_gameDetector = std::make_unique<GameDetectionService>(this);
  m_windowService = std::make_unique<WindowService>(this);
  m_windowMonitor = std::make_unique<WaylandWindowMonitor>(this);
  m_windowService->setWindowMonitor(m_windowMonitor.get());
  m_gameDetector->setWindowMonitor(m_windowMonitor.get());
  if (m_settingsManager->windowTrackingEnabled())
    m_windowMonitor->startMonitoring();
  else
    // Heal orphans: a script left behind by an older build, a deleted
    // settings file or a crash mid-toggle is removed so a disabled
    // switch never reports "Installed". Mirrors the Vulkan cleanup in
    // initialize().
    m_windowMonitor->uninstallBackend();
  connect(m_settingsManager.get(),
          &SettingsManager::windowTrackingEnabledChanged, this, [this]() {
            if (m_settingsManager->windowTrackingEnabled()) {
              m_windowMonitor->startMonitoring();
            } else {
              // Explicit opt-out: stop tracking and remove the script
              // package we installed (reinstalled on re-enable).
              m_windowMonitor->uninstallBackend();
            }
          });
  m_recordingManager = std::make_unique<RecordingManager>(this);
  
  m_recordingManager->setSettingsManager(m_settingsManager.get());
  m_recordingManager->setStorageManager(m_storageManager.get());
  m_recordingManager->setAudioMixer(m_audioMixer.get());
  m_recordingManager->setWindowService(m_windowService.get());
  m_recordingManager->setGameDetectionService(m_gameDetector.get());

  m_detectionDebounceTimer = new QTimer(this);
  m_detectionDebounceTimer->setSingleShot(true);
  m_detectionDebounceTimer->setInterval(3000);

  m_windowWaitTimer = new QTimer(this);
  m_windowWaitTimer->setSingleShot(true);
  m_windowWaitTimer->setInterval(kWindowWaitMs);

  m_trayManager = std::make_unique<SystemTrayManager>(this);
  m_autostartManager = std::make_unique<AutostartManager>(this);
  m_replayManager = std::make_unique<ReplayManager>(this);
  m_replayManager->setSettingsManager(m_settingsManager.get());
  m_shortcutManager = std::make_unique<GlobalShortcutManager>(this);
  m_shortcutManager->setEnabled(m_settingsManager->globalShortcutsEnabled());
  m_controlServer = std::make_unique<ControlServer>(this);
  connect(m_controlServer.get(), &ControlServer::toggleRecordingRequested,
          this, [this]() { requestToggleRecording(); });
  connect(m_controlServer.get(), &ControlServer::saveClipRequested, this,
          [this]() { requestReplay(); });
  connect(m_controlServer.get(), &ControlServer::showWindowRequested, this,
          [this]() { showMainWindow(); });
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
  context->setContextProperty("hardwareInfo", m_hardwareInfo.get());
  context->setContextProperty("audioMixer", m_audioMixer.get());
  context->setContextProperty("windowService", m_windowService.get());
  context->setContextProperty("windowMonitor", m_windowMonitor.get());
  context->setContextProperty("audioDeviceEnumerator", m_audioDeviceEnumerator.get());
  context->setContextProperty("globalShortcuts", m_shortcutManager.get());
}

void Application::connectSignals() {
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this, &Application::recordingStateChanged);
  connect(m_recordingManager.get(), &RecordingManager::recordingStarted, this, [this](const QString &filePath) {
            m_recordingLibrary->setActiveRecordingPath(filePath);
            // The background replay loop pauses while recording.
            m_replayManager->setPaused(true);
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingStopped, this, [this](const QString &filePath, qint64, qint64) {
            m_recordingLibrary->setActiveRecordingPath("");
            m_recordingLibrary->refresh();
            // Resume the replay loop if the game is still running.
            m_replayManager->setPaused(false);
            if (m_gameDetector && !m_gameDetector->currentGameName().isEmpty()) {
              m_replayManager->startDvr(m_gameDetector->currentGameName());
            }
          });
  connect(m_gameDetector.get(), &GameDetectionService::gameDetected, this, [this](const QString &gameName, qint64 pid) {
            qCInfo(appLog) << "Game detected:" << gameName;
            emit currentGameChanged();
            if (m_settingsManager->autoRecordEnabled()) {
              QString token = m_settingsManager->getRestoreToken(gameName);
              if (!token.isEmpty()) {
                m_detectionDebounceTimer->stop();
                m_pendingAutoRecordGame.clear();
                startWhenWindowReady(gameName, true);
              } else if (m_pendingAutoRecordGame != gameName ||
                         !m_detectionDebounceTimer->isActive()) {
                m_pendingAutoRecordGame = gameName;
                m_detectionDebounceTimer->start();
              }
            } else if (m_settingsManager->instantReplayEnabled()) {
              // Silent when a restore token exists (no portal popup);
              // otherwise arm through the same authorize-click channel
              // as recording — never a surprise picker at launch.
              if (!m_settingsManager->getRestoreToken(gameName).isEmpty()) {
                m_pendingDvrGame.clear();
                startWhenWindowReady(gameName, false);
              } else if (m_pendingDvrGame != gameName ||
                         !m_detectionDebounceTimer->isActive()) {
                m_pendingDvrGame = gameName;
                m_detectionDebounceTimer->start();
              }
            }
          });
  connect(m_windowMonitor.get(), &WaylandWindowMonitor::windowsChanged, this, [this]() {
            if (m_pendingWindowGame.isEmpty() ||
                !m_gameDetector->hasVisibleWindow())
              return;
            const QString game = m_pendingWindowGame;
            const bool isRecord = m_pendingWindowIsRecord;
            m_pendingWindowGame.clear();
            m_windowWaitTimer->stop();
            qCDebug(appLog) << "Window appeared for" << game
                            << "- starting deferred capture";
            if (isRecord)
              m_recordingManager->startRecording(game);
            else
              m_replayManager->startDvr(game);
          });
  connect(m_windowWaitTimer, &QTimer::timeout, this, [this]() {
            if (m_pendingWindowGame.isEmpty())
              return;
            // No window in time: fall back to the authorize prompt, never a
            // bare picker. The click then starts capture as the user's own
            // action (a picker there has context).
            const QString game = m_pendingWindowGame;
            const bool isRecord = m_pendingWindowIsRecord;
            m_pendingWindowGame.clear();
            qCDebug(appLog) << "No window appeared for" << game
                            << "- falling back to authorize prompt";
            if (isRecord)
              m_pendingAutoRecordGame = game;
            else
              m_pendingDvrGame = game;
            m_detectionDebounceTimer->start();
          });
  connect(m_detectionDebounceTimer, &QTimer::timeout, this, [this]() {
    const QString pending = !m_pendingAutoRecordGame.isEmpty()
                                ? m_pendingAutoRecordGame
                                : m_pendingDvrGame;
    if (!pending.isEmpty()) {
      m_notificationService->showNotification("New Game: " + pending, "Click to authorize Wayland capture for this game.", "trophy", 0, 2, true);
    }
  });
  connect(m_recordingManager.get(), &RecordingManager::restoreAuthNeeded, this, [this](const QString &game) {
            // Stale token: delete it so the next attempt can't reuse it,
            // then authorize again through the prompt. The picker that may
            // follow the click is the user's own action, never a surprise.
            qCDebug(appLog) << "Restore token stale for" << game
                            << "- re-authorizing via prompt";
            m_settingsManager->removeRestoreToken(game);
            m_pendingAutoRecordGame = game;
            m_detectionDebounceTimer->start();
          });
  connect(m_replayManager.get(), &ReplayManager::restoreAuthNeeded, this, [this](const QString &game) {
            qCDebug(appLog) << "DVR restore token stale for" << game
                            << "- re-authorizing via prompt";
            m_settingsManager->removeRestoreToken(game);
            m_pendingDvrGame = game;
            m_detectionDebounceTimer->start();
          });
  connect(m_gameDetector.get(), &GameDetectionService::gameClosed, this, [this](const QString &gameName) {
            emit currentGameChanged();
            if (m_pendingAutoRecordGame == gameName) {
              m_pendingAutoRecordGame.clear();
            }
            if (m_pendingDvrGame == gameName) {
              m_pendingDvrGame.clear();
            }
            if (m_pendingWindowGame == gameName) {
              m_pendingWindowGame.clear();
              m_windowWaitTimer->stop();
            }
            if (m_pendingAutoRecordGame.isEmpty() && m_pendingDvrGame.isEmpty()) {
              m_detectionDebounceTimer->stop();
            }
            m_replayManager->stopDvr();
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
  connect(m_settingsManager.get(), &SettingsManager::autoDeleteOldestChanged,
          this, [this]() {
            m_storageManager->setAutoDeleteOldest(
                m_settingsManager->autoDeleteOldest());
          });
  connect(m_storageManager.get(), &StorageManager::storageLimitReached, this,
          [this]() {
            if (m_suppressNextStorageNotification) {
              m_suppressNextStorageNotification = false;
              return;
            }
            if (m_settingsManager->autoDeleteOldest()) {
              m_notificationService->showNotification(
                  "Storage Full",
                  "Recordings exceed the storage limit. Oldest files will be auto-deleted, or free space manually.",
                  "dialog-warning", 0, 2);
            } else {
              m_notificationService->showNotification(
                  "Storage Full",
                  "Recordings exceed the storage limit. Enable auto-cleanup or delete old recordings manually.",
                  "dialog-warning", 0, 2);
            }
          });
  connect(m_storageManager.get(), &StorageManager::storageLimitCannotCleanup,
          m_recordingManager.get(), &RecordingManager::stopRecording);
  connect(m_storageManager.get(), &StorageManager::storageLimitCannotCleanup,
          this, [this]() {
            m_notificationService->showNotification(
                "Recording Stopped",
                "Recording stopped: recording exceeds maximum storage size.",
                "dialog-warning", 0, 2);
          });
  connect(m_storageManager.get(), &StorageManager::recordingDeleted, m_recordingLibrary.get(), &RecordingLibrary::removeRecording);
  // Order matters: the rename preference must be recorded before the
  // refresh recreates Recording objects (which consult it in addRecording).
  connect(m_storageManager.get(), &StorageManager::recordingRenamed, m_recordingLibrary.get(), &RecordingLibrary::onRecordingRenamed);
  connect(m_storageManager.get(), &StorageManager::recordingRenamed, m_recordingLibrary.get(), &RecordingLibrary::refresh);
  connect(m_notificationService.get(), &NotificationService::notificationClicked, this, [this](const QString &) {
            if (!m_pendingAutoRecordGame.isEmpty()) {
              m_notificationService->closeLastNotification();
              m_recordingManager->startRecording(m_pendingAutoRecordGame);
              m_pendingAutoRecordGame.clear();
            } else if (!m_pendingDvrGame.isEmpty()) {
              m_notificationService->closeLastNotification();
              m_replayManager->startDvr(m_pendingDvrGame);
              m_pendingDvrGame.clear();
            }
          });
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingAdded, m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingRemoved, m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingManager.get(), &RecordingManager::progressUpdated, this,
          [this](qint64, qint64) { m_storageManager->enforceStorageLimit(); });

  // --- System tray ---
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this,
          [this]() {
            if (m_trayManager) {
              m_trayManager->setRecording(
                  m_recordingManager->isRecording(),
                  m_recordingManager->currentGameName());
            }
          });
  connect(m_recordingManager.get(), &RecordingManager::progressUpdated, this,
          [this](qint64 duration, qint64 size) {
            if (m_trayManager) {
              m_trayManager->setProgress(duration, size);
            }
          });
  // These were never connected — recording start/stop produced no notice
  // at all (critical urgency alone can't show a notification nobody sends).
  connect(m_recordingManager.get(), &RecordingManager::recordingStarted, this,
          [this](const QString &) {
            m_notificationService->showRecordingStarted(
                m_recordingManager->currentGameName());
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingStopped, this,
          [this](const QString &, qint64 durationMs, qint64 sizeBytes) {
            const QString summary = QStringLiteral("%1 · %2").arg(
                NotificationService::formatDuration(durationMs),
                StorageManager::formatSize(sizeBytes));
            m_notificationService->showRecordingStopped(summary);
          });
  connect(m_trayManager.get(), &SystemTrayManager::showWindowRequested, this,
          [this]() { showMainWindow(); });
  connect(m_trayManager.get(), &SystemTrayManager::hideWindowRequested, this,
          [this]() { setMainWindowVisible(false); });
  connect(m_trayManager.get(), &SystemTrayManager::startRecordingRequested,
          this, [this]() {
            if (m_recordingManager->isRecording()) {
              return;
            }
            const QString game = m_gameDetector
                                     ? m_gameDetector->currentGameName()
                                     : QString();
            m_recordingManager->startRecording(game);
          });
  connect(m_trayManager.get(), &SystemTrayManager::stopRecordingRequested,
          this, [this]() {
            if (m_recordingManager->isRecording()) {
              m_recordingManager->stopRecording();
            }
          });
  connect(m_trayManager.get(), &SystemTrayManager::openRecordingsRequested,
          this, [this]() {
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(m_settingsManager->recordingsPath()));
          });
  connect(m_trayManager.get(), &SystemTrayManager::quitRequested, this,
          [this]() { requestQuit(); });
  connect(m_trayManager.get(), &SystemTrayManager::saveReplayRequested, this,
          [this]() { requestReplay(); });
  connect(m_shortcutManager.get(),
          &GlobalShortcutManager::toggleRecordingRequested, this,
          [this]() { requestToggleRecording(); });
  connect(m_shortcutManager.get(), &GlobalShortcutManager::saveClipRequested,
          this, [this]() { requestReplay(); });

  // --- Clips ---
  connect(m_replayManager.get(), &ReplayManager::replaySaved, this,
          [this](const QString &filePath, double seconds) {
            const QString game = m_replayManager->currentGame();
            const qint64 secs = std::max<qint64>(1, llround(seconds));
            const QString span = (secs == 1) ? QStringLiteral("Last 1 second")
                                             : QStringLiteral("Last %1 seconds").arg(secs);
            const QString size =
                StorageManager::formatSize(QFileInfo(filePath).size());
            m_notificationService->showNotification(
                game.isEmpty() ? QStringLiteral("Clip saved")
                               : QStringLiteral("%1 clip saved").arg(game),
                QStringLiteral("%1 · %2").arg(span, size),
                "trophy", 3000, 2); // critical: visible over screenshare
            m_recordingLibrary->refresh();
          });
  connect(m_replayManager.get(), &ReplayManager::replayFailed, this,
          [this](const QString &reason) {
            m_notificationService->showNotification(
                "Couldn't Save Clip", reason, "dialog-warning", 3000, 2);
          });
  connect(m_settingsManager.get(), &SettingsManager::instantReplayEnabledChanged,
          this, [this]() {
            if (!m_settingsManager->instantReplayEnabled()) {
              m_replayManager->stopDvr();
            } else if (m_gameDetector &&
                       !m_gameDetector->currentGameName().isEmpty() &&
                       !m_recordingManager->isRecording()) {
              m_replayManager->startDvr(m_gameDetector->currentGameName());
            }
          });
  connect(m_settingsManager.get(),
          &SettingsManager::globalShortcutsEnabledChanged, this, [this]() {
            if (m_shortcutManager)
              m_shortcutManager->setEnabled(
                  m_settingsManager->globalShortcutsEnabled());
          });

  // --- Autostart: settings are the source of truth; the file follows ---
  connect(m_settingsManager.get(), &SettingsManager::startOnLoginChanged,
          this, [this]() { syncAutostartFile(); });
}

bool Application::isRecording() const { return m_recordingManager && m_recordingManager->isRecording(); }
QString Application::currentGameName() const { return m_gameDetector ? m_gameDetector->currentGameName() : QString(); }

void Application::startWhenWindowReady(const QString &game, bool isRecord) {
  if (m_gameDetector && m_gameDetector->hasVisibleWindow()) {
    qCDebug(appLog) << "Window present for" << game
                    << "- starting capture immediately";
    if (isRecord)
      m_recordingManager->startRecording(game);
    else
      m_replayManager->startDvr(game);
    return;
  }
  // The portal can only restore against a concrete window; requesting now
  // would pop a picker for a game that is still launching. Wait for the
  // window (timeout falls back to the authorize prompt, never a picker).
  qCDebug(appLog) << "Deferring capture for" << game
                  << "until its window appears";
  m_pendingWindowGame = game;
  m_pendingWindowIsRecord = isRecord;
  m_windowWaitTimer->start();
}
bool Application::isWayland() const { return CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::Wayland; }

bool Application::isEncoderAvailable(const QString &name) const {
  if (name == "auto" || name == "x264") return true;
  for (const auto &enc : EncoderFactory::detectAvailableEncoders()) { if (enc.name == name) return true; }
  return false;
}

bool Application::suppressNextStorageNotification() const {
  return m_suppressNextStorageNotification;
}

void Application::setSuppressNextStorageNotification(bool suppress) {
  if (m_suppressNextStorageNotification != suppress) {
    m_suppressNextStorageNotification = suppress;
    emit suppressNextStorageNotificationChanged();
  }
}

bool Application::isTrayAvailable() const {
  return m_trayManager && m_trayManager->isAvailable();
}

QObject *Application::qmlRootWindow() const {
  if (!m_engine || m_engine->rootObjects().isEmpty()) {
    return nullptr;
  }
  return m_engine->rootObjects().first();
}

void Application::setMainWindowVisible(bool visible) {
  QObject *root = qmlRootWindow();
  if (!root) {
    return;
  }
  root->setProperty("visible", visible);
  if (visible) {
    if (QWindow *win = qobject_cast<QWindow *>(root)) {
      win->show();
      win->raise();
      win->requestActivate();
    }
  }
  if (m_trayManager) {
    m_trayManager->setWindowVisible(visible);
  }
}

void Application::showMainWindow() { setMainWindowVisible(true); }

bool Application::minimizeToTray() {
  if (!isTrayAvailable() || !m_settingsManager->minimizeToTray()) {
    return false;
  }
  setMainWindowVisible(false);
  if (!m_minimizeNotifyShown) {
    m_minimizeNotifyShown = true;
    m_notificationService->showNotification(
        "Trophy minimized",
        m_recordingManager->isRecording()
            ? "Recording continues in the background. Right-click the tray icon to stop."
            : "Trophy keeps running in the system tray.",
        "trophy", 5000, 1);
  }
  return true;
}

void Application::requestQuit() {
  if (m_recordingManager && m_recordingManager->isRecording()) {
    // Bring the window up and let the QML confirm dialog decide.
    showMainWindow();
    if (QObject *root = qmlRootWindow()) {
      QMetaObject::invokeMethod(root, "requestQuit", Qt::QueuedConnection);
    }
    return;
  }
  QCoreApplication::quit();
}

void Application::requestReplay() {
  if (!m_replayManager || !m_settingsManager) {
    return;
  }
  if (m_recordingManager && m_recordingManager->isRecording()) {
    m_notificationService->showNotification(
        "Already Recording",
        "You're already recording — the full session is being saved.",
        "trophy", 3000, 2);
    return;
  }
  if (!m_settingsManager->instantReplayEnabled()) {
    m_notificationService->showNotification(
        "Clips Are Off",
        "Clips are off. Enable them in Settings to buffer gameplay.",
        "trophy", 3000, 2);
    return;
  }
  if (!m_replayManager->isActive()) {
    m_notificationService->showNotification(
        "Nothing Buffered",
        "Launch a game first — there's nothing buffered yet.",
        "trophy", 3000, 2);
    return;
  }
  const int secs = std::max(1, m_settingsManager->clipBufferSeconds());
  m_notificationService->showNotification(
      "Saving Clip", QString("Saving the last %1 seconds…").arg(secs),
      "trophy", 3000, 2);
  m_replayManager->saveReplay();
}

void Application::requestToggleRecording() {
  if (!m_recordingManager) {
    return;
  }
  if (m_recordingManager->isRecording()) {
    m_recordingManager->stopRecording();
    return;
  }
  const QString game =
      m_gameDetector ? m_gameDetector->currentGameName() : QString();
  if (!game.isEmpty()) {
    m_recordingManager->startRecording(game);
    return;
  }
  // Same as in-app F9 with no game, but a global keypress deserves
  // feedback instead of a silent no-op.
  m_notificationService->showNotification(
      "Trophy", "No game detected — open Trophy to record manually.",
      "trophy", 5000, 1);
}

void Application::syncAutostartFile() {
  if (!m_autostartManager || !m_settingsManager) {
    return;
  }
  const bool want = m_settingsManager->startOnLogin();
  if (want == m_autostartManager->isEnabled()) {
    return;
  }
  if (!m_autostartManager->setEnabled(want, QCoreApplication::applicationFilePath())) {
    qCWarning(appLog) << "Failed to sync autostart entry";
  }
}

bool Application::glShimAvailable() const {
  return !GlShimEnrollment::libraryPath().isEmpty();
}

bool Application::glShimEnrolled() const {
  return GlShimEnrollment::isEnrolled();
}

bool Application::glShimActive() const {
  return GlShimEnrollment::isActiveInCurrentSession();
}

bool Application::setGlShimEnrolled(bool enrolled) {
  const bool ok = GlShimEnrollment::setEnrolled(enrolled);
  emit detectionStatusChanged();
  return ok;
}

bool Application::setVulkanLayerEnabled(bool enabled) {
  m_settingsManager->setVulkanLayerEnabled(enabled);
  bool ok;
  if (enabled) {
    m_vulkanLayerRegistered = ensureVulkanLayerRegistered();
    ok = m_vulkanLayerRegistered;
  } else {
    ok = removeVulkanLayerRegistration();
    m_vulkanLayerRegistered = false;
  }
  emit detectionStatusChanged();
  return ok;
}

bool Application::removeVulkanLayerRegistration() {
  // Opt-out removes our JSON so the loader stops loading the layer.
  // Absence counts as success: nothing registered is the desired state.
  return VulkanLayer::removeRegistration();
}

bool Application::ensureVulkanLayerRegistered() {
  return VulkanLayer::ensureRegistered();
}

} // namespace TrophyBackend