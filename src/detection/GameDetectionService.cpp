#include "GameDetectionService.h"
#include "DefaultAppCatalog.h"
#include "GameWhitelist.h"
#include "WaylandWindowMonitor.h"
#include "WindowInfo.h"

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QStandardPaths>

#include <cstring>
#include <unistd.h>

#include <cerrno>
#include <csignal>

#include <algorithm>

namespace TrophyBackend {

GameDetectionService::GameDetectionService(QObject *parent)
    : QObject(parent), m_whitelist(std::make_unique<GameWhitelist>()) {
  // Monotonic clock for the per-PID Steam-check TTL cache below.
  m_uptimeClock.start();
  m_server = new QLocalServer(this);
  connect(m_server, &QLocalServer::newConnection, this,
          &GameDetectionService::onNewConnection);

  // Watchdog: games rarely tear down cleanly (no vkDestroyInstance on
  // quit), so STOP/disconnect never arrives and the session outlives
  // the game. A single kill(pid, 0) probe every few seconds closes the
  // gap — targeted, not a scan. Armed while a game is tracked.
  m_livenessTimer = new QTimer(this);
  m_livenessTimer->setInterval(5000);
  m_livenessTimer->setSingleShot(false);
  connect(m_livenessTimer, &QTimer::timeout, this,
          &GameDetectionService::onLivenessTimeout);

  m_whitelist->load();

  // Games-page model: built once, contents rebuilt on state changes.
  // Built-in exclusions start hidden (page toggle opts in).
  m_gamesModel = std::make_unique<GameListModel>(this);
  m_gamesFilter = new GameListFilterModel(this);
  m_gamesFilter->setSourceModel(m_gamesModel.get());
  m_gamesFilter->setShowDefaultEntries(false);
  connect(this, &GameDetectionService::currentGameChanged, this,
          &GameDetectionService::rebuildModels);
  connect(this, &GameDetectionService::recentGamesChanged, this,
          &GameDetectionService::rebuildModels);
  connect(this, &GameDetectionService::whitelistChanged, this,
          &GameDetectionService::rebuildModels);
  connect(this, &GameDetectionService::blacklistChanged, this,
          &GameDetectionService::rebuildModels);
  connect(this, &GameDetectionService::tokenGamesChanged, this,
          &GameDetectionService::rebuildModels);
  rebuildModels();
}

GameDetectionService::~GameDetectionService() { stop(); }

void GameDetectionService::start() {
  if (m_running)
    return;
  qDebug() << "Starting game detection service (socket listener; layers/shims "
              "report here when installed)";

  // Preserve a live listener. Removing its filesystem socket would detach
  // the running server and let this instance take over the frozen endpoint.
  {
    QLocalSocket probe;
    probe.connectToServer("/tmp/trophy-detection.sock");
    if (probe.waitForConnected(300)) {
      qWarning() << "Game detection socket is already owned by a running"
                    " instance";
      return;
    }
  }

  // No live owner: remove a stale socket left by a crashed process.
  QLocalServer::removeServer("/tmp/trophy-detection.sock");

  if (!m_server->listen("/tmp/trophy-detection.sock")) {
    qCritical() << "Failed to start detection server:"
                << m_server->errorString();
    return;
  }

  m_running = true;
}

void GameDetectionService::stop() {
  if (!m_running)
    return;
  qInfo() << "Stopping game detection service";
  m_livenessTimer->stop();
  m_server->close();
  m_running = false;

  for (QLocalSocket *socket : m_activeConnections.keys()) {
    socket->disconnect();
    socket->close();
    socket->deleteLater();
  }
  m_activeConnections.clear();
  m_pidToGameName.clear();
  m_pidToRawKey.clear();
  m_pendingCandidates.clear();
  m_pendingRawKeys.clear();
  // Drop any tracked game too: otherwise isGameRunning() stays true with a
  // dead PID after stop/start (emits gameClosed so captures tear down).
  updateCurrentGame("", 0);
}

void GameDetectionService::onNewConnection() {
  QLocalSocket *socket = m_server->nextPendingConnection();
  if (!socket)
    return;

  connect(socket, &QLocalSocket::readyRead, this,
          &GameDetectionService::onSocketReadyRead);
  connect(socket, &QLocalSocket::disconnected, this,
          &GameDetectionService::onSocketDisconnected);
}

void GameDetectionService::setWindowMonitor(WaylandWindowMonitor *monitor) {
  if (m_monitor)
    disconnect(m_monitor, nullptr, this, nullptr);
  m_monitor = monitor;
  if (m_monitor) {
    connect(m_monitor, &WaylandWindowMonitor::windowsChanged, this,
            &GameDetectionService::onMonitorWindowsChanged);
  }
}

uint GameDetectionService::steamAppIdForPid(qint64 pid) {
  if (pid <= 0)
    return 0;
  QFile env(QString("/proc/%1/environ").arg(pid));
  if (!env.open(QIODevice::ReadOnly))
    return 0;
  // Environ is NUL-separated KEY=VALUE records.
  for (const QByteArray &rec : env.readAll().split('\0')) {
    if (rec.startsWith("SteamAppId=")) {
      bool ok = false;
      const uint id = rec.mid(sizeof("SteamAppId=") - 1).toUInt(&ok);
      if (ok && id > 0)
        return id;
    }
  }
  return 0;
}

uint GameDetectionService::steamAppIdFromAppId(const QString &appId) {
  static const QRegularExpression re("^steam_app_(\\d+)$",
                                     QRegularExpression::CaseInsensitiveOption);
  const QRegularExpressionMatch m = re.match(appId.trimmed());
  if (!m.hasMatch())
    return 0;
  return m.captured(1).toUInt();
}

bool GameDetectionService::processLooksLikeSteamGame(qint64 pid) {
  if (pid <= 0)
    return false;
  // Hot path (every windowsChanged per pending PID): cache per-PID
  // results for a few seconds. Still a targeted single-PID lookup —
  // no /proc scanning. Member (not static) so verdicts never leak
  // across service instances/tests; monotonic clock so wall-clock
  // jumps can't stretch or shrink the TTL. The short TTL bounds
  // PID-reuse staleness.
  const qint64 now = m_uptimeClock.elapsed();
  const auto hit = m_steamCheckCache.constFind(pid);
  if (hit != m_steamCheckCache.constEnd() && now - hit->second < kCacheTtlMs)
    return hit->first;
  bool result = false;
  if (steamAppIdForPid(pid) > 0) {
    result = true;
  } else {
    // Targeted single-PID lookup per shim event (not a /proc scan).
    char exe[512];
    const QString exeLink = QString("/proc/%1/exe").arg(pid);
    ssize_t n =
        ::readlink(exeLink.toLocal8Bit().constData(), exe, sizeof(exe) - 1);
    if (n > 0) {
      exe[n] = '\0';
      if (strstr(exe, "steamapps"))
        result = true;
    }
    if (!result) {
      QFile cmd(QString("/proc/%1/cmdline").arg(pid));
      if (cmd.open(QIODevice::ReadOnly)) {
        const QByteArray raw = cmd.readAll();
        if (raw.contains("steamapps"))
          result = true;
      }
    }
  }
  m_steamCheckCache.insert(pid, qMakePair(result, now));
  if (m_steamCheckCache.size() > kCacheCap) {
    for (auto it = m_steamCheckCache.begin();
         it != m_steamCheckCache.end();) {
      if (now - it.value().second >= kCacheTtlMs)
        it = m_steamCheckCache.erase(it);
      else
        ++it;
    }
    // Still over cap (256 fresh entries): drop the oldest rather than
    // growing without bound under rapid distinct PIDs.
    while (m_steamCheckCache.size() > kCacheCap)
      m_steamCheckCache.erase(m_steamCheckCache.begin());
  }
  return result;
}

bool GameDetectionService::coversOutput(const WindowInfo &w) {
  if (w.width <= 0 || w.height <= 0 || w.outputWidth <= 0 ||
      w.outputHeight <= 0)
    return false; // geometry unknown: strict fullscreen flag only
  const double coverage =
      (double)w.width * w.height / ((double)w.outputWidth * w.outputHeight);
  return coverage >= 0.9;
}

bool GameDetectionService::isConfidentGame(qint64 pid, const QString &gameName,
                                           uint steamAppId) {
  // Instant detect: explicit user rule, Proton/Steam marker, or the
  // executable living in a Steam library.
  if (m_whitelist->isWhitelisted(gameName))
    return true;
  if (steamAppId > 0)
    return true;
  if (processLooksLikeSteamGame(pid))
    return true;
  // Otherwise the focused window for the PID decides: real fullscreen,
  // or borderless covering the output.
  if (m_monitor) {
    const WindowInfo w = m_monitor->windowForPid(pid);
    if (!w.minimized && w.focused && (w.fullscreen || coversOutput(w)))
      return true;
  }
  return false;
}

bool GameDetectionService::isBlacklistedIdentity(
    const QString &rawKey, const QString &displayName) const {
  return (!rawKey.isEmpty() && m_whitelist->isBlacklisted(rawKey)) ||
         (!displayName.isEmpty() && m_whitelist->isBlacklisted(displayName));
}

GameDetectionService::ReporterIdentity
GameDetectionService::resolveReporterIdentity(const QString &reporter,
                                              const QString &exe,
                                              const QStringList &argv,
                                              uint steamAppId,
                                              const QString &winePrefix) {
  const auto basename = [](QString path) {
    path.replace('\\', '/');
    return path.section('/', -1);
  };
  const auto wrapper = [](const QString &name) {
    return QStringList{"wine", "wine64", "wine-preloader", "wine64-preloader",
                       "wineserver", "proton"}.contains(name.toLower());
  };
  const auto helper = [](const QString &name) {
    const QString key = GameWhitelist::normalizeKey(name);
    return QStringList{"steam", "steamwebhelper", "steam-runtime-launch-client",
                       "steam-runtime-launcher-service", "pressure-vessel-wrap",
                       "d3ddriverquery", "d3ddriverquery64", "steam gpu query",
                       "wineopenxr test instance", "hellovr_vulkan", "xalia",
                       "wineboot", "winecfg", "winedevice", "services", "explorer",
                       "rundll32", "crashpad_handler", "unitycrashhandler64",
                       "unitycrashhandler32", "crashreportclient"}.contains(key);
  };
  ReporterIdentity result;
  result.name = reporter.trimmed();
  QString executablePath = exe;
  QString executable = basename(exe);
  // Wine may rewrite argv[0] to the Windows executable, or keep the Wine
  // loader followed by it. Never search arbitrary arguments: an installer
  // or launcher can mention another executable without running it.
  if (wrapper(executable)) {
    executable.clear();
    executablePath.clear();
    if (!argv.isEmpty()) {
      QString candidate = argv.first();
      if (wrapper(basename(candidate)) && argv.size() > 1)
        candidate = argv.at(1);
      if (candidate.endsWith(".exe", Qt::CaseInsensitive)) {
        executable = basename(candidate);
        candidate.replace('\\', '/');
        executablePath = QStringLiteral("wine:") + winePrefix + ":" + candidate;
      }
    }
  }
  result.executable = executable;
  result.ignored = helper(basename(reporter)) || helper(executable) ||
                   (wrapper(basename(exe)) && executable.isEmpty());
  if (result.ignored)
    return result;
  const bool generic = GameWhitelist::isPlaceholderReporterKey(reporter) ||
                       QStringList{"unity", "unreal engine", "godot", "sdl_app"}
                           .contains(reporter.trimmed().toLower());
  if (steamAppId > 0)
    result.key = QStringLiteral("steam_app_%1").arg(steamAppId);
  else if (!executable.isEmpty())
    result.key = GameWhitelist::normalizeKey(
        executablePath.startsWith("wine:") ? executablePath : "exe:" + executablePath);
  else if (!generic)
    result.key = GameWhitelist::normalizeKey(reporter);
  if (generic && !executable.isEmpty())
    result.name = executable;
  // An unresolved null-name event has no identity. Even Steam evidence
  // must not turn the shared placeholder into a display-name/token key.
  if (generic && executable.isEmpty())
    result.name.clear();
  return result;
}

GameDetectionService::ReporterIdentity GameDetectionService::reporterIdentity(
    qint64 pid, const QString &reporter, uint steamAppId) {
  const QString exe = QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget();
  QFile command(QString("/proc/%1/cmdline").arg(pid));
  QStringList argv;
  if (command.open(QIODevice::ReadOnly)) {
    const QByteArray bytes = command.read(65537);
    if (bytes.size() <= 65536) {
      for (const QByteArray &arg : bytes.split('\0'))
        argv.append(QString::fromLocal8Bit(arg));
    }
  }
  const QString loader = QFileInfo(exe).fileName().toLower();
  if ((loader == "wine" || loader == "wine64" || loader.endsWith("-preloader")) &&
      !argv.isEmpty()) {
    int index = 0;
    if ((argv.first() == "wine" || argv.first() == "wine64" ||
         QFileInfo(argv.first()).fileName().endsWith("-preloader")) && argv.size() > 1)
      index = 1;
    QString candidate = argv.at(index);
    candidate.replace('\\', '/');
    static const QRegularExpression drive("^[A-Za-z]:");
    if (candidate.endsWith(".exe", Qt::CaseInsensitive) &&
        QDir::isRelativePath(candidate) && !drive.match(candidate).hasMatch()) {
      const QString cwd = QFileInfo(QString("/proc/%1/cwd").arg(pid)).symLinkTarget();
      if (!cwd.isEmpty())
        argv[index] = QDir::cleanPath(cwd + "/" + candidate);
    }
  }
  QString winePrefix;
  QFile environment(QString("/proc/%1/environ").arg(pid));
  if (environment.open(QIODevice::ReadOnly)) {
    const QByteArray bytes = environment.read(65537);
    if (bytes.size() <= 65536) {
      for (const QByteArray &entry : bytes.split('\0')) {
        if (entry.startsWith("WINEPREFIX="))
          winePrefix = QString::fromLocal8Bit(entry.mid(11));
      }
    }
  }
  if (winePrefix.isEmpty())
    winePrefix = QDir::homePath() + "/.wine";
  return resolveReporterIdentity(reporter, exe, argv, steamAppId, winePrefix);
}

void GameDetectionService::trackGame(qint64 pid, const QString &gameName,
                                     const QString &rawKey) {
  // Show the user's display name when one is registered; fall back to
  // the raw reporter class either way so dedup stays consistent.
  QString displayName = gameName;
  const QString matchKey = m_whitelist->isWhitelisted(rawKey) ? rawKey : gameName;
  if (m_whitelist->isWhitelisted(matchKey)) {
    const QString mapped = m_whitelist->getWhitelistName(matchKey);
    if (!mapped.isEmpty())
      displayName = mapped;
  }
  // Canonical match key: "Foo.EXE" and "foo" are one entry everywhere
  // (matching itself normalizes too, so either form would hit).
  const QString key =
      GameWhitelist::normalizeKey(rawKey.isEmpty() ? gameName : rawKey);

  bool isBetterName = true;
  if (m_pidToGameName.contains(pid)) {
    QString existingName = m_pidToGameName[pid];
    // Don't overwrite a specific name with a generic one
    if (displayName == "Unknown Vulkan Game" &&
        existingName != "Unknown Vulkan Game") {
      isBetterName = false;
    }
  }

  if (isBetterName) {
    bool isNewName = true;
    for (auto it = m_pidToRawKey.constBegin(); it != m_pidToRawKey.constEnd(); ++it) {
      if (it.value() == key && it.key() != pid) {
        isNewName = false;
        break;
      }
    }

    m_pidToGameName[pid] = displayName;
    m_pidToRawKey[pid] = key;

    if (isNewName) {
      qDebug() << "Game detected:" << displayName << "(PID:" << pid << ")";
      // A background graphics initialization must not steal a game with
      // a live visible window. Keep the candidate tracked; focus changes
      // below can select it once its actual window is in use.
      const WindowInfo incoming = m_monitor ? m_monitor->windowForPid(pid) : WindowInfo{};
      if (m_currentGameName.isEmpty() || !hasVisibleWindow() ||
          (!incoming.minimized && incoming.focused))
        updateCurrentGame(displayName, pid, key);
    } else {
      qDebug() << "Additional process for existing game detected:"
               << displayName << "(PID:" << pid << ")";
    }
  } else {
    qDebug() << "Ignoring redundant/generic start message for PID" << pid << ":"
             << displayName;
  }
}

void GameDetectionService::recheckCandidates() {
  if (m_pendingCandidates.isEmpty())
    return;
  const QList<qint64> pids = m_pendingCandidates.keys();
  for (qint64 pid : pids) {
    const QString name = m_pendingCandidates.value(pid);
    const QString rawKey = m_pendingRawKeys.value(pid, name);
    if (isBlacklistedIdentity(rawKey, name)) {
      m_pendingCandidates.remove(pid);
      m_pendingRawKeys.remove(pid);
      continue;
    }
    if (m_whitelist->isWhitelisted(rawKey) || isConfidentGame(pid, name, 0)) {
      m_pendingCandidates.remove(pid);
      m_pendingRawKeys.remove(pid);
      qDebug() << "Pending game confirmed by window state:" << name
               << "(PID:" << pid << ")";
      trackGame(pid, name, rawKey);
    }
  }
}

void GameDetectionService::onMonitorWindowsChanged() {
  recheckCandidates();
  if (m_monitor) {
    // Only already-reported games participate: a focused desktop/browser
    // is context, never independent process discovery.
    for (const WindowInfo &w : m_monitor->windows()) {
      if (!w.minimized && w.focused && m_pidToGameName.contains(w.pid)) {
        updateCurrentGame(m_pidToGameName.value(w.pid), w.pid,
                          m_pidToRawKey.value(w.pid));
        break;
      }
    }
  }
  // Reconcile on every snapshot while no game is tracked — not just the
  // first. The first emission can be empty (startMonitoring re-emits before
  // the backend's first real snapshot) or unfocused (user just clicked
  // Kickoff), and either would permanently miss an already-running game.
  // Focused/fullscreen + evidence gating inside makes reruns idempotent.
  if (m_currentGameName.isEmpty())
    reconcileRunningGames();
}

void GameDetectionService::reconcileRunningGames() {
  if (!m_monitor)
    return;
  // Focused windows first so the current game wins when several qualify.
  QList<WindowInfo> windows = m_monitor->windows();
  if (windows.isEmpty()) {
    qDebug() << "Reconcile: empty window snapshot, will retry on the next one";
    return;
  }
  std::sort(windows.begin(), windows.end(),
            [](const WindowInfo &a, const WindowInfo &b) {
              if (a.focused != b.focused)
                return a.focused > b.focused;
              if (a.fullscreen != b.fullscreen)
                return a.fullscreen > b.fullscreen;
              return a.id < b.id;
            });

  for (const WindowInfo &w : windows) {
    if (w.minimized || (!w.focused && !w.fullscreen))
      continue;
    QString name = w.appId;
    if (name.isEmpty())
      name = w.title;
    if (name.isEmpty() || w.pid <= 0)
      continue;
    const QString rawKey = w.appId.isEmpty() ? w.title : w.appId;
    // Resolve the display name before checking the veto: users can block
    // either the compositor/reporter key or the Steam manifest name.
    QString display = name;
    uint sid = steamAppIdForPid(w.pid);
    if (sid == 0)
      sid = steamAppIdFromAppId(rawKey);
    const ReporterIdentity identity = reporterIdentity(w.pid, name, sid);
    if (identity.ignored)
      continue;
    const QString key = sid > 0 ? QStringLiteral("steam_app_%1").arg(sid)
                               : (identity.key.isEmpty() ? rawKey : identity.key);
    if (sid > 0) {
      const QString steamName = getSteamAppName(sid);
      if (!steamName.isEmpty())
        display = steamName;
    }
    if (isBlacklistedIdentity(rawKey, display) ||
        isBlacklistedIdentity(key, identity.executable))
      continue;
    // Deduplicate identities, never display labels: two unrelated games
    // may both report Game.exe (or the same engine name).
    if (m_pidToGameName.contains(w.pid))
      continue; // same process already tracked under another spelling
    if (m_pidToRawKey.key(key, 0) != 0)
      continue; // already tracked
    // Same bar as live detection: whitelist or Steam evidence only.
    if (m_whitelist->isWhitelisted(name) || m_whitelist->isWhitelisted(key) ||
        processLooksLikeSteamGame(w.pid)) {
      qDebug() << "Reconciled already-running game:" << display
               << "(PID:" << w.pid << ")";
      trackGame(w.pid, display, key);
    }
  }
}

void GameDetectionService::onSocketReadyRead() {
  QLocalSocket *socket = qobject_cast<QLocalSocket *>(sender());
  if (!socket)
    return;

  while (socket->canReadLine()) {
    QString line = QString::fromUtf8(socket->readLine()).trimmed();
    const qsizetype commandEnd = line.indexOf(':');
    if (commandEnd < 0)
      continue;

    const QString command = line.left(commandEnd);
    if (command != "START" && command != "STOP")
      continue;

    bool pidOk = false;
    qint64 pid = -1;
    qsizetype pidEnd = -1;

    if (command == "STOP") {
      // Frozen wire format is STOP:<pid> (single colon). Accept a
      // trailing numeric field the same way START does, but never
      // require a second colon.
      QString pidField = line.mid(commandEnd + 1);
      const qsizetype extraColon = pidField.indexOf(':');
      if (extraColon >= 0)
        pidField.truncate(extraColon);
      pid = pidField.toLongLong(&pidOk);
      if (!pidOk || pid <= 0)
        continue;
    } else {
      pidEnd = line.indexOf(':', commandEnd + 1);
      if (pidEnd < 0)
        continue;
      pid = line.mid(commandEnd + 1, pidEnd - commandEnd - 1)
                .toLongLong(&pidOk);
      if (!pidOk || pid <= 0)
        continue;
    }

    if (command == "START") {
      // A reporter connection belongs to one process. Do not strand old
      // per-PID state if malformed traffic changes its claimed identity.
      if (m_activeConnections.contains(socket) &&
          m_activeConnections.value(socket) != pid)
        continue;
      // Split only the fixed prefix. Game names may contain colons; the
      // optional Steam app id is the final numeric field.
      QString gameName = line.mid(pidEnd + 1);
      uint steamAppId = 0;
      const qsizetype appIdSep = gameName.lastIndexOf(':');
      if (appIdSep >= 0) {
        bool appIdOk = false;
        const uint parsedAppId = gameName.mid(appIdSep + 1).toUInt(&appIdOk);
        if (appIdOk) {
          steamAppId = parsedAppId;
          gameName.truncate(appIdSep);
        }
      }
      if (gameName.isEmpty())
        continue;
      const QString reporterName = gameName;
      if (steamAppId == 0)
        steamAppId = steamAppIdForPid(pid);
      const ReporterIdentity identity = reporterIdentity(pid, reporterName, steamAppId);
      if (identity.ignored) {
        qDebug() << "Ignoring helper reporter:" << reporterName << "PID:" << pid;
        continue;
      }
      gameName = identity.name;
      if (steamAppId > 0) {
        QString steamName = getSteamAppName(steamAppId);
        if (!steamName.isEmpty()) {
          gameName = steamName;
        }
      }

      // Resolve the placeholder BEFORE applying exclusions. Its built-in
      // veto must not discard a positively identified Steam/native game.
      // Explicit real reporter/executable/game vetoes remain authoritative.
      const QString reporterVeto = GameWhitelist::isPlaceholderReporterKey(reporterName)
                                       ? QString() : reporterName;
      if (identity.key.isEmpty() || gameName.isEmpty() ||
          isBlacklistedIdentity(reporterVeto, gameName) ||
          isBlacklistedIdentity(identity.key, identity.executable)) {
        qDebug() << "Ignoring blacklisted process:" << gameName
                 << "(PID:" << pid << ")";
        continue;
      }

      const QString rawKey = identity.key;
      if (GameWhitelist::isPlaceholderReporterKey(reporterName) &&
          gameName == identity.name && m_pidToRawKey.value(pid) == rawKey &&
          m_pidToGameName.contains(pid))
        gameName = m_pidToGameName.value(pid);

      m_activeConnections[socket] = pid;

      // Confidence fusion: instant detect on whitelist / Steam markers,
      // or focused + fullscreen via the window monitor. Anything else
      // waits as a candidate until window state confirms it.
      if (m_whitelist->isWhitelisted(rawKey) ||
          isConfidentGame(pid, gameName, steamAppId)) {
        m_pendingCandidates.remove(pid);
        m_pendingRawKeys.remove(pid);
        trackGame(pid, gameName, rawKey);
      } else {
        qDebug() << "Low-confidence reporter, watching:" << gameName
                 << "(PID:" << pid << ")";
        m_pendingCandidates[pid] = gameName;
        m_pendingRawKeys[pid] = rawKey;
      }
    } else if (command == "STOP") {
      if (!m_activeConnections.contains(socket) ||
          m_activeConnections.value(socket) != pid)
        continue;
      m_activeConnections.remove(socket);

      // Only remove the PID if this was the last connection for it
      bool pidStillActive = false;
      for (qint64 activePid : m_activeConnections.values()) {
        if (activePid == pid) {
          pidStillActive = true;
          break;
        }
      }

      if (!pidStillActive) {
        m_pidToGameName.remove(pid);
        m_pidToRawKey.remove(pid);
        m_pendingCandidates.remove(pid);
        m_pendingRawKeys.remove(pid);
        if (m_currentGamePid == pid) {
          if (!m_pidToGameName.isEmpty())
            promoteNextTrackedGame();
          else
            updateCurrentGame("", 0);
        }
      }
    }
  }
}

void GameDetectionService::onSocketDisconnected() {
  QLocalSocket *socket = qobject_cast<QLocalSocket *>(sender());
  if (!socket)
    return;

  if (m_activeConnections.contains(socket)) {
    qint64 pid = m_activeConnections.value(socket);
    m_activeConnections.remove(socket);

    // Only remove the PID if this was the last connection for it
    bool pidStillActive = false;
    for (qint64 activePid : m_activeConnections.values()) {
      if (activePid == pid) {
        pidStillActive = true;
        break;
      }
    }

    if (!pidStillActive) {
      m_pidToGameName.remove(pid);
      m_pidToRawKey.remove(pid);
      m_pendingCandidates.remove(pid);
      m_pendingRawKeys.remove(pid);
      if (m_currentGamePid == pid) {
        if (!m_pidToGameName.isEmpty())
          promoteNextTrackedGame();
        else
          updateCurrentGame("", 0);
      }
    }
  }
  socket->deleteLater();
}

bool GameDetectionService::processExists(qint64 pid) {
  if (pid <= 0)
    return false;
  if (::kill(static_cast<pid_t>(pid), 0) == 0)
    return true;
  // EPERM: process exists but is not signalable. Anything else
  // (ESRCH) means it is gone.
  return errno == EPERM;
}

qint64 GameDetectionService::lowestLiveTrackedPid() const {
  // Stable tie-break by PID, but prefer an actual focused window, then a
  // successor belonging to the same game, then another visible game.
  qint64 bestPid = 0;
  int bestScore = -1;
  for (auto it = m_pidToGameName.constBegin();
       it != m_pidToGameName.constEnd(); ++it) {
    if (!processExists(it.key()) ||
        isBlacklistedIdentity(m_pidToRawKey.value(it.key()), it.value()))
      continue;
    const WindowInfo w = m_monitor ? m_monitor->windowForPid(it.key()) : WindowInfo{};
    const bool visible = w.pid > 0 && !w.minimized;
    int score = visible ? 2 : 0;
    if (!m_currentGameKey.isEmpty() && m_pidToRawKey.value(it.key()) == m_currentGameKey)
      score += 4;
    if (visible && w.focused)
      score += 8;
    if (score > bestScore) {
      bestPid = it.key();
      bestScore = score;
    }
  }
  return bestPid;
}

void GameDetectionService::promoteNextTrackedGame() {
  const qint64 nextPid = lowestLiveTrackedPid();
  if (nextPid > 0) {
    updateCurrentGame(m_pidToGameName.value(nextPid), nextPid,
                      m_pidToRawKey.value(nextPid));
    return;
  }
  // No tracked process is still alive: drop the proven-dead entries so
  // a later START for the same name detects fresh, then go idle.
  for (const qint64 pid : m_pidToGameName.keys()) {
    if (!processExists(pid) ||
        isBlacklistedIdentity(m_pidToRawKey.value(pid), m_pidToGameName.value(pid))) {
      m_pidToGameName.remove(pid);
      m_pidToRawKey.remove(pid);
      m_pendingCandidates.remove(pid);
      m_pendingRawKeys.remove(pid);
    }
  }
  if (m_pidToGameName.isEmpty()) {
    updateCurrentGame("", 0);
  } else {
    // A PID appeared between probe and prune (racy exec): promote lowest.
    const qint64 retryPid = m_pidToGameName.firstKey();
    updateCurrentGame(m_pidToGameName.value(retryPid), retryPid,
                      m_pidToRawKey.value(retryPid));
  }
}

void GameDetectionService::onLivenessTimeout() {
  if (m_currentGamePid <= 0 || m_currentGameName.isEmpty())
    return;
  if (processExists(m_currentGamePid))
    return;
  qInfo() << "Tracked game process" << m_currentGamePid << "is gone ("
          << m_currentGameName << ")- closing";
  const qint64 deadPid = m_currentGamePid;
  m_pidToGameName.remove(deadPid);
  m_pidToRawKey.remove(deadPid);
  m_pendingCandidates.remove(deadPid);
  m_pendingRawKeys.remove(deadPid);
  if (!m_pidToGameName.isEmpty())
    promoteNextTrackedGame();
  else
    updateCurrentGame("", 0);
}

QString GameDetectionService::getSteamAppName(uint appId) {
  if (appId == 0)
    return QString();

  QString home = QDir::homePath();
  QString mainSteamPath = "";

  // Common main steam locations
  QStringList mainPaths = {
      home + "/.steam/steam", home + "/.local/share/Steam",
      home + "/.var/app/com.valvesoftware.Steam/.steam/steam" // Flatpak
  };

  for (const QString &p : mainPaths) {
    if (QFile::exists(p + "/steamapps/libraryfolders.vdf")) {
      mainSteamPath = p;
      break;
    }
  }

  if (mainSteamPath.isEmpty())
    return QString();

  // Hot path (per row in rebuildModels, per START with a Steam id, per
  // reconcile window): stat-only fast path once the library is known.
  const QString cacheKey =
      mainSteamPath + QLatin1Char('\n') +
      QFileInfo(mainSteamPath + "/steamapps/libraryfolders.vdf")
          .lastModified()
          .toString(Qt::ISODateWithMs);
  if (cacheKey != m_steamLibCacheKey) {
    // Steam libraries changed (install/move): drop stale names.
    m_steamLibCacheKey = cacheKey;
    m_steamNameCache.clear();
  } else {
    const auto hit = m_steamNameCache.constFind(appId);
    if (hit != m_steamNameCache.constEnd()) {
      const QFileInfo manifest(hit->manifestPath);
      if (manifest.exists() && manifest.lastModified() == hit->modified)
        return hit->name;
      m_steamNameCache.remove(appId);
    }
  }

  QStringList libraryPaths;
  libraryPaths << mainSteamPath;

  // Parse libraryfolders.vdf to find other drives
  QFile libFile(mainSteamPath + "/steamapps/libraryfolders.vdf");
  if (libFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QString content = QString::fromUtf8(libFile.readAll());
    QRegularExpression pathRe("\"path\"\\s+\"([^\"]+)\"");
    QRegularExpressionMatchIterator it = pathRe.globalMatch(content);
    while (it.hasNext()) {
      QRegularExpressionMatch match = it.next();
      QString path = match.captured(1);
      if (!libraryPaths.contains(path)) {
        libraryPaths << path;
      }
    }
  }

  // Scan all found libraries for the app manifest
  for (const QString &libPath : libraryPaths) {
    QString acfPath =
        libPath + QString("/steamapps/appmanifest_%1.acf").arg(appId);
    if (QFile::exists(acfPath)) {
      QFile file(acfPath);
      if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString content = QString::fromUtf8(file.readAll());
        QRegularExpression nameRe("\"name\"\\s+\"([^\"]+)\"");
        QRegularExpressionMatch match = nameRe.match(content);
        if (match.hasMatch()) {
          const QString steamName = match.captured(1);
          // Positive hits only: a missing manifest may appear later
          // without the vdf mtime changing (old-format vdfs).
          m_steamNameCache.insert(
              appId, {steamName, acfPath, QFileInfo(acfPath).lastModified()});
          return steamName;
        }
      }
    }
  }
  return QString();
}

bool GameDetectionService::hasVisibleWindow() const {
  if (!m_monitor || m_currentGameName.isEmpty())
    return false;
  for (const WindowInfo &w : m_monitor->windows()) {
    if (w.pid <= 0 || w.minimized)
      continue;
    const auto it = m_pidToGameName.find(w.pid);
    if (it != m_pidToGameName.end() && it.value() == m_currentGameName)
      return true;
  }
  return false;
}

void GameDetectionService::updateCurrentGame(const QString &gameName,
                                             qint64 pid,
                                             const QString &rawKey) {
  const QString nextKey = gameName.isEmpty() ? QString() :
                         (rawKey.isEmpty() ? gameName : rawKey);
  if (gameName == m_currentGameName && pid == m_currentGamePid && nextKey == m_currentGameKey)
    return;

  QString oldGame = m_currentGameName;
  const QString oldKey = m_currentGameKey;
  m_currentGameName = gameName;
  m_currentGamePid = pid;
  m_currentGameKey =
      gameName.isEmpty() ? QString() : (rawKey.isEmpty() ? gameName : rawKey);

  // Arm the liveness watchdog while a game is tracked; disarm on clear.
  if (!gameName.isEmpty() && pid > 0) {
    if (!m_livenessTimer->isActive())
      m_livenessTimer->start();
  } else {
    m_livenessTimer->stop();
  }

  emit currentGameChanged();

  const bool changedIdentity = oldGame != gameName || oldKey != nextKey;
  if (!oldGame.isEmpty() && changedIdentity)
    emit gameClosed(oldGame);
  if (!gameName.isEmpty() && changedIdentity) {
    addToRecentGames(gameName, rawKey.isEmpty() ? gameName : rawKey);
    emit gameDetected(gameName, pid);
  }
}

void GameDetectionService::addToRecentGames(const QString &gameName,
                                            const QString &rawKey) {
  // Canonical match key: "Foo.EXE" and "foo" are one entry everywhere
  // (matching itself normalizes too, so either form would hit).
  const QString key =
      GameWhitelist::normalizeKey(rawKey.isEmpty() ? gameName : rawKey);
  const int dup = m_recentGames.indexOf(gameName);
  if (dup >= 0) {
    m_recentGames.removeAt(dup);
    m_recentKeys.removeAt(dup);
  }
  m_recentGames.prepend(gameName);
  m_recentKeys.prepend(key);
  while (m_recentGames.size() > 10) {
    m_recentGames.removeLast();
    m_recentKeys.removeLast();
  }
  emit recentGamesChanged();
}

bool GameDetectionService::addToWhitelist(const QString &wc,
                                          const QString &dn) {
  const QString key = wc.trimmed();
  if (key.isEmpty() || GameWhitelist::isPlaceholderReporterKey(key))
    return false;
  const bool changed = !m_whitelist->isWhitelisted(key) ||
                       m_whitelist->getWhitelistName(key) != dn.trimmed();
  m_whitelist->addToWhitelist(key, dn.trimmed());
  m_whitelist->save();
  emit whitelistChanged(); // rebuilds models either way
  return changed;
}
bool GameDetectionService::addToBlacklist(const QString &wc) {
  const QString key = wc.trimmed();
  if (key.isEmpty())
    return false;
  const bool changed = !m_whitelist->isBlacklisted(key);
  m_whitelist->addToBlacklist(key);
  retireBlockedGame(GameWhitelist::normalizeKey(key));
  m_whitelist->save();
  emit blacklistChanged(); // rebuilds models either way
  return changed;
}
bool GameDetectionService::removeFromWhitelist(const QString &wc) {
  const QString key = wc.trimmed();
  if (!m_whitelist->isWhitelisted(key))
    return false;
  m_whitelist->removeFromWhitelist(key);
  m_whitelist->save();
  emit whitelistChanged();
  return true;
}
bool GameDetectionService::removeFromBlacklist(const QString &wc) {
  const QString key = wc.trimmed();
  if (!m_whitelist->isBlacklisted(key))
    return false;
  m_whitelist->removeFromBlacklist(key);
  m_whitelist->save();
  emit blacklistChanged();
  return true;
}
bool GameDetectionService::moveToWhitelist(const QString &key,
                                           const QString &display) {
  const QString k = GameWhitelist::normalizeKey(key);
  if (k.isEmpty() || GameWhitelist::isPlaceholderReporterKey(k))
    return false;
  const bool changed = !m_whitelist->isWhitelisted(k);
  m_whitelist->addToWhitelist(k, display.trimmed());
  m_whitelist->save();
  emit whitelistChanged();
  return changed;
}

bool GameDetectionService::moveToBlacklist(const QString &key) {
  const QString k = GameWhitelist::normalizeKey(key);
  if (k.isEmpty())
    return false;
  const bool changed = !m_whitelist->isBlacklisted(k);
  m_whitelist->addToBlacklist(k);
  retireBlockedGame(k);
  m_whitelist->save();
  emit blacklistChanged();
  return changed;
}

bool GameDetectionService::unblockGame(const QString &key) {
  const QString k = GameWhitelist::normalizeKey(key);
  if (k.isEmpty())
    return false;
  const bool changed = m_whitelist->isBlacklisted(k);
  // removeFromBlacklist records the marker for built-ins so the seed
  // doesn't resurrect them; plain members just drop out.
  m_whitelist->removeFromBlacklist(k);
  if (!changed)
    return false;
  m_whitelist->save();
  emit blacklistChanged();
  return true;
}

bool GameDetectionService::isBlocked(const QString &keyOrName) const {
  return m_whitelist && m_whitelist->isBlacklisted(keyOrName);
}

void GameDetectionService::retireBlockedGame(const QString &canonicalKey) {
  if (canonicalKey.isEmpty())
    return;
  const QString vetoKey =
      defaultExclusionKey(GameWhitelist::normalizeKey(canonicalKey));
  const auto matchesVeto = [&vetoKey](const QString &identity) {
    if (defaultExclusionKey(GameWhitelist::normalizeKey(identity)) == vetoKey)
      return true;
    // Native reporters retain full executable identities. Apply the same
    // known executable-name veto used at START without changing that identity
    // or looking up any process. Arbitrary custom path vetoes stay exact.
    return defaultAppInfo(vetoKey) && identity.startsWith("exe:") &&
           defaultExclusionKey(GameWhitelist::normalizeKey(
               QFileInfo(identity.mid(4)).fileName())) == vetoKey;
  };
  // Retire every process of the identity, not just the representative
  // PID: otherwise a helper/second renderer can resurrect a blocked game
  // through focus selection or failover.
  for (qint64 pid : m_pidToGameName.keys()) {
    if (matchesVeto(m_pidToRawKey.value(pid)) ||
        matchesVeto(m_pidToGameName.value(pid))) {
      m_pidToGameName.remove(pid);
      m_pidToRawKey.remove(pid);
    }
  }
  for (qint64 pid : m_pendingCandidates.keys()) {
    if (matchesVeto(m_pendingRawKeys.value(pid)) ||
        matchesVeto(m_pendingCandidates.value(pid))) {
      m_pendingCandidates.remove(pid);
      m_pendingRawKeys.remove(pid);
    }
  }
  if (!matchesVeto(m_currentGameKey) && !matchesVeto(m_currentGameName))
    return;
  qInfo() << "Blocked live game" << m_currentGameName << "- retiring tracking";
  m_pidToGameName.remove(m_currentGamePid);
  m_pidToRawKey.remove(m_currentGamePid);
  m_pendingCandidates.remove(m_currentGamePid);
  m_pendingRawKeys.remove(m_currentGamePid);
  // Settles everything downstream: watchdog disarms, currentGameChanged
  // clears the dashboard, gameClosed stops any running capture cleanly.
  // History keeps the row, which now reads Blocked in the Games pool.
  updateCurrentGame("", 0);
}

bool GameDetectionService::addCustomGame(const QString &key,
                                         const QString &display, bool block) {
  const QString k = GameWhitelist::normalizeKey(key);
  if (k.isEmpty() || (!block && GameWhitelist::isPlaceholderReporterKey(k)))
    return false;
  m_whitelist->noteCustomKey(k);
  bool changed;
  if (block) {
    changed = !m_whitelist->isBlacklisted(k);
    m_whitelist->addToBlacklist(k);
    retireBlockedGame(k);
  } else {
    const QString dn = display.trimmed();
    changed = !m_whitelist->isWhitelisted(k) ||
              m_whitelist->getWhitelistName(k) != dn;
    m_whitelist->addToWhitelist(k, dn);
  }
  m_whitelist->save();
  emit whitelistChanged();
  emit blacklistChanged();
  return changed;
}

bool GameDetectionService::forgetGame(const QString &key) {
  const QString k = GameWhitelist::normalizeKey(key);
  if (k.isEmpty())
    return false;
  bool changed = m_whitelist->isWhitelisted(k) ||
                 m_whitelist->isBlacklisted(k) ||
                 m_whitelist->customKeys().contains(k);
  m_whitelist->removeFromWhitelist(k);
  m_whitelist->removeFromBlacklist(k);
  m_whitelist->forgetCustomKey(k);
  for (int i = m_recentGames.size() - 1; i >= 0; --i) {
    const QString raw =
        (i < m_recentKeys.size() && !m_recentKeys.at(i).isEmpty())
            ? m_recentKeys.at(i)
            : m_recentGames.at(i);
    if (GameWhitelist::normalizeKey(raw) == k ||
        GameWhitelist::normalizeKey(m_recentGames.at(i)) == k) {
      changed = true;
      m_recentGames.removeAt(i);
      if (i < m_recentKeys.size())
        m_recentKeys.removeAt(i);
    }
  }
  if (!changed)
    return false;
  m_whitelist->save();
  emit recentGamesChanged();
  emit whitelistChanged();
  emit blacklistChanged();
  return true;
}

bool GameDetectionService::addSteamGame(uint appId, const QString &dn) {
  if (appId == 0)
    return false;
  const QString key = QStringLiteral("steam_app_%1").arg(appId);
  m_whitelist->noteCustomKey(key);
  return addToWhitelist(key, dn.trimmed());
}
QStringList GameDetectionService::whitelist() const {
  return m_whitelist->whitelistEntries();
}
QStringList GameDetectionService::blacklist() const {
  return m_whitelist->blacklistEntries();
}

QString GameDetectionService::gameFilterText() const {
  return m_gamesFilter ? m_gamesFilter->filterText() : QString();
}

void GameDetectionService::setGameFilterText(const QString &text) {
  if (!m_gamesFilter)
    return;
  m_gamesFilter->setFilterText(text);
  emit gameFilterTextChanged();
}

bool GameDetectionService::showDefaultExclusions() const {
  return m_gamesFilter && m_gamesFilter->showDefaultEntries();
}

void GameDetectionService::setShowDefaultExclusions(bool show) {
  if (!m_gamesFilter)
    return;
  m_gamesFilter->setShowDefaultEntries(show);
  emit showDefaultExclusionsChanged();
}

void GameDetectionService::setTokenGames(const QStringList &games) {
  const QSet<QString> next(games.begin(), games.end());
  if (next == m_tokenGames)
    return;
  m_tokenGames = next;
  emit tokenGamesChanged();
}

void GameDetectionService::rebuildModels() {
  if (!m_gamesModel)
    return;
  // Sections: the live game pins first ("Now playing"); token holders
  // ("Ready to capture") prove silent capture — the only approved state;
  // everything else shares one pool ("Apps") carrying per-row
  // Prompt/Block state. Within a section rows sort alphabetically (the
  // live game excepted) so taps never teleport a row.
  QList<GameListEntry> rows;
  QSet<QString> claimed;
  QSet<QString> claimedTokens;
  const auto claim = [&](const QString &key, const QString &) {
    if (key.isEmpty() || claimed.contains(key))
      return false;
    claimed.insert(key);
    return true;
  };
  const auto listedFlag = [this](const QString &key, const QString &display) {
    // Block veto wins visually too: a watched game with a veto reads
    // Blocked, otherwise the switch looks stuck on Prompt.
    if (m_whitelist->isBlacklisted(key) || m_whitelist->isBlacklisted(display))
      return 2;
    if (m_whitelist->isWhitelisted(key) || m_whitelist->isWhitelisted(display))
      return 1;
    return 0;
  };
  const auto tokenFor = [this](const QString &key, const QString &display) {
    if (m_tokenGames.contains(key))
      return key;
    if (!display.isEmpty() && m_tokenGames.contains(display))
      return display;
    return QString();
  };
  const auto byDisplay = [](const GameListEntry &a, const GameListEntry &b) {
    return a.display.compare(b.display, Qt::CaseInsensitive) < 0;
  };

  if (!m_currentGameName.isEmpty()) {
    GameListEntry live;
    live.key =
        m_currentGameKey.isEmpty() ? m_currentGameName : m_currentGameKey;
    live.display = m_currentGameName;
    live.pid = m_currentGamePid;
    live.running = true;
    live.steamAppId = steamAppIdFromAppId(live.key);
    live.listed = listedFlag(live.key, live.display);
    live.tokenKey = tokenFor(live.key, live.display);
    live.hasToken = !live.tokenKey.isEmpty();
    if (live.hasToken)
      claimedTokens.insert(live.tokenKey);
    live.section = QStringLiteral("Now playing");
    rows.push_back(live);
    claim(live.key, live.display);
  }

  // Approved: every known identity holding a token (plus bare token-map
  // names with no other trace this session), alphabetically.
  QList<GameListEntry> approved;
  const auto considerApproved = [&](const QString &key,
                                    const QString &display) {
    const QString tk = tokenFor(key, display);
    if (tk.isEmpty() || claimedTokens.contains(tk))
      return;
    QString name = display.isEmpty() ? key : display;
    if (!claim(key, name))
      return;
    claimedTokens.insert(tk);
    GameListEntry e;
    e.key = key;
    e.display = name;
    e.steamAppId = steamAppIdFromAppId(key);
    if (e.display == e.key && e.steamAppId > 0) {
      const QString steamName = getSteamAppName(e.steamAppId);
      if (!steamName.isEmpty())
        e.display = steamName;
    }
    e.listed = listedFlag(key, name);
    e.hasToken = true;
    e.tokenKey = tk;
    e.section = QStringLiteral("Ready to capture");
    approved.push_back(e);
  };
  for (int i = 0; i < m_recentGames.size(); ++i) {
    const QString raw =
        (i < m_recentKeys.size() && !m_recentKeys.at(i).isEmpty())
            ? m_recentKeys.at(i)
            : m_recentGames.at(i);
    considerApproved(raw, m_recentGames.at(i));
  }
  for (const QString &key : m_whitelist->whitelistEntries()) {
    QString display = m_whitelist->getWhitelistName(key);
    if (display.isEmpty())
      display = key;
    considerApproved(key, display);
  }
  for (const QString &tokenName : m_tokenGames)
    considerApproved(tokenName, tokenName);
  std::sort(approved.begin(), approved.end(), byDisplay);
  rows.append(approved);

  // Pool ("Apps"): every non-live, non-approved identity in one
  // alphabetical run, so taps never teleport a row.
  QList<GameListEntry> pool;
  for (const QString &key : m_whitelist->whitelistEntries()) {
    QString display = m_whitelist->getWhitelistName(key);
    const uint sid = steamAppIdFromAppId(key);
    if (display.isEmpty() || display == key) {
      if (sid > 0) {
        const QString steamName = getSteamAppName(sid);
        if (!steamName.isEmpty())
          display = steamName;
      }
      if (display.isEmpty())
        display = key;
    }
    if (!claim(key, display))
      continue;
    GameListEntry e;
    e.key = key;
    e.display = display;
    e.steamAppId = sid;
    e.listed = listedFlag(key, display);
    e.section = QStringLiteral("Apps");
    pool.push_back(e);
  }

  for (int i = 0; i < m_recentGames.size(); ++i) {
    const QString &name = m_recentGames.at(i);
    const QString raw =
        (i < m_recentKeys.size() && !m_recentKeys.at(i).isEmpty())
            ? m_recentKeys.at(i)
            : name;
    if (!claim(raw, name))
      continue;
    GameListEntry e;
    e.display = name;
    e.key = raw;
    e.pid = 0;
    e.running = false;
    e.steamAppId = steamAppIdFromAppId(e.key);
    e.listed = listedFlag(e.key, e.display);
    e.section = QStringLiteral("Apps");
    pool.push_back(e);
  }

  const QStringList defaults = GameWhitelist::defaultBlacklistEntries();
  const QSet<QString> systemKeys(defaults.begin(), defaults.end());
  const auto pushBlocked = [&](GameListEntry e) {
    if (!claim(e.key, e.display))
      return;
    pool.push_back(e);
  };
  for (const QString &key : m_whitelist->blacklistEntries()) {
    GameListEntry e;
    e.key = key;
    e.listed = 2;
    e.isDefault = systemKeys.contains(key);
    e.display = e.isDefault ? GameWhitelist::prettySystemName(key) : key;
    e.section = QStringLiteral("Apps");
    pushBlocked(e);
  }
  for (const QString &key : m_whitelist->removedDefaults()) {
    GameListEntry e;
    e.key = key;
    e.display = GameWhitelist::prettySystemName(key);
    e.listed = 0;
    e.isDefault = true;
    e.section = QStringLiteral("Apps");
    pushBlocked(e);
  }
  for (const QString &key : m_whitelist->customKeys()) {
    GameListEntry e;
    e.key = key;
    e.display = m_whitelist->getWhitelistName(key);
    if (e.display.isEmpty())
      e.display = key;
    e.steamAppId = steamAppIdFromAppId(key);
    e.listed = listedFlag(key, e.display);
    e.section = QStringLiteral("Apps");
    if (!claim(e.key, e.display))
      continue;
    pool.push_back(e);
  }
  std::sort(pool.begin(), pool.end(), byDisplay);
  rows.append(pool);
  m_gamesModel->setEntries(rows);
}

} // namespace TrophyBackend
