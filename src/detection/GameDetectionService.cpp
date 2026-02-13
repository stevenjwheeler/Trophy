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
#include <QTextStream>

namespace GameRecorder {

GameDetectionService::GameDetectionService(QObject *parent)
    : QObject(parent), m_heuristics(std::make_unique<ProcessHeuristics>()),
      m_whitelist(std::make_unique<GameWhitelist>()) {
  m_pollTimer = new QTimer(this);
  m_pollTimer->setInterval(2000); // Check every 2 seconds
  connect(m_pollTimer, &QTimer::timeout, this,
          &GameDetectionService::checkForGames);

  // Load whitelist
  m_whitelist->load();
}

GameDetectionService::~GameDetectionService() { stop(); }

void GameDetectionService::start() {
  if (m_running) {
    return;
  }

  qInfo() << "Starting game detection service";
  m_running = true;
  m_pollTimer->start();

  // Immediate check
  checkForGames();
}

void GameDetectionService::stop() {
  if (!m_running) {
    return;
  }

  qInfo() << "Stopping game detection service";
  m_running = false;
  m_pollTimer->stop();
}

void GameDetectionService::checkForGames() {
  // Scan /proc for running processes
  QDir procDir("/proc");
  QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

  QString bestGame;
  qint64 bestPid = 0;
  float bestScore = 0.0f;
  QSet<qint64> nextNonGamePids;

  for (const QString &entry : entries) {
    bool ok;
    qint64 pid = entry.toLongLong(&ok);
    if (!ok) {
      continue;
    }

    // Optimization: If we already determined this PID is not a game, skip it
    // but keep it in the cache for the next iteration (since it's still
    // running)
    if (m_nonGamePids.contains(pid)) {
      nextNonGamePids.insert(pid);
      continue;
    }

    ProcessHeuristics::ProcessInfo info = m_heuristics->getProcessInfo(pid);
    if (info.name.isEmpty()) {
      continue;
    }

    // Check blacklist first
    if (m_whitelist->isBlacklisted(info.windowClass) ||
        m_whitelist->isBlacklisted(info.name)) {
      nextNonGamePids.insert(pid);
      continue;
    }

    // Check whitelist (auto-match)
    QString whitelistName = m_whitelist->getWhitelistName(info.windowClass);
    if (!whitelistName.isEmpty()) {
      if (bestScore < 1.0f) {
        bestGame = whitelistName;
        bestPid = pid;
        bestScore = 1.0f;
      }
      // Even if whitelisted, we don't cache it as non-game
      continue;
    }

    // Use heuristics
    float score = m_heuristics->gameConfidence(info);
    if (score > 0.5f) {
      if (score > bestScore) {
        bestGame = info.name;
        bestPid = pid;
        bestScore = score;

        // Use Steam game name if available
        if (!info.steamGameName.isEmpty()) {
          bestGame = info.steamGameName;
        }
      }
    } else {
      // Mark as checked non-game
      nextNonGamePids.insert(pid);
    }
  }

  // Update cache (automatically drops dead PIDs)
  m_nonGamePids = std::move(nextNonGamePids);

  // Update current game
  if (bestGame != m_currentGameName) {
    updateCurrentGame(bestGame, bestPid);
  }
}

void GameDetectionService::updateCurrentGame(const QString &gameName,
                                             qint64 pid) {
  QString oldGame = m_currentGameName;

  m_currentGameName = gameName;
  m_currentGamePid = pid;

  emit currentGameChanged();

  if (!oldGame.isEmpty() && gameName.isEmpty()) {
    emit gameClosed(oldGame);
    qInfo() << "Game closed:" << oldGame;
  } else if (!gameName.isEmpty() && oldGame != gameName) {
    addToRecentGames(gameName);
    emit gameDetected(gameName, pid);
    qInfo() << "Game detected:" << gameName << "PID:" << pid;
  }
}

void GameDetectionService::addToRecentGames(const QString &gameName) {
  m_recentGames.removeAll(gameName);
  m_recentGames.prepend(gameName);

  // Keep only last 10
  while (m_recentGames.size() > 10) {
    m_recentGames.removeLast();
  }

  emit recentGamesChanged();
}

void GameDetectionService::addToWhitelist(const QString &windowClass,
                                          const QString &displayName) {
  m_whitelist->addToWhitelist(windowClass, displayName);
  m_whitelist->save();
}

void GameDetectionService::addToBlacklist(const QString &windowClass) {
  m_whitelist->addToBlacklist(windowClass);
  m_whitelist->save();
}

void GameDetectionService::removeFromWhitelist(const QString &windowClass) {
  m_whitelist->removeFromWhitelist(windowClass);
  m_whitelist->save();
}

void GameDetectionService::removeFromBlacklist(const QString &windowClass) {
  m_whitelist->removeFromBlacklist(windowClass);
  m_whitelist->save();
}

QStringList GameDetectionService::whitelist() const {
  return m_whitelist->whitelistEntries();
}

QStringList GameDetectionService::blacklist() const {
  return m_whitelist->blacklistEntries();
}

} // namespace GameRecorder
