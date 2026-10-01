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
class WaylandWindowMonitor;
struct WindowInfo;
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

  /**
   * @brief Whether the compositor currently shows a window for the game.
   *
   * Portal restore data references concrete windows (KDE matches by appId +
   * title), so firing a capture request before any game window exists
   * guarantees a picker even with a valid token. Application waits for
   * this before starting a token-armed capture.
   */
  bool hasVisibleWindow() const;

  // Whitelist/Blacklist Management
  void addToWhitelist(const QString &windowClass, const QString &displayName);
  void addToBlacklist(const QString &windowClass);
  void removeFromWhitelist(const QString &windowClass);
  void removeFromBlacklist(const QString &windowClass);
  QStringList whitelist() const;
  QStringList blacklist() const;

  /**
   * @brief Attach the compositor window monitor for confidence fusion.
   * Without one, only whitelisted / Steam-identified shim events detect.
   */
  void setWindowMonitor(WaylandWindowMonitor *monitor);

  // SteamAppId/SteamGameId from a process environment (what the Vulkan
  // layer reads via getenv, but usable for any PID after the fact).
  static uint steamAppIdForPid(qint64 pid);

  // "steam_app_1422450" -> 1422450, anything else -> 0. Window classes
  // from Steam games carry the id; resolving it gives display names.
  static uint steamAppIdFromAppId(const QString &appId);

signals:
  void gameDetected(const QString &gameName, qint64 pid);
  void gameClosed(const QString &gameName);
  void currentGameChanged();
  void recentGamesChanged();

private slots:
  void onNewConnection();
  void onSocketReadyRead();
  void onSocketDisconnected();
  void onMonitorWindowsChanged();

private:
  // Grant the test class access to private members (fixes your build errors)
  friend class GameDetectionServiceTest;

  // Internal identification helpers
  QString getSteamAppName(uint appId);

  /**
   * @brief Decide whether a shim event is confident enough to report.
   * Blacklist is checked by the caller. Instant detect on: whitelist
   * hit, SteamAppId present, or the exe living under a steamapps library.
   * Otherwise the focused + fullscreen window for the PID decides.
   */
  bool isConfidentGame(qint64 pid, const QString &gameName, uint steamAppId);
  static bool processLooksLikeSteamGame(qint64 pid);

  void trackGame(qint64 pid, const QString &gameName);
  void recheckCandidates();

  /**
   * @brief Detect games that were already running when Trophy started.
   * Shims only report at graphics init, so a pre-existing game never
   * sends START. Runs automatically on the first window snapshot after
   * start(), and is public for explicit calls. Only focused/fullscreen
   * windows with whitelist or Steam evidence are reported — same bar as
   * live detection, never a guess.
   */
  void reconcileRunningGames();

  /**
   * @brief Borderless-windowed heuristic: a focused window covering most
   * of its output counts like fullscreen (many games never set the flag).
   */
  static bool coversOutput(const WindowInfo &w);

  void updateCurrentGame(const QString &gameName, qint64 pid);
  void addToRecentGames(const QString &gameName);

  bool m_running = false;
  QString m_currentGameName;
  qint64 m_currentGamePid = 0;
  QStringList m_recentGames;

  QLocalServer *m_server;
  QMap<QLocalSocket *, qint64> m_activeConnections;
  QMap<qint64, QString> m_pidToGameName;
  // Shim reporters that did not (yet) pass the confidence gate. Pruned
  // on STOP/disconnect; re-checked whenever window state changes so a
  // game that starts windowed still detects when it goes fullscreen.
  QMap<qint64, QString> m_pendingCandidates;

  WaylandWindowMonitor *m_monitor = nullptr;
  std::unique_ptr<GameWhitelist> m_whitelist;
};

} // namespace TrophyBackend