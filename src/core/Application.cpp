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
#include "BugReport.h"
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
#include "audio/PipeWireAudioSource.h"
#include "capture/CaptureFactory.h"
#include "detection/GameDetectionService.h"
#include "detection/GameIconProvider.h"
#include "detection/GameWhitelist.h"
#include "detection/GlShimEnrollment.h"
#include "detection/VulkanLayer.h"
#include "detection/WaylandWindowMonitor.h"
#include "encoding/EncoderFactory.h"
#include "library/ActivityFeed.h"
#include "library/RecordingLibrary.h"
#include "library/WaveformCache.h"
#include "library/FilmstripCache.h"
#include "settings/SettingsManager.h"
#include "UpdateChecker.h"

#include <QDateTime>
#include <QDBusServiceWatcher>
#include <QDBusConnection>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/log.h>
}
#include <cstdio>

Q_LOGGING_CATEGORY(appLog, "trophy.app")

namespace TrophyBackend {

Application::Application(int &argc, char **argv)
    : QApplication(argc, argv) {}

Application::~Application() {
  // Retiring pipelines retain raw mixer/settings/storage pointers. Settle
  // managers before those dependencies (member order would destroy them first).
  if (m_gameDetector) disconnect(m_gameDetector.get(), nullptr, this, nullptr);
  if (m_recordingManager) disconnect(m_recordingManager.get(), nullptr, this, nullptr);
  if (m_replayManager) disconnect(m_replayManager.get(), nullptr, this, nullptr);
  m_engine.reset();
  m_controlServer.reset();
  m_recordingManager.reset();
  m_replayManager.reset();
}

bool Application::initialize() {
  // 1. Force Qt to enable debug logging for our categories
  // This ensures messages actually reach our handler
  QLoggingCategory::setFilterRules("*.debug=true\n"
                                   "qt.*.debug=false\n"
                                   "qml.debug=false");

  // FFmpeg (our pipeline and Qt Multimedia's playback backend share
  // the process-global av_log level) dumps full stream metadata at
  // INFO on every container open — the player opens each file four
  // times (video + one audio pipeline per stream), spamming the
  // console with identical blocks that bypass the message handler
  // below entirely. Keep warnings/errors; the dumps come back with
  // TROPHY_FFMPEG_VERBOSE=1 in the environment.
  if (qEnvironmentVariableIsEmpty("TROPHY_FFMPEG_VERBOSE"))
    av_log_set_level(AV_LOG_WARNING);

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
    bool isTrophySource = category.startsWith("trophy.") || filePath.contains("/src/") ||
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

  // Claim the control socket early (before QML/tray/detection): remote
  // --toggle-record/--save-clip/--show-window commands sent during startup
  // would otherwise find nobody listening and be dropped. Normally main()'s
  // instance lock guarantees no rival; a false return here means we lost a
  // concurrent-startup race, so forward a raise and refuse to run doubled.
  if (m_controlServer && !m_controlServer->listen()) {
    ControlServer::sendCommand(QStringLiteral("show-window")); // best effort
    qCCritical(appLog) << "Another Trophy instance is running; exiting.";
    return false;
  }
  // The Vulkan layer is optional: only (re)register when enabled. A user
  // who opted out keeps no layer JSON behind.
  if (m_settingsManager->vulkanLayerEnabled()) {
    m_vulkanLayerRegistered = ensureVulkanLayerRegistered();
  } else {
    removeVulkanLayerRegistration();
    m_vulkanLayerRegistered = false;
  }
  // A portable bundle's old mount path may have vanished. Refresh only a
  // previously opted-in GL enrollment; default-off installs remain untouched.
  if (!GlShimEnrollment::refreshEnrolledBundle())
    qCWarning(appLog) << "Could not refresh the opted-in GL shim enrollment";
  emit detectionStatusChanged();
  registerQmlTypes();

  m_engine = std::make_unique<QQmlApplicationEngine>();
  m_engine->addImageProvider("gameicon", new GameIconProvider);
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
  initialProperties.insert("updateChecker", QVariant::fromValue(m_updateChecker.get()));

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

  if (isWayland()) {
    // Capability discovery must not introspect a hung portal on the GUI
    // thread. The library remains usable when capture is unavailable.
    auto *portal = new QDBusServiceWatcher("org.freedesktop.portal.Desktop",
        QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(portal, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this] { queryCaptureAvailability(); });
    queryCaptureAvailability();
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
  m_activityFeed = std::make_unique<ActivityFeed>(this);
  m_waveformCache = std::make_unique<WaveformCache>(this);
  m_filmstripCache = std::make_unique<FilmstripCache>(this);
  m_audioMixer = std::make_unique<AudioMixer>(48000, 2, this);
  m_audioDeviceEnumerator = std::make_unique<AudioDeviceEnumerator>(this);
  m_gameDetector = std::make_unique<GameDetectionService>(this);
  m_windowService = std::make_unique<WindowService>(this);
  m_windowMonitor = std::make_unique<WaylandWindowMonitor>(this);
  m_windowService->setWindowMonitor(m_windowMonitor.get());
  m_gameDetector->setWindowMonitor(m_windowMonitor.get());
  // Required infrastructure: migrate the former opt-in without removing an
  // existing script, including when upgrading from an opted-out installation.
  m_settingsManager->setWindowTrackingEnabled(true);
  // All launches, including first run, check without installing/enabling.
  // Installation needs the wizard's Install action or an explicit Repair.
  m_windowMonitor->startMonitoring(false);
  connect(m_settingsManager.get(),
          &SettingsManager::windowTrackingEnabledChanged, this, [this]() {
            // Reset-to-defaults/legacy clients must not uninstall a required
            // component. Keep the persisted compatibility flag enabled.
            m_settingsManager->setWindowTrackingEnabled(true);
          });
  m_recordingManager = std::make_unique<RecordingManager>(this);
  
  m_recordingManager->setSettingsManager(m_settingsManager.get());
  // Hardware discovery can wait on a driver. The dashboard reads cached
  // text while this plain-data worker warms the shared encoder catalogue.
  auto *encoderProbe = new QFutureWatcher<QString>(m_recordingManager.get());
  QPointer<RecordingManager> encoderTarget(m_recordingManager.get());
  connect(encoderProbe, &QFutureWatcher<QString>::finished, m_recordingManager.get(),
          [encoderProbe, encoderTarget] {
    const QString name = encoderProbe->result();
    encoderProbe->deleteLater();
    if (encoderTarget) encoderTarget->setDetectedEncoderName(name);
  });
  encoderProbe->setFuture(QtConcurrent::run([] {
    const auto encoders = EncoderFactory::detectAvailableEncoders();
    return encoders.empty() ? QStringLiteral("Software") : encoders.front().description;
  }));
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

  m_audioWaitTimer = new QTimer(this);
  m_audioWaitTimer->setSingleShot(false);
  m_audioWaitTimer->setInterval(kAudioWaitMs);

  m_trayManager = std::make_unique<SystemTrayManager>(this);
  m_autostartManager = std::make_unique<AutostartManager>(this);
  m_replayManager = std::make_unique<ReplayManager>(this);
  m_replayManager->setSettingsManager(m_settingsManager.get());
  m_replayManager->setGameDetectionService(m_gameDetector.get());
  m_replayManager->setStorageManager(m_storageManager.get());
  m_recordingLibrary->setStorageManager(m_storageManager.get());
  m_shortcutManager = std::make_unique<GlobalShortcutManager>(this);
  m_shortcutManager->setEnabled(m_settingsManager->globalShortcutsEnabled());
  m_controlServer = std::make_unique<ControlServer>(this);
  connect(m_controlServer.get(), &ControlServer::toggleRecordingRequested,
          this, [this]() { requestToggleRecording(); });
  connect(m_controlServer.get(), &ControlServer::saveClipRequested, this,
          [this]() { requestReplay(); });
  connect(m_controlServer.get(), &ControlServer::showWindowRequested, this,
          [this]() { showMainWindow(); });

  // Update check: compares against the GitHub release feed.
  // applicationVersion() carries APP_VERSION (the CI tag on
  // release builds). Branch stamps never compare against a
  // release tag — parseVersion rejects them, so a dev build
  // stays silent instead of reading as "behind".
  m_updateChecker = std::make_unique<UpdateChecker>(this);
  m_updateChecker->setCurrentVersion(
      QCoreApplication::applicationVersion());
  connect(m_updateChecker.get(), &UpdateChecker::checkFinished, this,
          [this](bool newerAvailable, const QString &latestVersion,
                 const QString &releaseUrl) {
            m_latestReleaseUrl = releaseUrl;
            if (!newerAvailable ||
                !m_settingsManager->updateCheckEnabled())
              return;
            // Normal urgency + a single "View Release" button.
            // Deliberately not critical: an update never needs
            // to shout over a fullscreen game, and the Info
            // page carries the same status for anyone who
            // misses the transient.
            m_notificationService->showNotification(
                QStringLiteral("Trophy Update Available"),
                QStringLiteral("Trophy %1 is available — you're on %2.")
                    .arg(latestVersion,
                         QCoreApplication::applicationVersion()),
                QStringLiteral("trophy"), 8000, 1, false,
                QStringLiteral("open-update"),
                QStringLiteral("View Release"));
          });
  // Daily cadence while running: first check shortly after
  // startup (never racing launch), then every 24h. The app
  // can't check while closed, so "daily" means once per day
  // the app is actually up.
  m_updateCheckTimer = new QTimer(this);
  m_updateCheckTimer->setSingleShot(false);
  m_updateCheckTimer->setInterval(24 * 60 * 60 * 1000);
  connect(m_updateCheckTimer, &QTimer::timeout, this, [this]() {
    if (m_settingsManager->updateCheckEnabled())
      m_updateChecker->checkNow();
  });
  QTimer::singleShot(kUpdateCheckDelayMs, this, [this]() {
    if (m_settingsManager->updateCheckEnabled())
      m_updateChecker->checkNow();
    m_updateCheckTimer->start();
  });
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
  context->setContextProperty("activityFeed", m_activityFeed.get());
  context->setContextProperty("waveformCache", m_waveformCache.get());
  context->setContextProperty("filmstripCache", m_filmstripCache.get());
  context->setContextProperty("gameDetector", m_gameDetector.get());
  context->setContextProperty("notificationService", m_notificationService.get());
  context->setContextProperty("hardwareInfo", m_hardwareInfo.get());
  context->setContextProperty("audioMixer", m_audioMixer.get());
  context->setContextProperty("windowService", m_windowService.get());
  context->setContextProperty("windowMonitor", m_windowMonitor.get());
  context->setContextProperty("audioDeviceEnumerator", m_audioDeviceEnumerator.get());
  context->setContextProperty("globalShortcuts", m_shortcutManager.get());
  context->setContextProperty("replayManager", m_replayManager.get());
  context->setContextProperty("updateChecker", m_updateChecker.get());
}

void Application::connectSignals() {
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this, &Application::recordingStateChanged);
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this, &Application::recordingAvailabilityChanged);
  connect(m_recordingManager.get(), &RecordingManager::recordingStarted, this, [this](const QString &filePath) {
            m_recordingLibrary->setActiveRecordingPath(filePath);
            // The background replay loop pauses while recording.
            m_replayManager->setPaused(true);
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingFinished, this, [this]() {
            m_recordingLibrary->setActiveRecordingPath("");
            m_recordingLibrary->refresh();
            // Resume the replay loop if the game is still running.
            m_replayManager->setPaused(false);
            const QString game = m_gameDetector ? m_gameDetector->currentGameName() : QString();
            if (isCurrentCaptureGame(game) && !restoreTokenForGame(game).isEmpty()) {
              startWhenWindowReady(game, false);
            }
          });
  connect(m_gameDetector.get(), &GameDetectionService::gameDetected, this, [this](const QString &gameName, qint64 pid) {
            qCInfo(appLog) << "Game detected:" << gameName;
            emit currentGameChanged();
            if (m_settingsManager->autoRecordEnabled()) {
              QString token = restoreTokenForGame(gameName);
              if (!token.isEmpty()) {
                m_detectionDebounceTimer->stop();
                m_pendingAutoRecordGame.clear();
                authorizeGame(gameName);
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
              if (!restoreTokenForGame(gameName).isEmpty()) {
                m_pendingDvrGame.clear();
                authorizeGame(gameName);
                startWhenWindowReady(gameName, false);
              } else if (m_pendingDvrGame != gameName ||
                         !m_detectionDebounceTimer->isActive()) {
                m_pendingDvrGame = gameName;
                m_detectionDebounceTimer->start();
              }
            }
          });
  // Approval is derived from portal restore tokens: whenever the token
  // map changes, push the holding games to the detector so the Games
  // page's Ready section stays truthful.
  connect(m_settingsManager.get(), &SettingsManager::waylandRestoreTokensChanged,
          this, [this]() {
            if (m_gameDetector && m_settingsManager) {
              m_gameDetector->setTokenGames(
                  m_settingsManager->waylandRestoreTokens().keys());
            }
          });
  if (m_gameDetector && m_settingsManager) {
    m_gameDetector->setTokenGames(
        m_settingsManager->waylandRestoreTokens().keys());
  }
  connect(m_windowMonitor.get(), &WaylandWindowMonitor::windowsChanged, this, [this]() {
            if (!isCurrentCaptureGame(m_pendingWindowGame) ||
                !m_gameDetector->hasVisibleWindow())
              return;
            const QString game = m_pendingWindowGame;
            const bool isRecord = m_pendingWindowIsRecord;
            m_pendingWindowGame.clear();
            m_windowWaitTimer->stop();
            qCDebug(appLog) << "Window appeared for" << game
                            << "- waiting for settled game audio";
            startWhenAudioSettled(game, isRecord);
          });
  // A launcher can exit before the window/audio wait completes. Tracking
  // deliberately keeps the same identity (no gameDetected on PID handoff),
  // but its pending capture must be retried against the surviving process.
  connect(m_gameDetector.get(), &GameDetectionService::currentGameChanged,
          this, &Application::resumePendingCapture);
  connect(m_windowWaitTimer, &QTimer::timeout, this,
          [this]() { onWindowWaitTimeout(); });
  connect(m_detectionDebounceTimer, &QTimer::timeout, this,
          [this]() { onAuthorizeDebounceFired(); });
  connect(m_audioWaitTimer, &QTimer::timeout, this, [this]() {
    if (!isCurrentCaptureGame(m_pendingAudioGame)) {
      if (!m_pendingAudioGame.isEmpty() &&
          m_gameDetector->currentGameName() == m_pendingAudioGame &&
          !isGameBlocked(m_pendingAudioGame))
        return; // Let liveness promote the successor or close the game.
      m_pendingAudioGame.clear();
      ++m_audioWaitGeneration;
      m_audioWaitTimer->stop();
      return;
    }
    // A slow PipeWire query never stacks and never blocks the GUI: skip
    // ticks while a probe is in flight (skipped ticks don't count toward
    // the timeout — only resolved polls do).
    if (m_audioProbeInflight)
      return;
    const QString game = m_pendingAudioGame;
    const bool isRecord = m_pendingAudioIsRecord;
    qint64 pid = 0;
    if (m_gameDetector && m_gameDetector->currentGameName() == game)
      pid = m_gameDetector->currentGamePid();
    probeGameAudioAsync(game, pid, m_audioWaitGeneration,
                        [this, game, isRecord](bool present) {
                          // Superseded (game closed / re-armed): the
                          // generation check already passed, so also match
                          // the pending identity before touching counters.
                           if (m_pendingAudioGame != game || !isCurrentCaptureGame(game))
                            return;
                          ++m_audioWaitTicks;
                          if (present) {
                            if (++m_audioWaitHits >= 2) {
                              qCDebug(appLog)
                                  << "Game audio settled for" << game
                                  << "- starting";
                              m_pendingAudioGame.clear();
                              ++m_audioWaitGeneration;
                              m_audioWaitTimer->stop();
                              if (isRecord)
                                m_recordingManager->startRecording(game);
                              else
                                m_replayManager->startDvr(game);
                              return;
                            }
                          } else {
                            m_audioWaitHits = 0;
                          }
                          if (m_audioWaitTicks >= kAudioWaitMaxTicks) {
                            qCDebug(appLog)
                                << "Game audio never appeared for" << game
                                << "- starting anyway (monitor fallback)";
                            m_pendingAudioGame.clear();
                            ++m_audioWaitGeneration;
                            m_audioWaitTimer->stop();
                            if (isRecord)
                              m_recordingManager->startRecording(game);
                            else
                              m_replayManager->startDvr(game);
                          }
                        });
  });
  connect(m_recordingManager.get(), &RecordingManager::restoreAuthNeeded, this, [this](const QString &game, const QString &identity) {
            if (!isCurrentCaptureGame(game) || identity != m_gameDetector->currentGameKey())
              return;
            // Stale token: delete it so the next attempt can't reuse it,
            // then authorize again through the prompt. The picker that may
            // follow the click is the user's own action, never a surprise.
            qCDebug(appLog) << "Restore token stale for" << game
                            << "- re-authorizing via prompt";
            m_settingsManager->removeRestoreToken(game);
            m_settingsManager->removeRestoreToken(m_gameDetector->captureKeyForGame(game));
            m_pendingAutoRecordGame = game;
            m_detectionDebounceTimer->start();
          });
  connect(m_replayManager.get(), &ReplayManager::restoreAuthNeeded, this, [this](const QString &game, const QString &identity) {
            if (!isCurrentCaptureGame(game) || identity != m_gameDetector->currentGameKey())
              return;
            qCDebug(appLog) << "DVR restore token stale for" << game
                            << "- re-authorizing via prompt";
            m_settingsManager->removeRestoreToken(game);
            m_settingsManager->removeRestoreToken(m_gameDetector->captureKeyForGame(game));
            m_pendingDvrGame = game;
            m_detectionDebounceTimer->start();
          });
  // Unblocking the live game re-arms its prompt: the debounce dropped it
  // as blocked, and same-game re-detects never refire. Tokened games
  // start silently, so only the no-token prompt paths re-arm.
  connect(m_gameDetector.get(), &GameDetectionService::blacklistChanged, this, [this]() {
            if (!m_gameDetector || m_gameDetector->currentGameName().isEmpty())
              return;
            const QString game = m_gameDetector->currentGameName();
            if (isGameBlocked(game) || !m_pendingAutoRecordGame.isEmpty() ||
                !m_pendingDvrGame.isEmpty() ||
                !restoreTokenForGame(game).isEmpty())
              return;
            if (m_settingsManager->autoRecordEnabled())
              m_pendingAutoRecordGame = game;
            else if (m_settingsManager->instantReplayEnabled())
              m_pendingDvrGame = game;
            else
              return;
            m_detectionDebounceTimer->start();
          });
  connect(m_gameDetector.get(), &GameDetectionService::gameClosed, this, [this](const QString &gameName) {
            emit currentGameChanged();
            // A resident authorize prompt for the closing game must not
            // linger: it would show a dead name, and clicks on it hit
            // cleared pendings (vanish, nothing starts).
            if (m_pendingAutoRecordGame == gameName ||
                m_pendingDvrGame == gameName) {
              m_notificationService->closeAuthorizePrompt(gameName);
            }
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
            if (m_pendingAudioGame == gameName) {
              m_pendingAudioGame.clear();
              ++m_audioWaitGeneration;
              m_audioWaitTimer->stop();
            }
            if (m_pendingAutoRecordGame.isEmpty() && m_pendingDvrGame.isEmpty()) {
              m_detectionDebounceTimer->stop();
            }
            m_replayManager->stopDvr();
            if ((m_recordingManager->isRecording() ||
                 m_recordingManager->state() == RecordingManager::RecordingState::Initializing) &&
                m_recordingManager->currentGameName() == gameName) {
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
  // Live audio retargeting: device/app picks apply to whichever pipeline
  // is running (full recording wins over DVR). Without a session the
  // setters no-op and the next build picks the new values up.
  connect(m_settingsManager.get(), &SettingsManager::microphoneDeviceChanged,
          this, [this]() {
            if (m_recordingManager && m_recordingManager->isRecording())
              m_recordingManager->applyMicrophoneDevice();
            else if (m_replayManager && m_replayManager->isActive())
              m_replayManager->applyMicrophoneDevice();
          });
  connect(m_settingsManager.get(), &SettingsManager::gameAudioDeviceChanged,
          this, [this]() {
            if (m_recordingManager && m_recordingManager->isRecording())
              m_recordingManager->applyGameAudioDevice();
            else if (m_replayManager && m_replayManager->isActive())
              m_replayManager->applyGameAudioDevice();
          });
  connect(m_settingsManager.get(), &SettingsManager::voiceChatAppChanged, this,
          [this]() {
            if (m_recordingManager && m_recordingManager->isRecording())
              m_recordingManager->applyVoiceChatApp();
            else if (m_replayManager && m_replayManager->isActive())
              m_replayManager->applyVoiceChatApp();
          });
  // Layout changes (mode/tracks) can't hot-swap: the DVR rebuilds at the
  // next segment rotation, recordings at the next start. Say so when the
  // change lands mid-DVR, or the new layout seemingly "does nothing" for
  // up to a rotation. (During recording the UI locks these outright.)
  // The copy states the rotation bound concretely, from the constant.
  const QString segmentBound = QStringLiteral("Takes effect within %1 seconds.")
                                   .arg(ReplayManager::kSegmentSeconds);
  connect(m_settingsManager.get(), &SettingsManager::audioModeChanged, this,
          [this, segmentBound]() {
            if (m_replayManager && m_replayManager->isActive())
              // Critical like the other transient notices: the user is
              // fullscreen gaming, where normal urgency never surfaces.
              m_notificationService->showNotification(
                  "Audio Mode Changed", segmentBound, "trophy", 3000, 2);
          });
  connect(m_settingsManager.get(), &SettingsManager::separateAudioTracksChanged,
          this, [this, segmentBound]() {
            if (m_replayManager && m_replayManager->isActive())
              m_notificationService->showNotification(
                  "Track Layout Changed", segmentBound, "trophy", 3000, 2);
          });
  connect(m_storageManager.get(), &StorageManager::storageLimitReached, this,
          [this]() {
            if (m_suppressNextStorageNotification) {
              m_suppressNextStorageNotification = false;
              return;
            }
            if (!m_settingsManager->notifyStorage())
              return;
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
            // Same conventions as storageLimitReached above: settings-driven
            // changes that already gave their own feedback stay quiet, and
            // the notifyStorage toggle gates the popup. Explicit start
            // aborts still surface via RecordingManager::recordingError
            // (in-app dialog), so no press is ever fully silent.
            if (m_suppressNextStorageNotification) {
              m_suppressNextStorageNotification = false;
              return;
            }
            if (!m_settingsManager->notifyStorage())
              return;
            m_notificationService->showNotification(
                "Storage Full",
                "Recordings exceed the storage limit. Enable auto-cleanup or delete old recordings manually.",
                "dialog-warning", 0, 2);
          });
  connect(m_storageManager.get(), &StorageManager::recordingDeleted, m_recordingLibrary.get(), &RecordingLibrary::removeRecording);
  // Bulk-delete chain: the worker unlinks in the background, the batch
  // signal settles the model once, and finish unsuppresses the watcher
  // with one catch-up refresh. While suppressed, per-file watcher
  // refreshes on both sides are skipped (the storm that froze the UI
  // on multi-select deletes: N size rescans + N model rebuilds).
  connect(m_storageManager.get(), &StorageManager::recordingsDeleted, m_recordingLibrary.get(), &RecordingLibrary::removeRecordings);
  connect(m_storageManager.get(), &StorageManager::bulkDeleteStarted, m_recordingLibrary.get(), [this]() {
    m_recordingLibrary->setBulkOperationActive(true);
  });
  connect(m_storageManager.get(), &StorageManager::bulkDeleteFinished, m_recordingLibrary.get(), [this](int, int) {
    m_recordingLibrary->setBulkOperationActive(false);
    m_recordingLibrary->refresh();
  });
  // Order matters: the rename preference must be recorded before the
  // refresh recreates Recording objects (which consult it in addRecording).
  connect(m_storageManager.get(), &StorageManager::recordingRenamed, m_recordingLibrary.get(), &RecordingLibrary::onRecordingRenamed);
  connect(m_storageManager.get(), &StorageManager::recordingRenamed, m_recordingLibrary.get(), &RecordingLibrary::refresh);
  connect(m_notificationService.get(), &NotificationService::notificationClicked, this, [this](const QString &action) {
            // Update notice: open the releases page the
            // check reported. Handled before the authorize
            // dismissal below so it never clears pending
            // state (the two notices never coexist).
            if (action == QStringLiteral("open-update")) {
              if (!m_latestReleaseUrl.isEmpty())
                QDesktopServices::openUrl(QUrl(m_latestReleaseUrl));
              m_notificationService->closeLastNotification();
              return;
            }
            // Three-way authorize prompt: "authorize" starts capture and
            // allow-lists the game (portal token follows at start);
            // "block" vetoes it in the Games list with no capture;
            // anything else is a dismissal — drop pending, change nothing,
            // start nothing. The game stays on Prompt and re-prompts on
            // its next launch.
            if (action != QStringLiteral("authorize") && action != QStringLiteral("block")) {
              m_pendingAutoRecordGame.clear();
              m_pendingDvrGame.clear();
              m_detectionDebounceTimer->stop();
              return;
            }
            QString game;
            bool isRecord = true;
            if (!m_pendingAutoRecordGame.isEmpty()) {
              game = m_pendingAutoRecordGame;
              m_pendingAutoRecordGame.clear();
            } else if (!m_pendingDvrGame.isEmpty()) {
              game = m_pendingDvrGame;
              isRecord = false;
              m_pendingDvrGame.clear();
            }
            if (!isCurrentCaptureGame(game)) {
              m_notificationService->closeLastNotification();
              return;
            }
            if (action == QStringLiteral("block")) {
              m_notificationService->closeLastNotification();
              blockGame(game);
              return;
            }
            m_captureSelectionForRecording = isRecord;
            m_notificationService->showCaptureSelectionHint(game);
            if (isRecord)
              m_recordingManager->startRecording(game);
            else
              m_replayManager->startDvr(game);
            // Includes synchronous refusal/failure before a state transition.
            if (isRecord ? m_recordingManager->state() != RecordingManager::RecordingState::Initializing
                         : !m_replayManager->isStarting())
              m_notificationService->closeCaptureSelectionHint();
            // Prompt click authorizes capture: allow-list the game.
            authorizeGame(game);
          });
  connect(m_recordingManager.get(), &RecordingManager::captureSelectionFinished,
          this, [this] {
            if (m_captureSelectionForRecording)
              m_notificationService->closeCaptureSelectionHint();
          });
  connect(m_recordingManager.get(), &RecordingManager::stateChanged,
          this, [this](RecordingManager::RecordingState state) {
            if (m_captureSelectionForRecording &&
                state != RecordingManager::RecordingState::Initializing)
              m_notificationService->closeCaptureSelectionHint();
          });
  connect(m_replayManager.get(), &ReplayManager::captureSelectionFinished,
          this, [this] {
            if (!m_captureSelectionForRecording)
              m_notificationService->closeCaptureSelectionHint();
          });
  connect(m_replayManager.get(), &ReplayManager::replayStateChanged,
          this, [this] {
            if (!m_captureSelectionForRecording && !m_replayManager->isStarting())
              m_notificationService->closeCaptureSelectionHint();
          });
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingAdded, m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingRemoved, m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingsRemoved, m_storageManager.get(), &StorageManager::refresh);
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
            if (m_settingsManager->notifyRecordings())
              m_notificationService->showRecordingStarted(
                  m_recordingManager->currentGameName());
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingStopped, this,
          [this](const QString &, qint64 durationMs, qint64 sizeBytes) {
            playRecordingStoppedPing(false);
            if (!m_settingsManager->notifyRecordings())
              return;
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
  connect(m_replayManager.get(), &ReplayManager::replayStateChanged, this,
          [this]() {
            if (!m_replayManager->isActive())
              return;
            if (!m_settingsManager->notifyClips())
              return;
            const QString game = m_replayManager->currentGame();
            const int secs =
                std::max(1, m_settingsManager->clipBufferSeconds());
            // Queued: waits out "Recording Saved"'s full display window,
            // then appears stacked alongside (never replacing it).
            m_notificationService->showQueuedNotification(
                game.isEmpty() ? QStringLiteral("Ready to clip")
                               : QStringLiteral("Ready to clip %1").arg(game),
                QStringLiteral("Buffering the last %1 seconds.").arg(secs),
                "trophy", 3000, 2); // critical: visible over fullscreen game
          });
  connect(m_replayManager.get(), &ReplayManager::replaySaved, this,
          [this](const QString &filePath, double seconds) {
            playClipPing(false);
            if (m_settingsManager->notifyClips()) {
              const QString game = m_replayManager->currentGame();
              const qint64 secs = std::max<qint64>(1, llround(seconds));
              const QString span =
                  (secs == 1) ? QStringLiteral("Last 1 second")
                              : QStringLiteral("Last %1 seconds").arg(secs);
              const QString size =
                  StorageManager::formatSize(QFileInfo(filePath).size());
              m_notificationService->showNotification(
                  game.isEmpty() ? QStringLiteral("Clip saved")
                                 : QStringLiteral("%1 clip saved").arg(game),
                  QStringLiteral("%1 · %2").arg(span, size),
                  "trophy", 3000, 2); // critical: visible over screenshare
            }
            m_recordingLibrary->refresh();
          });
  connect(m_replayManager.get(), &ReplayManager::replayFailed, this,
          [this](const QString &reason) {
            m_notificationService->showNotification(
                "Couldn't Save Clip", reason, "dialog-warning", 3000, 2);
          });
  // Player-range clips take the same completion path as DVR clips: chime,
  // notice with the real kept span, library refresh (the feed picks the
  // new _Replay.mkv up like any other clip).
  connect(m_recordingLibrary.get(), &RecordingLibrary::clipSaved, this,
          [this](const QString &filePath, const QString &gameName,
                 double seconds) {
            playClipPing(false);
            if (m_settingsManager->notifyClips()) {
              const qint64 secs = std::max<qint64>(1, llround(seconds));
              const QString span =
                  (secs == 1) ? QStringLiteral("1 second")
                              : QStringLiteral("%1 seconds").arg(secs);
              const QString size =
                  StorageManager::formatSize(QFileInfo(filePath).size());
              m_notificationService->showNotification(
                  gameName.isEmpty() ? QStringLiteral("Clip saved")
                                     : QStringLiteral("%1 clip saved").arg(gameName),
                  QStringLiteral("%1 · %2").arg(span, size),
                  "trophy", 3000, 2); // critical: visible over screenshare
            }
            m_recordingLibrary->refresh();
          });
  connect(m_recordingLibrary.get(), &RecordingLibrary::clipSaveFailed, this,
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

bool Application::gameAudioNodePresent(const QString &game, qint64 pid) {
  // PW_ID_ANY without pulling pipewire headers into Application.
  constexpr uint32_t kNoNode = 0xFFFFFFFFu;
  if (pid > 0 &&
      PipeWireAudioSource::findNodeByPid(static_cast<uint32_t>(pid)) !=
          kNoNode) {
    return true;
  }
  if (!game.isEmpty() &&
      PipeWireAudioSource::findNodeByProcessName(game) != kNoNode) {
    return true;
  }
  return false;
}

void Application::startWhenAudioSettled(const QString &game, bool isRecord) {
  if (!isCurrentCaptureGame(game))
    return;
  qint64 pid = 0;
  if (m_gameDetector && m_gameDetector->currentGameName() == game)
    pid = m_gameDetector->currentGamePid();
  // The first presence check is async too (same multi-second PipeWire
  // cost as the polls): start now on delivery, else enter the wait loop.
  // Pending is armed up-front so a game-close in between supersedes us.
  m_pendingAudioGame = game;
  m_pendingAudioIsRecord = isRecord;
  m_audioWaitHits = 0;
  m_audioWaitTicks = 0;
  const quint64 generation = ++m_audioWaitGeneration;
  probeGameAudioAsync(game, pid, generation, [this, game, isRecord](bool present) {
    if (m_pendingAudioGame != game || !isCurrentCaptureGame(game))
      return; // superseded (game closed / re-armed)
    if (present) {
      qCDebug(appLog) << "Game audio present for" << game
                      << "- starting capture immediately";
      m_pendingAudioGame.clear();
      ++m_audioWaitGeneration;
      if (isRecord)
        m_recordingManager->startRecording(game);
      else
        m_replayManager->startDvr(game);
      return;
    }
    // Node not there yet (launching game): wait for it to appear and look
    // stable across two polls; the timeout starts anyway with today's
    // monitor-fallback behavior so a silent game never blocks capture.
    qCDebug(appLog) << "Deferring capture for" << game
                    << "until its audio node settles";
    m_audioWaitTimer->start();
  });
}

void Application::probeGameAudioAsync(
    const QString &game, qint64 pid, quint64 generation,
    const std::function<void(bool)> &onResult) {
  QPointer<Application> self(this);
  const quint64 ticket = claimAudioProbe();
  (void)QtConcurrent::run([self, game, pid, ticket, generation, onResult]() {
    // Static pure lookup: no Application state, worker-safe.
    const bool present = gameAudioNodePresent(game, pid);
    if (self.isNull())
      return;
    QMetaObject::invokeMethod(
        self,
        [self, ticket, generation, onResult, present]() {
          if (self.isNull())
            return;
          self->applyGameAudioProbeResult(ticket, generation, present, onResult);
        },
        Qt::QueuedConnection);
  });
}

quint64 Application::claimAudioProbe() {
  // Zero denotes no owner, including after a completion has been consumed.
  m_audioProbeTicket = ++m_nextAudioProbeTicket;
  if (m_audioProbeTicket == 0)
    m_audioProbeTicket = ++m_nextAudioProbeTicket;
  m_audioProbeInflight = true;
  return m_audioProbeTicket;
}

void Application::applyGameAudioProbeResult(
    quint64 ticket, quint64 generation, bool present,
    const std::function<void(bool)> &onResult) {
  // Generations control result validity, not ownership of the in-flight
  // claim: a stale sole owner must release it, but an older completion
  // must never release a replacement worker's claim.
  if (ticket == 0 || ticket != m_audioProbeTicket)
    return;
  m_audioProbeTicket = 0;
  m_audioProbeInflight = false;
  if (generation != m_audioWaitGeneration)
    return;
  onResult(present);
}

void Application::authorizeGame(const QString &game) {
  if (!m_gameDetector || game.isEmpty())
    return;
  // Prefer the detector's raw match key (reporter class / steam_app_<id>)
  // so future launches re-match; fall back to the display name (Steam
  // resolution is deterministic, so that re-matches too).
  QString key = game;
  if (m_gameDetector->currentGameName() == game &&
      !m_gameDetector->currentGameKey().isEmpty())
    key = m_gameDetector->currentGameKey();
  // The Vulkan null-name placeholder is not an identity: whitelisting it
  // would alias every unidentified reporter to this game (repaired at
  // load in GameWhitelist). Steam-identified games arrive with a
  // steam_app_<id> key, which whitelists normally.
  if (GameWhitelist::isPlaceholderReporterKey(key)) {
    qCInfo(appLog) << "Skipping whitelist for placeholder reporter key on"
                   << game << "- needs a Steam/exe identity";
    return;
  }
  // Claim a legacy display-name token once, under the stable identity.
  // Future renames keep approval; another Game.exe cannot inherit it.
  const QString token = m_settingsManager ? restoreTokenForGame(game) : QString();
  if (!token.isEmpty())
    m_settingsManager->setRestoreTokenForIdentity(key, game, token);
  if (m_gameDetector->addToWhitelist(key, game))
    qCInfo(appLog) << "Auto-whitelisted authorized game:" << game << "("
                   << key << ")";
}

void Application::onWindowWaitTimeout() {
  if (m_pendingWindowGame.isEmpty())
    return;
  // Slow approved launches remain pending. An unapproved game may only
  // continue through an explicit authorize action, never a bare picker.
  const QString game = m_pendingWindowGame;
  const bool isRecord = m_pendingWindowIsRecord;
  if (m_gameDetector && m_gameDetector->currentGameName() == game &&
      m_gameDetector->currentGameKey() == m_pendingCaptureKey &&
      !isGameBlocked(game) && !restoreTokenForGame(game).isEmpty()) {
    // Slow launches (notably VR) can take longer than 30 seconds. Keep
    // listening for the window; don't silently discard an approved start.
    qCInfo(appLog) << "Still waiting for window for approved game" << game
                   << "- capture remains pending";
    return;
  }
  m_pendingWindowGame.clear();
  // Blocked and approved games never prompt: the silent path already
  // authorized approved games, and no window appeared within the wait
  // (stray reporter, exited PID, slow compositor state). Arming a
  // "New Game" prompt here is the phantom-prompt bug — drop it.
  if (!shouldShowAuthorizePrompt(game)) {
    qCInfo(appLog) << "No window appeared for" << game
                   << "- dropping instead of prompting (blocked or approved)";
    return;
  }
  qCDebug(appLog) << "No window appeared for" << game
                  << "- falling back to authorize prompt";
  if (isRecord)
    m_pendingAutoRecordGame = game;
  else
    m_pendingDvrGame = game;
  if (m_detectionDebounceTimer)
    m_detectionDebounceTimer->start();
}

void Application::onAuthorizeDebounceFired() {
  const QString pending = !m_pendingAutoRecordGame.isEmpty()
                              ? m_pendingAutoRecordGame
                              : m_pendingDvrGame;
  if (pending.isEmpty())
    return;
  if (!shouldShowAuthorizePrompt(pending)) {
    qCInfo(appLog) << "Skipping authorize prompt for" << pending
                   << "(blocked or already approved)";
    if (m_pendingAutoRecordGame == pending)
      m_pendingAutoRecordGame.clear();
    if (m_pendingDvrGame == pending)
      m_pendingDvrGame.clear();
    return;
  }
  qCInfo(appLog) << "Showing authorize prompt for" << pending;
  m_notificationService->showNotification(
      "New Game: " + pending,
      "Click to authorize Wayland capture for this game, or Block it.",
      "trophy", 0, 2, true);
}

bool Application::shouldShowAuthorizePrompt(const QString &game) const {
  if (!isCurrentCaptureGame(game) || !m_settingsManager)
    return false;
  // A game blocked while detection was pending never prompts (unblocking
  // re-arms, so the row's next state always applies).
  if (isGameBlocked(game))
    return false;
  // Approval is token-derived: a game holding a restore token was already
  // authorized through the silent path. Prompting it again (a window-wait
  // fallback that raced a token save, or a re-detection while the silent
  // start is still settling) only produces phantom prompts for approved
  // games.
  return restoreTokenForGame(game).isEmpty();
}

QString Application::restoreTokenForGame(const QString &game) const {
  if (!m_settingsManager)
    return {};
  const QString key = m_gameDetector ? m_gameDetector->captureKeyForGame(game) : game;
  return m_settingsManager->getRestoreTokenForIdentity(key, game);
}

bool Application::isCurrentCaptureGame(const QString &game) const {
  return !game.isEmpty() && m_gameDetector &&
         m_gameDetector->currentGameName() == game &&
         GameDetectionService::processExists(m_gameDetector->currentGamePid()) &&
         !isGameBlocked(game);
}

void Application::blockGame(const QString &game) {
  if (!m_gameDetector || game.isEmpty())
    return;
  QString key = game;
  if (m_gameDetector->currentGameName() == game &&
      !m_gameDetector->currentGameKey().isEmpty())
    key = m_gameDetector->currentGameKey();
  if (m_gameDetector->moveToBlacklist(key))
    qCInfo(appLog) << "Blocked game from authorize prompt:" << game << "("
                   << key << ")";
}

bool Application::isGameBlocked(const QString &game) const {
  if (!m_gameDetector || game.isEmpty())
    return false;
  if (m_gameDetector->currentGameName() == game &&
      !m_gameDetector->currentGameKey().isEmpty() &&
      m_gameDetector->isBlocked(m_gameDetector->currentGameKey()))
    return true;
  return m_gameDetector->isBlocked(game);
}

void Application::startWhenWindowReady(const QString &game, bool isRecord) {
  if (game.isEmpty() || !m_gameDetector ||
      m_gameDetector->currentGameName() != game || isGameBlocked(game))
    return;
  m_pendingCaptureKey = m_gameDetector->currentGameKey();
  if (isCurrentCaptureGame(game) && m_gameDetector->hasVisibleWindow()) {
    qCDebug(appLog) << "Window present for" << game
                    << "- waiting for settled game audio";
    startWhenAudioSettled(game, isRecord);
    return;
  }
  // The portal can only restore against a concrete window; requesting now
  // would pop a picker for a game that is still launching. Wait for the
  // window; approved slow launches remain pending rather than prompting.
  qCDebug(appLog) << "Deferring capture for" << game
                  << "until its window appears";
  m_pendingWindowGame = game;
  m_pendingWindowIsRecord = isRecord;
  m_windowWaitTimer->start();
}

void Application::resumePendingCapture() {
  const QString game = m_gameDetector->currentGameName();
  if (!isCurrentCaptureGame(game) ||
      m_gameDetector->currentGameKey() != m_pendingCaptureKey)
    return;
  const bool waitingWindow = m_pendingWindowGame == game;
  const bool waitingAudio = m_pendingAudioGame == game;
  if (!waitingWindow && !waitingAudio)
    return;
  const bool isRecord = waitingWindow ? m_pendingWindowIsRecord : m_pendingAudioIsRecord;
  m_pendingWindowGame.clear();
  m_windowWaitTimer->stop();
  m_pendingAudioGame.clear();
  ++m_audioWaitGeneration;
  m_audioWaitTimer->stop();
  qCInfo(appLog) << "Resuming pending capture after process handoff for" << game
                 << "PID:" << m_gameDetector->currentGamePid();
  startWhenWindowReady(game, isRecord);
}
bool Application::isWayland() const { return CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::Wayland; }

bool Application::canStartRecording() const {
  return m_captureAvailable && m_recordingManager &&
         m_recordingManager->state() == RecordingManager::RecordingState::Idle;
}

QString Application::recordingPhase() const {
  if (!m_recordingManager)
    return QStringLiteral("idle");
  switch (m_recordingManager->state()) {
  case RecordingManager::RecordingState::Initializing: return QStringLiteral("initializing");
  case RecordingManager::RecordingState::Recording: return QStringLiteral("recording");
  case RecordingManager::RecordingState::Stopping: return QStringLiteral("stopping");
  case RecordingManager::RecordingState::Error: return QStringLiteral("error");
  default: return QStringLiteral("idle");
  }
}

void Application::queryCaptureAvailability() {
  const quint64 generation = ++m_captureAvailabilityGeneration;
  m_captureAvailable = false;
  m_captureAvailabilityPending = true;
  emit captureAvailabilityChanged();
  emit recordingAvailabilityChanged();
  CaptureFactory::requestPipeWireAvailability(this, [this, generation](bool available) {
    applyCaptureAvailability(generation, available);
  });
}

void Application::applyCaptureAvailability(quint64 generation, bool available) {
  if (generation != m_captureAvailabilityGeneration)
    return;
  m_captureAvailable = available;
  m_captureAvailabilityPending = false;
  emit captureAvailabilityChanged();
  emit recordingAvailabilityChanged();
  if (!available)
    qCWarning(appLog) << "ScreenCast portal unavailable; capture requests will report errors";
}

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

void Application::playClipPing(bool preview) {
  playChime(QStringLiteral(":/sounds/clip-saved.wav"),
            QStringLiteral("clip-saved.wav"), preview);
}

void Application::playRecordingStoppedPing(bool preview) {
  playChime(QStringLiteral(":/sounds/recording-stopped.wav"),
            QStringLiteral("recording-stopped.wav"), preview);
}

void Application::playChime(const QString &resource, const QString &fileName,
                            bool preview) {
  if (!preview && (!m_settingsManager || !m_settingsManager->completionSoundEnabled()))
    return;
  // pw-play needs a real file: extract the bundled chime to cache once.
  const QString dest =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
      QStringLiteral("/trophy/sounds/") + fileName;
  if (!QFile::exists(dest)) {
    QDir().mkpath(QFileInfo(dest).path());
    if (!QFile::copy(resource, dest)) {
      qCWarning(appLog) << "Could not extract chime" << resource << "to"
                        << dest;
      return;
    }
  }
  // Fire-and-forget: a missing/slow player must never hang the UI thread.
  if (!QProcess::startDetached(QStringLiteral("pw-play"),
                               QStringList(dest))) {
    qCWarning(appLog) << "Could not start pw-play for chime" << fileName;
  }
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
        "Trophy",
        "Launch a game first — there's nothing buffered yet.",
        "trophy", 5000, 1);
    return;
  }
  // Storage pre-flight (mirrors the in-manager gate in saveReplay):
  // refuse early with a "Storage Full" notice instead of remuxing a
  // clip that would push usage over the limit.
  m_storageManager->refresh();
  if (m_storageManager->isOverLimit()) {
    m_notificationService->showNotification(
        "Storage Full",
        "Storage is full — free space or enable auto-cleanup to save "
        "clips.",
        "dialog-warning", 3000, 2);
    return;
  }
  const int secs = std::max(1, m_settingsManager->clipBufferSeconds());
  m_notificationService->showNotification(
      "Saving Clip", QString("Saving the last %1 seconds…").arg(secs),
      "trophy", 3000, 2);
  m_replayManager->saveReplay();
}

void Application::setTrackMuted(int trackIndex, bool muted) {
  // Mute is a user preference, not pipeline state: set it on both mixers
  // so it holds whichever pipeline runs next. Rare UI action — logging it
  // traces dashboard mute clicks end to end.
  qCInfo(appLog) << "Track" << trackIndex << (muted ? "muted" : "unmuted");
  if (m_audioMixer)
    m_audioMixer->setSemanticTrackMuted(trackIndex, muted);
  if (m_replayManager)
    m_replayManager->setSemanticTrackMuted(trackIndex, muted);
}

bool Application::isTrackMuted(int trackIndex) const {
  if (m_recordingManager && m_recordingManager->isRecording() && m_audioMixer)
    return m_audioMixer->isSemanticTrackMuted(trackIndex);
  if (m_replayManager && m_replayManager->isActive())
    return m_replayManager->isSemanticTrackMuted(trackIndex);
  if (m_audioMixer)
    return m_audioMixer->isSemanticTrackMuted(trackIndex);
  return false;
}

float Application::trackLevel(int trackIndex) const {
  if (m_recordingManager && m_recordingManager->isRecording() && m_audioMixer)
    return m_audioMixer->semanticTrackLevel(trackIndex);
  if (m_replayManager && m_replayManager->isActive())
    return m_replayManager->semanticTrackLevel(trackIndex);
  return 0.0f;
}

void Application::requestToggleRecording() {  if (!m_recordingManager) {
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

void Application::reportBug() {
  const QString tail = readLogTail(bugReportLogPath());
  QDesktopServices::openUrl(bugReportUrl(applicationVersion(), tail));
}

void Application::syncAutostartFile() {
  if (!m_autostartManager || !m_settingsManager) {
    return;
  }
  const bool want = m_settingsManager->startOnLogin();
  if (!want && !m_autostartManager->isEnabled()) {
    return;
  }
  QString flatpakId = QString::fromUtf8(qgetenv("FLATPAK_ID"));
  if (flatpakId.isEmpty() && QFile::exists(QStringLiteral("/.flatpak-info"))) {
    flatpakId = QStringLiteral("io.github.stevenjwheeler.Trophy");
  }
  // Refresh managed entries too: older packaged versions stored a transient
  // AppImage mount path or the sandbox-only /app/bin executable.
  if (!m_autostartManager->setEnabled(
          want, QCoreApplication::applicationFilePath(),
          QString::fromUtf8(qgetenv("APPIMAGE")), flatpakId)) {
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
