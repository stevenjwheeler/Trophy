/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "GameDetectionService.h"
#include "GameWhitelist.h"
#include "ProcessHeuristics.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace GameRecorder {

// Filter for management processes, wrappers, and Windows-on-Linux shims
static const QSet<QString> IGNORED_PROCESS_NAMES = {
    "reaper",           "pressure-vessel-reaper",
    "steamwebhelper",   "steam.exe",
    "steamservice.exe", "explorer.exe",
    "services.exe",     "winedevice.exe",
    "plugplay.exe",     "gamemoded",
    "wine-preloader",   "wine64-preloader",
    "svchost.exe",      "srt-bwrap",
    "pv-adverb",        "rpcss.exe",
    "tabtip.exe",       "xalia.exe",
    "conhost.exe",      "mount.ntfs",
    "python3",          "python",
    "python.exe",       "python3.exe",
    "kwin_wayland",     "kwin_wayland.exe",
    "kwin_wayland.exe", "kwin_wayland.exe"};

GameDetectionService::GameDetectionService(QObject *parent)
    : QObject(parent), m_heuristics(std::make_unique<ProcessHeuristics>()),
      m_whitelist(std::make_unique<GameWhitelist>()) {
  m_pollTimer = new QTimer(this);
  m_pollTimer->setInterval(2000);
  connect(m_pollTimer, &QTimer::timeout, this,
          &GameDetectionService::checkForGames);

  m_whitelist->load();
}

GameDetectionService::~GameDetectionService() { stop(); }

void GameDetectionService::start() {
  if (m_running)
    return;
  qInfo() << "Starting game detection service";
  m_running = true;
  m_pollTimer->start();
  checkForGames();
}

void GameDetectionService::stop() {
  if (!m_running)
    return;
  qInfo() << "Stopping game detection service";
  m_running = false;
  m_pollTimer->stop();
}

uint GameDetectionService::getSteamAppId(qint64 pid) {
  QFile envFile(QString("/proc/%1/environ").arg(pid));
  if (!envFile.open(QIODevice::ReadOnly))
    return 0;

  // Read initial chunk (AppID is usually near the start of environment)
  QByteArray envData = envFile.read(4096);
  envFile.close();

  QList<QByteArray> envVars = envData.split('\0');
  for (const QByteArray &var : envVars) {
    if (var.startsWith("SteamAppId=")) {
      return var.mid(11).toUInt();
    }
  }
  return 0;
}

QString GameDetectionService::getSteamAppName(uint appId) {
  if (appId == 0)
    return QString();

  if (m_steamNameCache.contains(appId)) {
    return m_steamNameCache.value(appId);
  }

  // Typical path for CachyOS/Arch Steam installs
  QString steamPath = QDir::homePath() + "/.local/share/Steam/steamapps/";
  QString manifestPath = steamPath + QString("appmanifest_%1.acf").arg(appId);

  QFile file(manifestPath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return QString();

  QTextStream in(&file);
  while (!in.atEnd()) {
    QString line = in.readLine().trimmed();
    if (line.startsWith("\"name\"", Qt::CaseInsensitive)) {
      QStringList parts = line.split("\"", Qt::SkipEmptyParts);
      if (parts.size() >= 2) {
        QString title = parts.last();
        m_steamNameCache.insert(appId, title);
        return title;
      }
    }
  }
  return QString();
}

QString GameDetectionService::formatDisplayName(const QString &rawName) {
  QString name = rawName;
  if (name.endsWith(".exe", Qt::CaseInsensitive))
    name.chop(4);

  name.replace('_', ' ');
  name.replace('-', ' ');

  if (!name.isEmpty()) {
    name[0] = name[0].toUpper();
  }
  return name.trimmed();
}

void GameDetectionService::checkForGames() {
  QDir procDir("/proc");
  QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

  QString bestGame;
  qint64 bestPid = 0;
  float bestScore = 0.0f;
  QSet<qint64> nextNonGamePids;

  for (const QString &entry : entries) {
    bool ok;
    qint64 pid = entry.toLongLong(&ok);
    if (!ok)
      continue;

    if (m_nonGamePids.contains(pid)) {
      nextNonGamePids.insert(pid);
      continue;
    }

    ProcessHeuristics::ProcessInfo info = m_heuristics->getProcessInfo(pid);
    if (info.name.isEmpty())
      continue;

    if (IGNORED_PROCESS_NAMES.contains(info.name.toLower())) {
      nextNonGamePids.insert(pid);
      continue;
    }

    if (m_whitelist->isBlacklisted(info.windowClass) ||
        m_whitelist->isBlacklisted(info.name)) {
      nextNonGamePids.insert(pid);
      continue;
    }

    uint steamAppId = getSteamAppId(pid);
    float score = m_heuristics->gameConfidence(info);

    if (info.name.endsWith(".exe", Qt::CaseInsensitive)) {
      score += 0.2f;
    }

    if (score > 0.5f) {
      if (score > bestScore) {
        bestPid = pid;
        bestScore = score;

        QString officialName = getSteamAppName(steamAppId);

        if (!officialName.isEmpty()) {
          bestGame = officialName;
        } else if (!info.steamGameName.isEmpty()) {
          bestGame = info.steamGameName;
        } else {
          bestGame = formatDisplayName(info.name);
        }
      }
    } else {
      nextNonGamePids.insert(pid);
    }
  }

  m_nonGamePids = std::move(nextNonGamePids);

  if (bestGame != m_currentGameName || bestPid != m_currentGamePid) {
    updateCurrentGame(bestGame, bestPid);
  }
}

void GameDetectionService::updateCurrentGame(const QString &gameName,
                                             qint64 pid) {
  if (gameName == m_currentGameName && pid == m_currentGamePid)
    return;

  QString oldGame = m_currentGameName;
  m_currentGameName = gameName;
  m_currentGamePid = pid;

  // This is what the UI listens to!
  emit currentGameChanged();

  if (gameName.isEmpty()) {
    if (!oldGame.isEmpty()) {
      emit gameClosed(oldGame);
      qInfo() << "Game closed:" << oldGame;
    }
  } else {
    // Only announce a "New" detection if the name actually changed
    // or if there was nothing running before
    if (oldGame != gameName) {
      addToRecentGames(gameName);
      emit gameDetected(gameName, pid);
      qInfo() << "Game detected:" << gameName << "PID:" << pid;
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

} // namespace GameRecorder