#include "GameDetectionService.h"
#include "GameWhitelist.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QStandardPaths>

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
  qInfo() << "Starting game detection service (Vulkan Layer mode)";
  
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

        bool isBetterName = true;
        if (m_pidToGameName.contains(pid)) {
          QString existingName = m_pidToGameName[pid];
          // Don't overwrite a specific name with a generic one
          if (gameName == "Unknown Vulkan Game" && existingName != "Unknown Vulkan Game") {
            isBetterName = false;
          }
        }

        if (isBetterName) {

          bool isNewName = true;
          for (const QString &name : m_pidToGameName.values()) {
              if (name == gameName && m_pidToGameName.key(name) != pid) {
                  isNewName = false;
                  break;
              }
          }

          m_pidToGameName[pid] = gameName;
          
          if (isNewName) {
            qDebug() << "Vulkan game started:" << gameName << "(PID:" << pid << ")";
            updateCurrentGame(gameName, pid);
          } else {
            qDebug() << "Additional process for existing game detected:" << gameName << "(PID:" << pid << ")";
          }
        } else {
          qDebug() << "Ignoring redundant/generic start message for PID" << pid << ":" << gameName;
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

uint GameDetectionService::getSteamAppId(qint64 pid) {
  Q_UNUSED(pid)
  return 0; // No longer used, but kept for interface compatibility if needed
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

QString GameDetectionService::formatDisplayName(const QString &rawName) {
  return rawName;
}

void GameDetectionService::updateCurrentGame(const QString &gameName,
                                             qint64 pid) {
  if (gameName == m_currentGameName && pid == m_currentGamePid)
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