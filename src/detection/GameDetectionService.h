/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QLocalServer>
#include <QLocalSocket>
#include <QMap>
#include <QObject>
#include <QSet>
#include <memory>

namespace TrophyBackend {

class GameWhitelist;
class GameWhitelist;
class GameDetectionServiceTest; // Forward declaration for unit testing

class GameDetectionService : public QObject {
  Q_OBJECT

  // Property system for UI/QML synchronization
  Q_PROPERTY(
      QString currentGameName READ currentGameName NOTIFY currentGameChanged)
  Q_PROPERTY(bool isGameRunning READ isGameRunning NOTIFY currentGameChanged)
  Q_PROPERTY(
      qint64 currentGamePid READ currentGamePid NOTIFY currentGameChanged)
  Q_PROPERTY(QStringList recentGames READ recentGames NOTIFY recentGamesChanged)

public:
  explicit GameDetectionService(QObject *parent = nullptr);
  ~GameDetectionService();

  void start();
  void stop();

  // Getters for properties
  bool isGameRunning() const {
    return m_currentGamePid > 0 && !m_currentGameName.isEmpty();
  }
  QString currentGameName() const { return m_currentGameName; }
  qint64 currentGamePid() const { return m_currentGamePid; }
  QStringList recentGames() const { return m_recentGames; }

  // Whitelist/Blacklist Management
  void addToWhitelist(const QString &windowClass, const QString &displayName);
  void addToBlacklist(const QString &windowClass);
  void removeFromWhitelist(const QString &windowClass);
  void removeFromBlacklist(const QString &windowClass);
  QStringList whitelist() const;
  QStringList blacklist() const;

signals:
  void gameDetected(const QString &gameName, qint64 pid);
  void gameClosed(const QString &gameName);
  void currentGameChanged();
  void recentGamesChanged();

private slots:
  void onNewConnection();
  void onSocketReadyRead();
  void onSocketDisconnected();

private:
  // Grant the test class access to private members (fixes your build errors)
  friend class GameDetectionServiceTest;

  // Internal identification helpers
  uint getSteamAppId(qint64 pid);
  QString getSteamAppName(uint appId);
  QString formatDisplayName(const QString &rawName);

  void updateCurrentGame(const QString &gameName, qint64 pid);
  void addToRecentGames(const QString &gameName);

  bool m_running = false;
  QString m_currentGameName;
  qint64 m_currentGamePid = 0;
  QStringList m_recentGames;

  QLocalServer *m_server;
  QMap<QLocalSocket *, qint64> m_activeConnections;
  QMap<qint64, QString> m_pidToGameName;

  std::unique_ptr<GameWhitelist> m_whitelist;
};

} // namespace TrophyBackend