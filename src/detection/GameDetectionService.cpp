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

#include <algorithm>

namespace TrophyBackend {

GameDetectionService::GameDetectionService(QObject *parent)
    : QObject(parent), m_whitelist(std::make_unique<GameWhitelist>()) {
  m_server = new QLocalServer(this);
  connect(m_server, &QLocalServer::newConnection, this,
          &GameDetectionService::onNewConnection);

  m_whitelist->load();
}

GameDetectionService::~GameDetectionService() { stop(); }

void GameDetectionService::start() {
  if (m_running)
    return;
  qDebug() << "Starting game detection service (socket listener; layers/shims report here when installed)";
  
  // Remove existing socket file if it exists
  QLocalServer::removeServer("/tmp/trophy-detection.sock");
  
  if (!m_server->listen("/tmp/trophy-detection.sock")) {
    qCritical() << "Failed to start detection server:" << m_server->errorString();
    return;
  }
  
  m_running = true;
}

void GameDetectionService::stop() {
  if (!m_running)
    return;
  qInfo() << "Stopping game detection service";
  m_server->close();
  m_running = false;
  
  for (QLocalSocket *socket : m_activeConnections.keys()) {
    socket->disconnect();
    socket->close();
    socket->deleteLater();
  }
  m_activeConnections.clear();
  m_pidToGameName.clear();
  m_pendingCandidates.clear();
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
  ssize_t n = ::readlink(exeLink.toLocal8Bit().constData(), exe, sizeof(exe) - 1);
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
  const double coverage = (double)w.width * w.height /
                          ((double)w.outputWidth * w.outputHeight);
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

void GameDetectionService::trackGame(qint64 pid, const QString &gameName) {
  // Show the user's display name when one is registered; fall back to
  // the raw reporter class either way so dedup stays consistent.
  QString displayName = gameName;
  if (m_whitelist->isWhitelisted(gameName)) {
    const QString mapped = m_whitelist->getWhitelistName(gameName);
    if (!mapped.isEmpty())
      displayName = mapped;
  }

  bool isBetterName = true;
  if (m_pidToGameName.contains(pid)) {
    QString existingName = m_pidToGameName[pid];
    // Don't overwrite a specific name with a generic one
    if (displayName == "Unknown Vulkan Game" && existingName != "Unknown Vulkan Game") {
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

    if (isNewName) {
      qDebug() << "Game detected:" << displayName << "(PID:" << pid << ")";
      updateCurrentGame(displayName, pid);
    } else {
      qDebug() << "Additional process for existing game detected:" << displayName << "(PID:" << pid << ")";
    }
  } else {
    qDebug() << "Ignoring redundant/generic start message for PID" << pid << ":" << displayName;
  }
}

void GameDetectionService::recheckCandidates() {
  if (m_pendingCandidates.isEmpty())
    return;
  const QList<qint64> pids = m_pendingCandidates.keys();
  for (qint64 pid : pids) {
    const QString name = m_pendingCandidates.value(pid);
    if (m_whitelist->isBlacklisted(name)) {
      m_pendingCandidates.remove(pid);
      continue;
    }
    if (isConfidentGame(pid, name, 0)) {
      m_pendingCandidates.remove(pid);
      qDebug() << "Pending game confirmed by window state:" << name << "(PID:" << pid << ")";
      trackGame(pid, name);
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
  std::sort(windows.begin(), windows.end(), [](const WindowInfo &a,
                                               const WindowInfo &b) {
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
    if (m_whitelist->isBlacklisted(name))
      continue;
    if (m_pidToGameName.key(name, 0) != 0)
      continue; // already tracked
    // Same bar as live detection: whitelist or Steam evidence only.
    if (m_whitelist->isWhitelisted(name) || processLooksLikeSteamGame(w.pid)) {
      // Resolve a display name the same way live reports do: Steam
      // manifest via environ id or the steam_app_<id> window class.
      QString display = name;
      uint sid = steamAppIdForPid(w.pid);
      if (sid == 0)
        sid = steamAppIdFromAppId(w.appId.isEmpty() ? w.title : w.appId);
      if (sid > 0) {
        const QString steamName = getSteamAppName(sid);
        if (!steamName.isEmpty())
          display = steamName;
      }
      qDebug() << "Reconciled already-running game:" << display
               << "(PID:" << w.pid << ")";
      trackGame(w.pid, display);
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
    QStringList parts = line.split(':');

    if (parts.size() >= 2) {
      QString command = parts[0];
      qint64 pid = parts[1].toLongLong();

      if (command == "START" && parts.size() >= 3) {
        QString gameName = parts[2];
        uint steamAppId = (parts.size() >= 4) ? parts[3].toUInt() : 0;

        if (steamAppId > 0) {
          QString steamName = getSteamAppName(steamAppId);
          if (!steamName.isEmpty()) {
            gameName = steamName;
          }
        }

        if (m_whitelist->isBlacklisted(gameName)) {
          qDebug() << "Ignoring blacklisted process:" << gameName << "(PID:" << pid << ")";
          return;
        }

        m_activeConnections[socket] = pid;

        // Confidence fusion: instant detect on whitelist / Steam markers,
        // or focused + fullscreen via the window monitor. Anything else
        // waits as a candidate until window state confirms it.
        if (isConfidentGame(pid, gameName, steamAppId)) {
          trackGame(pid, gameName);
        } else {
          qDebug() << "Low-confidence reporter, watching:" << gameName << "(PID:" << pid << ")";
          m_pendingCandidates[pid] = gameName;
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
          m_pendingCandidates.remove(pid);
          if (m_currentGamePid == pid) {
            if (!m_pidToGameName.isEmpty()) {
              qint64 nextPid = m_pidToGameName.keys().last();
              updateCurrentGame(m_pidToGameName[nextPid], nextPid);
            } else {
              updateCurrentGame("", 0);
            }
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
      m_pendingCandidates.remove(pid);
      if (m_currentGamePid == pid) {
        if (!m_pidToGameName.isEmpty()) {
          qint64 nextPid = m_pidToGameName.keys().last();
          updateCurrentGame(m_pidToGameName[nextPid], nextPid);
        } else {
          updateCurrentGame("", 0);
        }
      }
    }
  }
  socket->deleteLater();
}

QString GameDetectionService::getSteamAppName(uint appId) {
  if (appId == 0)
    return QString();

  QString home = QDir::homePath();
  QString mainSteamPath = "";
  
  // Common main steam locations
  QStringList mainPaths = {
      home + "/.steam/steam",
      home + "/.local/share/Steam",
      home + "/.var/app/com.valvesoftware.Steam/.steam/steam" // Flatpak
  };

  for (const QString &p : mainPaths) {
      if (QFile::exists(p + "/steamapps/libraryfolders.vdf")) {
          mainSteamPath = p;
          break;
      }
  }

  if (mainSteamPath.isEmpty()) return QString();

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
    QString acfPath = libPath + QString("/steamapps/appmanifest_%1.acf").arg(appId);
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
                                             qint64 pid) {  if (gameName == m_currentGameName && pid == m_currentGamePid)
    return;

  QString oldGame = m_currentGameName;
  m_currentGameName = gameName;
  m_currentGamePid = pid;

  emit currentGameChanged();

  if (gameName.isEmpty()) {
    if (!oldGame.isEmpty()) {
      emit gameClosed(oldGame);
    }
  } else {
    if (oldGame != gameName) {
      addToRecentGames(gameName);
      emit gameDetected(gameName, pid);
    }
  }
}

void GameDetectionService::addToRecentGames(const QString &gameName) {
  m_recentGames.removeAll(gameName);
  m_recentGames.prepend(gameName);
  while (m_recentGames.size() > 10)
    m_recentGames.removeLast();
  emit recentGamesChanged();
}

void GameDetectionService::addToWhitelist(const QString &wc,
                                          const QString &dn) {
  m_whitelist->addToWhitelist(wc, dn);
  m_whitelist->save();
}
void GameDetectionService::addToBlacklist(const QString &wc) {
  m_whitelist->addToBlacklist(wc);
  m_whitelist->save();
}
void GameDetectionService::removeFromWhitelist(const QString &wc) {
  m_whitelist->removeFromWhitelist(wc);
  m_whitelist->save();
}
void GameDetectionService::removeFromBlacklist(const QString &wc) {
  m_whitelist->removeFromBlacklist(wc);
  m_whitelist->save();
}
QStringList GameDetectionService::whitelist() const {
  return m_whitelist->whitelistEntries();
}
QStringList GameDetectionService::blacklist() const {
  return m_whitelist->blacklistEntries();
}

} // namespace TrophyBackend