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

#include <QMessageBox>

#include "AppRestart.h"
#include "Application.h"
#include "AutostartManager.h"
#include "BugReport.h"
#include "ControlServer.h"
#include "CrashHandler.h"
#include "DeletionQueue.h"
#include "DesktopIntegration.h"
#include "DiscordPresence.h"
#include "GlobalShortcutManager.h"
#include "HardwareInfo.h"
#include "troubleshooters/DmaBufTroubleshooter.h"
#include "InstallInfo.h"
#include "NotificationService.h"
#include "RecordingManager.h"
#include "ReplayManager.h"
#include "RuntimeLibraries.h"
#include "StorageManager.h"
#include "SystemTrayManager.h"
#include "UpdateChecker.h"
#include "UpdateInstaller.h"
#include "audio/AudioDeviceEnumerator.h"
#include "audio/AudioApplicationRegistry.h"
#include "audio/AudioMixer.h"
#include "audio/PipeWireAudioSource.h"
#include "capture/CaptureFactory.h"
#include "detection/GameDetectionService.h"
#include "detection/GameIconProvider.h"
#include "detection/GameWhitelist.h"
#include "detection/LegacyDetectionCleanup.h"
#include "encoding/EncoderFactory.h"
#include "library/ActivityFeed.h"
#include "library/FilmstripCache.h"
#include "library/PreviewCache.h"
#include "library/RecordingLibrary.h"
#include "library/ShareExporter.h"
#include "library/WaveformCache.h"
#include "settings/SettingsManager.h"

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
#include <cstdlib>

Q_LOGGING_CATEGORY(appLog, "trophy.app")

namespace TrophyBackend {

// NVIDIA's driver downclocks memory transfer rate while a process uses CUDA
// (which NVENC does), hurting game framerate. Installing an application profile
// with setting 0x166c5e=0 disables that behaviour for this binary. The profile
// is user-local and harmless on non-NVIDIA systems.
static void installNvidiaNoStablePerfLimitProfile() {
  if (access("/proc/driver/nvidia/version", F_OK) != 0)
    return;

  const char *home = std::getenv("HOME");
  if (!home)
    return;

  QDir dir(QString::fromUtf8(home) + "/.nv/nvidia-application-profiles-rc.d");
  if (!dir.exists() && !dir.mkpath("."))
    return;

  QFile file(dir.filePath("10-trophy-cuda-no-stable-perf-limit"));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
    return;

  static const char *profile =
      "{\n"
      "    \"profiles\": [\n"
      "        {\n"
      "            \"name\": \"TrophyCudaNoStablePerfLimit\",\n"
      "            \"settings\": [\"0x166c5e\", 0]\n"
      "        }\n"
      "    ],\n"
      "    \"rules\": [\n"
      "        { \"pattern\": \"trophy\", \"profile\": \"TrophyCudaNoStablePerfLimit\" }\n"
      "    ]\n"
      "}\n";
  file.write(profile);
  file.close();
}

Application::Application(int &argc, char **argv)
    : QApplication(argc, argv),
      m_startFullRecording([this](const QString &game) {
        m_recordingManager->startRecording(game);
      }) {}

Application::~Application() {
  m_discordPresence.reset();
  // Retiring pipelines retain raw mixer/settings/storage pointers. Settle
  // managers before those dependencies (member order would destroy them first).
  if (m_gameDetector) disconnect(m_gameDetector.get(), nullptr, this, nullptr);
  if (m_recordingManager) disconnect(m_recordingManager.get(), nullptr, this, nullptr);
  if (m_replayManager) disconnect(m_replayManager.get(), nullptr, this, nullptr);
  m_engine.reset();
  m_dmaBufTroubleshooter.reset();
  m_shareExporter.reset();
  m_deletionQueue.reset();
  m_controlServer.reset();
  m_recordingManager.reset();
  m_replayManager.reset();
  m_recordingEngine.reset();
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

  // A crash writes nothing through the message handler, so arm a fatal-signal
  // handler that appends its own report to the same log. Core dumps still work.
  installCrashHandler(logPath);

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

  // Trophy has no native X11 path: the portal ScreenCast interface is a
  // Wayland-session feature and CaptureFactory has no X11 source. Refuse before
  // any service, tray icon or window exists, and say why -- launched from a
  // desktop icon, the alternative is an app that silently does nothing. The
  // dialog is skipped on headless platforms (tests, packaging smoke), where
  // nobody could see it.
  if (CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::X11) {
    const QString message = tr("Trophy does not support native X11 sessions. "
                               "Please switch to a Wayland session.");
    qCCritical(appLog) << message;
    emit fatalError(message);
    const QString platform = QGuiApplication::platformName();
    if (platform != QLatin1String("offscreen") && platform != QLatin1String("minimal")) {
      QMessageBox box(QMessageBox::Critical, tr("Trophy requires Wayland"),
                      tr("Trophy does not support native X11 sessions.\n\n"
                         "Log in to a Wayland desktop session (KDE Plasma and GNOME are "
                         "verified) and start Trophy again."),
                      QMessageBox::Ok);
      box.setWindowIcon(QApplication::windowIcon());
      box.exec();
    }
    return false;
  }

  // Best-effort: prevent NVIDIA driver from downclocking while NVENC is active.
  installNvidiaNoStablePerfLimitProfile();

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
  // One-way migration away from injected graphics detection.
  removeLegacyDetectionEnrollment();
  registerQmlTypes();

  m_engine = std::make_unique<QQmlApplicationEngine>();
  m_engine->addImageProvider("gameicon", new GameIconProvider);
  m_engine->addImageProvider("hoverpreview", m_previewCache->createImageProvider());
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
  } else {
    qCInfo(appLog) << "No system tray host detected";
  }

  // Start minimized does not depend on a tray icon: GNOME has no legacy tray,
  // but hiding still works because launching Trophy again restores the window.
  applyStartupVisibility();

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
  // Which half of the runtime came from where: the AppImage bundles Qt/FFmpeg
  // but prefers the host's libva and driver stack, and this is the first thing
  // to check when a machine reports unexpected encoder or ABI behaviour.
  if (appLog().isDebugEnabled()) {
    for (const QString &line :
         runtimeLibraryReport().split(QLatin1Char('\n'), Qt::SkipEmptyParts))
      qCDebug(appLog).noquote() << line;
  }
  if (m_desktopIntegration) {
    // A startup install happens before any event loop, so the launcher and
    // icon caches are refreshed here instead.
    if (DesktopIntegration::consumeStartupInstall())
        m_desktopIntegration->refreshCachesAfterStartup();
    // The entry is hidden while another AppImage manager lists Trophy. If that
    // entry has since been removed, restore ours so there is a launcher.
    m_desktopIntegration->refreshVisibility();
    const QString registered = m_desktopIntegration->registeredDesktopFile();
    qCInfo(appLog) << "Desktop registration:" << (m_desktopIntegration->isAppImage() ? "appimage" : "installed")
                   << "entry:" << (registered.isEmpty() ? QStringLiteral("none (portal treats Trophy as a generic app; capture approvals are not per-app)")
                                                       : registered)
                   << "native dialogs:" << useNativeDialogs()
                   << "platform theme:" << (qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME")
                                                ? qEnvironmentVariable("QT_QPA_PLATFORMTHEME")
                                                : QStringLiteral("(default)"));
  }

  const QString toVersion = QCoreApplication::applicationVersion();
  if (UpdateInstaller::shouldAnnounceUpdate(m_postUpdateFromVersion, toVersion)) {
    if (m_notificationService) {
      m_notificationService->showQueuedNotification(
          QStringLiteral("Trophy updated"),
          UpdateInstaller::updateNoticeBody(m_postUpdateFromVersion, toVersion),
          QString(), 8000, 1);
    }
    qCInfo(appLog) << "Updated from" << m_postUpdateFromVersion << "to"
                   << toVersion << "- post-update notice shown";
  } else if (!m_postUpdateFromVersion.isEmpty()) {
    qCInfo(appLog) << "Relaunched by the updater for the same version"
                   << toVersion << "- no notice";
  }
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
  m_previewCache = std::make_unique<PreviewCache>(this);
  m_audioMixer = std::make_unique<AudioMixer>(48000, 2, this);
  m_audioApplicationRegistry = std::make_unique<AudioApplicationRegistry>(this);
  m_audioDeviceEnumerator = std::make_unique<AudioDeviceEnumerator>(this);
  m_gameDetector = std::make_unique<GameDetectionService>(this);
  m_audioDeviceEnumerator->setApplicationRegistry(m_audioApplicationRegistry.get());
  m_gameDetector->setAudioApplicationRegistry(m_audioApplicationRegistry.get());
  m_audioApplicationRegistry->start();
  m_recordingEngine = std::make_shared<RecordingEngine>();
  m_dmaBufTroubleshooter = std::make_unique<DmaBufTroubleshooter>(this);
  connect(m_recordingEngine.get(), &RecordingEngine::stateChanged, this, [this] {
    if (m_dmaBufTroubleshooter)
      m_dmaBufTroubleshooter->setAvailable(
          m_recordingEngine->state() == RecordingEngine::State::Idle);
  });
  m_recordingEngine->setSettingsManager(m_settingsManager.get());
  m_recordingEngine->setAudioMixer(m_audioMixer.get());
  m_recordingEngine->setGameDetectionService(m_gameDetector.get());
  m_recordingManager = std::make_unique<RecordingManager>(m_recordingEngine, this);
  
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
  m_replayManager = std::make_unique<ReplayManager>(m_recordingEngine, this);
  connect(m_replayManager.get(), &ReplayManager::captureAuthorizationChanged,
          m_recordingManager.get(), &RecordingManager::captureRecoveryChanged);
  m_replayManager->setSettingsManager(m_settingsManager.get());
  m_replayManager->setStorageManager(m_storageManager.get());
  m_recordingLibrary->setStorageManager(m_storageManager.get());
  m_shareExporter = std::make_unique<ShareExporter>(m_storageManager.get(), this);
  m_deletionQueue = std::make_unique<DeletionQueue>(m_storageManager.get(), this);
  connect(m_deletionQueue.get(), &DeletionQueue::hiddenPathsChanged,
          m_recordingLibrary.get(), &RecordingLibrary::setHiddenRecordings);
  m_shortcutManager = std::make_unique<GlobalShortcutManager>(this);
  m_shortcutManager->setEnabled(m_settingsManager->globalShortcutsEnabled());
  m_controlServer = std::make_unique<ControlServer>(this);
  m_discordPresence = std::make_unique<DiscordPresence>(this);
  connect(m_controlServer.get(), &ControlServer::toggleRecordingRequested,
          this, [this]() { requestToggleRecording(); });
  connect(m_controlServer.get(), &ControlServer::saveClipRequested, this,
          [this]() { requestReplay(); });
  connect(m_controlServer.get(), &ControlServer::bookmarkRequested, this,
          &Application::requestBookmark);
  connect(m_controlServer.get(), &ControlServer::showWindowRequested, this,
          [this]() { showMainWindow(); });

  // Update check: compares against the GitHub release feed.
  // applicationVersion() carries APP_VERSION (the CI tag on
  // release builds). Branch stamps never compare against a
  // release tag — parseVersion rejects them, so a dev build
  // stays silent instead of reading as "behind".
  m_updateChecker = std::make_unique<UpdateChecker>(this);
  m_updateChecker->setInstallKind(InstallInfo::detect());
  const QByteArray forceFake = qgetenv("TROPHY_FORCE_FAKE_UPDATE");
  if (!forceFake.isEmpty() && forceFake != "0") {
    m_updateChecker->setForceFakeUpdate(true);
    const QByteArray v = qgetenv("TROPHY_FAKE_UPDATE_VERSION");
    if (!v.isEmpty())
      m_updateChecker->setFakeUpdateVersion(QString::fromUtf8(v));
  }
  m_updateChecker->setCurrentVersion(
      QCoreApplication::applicationVersion());

  m_updateInstaller = std::make_unique<UpdateInstaller>(this);
  m_desktopIntegration = std::make_unique<DesktopIntegration>(this);
  if (m_desktopIntegration) {
    connect(m_desktopIntegration.get(), &DesktopIntegration::integrationChanged,
            this, &Application::useNativeDialogsChanged);
  }
  const QByteArray forceFail = qgetenv("TROPHY_FORCE_FAIL_UPDATE");
  if (!forceFail.isEmpty() && forceFail != "0")
    m_updateInstaller->setForceFail(true);
  connect(m_updateInstaller.get(), &UpdateInstaller::finished, this,
          [this](bool ok, const QString &message) {
            if (ok)
              return;
            qWarning("UpdateInstaller finished with failure: %s",
                     message.toUtf8().constData());
            if (m_notificationService)
              m_notificationService->showNotification(
                  QStringLiteral("Trophy"),
                  QStringLiteral("Auto-update failed, we have opened the "
                                 "releases page for you to update manually."),
                  QStringLiteral("trophy"), 8000, 1);
            // Auto-fallback: open the release/asset page so the user
            // can download and install manually.
            const QString url = m_updateChecker && !m_updateChecker->latestAssetUrl().isEmpty()
                                    ? m_updateChecker->latestAssetUrl()
                                    : (m_latestReleaseUrl.isEmpty()
                                           ? QString()
                                           : m_latestReleaseUrl);
            if (!url.isEmpty()) {
              QDesktopServices::openUrl(QUrl(url));
            }
          });
  if (m_recordingManager) {
    m_updateInstaller->setRecording(m_recordingManager->isRecording());
    connect(m_recordingManager.get(), &RecordingManager::stateChanged, this,
            [this](RecordingManager::RecordingState) {
              if (m_updateInstaller && m_recordingManager)
                m_updateInstaller->setRecording(
                    m_recordingManager->isRecording());
            });
  }
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
  context->setContextProperty("previewCache", m_previewCache.get());
  context->setContextProperty("shareExporter", m_shareExporter.get());
  context->setContextProperty("deletionQueue", m_deletionQueue.get());
  context->setContextProperty("gameDetector", m_gameDetector.get());
  context->setContextProperty("notificationService", m_notificationService.get());
  context->setContextProperty("hardwareInfo", m_hardwareInfo.get());
  context->setContextProperty("dmaBufTroubleshooter", m_dmaBufTroubleshooter.get());
  context->setContextProperty("audioMixer", m_audioMixer.get());
  context->setContextProperty("audioDeviceEnumerator", m_audioDeviceEnumerator.get());
  context->setContextProperty("globalShortcuts", m_shortcutManager.get());
  context->setContextProperty("replayManager", m_replayManager.get());
  context->setContextProperty("updateChecker", m_updateChecker.get());
  context->setContextProperty("updateInstaller", m_updateInstaller.get());
  context->setContextProperty("desktopIntegration", m_desktopIntegration.get());
  context->setContextProperty("installKind",
                             static_cast<int>(InstallInfo::detect()));
}

void Application::preparePlayerCaches(const QString &filePath) {
  if (filePath.isEmpty() || !QFileInfo(filePath).isFile())
    return;
  // A single peaks request decodes/caches every audio track in one pass.
  // Opening the player during preparation joins these same pending jobs.
  if (m_waveformCache)
    m_waveformCache->requestPeaks(filePath, 0);
  if (m_filmstripCache)
    m_filmstripCache->requestThumbs(filePath, 32); // PlayerPage.stripCount
  if (m_previewCache)
    m_previewCache->preparePreview(filePath);
}

void Application::updateDiscordPresence() {
  if (!m_discordPresence || !m_settingsManager || !m_recordingManager || !m_replayManager)
    return;
  // Privacy preferences gate lookups before adopting the current capture.
  // IPC updates are queued, so both snapshots settle before publishing.
  m_discordPresence->setPreferences(m_settingsManager->discordPresenceEnabled(),
      m_settingsManager->discordShowGameNames(), m_settingsManager->discordShowWhenIdle());
  m_discordPresence->setCaptureState(m_recordingManager->isRecording(),
      m_recordingManager->currentGameName(), m_replayManager->isActive(),
      m_replayManager->currentGame(), m_recordingManager->capturedSteamAppId(),
      m_replayManager->capturedSteamAppId());
}

void Application::connectSignals() {
  connect(m_recordingManager.get(), &RecordingManager::stateChanged,
          this, &Application::updateDiscordPresence);
  connect(m_recordingManager.get(), &RecordingManager::currentGameNameChanged,
          this, &Application::updateDiscordPresence);
  connect(m_replayManager.get(), &ReplayManager::replayStateChanged,
          this, &Application::updateDiscordPresence);
  connect(m_settingsManager.get(), &SettingsManager::discordPresenceEnabledChanged,
          this, &Application::updateDiscordPresence);
  connect(m_settingsManager.get(), &SettingsManager::discordShowGameNamesChanged,
          this, &Application::updateDiscordPresence);
  connect(m_settingsManager.get(), &SettingsManager::discordShowWhenIdleChanged,
          this, &Application::updateDiscordPresence);
  updateDiscordPresence();
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this, &Application::recordingStateChanged);
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this, &Application::recordingAvailabilityChanged);
  connect(m_recordingManager.get(), &RecordingManager::captureRecoveryChanged,
          this, &Application::onCaptureRecoveryChanged);
  connect(m_recordingManager.get(), &RecordingManager::recordingPathPrepared,
          m_recordingLibrary.get(), &RecordingLibrary::setActiveRecordingPath);
  connect(m_recordingManager.get(), &RecordingManager::recordingStarted, this, [this](const QString &) {
            // The background replay loop pauses while recording.
            m_replayManager->setPaused(true);
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingFinished, this, [this]() {
            m_recordingLibrary->setActiveRecordingPath("");
             m_recordingLibrary->refresh();
             resumeReplayAfterRecording();
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingStopping,
          this, &Application::resumeReplayAfterRecording);
  connect(m_gameDetector.get(), &GameDetectionService::gameDetected, this,
          &Application::onGameDetected);
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
  // A launcher can exit before the window/audio wait completes. Tracking
  // deliberately keeps the same identity (no gameDetected on PID handoff),
  // but its pending capture must be retried against the surviving process.
  connect(m_gameDetector.get(), &GameDetectionService::currentGameChanged,
          this, &Application::resumePendingCapture);
  connect(m_windowWaitTimer, &QTimer::timeout, this,
          [this]() { onWindowWaitTimeout(); });
  connect(m_detectionDebounceTimer, &QTimer::timeout, this,
          [this]() { onAuthorizeDebounceFired(); });
  connect(m_audioWaitTimer, &QTimer::timeout,
          this, &Application::onAudioWaitTimeout);
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
            m_authorizePromptTokenInvalid = true;
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
            m_authorizePromptTokenInvalid = true;
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
  connect(m_gameDetector.get(), &GameDetectionService::gameClosed,
          this, &Application::clearPendingCaptureForGame);
  connect(m_gameDetector.get(), &GameDetectionService::gameClosed, this, [this](const QString &gameName) {
            qCInfo(appLog) << "Game ended:" << gameName;
            m_authorizePromptTokenInvalid = false;
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
             if (m_recordingEngine) m_recordingEngine->applyMicrophoneDevice();
          });
  connect(m_settingsManager.get(), &SettingsManager::gameAudioDeviceChanged,
          this, [this]() {
             if (m_recordingEngine) m_recordingEngine->applyGameAudioDevice();
          });
  connect(m_settingsManager.get(), &SettingsManager::voiceChatAppChanged, this,
          [this]() {
             if (m_recordingEngine) m_recordingEngine->applyVoiceChatApp();
          });
  // Layout changes (mode/tracks) can't hot-swap: the DVR rebuilds at the
  // next segment rotation, recordings at the next start. Say so when the
  // change lands mid-DVR, or the new layout seemingly "does nothing" for
  // up to a rotation. (During recording the UI locks these outright.)
  // The copy states the rotation bound concretely, from the constant.
  const QString segmentBound = QStringLiteral("Takes effect within %1 seconds.")
                                    .arg(RecordingEngine::kReconfigurationSeconds);
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
                m_settingsManager->autoDeleteOldest()
                    ? "No eligible recordings remain for cleanup. Protected captures and the current recording are kept. Unprotect or delete captures, or increase the storage limit."
                    : "Recordings exceed the storage limit. Enable auto-cleanup, delete captures or increase the limit. Protected captures are never auto-deleted.",
                "dialog-warning", 0, 2);
          });
  connect(m_storageManager.get(), &StorageManager::recordingDeleted, m_recordingLibrary.get(), &RecordingLibrary::removeRecording);
  connect(m_shareExporter.get(), &ShareExporter::finished, this, [this](const QString &path) {
    m_notificationService->showQueuedNotification(
        "Export saved", QFileInfo(path).fileName(), "video-x-generic", 5000, 1);
  });
  // Feed each finished export back into the measured cost model, so the
  // dialog's estimate matches this machine instead of a guessed constant.
  connect(m_shareExporter.get(), &ShareExporter::exportSample, this,
          [this](double exportSeconds, double videoSeconds, int mode, int tracks,
                 bool av1Source) {
            m_settingsManager->recordExportSample(exportSeconds, videoSeconds, mode,
                                                  tracks, av1Source);
          });
  connect(m_shareExporter.get(), &ShareExporter::failed, this, [this](const QString &reason) {
    m_notificationService->showQueuedNotification("Export failed", reason, "dialog-error", 5000, 2);
  });
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
              openUpdateDialog();
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
              m_authorizePromptTokenInvalid = false;
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
            // Don't start capturing a transient window (mod loader / splash).
            // Defer until a real game window appears.
            if (m_gameDetector && !m_gameDetector->isGamePayloadReady()) {
              qCInfo(appLog) << "Deferring capture for" << game
                             << "(no visible game window yet)";
              m_pendingWindowGame = game;
              m_pendingWindowIsRecord = isRecord;
              m_windowWaitTimer->start();
              return;
            }
            if (isRecord) {
              if (m_recordingManager->requiresCaptureSelection())
                m_recordingManager->resumeCapture();
              else
                m_recordingManager->startRecording(game);
            } else if (!m_recordingManager->requiresCaptureSelection())
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
          [this](const QString &filePath, qint64, qint64, bool) {
            m_recordingLibrary->refreshAnnotations(filePath);
            preparePlayerCaches(filePath);
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingCompletionReady, this,
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
            if (m_recordingManager->requiresCaptureSelection())
              m_recordingManager->resumeCapture();
            else
              m_recordingManager->startRecording(game);
          });
  connect(m_trayManager.get(), &SystemTrayManager::stopRecordingRequested,
          this, [this]() {
            cancelPendingCapture();
            if (m_recordingManager->isRecording()) {
              m_recordingManager->stopRecording();
            }
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
            preparePlayerCaches(filePath);
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
            m_recordingLibrary->refreshAnnotations(filePath);
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
            preparePlayerCaches(filePath);
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
          this, &Application::onInstantReplayEnabledChanged);
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

void Application::onGameDetected(QString gameName) {
  // Authorization may synchronously rebuild the detector's instances. Own
  // the name throughout this callback rather than borrowing a signal argument.
  const auto source = m_gameDetector->currentGameSource();
  qCInfo(appLog) << "Game detected:" << gameName << "from"
                << (source.isEmpty() ? QStringLiteral("unknown") : source);
  emit currentGameChanged();
  if (m_settingsManager->autoRecordEnabled()) {
    const auto token = restoreTokenForGame(gameName);
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
}

bool Application::isRecording() const { return m_recordingManager && m_recordingManager->isRecording(); }
QString Application::currentGameName() const { return m_gameDetector ? m_gameDetector->currentGameName() : QString(); }

bool Application::gameAudioNodePresent(const QString &game, qint64 pid) {
  // PW_ID_ANY without pulling pipewire headers into Application.
  constexpr uint32_t kNoNode = 0xFFFFFFFFu;
  Q_UNUSED(game);
  return pid > 0 && PipeWireAudioSource::findNodeForGame(
                        static_cast<uint32_t>(pid), {}) != kNoNode;
}

void Application::startWhenAudioSettled(const QString &game, bool isRecord) {
  if (stopPendingCaptureForSelection())
    return;
  if (!isCurrentCaptureGame(game))
    return;
  if (!m_gameDetector->isGamePayloadReady()) {
    startWhenWindowReady(game, isRecord);
    return;
  }
  qint64 pid = 0;
  if (m_gameDetector && m_gameDetector->currentGameName() == game)
    pid = m_gameDetector->currentGamePid();
  // The first presence check is async too (same multi-second PipeWire
  // cost as the polls): start now on delivery, else enter the wait loop.
  // Pending is armed up-front so a game-close in between supersedes us.
  m_pendingAudioGame = game;
  m_pendingAudioInstance = m_gameDetector->captureIdentityInfo(game)
                               .value("detectedGameInstance").toString();
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
        m_startFullRecording(game);
      else
        m_replayManager->startDvr(game);
      emit recordingStateChanged();
      emit recordingAvailabilityChanged();
      return;
    }
    // Node not there yet (launching game): wait for it to appear and look
    // stable across two polls; the timeout starts anyway with today's
    // silent-wait behavior so a game without audio never blocks video capture.
    qCDebug(appLog) << "Deferring capture for" << game
                    << "until its audio node settles";
    m_audioWaitTimer->start();
  });
  emit recordingStateChanged();
  emit recordingAvailabilityChanged();
}

void Application::onAudioWaitTimeout() {
  if (stopPendingCaptureForSelection())
    return;
  if (!m_pendingAudioGame.isEmpty() && !pendingAudioIdentityIsCurrent()) {
    cancelPendingCapture();
    return;
  }
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
  if (!m_gameDetector->isGamePayloadReady() || m_audioProbeInflight)
    return; // Never stack slow probes or consume timeout ticks for them.
  const QString game = m_pendingAudioGame;
  const bool isRecord = m_pendingAudioIsRecord;
  probeGameAudioAsync(game, m_gameDetector->currentGamePid(), m_audioWaitGeneration,
                     [this, game, isRecord](bool present) {
    if (m_pendingAudioGame != game || !isCurrentCaptureGame(game))
      return;
    ++m_audioWaitTicks;
    m_audioWaitHits = present ? m_audioWaitHits + 1 : 0;
    if (m_audioWaitHits < 2 && m_audioWaitTicks < kAudioWaitMaxTicks)
      return;
    qCDebug(appLog) << (m_audioWaitHits >= 2 ? "Game audio settled for"
                                          : "Game audio wait expired for")
                    << game << "- starting capture";
    m_pendingAudioGame.clear();
    ++m_audioWaitGeneration;
    m_audioWaitTimer->stop();
    if (isRecord)
      m_startFullRecording(game);
    else
      m_replayManager->startDvr(game);
    emit recordingStateChanged();
    emit recordingAvailabilityChanged();
  });
}

bool Application::pendingAudioIdentityIsCurrent() const {
  return m_gameDetector &&
         m_gameDetector->currentGameName() == m_pendingAudioGame &&
         (m_pendingCaptureKey.isEmpty() ||
          m_gameDetector->currentGameKey() == m_pendingCaptureKey) &&
         (m_pendingAudioInstance.isEmpty() ||
          m_gameDetector->captureIdentityInfo(m_pendingAudioGame)
                  .value("detectedGameInstance").toString() == m_pendingAudioInstance);
}

void Application::probeGameAudioAsync(
    const QString &game, qint64 pid, quint64 generation,
    const std::function<void(bool)> &onResult) {
  QPointer<Application> self(this);
  const quint64 ticket = claimAudioProbe();
  const auto lookup = m_gameAudioLookup;
  (void)QtConcurrent::run([self, game, pid, ticket, generation, onResult, lookup]() {
    // Copy the lookup before dispatch; the worker never reads Application state.
    const bool present = lookup(game, pid);
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
  if (!m_pendingAudioGame.isEmpty() && !pendingAudioIdentityIsCurrent()) {
    cancelPendingCapture();
    return;
  }
  if (stopPendingCaptureForSelection())
    return;
  if (m_gameDetector && !m_gameDetector->isGamePayloadReady() &&
      !m_pendingAudioGame.isEmpty()) {
    const auto game = std::exchange(m_pendingAudioGame, {});
    ++m_audioWaitGeneration;
    if (m_audioWaitTimer)
      m_audioWaitTimer->stop();
    startWhenWindowReady(game, m_pendingAudioIsRecord);
    return;
  }
  onResult(present);
}

bool Application::stopPendingCaptureForSelection() {
  if (!m_recordingManager || !m_recordingManager->requiresCaptureSelection())
    return false;
  m_pendingAudioGame.clear();
  m_pendingWindowGame.clear();
  m_pendingAutoRecordGame.clear();
  m_pendingDvrGame.clear();
  ++m_audioWaitGeneration; // Retire in-flight results without releasing their claim.
  if (m_audioWaitTimer)
    m_audioWaitTimer->stop();
  if (m_windowWaitTimer)
    m_windowWaitTimer->stop();
  if (m_detectionDebounceTimer)
    m_detectionDebounceTimer->stop();
  return true;
}

void Application::onInstantReplayEnabledChanged() {
  if (!m_settingsManager->instantReplayEnabled()) {
    m_replayManager->stopDvr();
  } else if (!stopPendingCaptureForSelection() && m_gameDetector &&
             !m_gameDetector->currentGameName().isEmpty() &&
             !m_recordingManager->isRecording() &&
             !m_recordingManager->isRecoveringCapture()) {
    startWhenWindowReady(m_gameDetector->currentGameName(), false);
  }
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
  if (!m_pendingWindowInstance.isEmpty() && m_gameDetector &&
      m_gameDetector->captureIdentityInfo(game)
              .value("detectedGameInstance").toString() != m_pendingWindowInstance) {
    m_pendingWindowGame.clear();
    m_pendingWindowInstance.clear();
    return;
  }
  if (isCurrentCaptureGame(game) && m_gameDetector->isGamePayloadReady()) {
    m_pendingWindowGame.clear();
    m_pendingWindowInstance.clear();
    startWhenWindowReady(game, isRecord);
    return;
  }
  if (m_gameDetector && m_gameDetector->currentGameName() == game &&
      m_gameDetector->currentGameKey() == m_pendingCaptureKey &&
      !isGameBlocked(game) && !restoreTokenForGame(game).isEmpty()) {
    // Slow launches (notably VR) can take longer than 30 seconds. Keep
    // listening for the window; don't silently discard an approved start.
    qCInfo(appLog) << "Still waiting for game payload for approved game" << game
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
    m_authorizePromptTokenInvalid = false;
    qCInfo(appLog) << "Skipping authorize prompt for" << pending
                   << "(blocked or already approved)";
    if (m_pendingAutoRecordGame == pending)
      m_pendingAutoRecordGame.clear();
    if (m_pendingDvrGame == pending)
      m_pendingDvrGame.clear();
    return;
  }
  // Process liveness, not compositor window geometry, gates authorization.
  if (m_gameDetector && !m_gameDetector->isGamePayloadReady()) {
    qCDebug(appLog) << "Deferring authorize prompt for" << pending
                    << "(game process not available yet)";
    return;
  }
  qCInfo(appLog) << "Showing authorize prompt for" << pending;
  m_notificationService->showNotification(
      "New Game: " + pending,
      std::exchange(m_authorizePromptTokenInvalid, false)
          ? "Authorization is no longer valid — click to authorize Wayland "
            "capture for this game again, or Block it."
          : "Click to authorize Wayland capture for this game, or Block it.",
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

void Application::resumeReplayAfterRecording() {
  if (m_recordingManager->isRecoveringCapture() ||
      m_recordingManager->requiresCaptureSelection())
    return; // Saving a part must not turn a full-recording handoff into DVR.
  m_replayManager->setPaused(false);
  if (m_recordingManager->lastRecordingFailed())
    return; // A terminal capture failure must not silently retry through DVR.
  const QString game = m_gameDetector ? m_gameDetector->currentGameName() : QString();
  if (isCurrentCaptureGame(game) && !restoreTokenForGame(game).isEmpty()) {
    if (m_recordingEngine && m_recordingEngine->canReuseCapture(game))
      m_replayManager->startDvr(game);
    else
      startWhenWindowReady(game, false);
  }
}

void Application::startWhenWindowReady(const QString &game, bool isRecord) {
  if (game.isEmpty() || !m_gameDetector ||
      m_gameDetector->currentGameName() != game || isGameBlocked(game))
    return;
  if (m_recordingManager && m_recordingManager->requiresCaptureSelection())
    return; // A closed portal grant needs an explicit user selection.
  m_pendingCaptureKey = m_gameDetector->currentGameKey();
  if (isCurrentCaptureGame(game) && m_gameDetector->isGamePayloadReady()) {
    qCDebug(appLog) << "Game process present for" << game
                    << "- waiting for settled game audio";
    startWhenAudioSettled(game, isRecord);
    return;
  }
  // Do not bind the grant to a known game-specific launcher that disappears
  // on Play. Process evidence cannot prove window ownership; it can exclude
  // the metadata-backed bootstrap before asking the portal to restore.
  qCDebug(appLog) << "Deferring capture for" << game
                  << "until its game payload appears";
  m_pendingWindowGame = game;
  m_pendingWindowInstance = m_gameDetector->captureIdentityInfo(game)
                                .value("detectedGameInstance").toString();
  m_pendingWindowIsRecord = isRecord;
  m_windowWaitTimer->start();
  emit recordingStateChanged();
  emit recordingAvailabilityChanged();
}

void Application::resumePendingCapture() {
  const QString game = m_gameDetector->currentGameName();
  // First-time authorization must also wait for the payload, not present a
  // picker for the bootstrap. PID promotion wakes a deferred notification.
  if ((m_pendingAutoRecordGame == game || m_pendingDvrGame == game) &&
      m_gameDetector->isGamePayloadReady() && shouldShowAuthorizePrompt(game)) {
    m_detectionDebounceTimer->start();
  }
  if (!isCurrentCaptureGame(game) ||
      m_gameDetector->currentGameKey() != m_pendingCaptureKey)
    return;
  const bool waitingWindow = m_pendingWindowGame == game;
  if (waitingWindow && !m_pendingWindowInstance.isEmpty() &&
      m_gameDetector->captureIdentityInfo(game)
              .value("detectedGameInstance").toString() != m_pendingWindowInstance) {
    m_pendingWindowGame.clear();
    m_pendingWindowInstance.clear();
    m_windowWaitTimer->stop();
    return;
  }
  const bool waitingAudio = m_pendingAudioGame == game;
  if (!waitingWindow && !waitingAudio)
    return;
  const bool isRecord = waitingWindow ? m_pendingWindowIsRecord : m_pendingAudioIsRecord;
  m_pendingWindowGame.clear();
  m_pendingWindowInstance.clear();
  m_windowWaitTimer->stop();
  m_pendingAudioGame.clear();
  ++m_audioWaitGeneration;
  m_audioWaitTimer->stop();
  qCInfo(appLog) << "Resuming pending capture after process handoff for" << game
                 << "PID:" << m_gameDetector->currentGamePid();
  startWhenWindowReady(game, isRecord);
  emit recordingStateChanged();
  emit recordingAvailabilityChanged();
}

void Application::clearPendingCaptureForGame(const QString &game) {
  bool changed = false;
  if (!game.isEmpty() && m_pendingWindowGame == game) {
    m_pendingWindowGame.clear();
    m_pendingWindowInstance.clear();
    if (m_windowWaitTimer)
      m_windowWaitTimer->stop();
    changed = true;
  }
  if (!game.isEmpty() && m_pendingAudioGame == game) {
    m_pendingAudioGame.clear();
    m_pendingAudioInstance.clear();
    ++m_audioWaitGeneration;
    if (m_audioWaitTimer)
      m_audioWaitTimer->stop();
    changed = true;
  }
  if (changed) {
    // Native state stays Idle throughout launch/audio waits. Notify the
    // derived phase explicitly so QML cannot retain a closed launch.
    emit recordingStateChanged();
    emit recordingAvailabilityChanged();
  }
}

void Application::cancelPendingCapture() {
  m_pendingWindowGame.clear();
  m_pendingWindowInstance.clear();
  m_pendingAudioGame.clear();
  m_pendingAudioInstance.clear();
  m_pendingAutoRecordGame.clear();
  m_pendingDvrGame.clear();
  ++m_audioWaitGeneration;
  if (m_windowWaitTimer)
    m_windowWaitTimer->stop();
  if (m_audioWaitTimer)
    m_audioWaitTimer->stop();
  if (m_detectionDebounceTimer)
    m_detectionDebounceTimer->stop();
  emit recordingStateChanged();
  emit recordingAvailabilityChanged();
}
bool Application::isWayland() const { return CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::Wayland; }

void Application::onCaptureRecoveryChanged() {
  stopPendingCaptureForSelection();
  // recordingPhase/canRestartTrophy depend on recovery intent as well as the
  // native recording state. An idle cancelled retry has no stateChanged signal.
  emit recordingStateChanged();
  emit recordingAvailabilityChanged();
  if (m_recordingManager && m_replayManager &&
      !m_recordingManager->isRecoveringCapture() &&
      !m_recordingManager->requiresCaptureSelection() &&
      m_recordingManager->state() == RecordingManager::RecordingState::Idle)
    m_replayManager->setPaused(false);
}

bool Application::canStartRecording() const {
  return m_captureAvailable && m_recordingManager &&
         m_pendingWindowGame.isEmpty() &&
         (m_pendingAudioGame.isEmpty() || !m_pendingAudioIsRecord) &&
         !m_recordingManager->isRecoveringCapture() &&
         m_recordingManager->state() == RecordingManager::RecordingState::Idle;
}

QString Application::recordingPhase() const {
  if (!m_recordingManager)
    return QStringLiteral("idle");
  if ((!m_pendingWindowGame.isEmpty() && m_pendingWindowIsRecord) ||
      (!m_pendingAudioGame.isEmpty() && m_pendingAudioIsRecord))
    return QStringLiteral("waiting-game");
  if (m_recordingManager->isRecoveringCapture())
    return QStringLiteral("initializing");
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

bool Application::isRunWithoutTraySupported() const {
  // GNOME has no StatusNotifierItem tray by default. Hiding the window still
  // keeps Trophy (and recordings) alive; the single-instance guard restores
  // the window when Trophy is launched again. Note this does NOT make the
  // app appear in GNOME's "Background Apps": that list is fed by the
  // xdg-desktop-portal background monitor, which only enumerates Flatpak
  // instances, so native/AppImage apps cannot register there.
  const QString desktop = (qgetenv("XDG_CURRENT_DESKTOP") + " " +
                           qgetenv("XDG_SESSION_DESKTOP"))
                              .toUpper();
  return desktop.contains("GNOME");
}

bool Application::useNativeDialogs() const {
  // Use the host's native (portal) dialogs when the portal can resolve our
  // app id: a reverse-DNS desktop entry exists in an XDG application directory, either
  // from our own integration, a distro package, or a third-party AppImage
  // manager. Otherwise fall back to Qt's own dialog, which does not depend on
  // portal app-id registration.
  if (!m_desktopIntegration)
    return true;
  if (!m_desktopIntegration->isAppImage())
    return true;
  return m_desktopIntegration->isRegistered();
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

bool Application::applyStartupVisibility() {
  const bool requested =
      (m_settingsManager && m_settingsManager->startMinimized()) || m_forceMinimized;
  if (!requested)
    return false;
  // Only hide when there is a way back: a tray icon, or a desktop where
  // hiding is supported (relaunching Trophy shows the running window).
  if (!(isTrayAvailable() || isRunWithoutTraySupported())) {
    qCInfo(appLog) << "Start minimized requested, but this desktop has no tray "
                      "host and no background mode; showing the window";
    return false;
  }
  if (isTrayAvailable()) {
    qCInfo(appLog) << "Starting minimized to system tray";
  } else {
    qCInfo(appLog) << "Starting hidden in the background (no tray host; launch "
                      "Trophy again to show the window)";
    // Without a tray icon there is no visual sign Trophy is running; say so
    // once. The updater's relaunch already announces itself with its own
    // notice, so skip the duplicate there.
    if (m_notificationService && m_postUpdateFromVersion.isEmpty()) {
      m_notificationService->showNotification(
          "Trophy started in the background",
          "Trophy is running. Launch Trophy again to open the window.",
          "trophy", 5000, 1);
    }
  }
  setMainWindowVisible(false);
  return true;
}

bool Application::minimizeToTray() {
  if (!(isTrayAvailable() || isRunWithoutTraySupported()) ||
      !m_settingsManager->minimizeToTray()) {
    return false;
  }
  setMainWindowVisible(false);
  if (!m_minimizeNotifyShown) {
    m_minimizeNotifyShown = true;
    const bool onTray = isTrayAvailable();
    m_notificationService->showNotification(
        "Trophy minimized",
        m_recordingManager->isRecording()
            ? (onTray ? "Recording continues in the background. Right-click the tray icon to stop."
                      : "Recording continues in the background. Launch Trophy again to open the window.")
            : (onTray ? "Trophy keeps running in the system tray."
                      : "Trophy keeps running in the background. Launch Trophy again to open the window."),
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
  if (m_recordingManager && m_recordingManager->isRecording()) {
    requestBookmark();
    return;
  }
  if (!m_replayManager || !m_settingsManager) {
    return;
  }
  if (!m_settingsManager->instantReplayEnabled()) {
    m_notificationService->showNotification(
        "Clips Are Off",
        "Clips are off. Enable them in Settings to buffer gameplay.",
        "trophy", 3000, 2);
    return;
  }
  if (!m_replayManager->isActive() && !m_replayManager->isRecoveringCapture()) {
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
}

bool Application::isTrackMuted(int trackIndex) const {
  if (m_recordingManager && m_recordingManager->isRecording() && m_audioMixer)
    return m_audioMixer->isSemanticTrackMuted(trackIndex);
  if (m_audioMixer)
    return m_audioMixer->isSemanticTrackMuted(trackIndex);
  return false;
}

void Application::requestBookmark() {
  if (!m_recordingManager || !m_notificationService) return;
  if (!m_recordingManager->isRecording()) {
    m_notificationService->showNotification("Bookmark", "Start a full recording to bookmark moments.", {}, 5000, 2);
    return;
  }
  const bool saved = m_recordingManager->addBookmark();
  if (saved) playClipPing(false);
  m_notificationService->showNotification(
      saved ? "Bookmark saved" : "Bookmark failed",
      saved ? "Find this moment on the recording's player timeline."
            : "Could not save the bookmark. Wait for video frames and check recording-folder permissions.",
      {}, 5000, 2);
}

void Application::installUpdate() {
  if (!m_updateChecker || !m_updateInstaller)
    return;
  if (m_recordingManager && m_recordingManager->isRecording()) {
    qWarning("installUpdate: blocked while recording");
    if (m_notificationService)
      m_notificationService->showNotification(
          QStringLiteral("Trophy"),
          QStringLiteral("Stop recording before updating."), QStringLiteral("trophy"),
          5000, 2);
    return;
  }
  const int kind = static_cast<int>(InstallInfo::detect());
  qWarning("installUpdate: kind=%d asset=%s", kind,
           m_updateChecker->latestAssetUrl().toUtf8().constData());
  if (kind != 0) {
    const QString url = m_updateChecker->latestAssetUrl();
    if (!url.isEmpty())
      QDesktopServices::openUrl(QUrl(url));
    return;
  }
  if (m_updateChecker->forceFakeUpdate()) {
    m_updateInstaller->simulate();
    return;
  }
  const QString target = QString::fromUtf8(qgetenv("APPIMAGE"));
  m_updateInstaller->installAppImage(m_updateChecker->latestAssetUrl(),
                                     m_updateChecker->latestDigest(),
                                     target);
}

QString Application::restartTarget() const {
  // Prefer the AppImage the user launched: the executable path of a mounted
  // AppImage disappears when this process exits.
  QString target = QString::fromUtf8(qgetenv("APPIMAGE"));
  if (target.isEmpty() && !qgetenv("APPDIR").isEmpty())
    return QString();
  if (target.isEmpty())
    target = QCoreApplication::applicationFilePath();
  const QFileInfo info(target);
  return info.isFile() && info.isExecutable() ? info.absoluteFilePath() : QString();
}

bool Application::canRestartTrophy() const {
  if (m_recordingManager && m_recordingManager->isRecording())
    return false;
  return !restartTarget().isEmpty();
}

bool Application::restartTrophy() {
  if (m_recordingManager && m_recordingManager->isRecording()) {
    qWarning("restartTrophy: blocked while recording");
    if (m_notificationService)
      m_notificationService->showNotification(
          QStringLiteral("Trophy"),
          QStringLiteral("Stop recording before restarting."),
          QStringLiteral("trophy"), 5000, 2);
    return false;
  }

  const QString target = restartTarget();
  if (target.isEmpty()) {
    qWarning("restartTrophy: no relaunch target; cannot restart");
    return false;
  }

  if (!AppRestart::scheduleAfterExit(target)) {
    qWarning("restartTrophy: could not schedule the relaunch of %s",
             target.toUtf8().constData());
    if (m_notificationService)
      m_notificationService->showNotification(
          QStringLiteral("Trophy"),
          QStringLiteral("Could not restart Trophy automatically. Quit and start it again."),
          QStringLiteral("trophy"), 6000, 2);
    return false;
  }

  qCInfo(appLog) << "Restarting after exit:" << target;
  QCoreApplication::quit();
  return true;
}

void Application::openUpdateDialog() {
  showMainWindow();
  emit updateDialogRequested();
}

QVariantMap Application::captureHealth() const {
  if (m_recordingManager && m_recordingManager->isRecording()) {
    auto result = m_recordingManager->captureHealth();
    result["mode"] = QStringLiteral("recording");
    result["gameName"] = m_recordingManager->currentGameName();
    return result;
  }
  if (m_replayManager && (m_replayManager->isActive() || m_replayManager->isRecoveringCapture())) {
    auto result = m_replayManager->captureHealth();
    result["gameName"] = m_replayManager->currentGame();
    return result;
  }
  return {};
}

float Application::trackLevel(int trackIndex) const {
  if (m_recordingManager && m_recordingManager->isRecording() && m_audioMixer)
    return m_audioMixer->semanticTrackLevel(trackIndex);
  if (m_recordingEngine && m_recordingEngine->hasDemand() && m_audioMixer)
    return m_audioMixer->semanticTrackLevel(trackIndex);
  return 0.0f;
}

void Application::requestToggleRecording() {  if (!m_recordingManager) {
    return;
  }
  if (!m_pendingWindowGame.isEmpty()) {
    const bool wasRecording = m_pendingWindowIsRecord;
    cancelPendingCapture();
    if (wasRecording)
      return;
  }
  if (!m_pendingAudioGame.isEmpty() && m_pendingAudioIsRecord) {
    cancelPendingCapture();
    return;
  }
  if (m_recordingManager->requiresCaptureSelection() &&
      m_recordingManager->state() == RecordingManager::RecordingState::Idle) {
    m_recordingManager->resumeCapture();
    return;
  }
  if (m_recordingManager->isRecording() || m_recordingManager->isRecoveringCapture()) {
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
  // Refresh managed entries too: older packaged versions stored a transient
  // AppImage mount path.
  if (!m_autostartManager->setEnabled(
          want, QCoreApplication::applicationFilePath(),
          QString::fromUtf8(qgetenv("APPIMAGE")))) {
    qCWarning(appLog) << "Failed to sync autostart entry";
  }
}

} // namespace TrophyBackend
