/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <memory>

namespace GameRecorder {

class ProcessHeuristics;
class GameWhitelist;

/**
 * @brief Detects running games and monitors focus
 *
 * Uses a combination of window monitoring, process heuristics,
 * and user-defined rules to identify games.
 */
class GameDetectionService : public QObject {
  Q_OBJECT

  friend class GameDetectionServiceTest;

  Q_PROPERTY(
      QString currentGameName READ currentGameName NOTIFY currentGameChanged)
  Q_PROPERTY(
      qint64 currentGamePid READ currentGamePid NOTIFY currentGameChanged)
  Q_PROPERTY(bool isGameRunning READ isGameRunning NOTIFY currentGameChanged)
  Q_PROPERTY(QStringList recentGames READ recentGames NOTIFY recentGamesChanged)

public:
  explicit GameDetectionService(QObject *parent = nullptr);
  ~GameDetectionService() override;

  void start();
  void stop();

  QString currentGameName() const { return m_currentGameName; }
  qint64 currentGamePid() const { return m_currentGamePid; }
  bool isGameRunning() const { return !m_currentGameName.isEmpty(); }
  QStringList recentGames() const { return m_recentGames; }

  // Whitelist/Blacklist management
  Q_INVOKABLE void addToWhitelist(const QString &windowClass,
                                  const QString &displayName = QString());
  Q_INVOKABLE void addToBlacklist(const QString &windowClass);
  Q_INVOKABLE void removeFromWhitelist(const QString &windowClass);
  Q_INVOKABLE void removeFromBlacklist(const QString &windowClass);

  Q_INVOKABLE QStringList whitelist() const;
  Q_INVOKABLE QStringList blacklist() const;

signals:
  void gameDetected(const QString &gameName, qint64 pid);
  void gameClosed(const QString &gameName);
  void currentGameChanged();
  void recentGamesChanged();

private slots:
  void checkForGames();

private:
  void updateCurrentGame(const QString &gameName, qint64 pid);
  void addToRecentGames(const QString &gameName);

  std::unique_ptr<ProcessHeuristics> m_heuristics;
  std::unique_ptr<GameWhitelist> m_whitelist;
  QTimer *m_pollTimer = nullptr;

  QString m_currentGameName;
  qint64 m_currentGamePid = 0;
  QStringList m_recentGames;
  bool m_running = false;
  QSet<qint64> m_nonGamePids;
};

} // namespace GameRecorder
