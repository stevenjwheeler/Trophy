#include "GameDetectionService.h"
#include "GameWhitelist.h"
#include "WaylandWindowMonitor.h"
#include "WindowInfo.h"

#include <QDebug>
#include <QDir>
#include <QFile>
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
  if (steamAppIdForPid(pid) > 0)
    return true;
  // Targeted single-PID lookup per shim event (not a /proc scan).
  char exe[512];
  const QString exeLink = QString("/proc/%1/exe").arg(pid);
  ssize_t n =
      ::readlink(exeLink.toLocal8Bit().constData(), exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = '\0';
    if (strstr(exe, "steamapps"))
      return true;
  }
  QFile cmd(QString("/proc/%1/cmdline").arg(pid));
  if (cmd.open(QIODevice::ReadOnly)) {
    const QByteArray raw = cmd.readAll();
    if (raw.contains("steamapps"))
      return true;
  }
  return false;
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
    if (w.focused && (w.fullscreen || coversOutput(w)))
      return true;
  }
  return false;
}

bool GameDetectionService::isBlacklistedIdentity(
    const QString &rawKey, const QString &displayName) const {
  return (!rawKey.isEmpty() && m_whitelist->isBlacklisted(rawKey)) ||
         (!displayName.isEmpty() && m_whitelist->isBlacklisted(displayName));
}

void GameDetectionService::trackGame(qint64 pid, const QString &gameName,
                                     const QString &rawKey) {
  // Show the user's display name when one is registered; fall back to
  // the raw reporter class either way so dedup stays consistent.
  QString displayName = gameName;
  if (m_whitelist->isWhitelisted(gameName)) {
    const QString mapped = m_whitelist->getWhitelistName(gameName);
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
    for (const QString &name : m_pidToGameName.values()) {
      if (name == displayName && m_pidToGameName.key(name) != pid) {
        isNewName = false;
        break;
      }
    }

    m_pidToGameName[pid] = displayName;
    m_pidToRawKey[pid] = key;

    if (isNewName) {
      qDebug() << "Game detected:" << displayName << "(PID:" << pid << ")";
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
    if (isConfidentGame(pid, name, 0)) {
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
    if (!w.focused && !w.fullscreen)
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
    if (sid > 0) {
      const QString steamName = getSteamAppName(sid);
      if (!steamName.isEmpty())
        display = steamName;
    }
    if (isBlacklistedIdentity(rawKey, display))
      continue;
    if (m_pidToGameName.key(name, 0) != 0)
      continue; // already tracked
    // Same bar as live detection: whitelist or Steam evidence only.
    if (m_whitelist->isWhitelisted(name) || processLooksLikeSteamGame(w.pid)) {
      qDebug() << "Reconciled already-running game:" << display
               << "(PID:" << w.pid << ")";
      trackGame(w.pid, display, rawKey);
    } else {
      qDebug() << "Reconcile: focused/fullscreen window is not game evidence:"
               << name << "(PID:" << w.pid << ")";
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
    const qsizetype pidEnd =
        commandEnd >= 0 ? line.indexOf(':', commandEnd + 1) : -1;
    if (commandEnd < 0 || pidEnd < 0)
      continue;

    const QString command = line.left(commandEnd);
    bool pidOk = false;
    const qint64 pid =
        line.mid(commandEnd + 1, pidEnd - commandEnd - 1).toLongLong(&pidOk);
    if (!pidOk || pid <= 0)
      continue;

    if (command == "START") {
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

      if (steamAppId > 0) {
        QString steamName = getSteamAppName(steamAppId);
        if (!steamName.isEmpty()) {
          gameName = steamName;
        }
      }

      if (isBlacklistedIdentity(reporterName, gameName)) {
        qDebug() << "Ignoring blacklisted process:" << gameName
                 << "(PID:" << pid << ")";
        continue;
      }

      m_activeConnections[socket] = pid;

      // Confidence fusion: instant detect on whitelist / Steam markers,
      // or focused + fullscreen via the window monitor. Anything else
      // waits as a candidate until window state confirms it.
      if (isConfidentGame(pid, gameName, steamAppId)) {
        trackGame(pid, gameName, reporterName);
      } else {
        qDebug() << "Low-confidence reporter, watching:" << gameName
                 << "(PID:" << pid << ")";
        m_pendingCandidates[pid] = gameName;
        m_pendingRawKeys[pid] = reporterName;
      }
    } else if (command == "STOP") {
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
          if (!m_pidToGameName.isEmpty()) {
            qint64 nextPid = m_pidToGameName.keys().last();
            updateCurrentGame(m_pidToGameName[nextPid], nextPid,
                              m_pidToRawKey.value(nextPid));
          } else {
            updateCurrentGame("", 0);
          }
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
        if (!m_pidToGameName.isEmpty()) {
          qint64 nextPid = m_pidToGameName.keys().last();
          updateCurrentGame(m_pidToGameName[nextPid], nextPid,
                            m_pidToRawKey.value(nextPid));
        } else {
          updateCurrentGame("", 0);
        }
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
  if (!m_pidToGameName.isEmpty()) {
    const qint64 nextPid = m_pidToGameName.lastKey();
    updateCurrentGame(m_pidToGameName.value(nextPid), nextPid,
                      m_pidToRawKey.value(nextPid));
  } else {
    updateCurrentGame("", 0);
  }
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
          return match.captured(1);
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
    if (w.pid <= 0)
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
  if (gameName == m_currentGameName && pid == m_currentGamePid)
    return;

  QString oldGame = m_currentGameName;
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

  if (!oldGame.isEmpty() && oldGame != gameName)
    emit gameClosed(oldGame);
  if (!gameName.isEmpty() && oldGame != gameName) {
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
  if (key.isEmpty())
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
  if (k.isEmpty())
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
  if (canonicalKey.isEmpty() || m_currentGameName.isEmpty())
    return;
  if (GameWhitelist::normalizeKey(m_currentGameKey) != canonicalKey &&
      GameWhitelist::normalizeKey(m_currentGameName) != canonicalKey)
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
  if (k.isEmpty())
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
  // everything else shares one pool ("Games") carrying per-row
  // Prompt/Block state. Within a section rows sort alphabetically (the
  // live game excepted) so taps never teleport a row.
  QList<GameListEntry> rows;
  QSet<QString> claimed;
  const auto claim = [&](const QString &key, const QString &display) {
    if (key.isEmpty() || claimed.contains(key) ||
        (!display.isEmpty() && claimed.contains(display)))
      return false;
    claimed.insert(key);
    if (!display.isEmpty())
      claimed.insert(display);
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
    if (tk.isEmpty())
      return;
    QString name = display.isEmpty() ? key : display;
    if (!claim(key, name))
      return;
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

  // Pool ("Games"): every non-live, non-approved identity in one
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
    e.section = QStringLiteral("Games");
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
    e.section = QStringLiteral("Games");
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
    e.section = QStringLiteral("Games");
    pushBlocked(e);
  }
  for (const QString &key : m_whitelist->removedDefaults()) {
    GameListEntry e;
    e.key = key;
    e.display = GameWhitelist::prettySystemName(key);
    e.listed = 0;
    e.isDefault = true;
    e.section = QStringLiteral("Games");
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
    e.section = QStringLiteral("Games");
    if (!claim(e.key, e.display))
      continue;
    pool.push_back(e);
  }
  std::sort(pool.begin(), pool.end(), byDisplay);
  rows.append(pool);
  m_gamesModel->setEntries(rows);
}

} // namespace TrophyBackend
