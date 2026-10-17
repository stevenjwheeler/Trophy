/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QAbstractItemModel>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <memory>

#include "GameListModel.h"

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
  // QML-facing unified games list (live + history + user lists,
  // filtered by gameFilterText; built-in exclusions hide unless
  // showDefaultExclusions is set).
  Q_PROPERTY(QAbstractItemModel *gamesModel READ gamesModel CONSTANT)
  Q_PROPERTY(QString gameFilterText READ gameFilterText WRITE setGameFilterText
                 NOTIFY gameFilterTextChanged)
  Q_PROPERTY(bool showDefaultExclusions READ showDefaultExclusions WRITE
                 setShowDefaultExclusions NOTIFY showDefaultExclusionsChanged)

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
  // Raw match key for the live game (reporter class / steam_app_<id>).
  QString currentGameKey() const { return m_currentGameKey; }
  QString captureKeyForGame(const QString &name) const {
    return name == m_currentGameName && !m_currentGameKey.isEmpty()
               ? m_currentGameKey : name;
  }
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

  // Whitelist/Blacklist Management (invokable for the Games page;
  // true when the list actually changed, for UI feedback)
  Q_INVOKABLE bool addToWhitelist(const QString &windowClass,
                                  const QString &displayName);
  Q_INVOKABLE bool addToBlacklist(const QString &windowClass);
  Q_INVOKABLE bool removeFromWhitelist(const QString &windowClass);
  Q_INVOKABLE bool removeFromBlacklist(const QString &windowClass);
  // Whitelist a Steam game by AppID: stored under the steam_app_<id> key
  // form the matcher already understands (window appIds, shim names).
  Q_INVOKABLE bool addSteamGame(uint appId, const QString &displayName);
  // Atomic list moves for the Games page: one mutation, one signal, one
  // model reset. Separate remove+add calls from QML straddle a reset that
  // tears down the invoking delegate mid-handler and loses the second
  // half (seen live: Allow on a system row removed it but never re-added
  // it anywhere).
  Q_INVOKABLE bool moveToWhitelist(const QString &key, const QString &display);
  Q_INVOKABLE bool moveToBlacklist(const QString &key);
  // Drop the block veto for a key (Prompt pole). Whitelist membership is
  // orthogonal and untouched: poles govern capture behavior, the x button
  // governs existence. True when a veto was actually cleared.
  Q_INVOKABLE bool unblockGame(const QString &key);
  // True when the key or name carries a block veto (normalized either
  // way). Application suppresses the authorize prompt for blocked games.
  Q_INVOKABLE bool isBlocked(const QString &keyOrName) const;
  // Manual entry from the Add dialog: records the key as user-known (so
  // it stays visible with no rule attached) plus the chosen list.
  Q_INVOKABLE bool addCustomGame(const QString &key, const QString &display,
                                 bool block);
  // Full deletion: drops whitelist/blacklist entries, user-known mark,
  // and history. Built-ins fall back to their neutral marker row.
  Q_INVOKABLE bool forgetGame(const QString &key);
  QStringList whitelist() const;
  QStringList blacklist() const;

  QAbstractItemModel *gamesModel() const { return m_gamesFilter; }
  // Presentation-only lookup for older recordings that carry a title, not
  // an app identity. Ambiguous titles must not borrow another game's icon.
  Q_INVOKABLE QUrl iconSourceForGame(const QString &name) const;
  QString gameFilterText() const;
  void setGameFilterText(const QString &text);
  // Portal restore-token games (silent capture). Pushed by Application
  // from SettingsManager; approval is derived, never stored here.
  void setTokenGames(const QStringList &games);
  bool showDefaultExclusions() const;
  void setShowDefaultExclusions(bool show);

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

  /**
   * @brief Whether a process currently exists (signal 0 probe).
   *
   * EPERM counts as alive (process exists but is not signalable).
   * Pure predicate so tests can cover it without timers.
   */
  static bool processExists(qint64 pid);

signals:
  void gameDetected(const QString &gameName, qint64 pid);
  void gameClosed(const QString &gameName);
  void currentGameChanged();
  void recentGamesChanged();
  void whitelistChanged();
  void blacklistChanged();
  void gameFilterTextChanged();
  void showDefaultExclusionsChanged();
  void tokenGamesChanged();

private slots:
  void onNewConnection();
  void onSocketReadyRead();
  void onSocketDisconnected();
  void onMonitorWindowsChanged();
  void onLivenessTimeout();

private:
  // Grant the test class access to private members (fixes your build errors)
  friend class GameDetectionServiceTest;
  friend class ApplicationAuthorizePromptTest;

  // Internal identification helpers
  QString getSteamAppName(uint appId);
  struct ReporterIdentity {
    QString key;
    QString name;
    QString executable;
    bool ignored = false;
  };
  // Pure resolver, also used by daemon-free fixtures. Inputs come only
  // from the PID named in a START event, never a process scan.
  static ReporterIdentity resolveReporterIdentity(const QString &reporter,
                                                  const QString &exe,
                                                  const QStringList &argv,
                                                  uint steamAppId,
                                                  const QString &winePrefix = {});
  ReporterIdentity reporterIdentity(qint64 pid, const QString &reporter,
                                    uint steamAppId);

  /**
   * @brief Decide whether a shim event is confident enough to report.
   * Blacklist is checked by the caller. Instant detect on: whitelist
   * hit, SteamAppId present, or the exe living under a steamapps library.
   * Otherwise the focused + fullscreen window for the PID decides.
   */
  bool isConfidentGame(qint64 pid, const QString &gameName, uint steamAppId);
  bool isBlacklistedIdentity(const QString &rawKey,
                             const QString &displayName) const;
  bool processLooksLikeSteamGame(qint64 pid);

  void trackGame(qint64 pid, const QString &gameName,
                 const QString &rawKey = QString());
  void recheckCandidates();
  // A veto landing on the live game retires it (dashboard back to IDLE,
  // gameClosed for clean capture teardown). Called by every path that
  // adds a veto; no-op unless the canonical key matches the tracked game.
  void retireBlockedGame(const QString &canonicalKey);
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

  void updateCurrentGame(const QString &gameName, qint64 pid,
                         const QString &rawKey = QString());
  /**
   * @brief Failover target, preferring focus, same identity, then visibility.
   * PID order breaks ties. Only reported PIDs are probed; never a scan.
   */
  qint64 lowestLiveTrackedPid() const;
  /**
   * @brief Failover after the tracked PID went away (STOP / disconnect /
   * liveness). Promotes the best still-running tracked PID; when none
   * is alive, drops the proven-dead entries (so a later START for the
   * same name detects fresh) and goes idle.
   */
  void promoteNextTrackedGame();
  void addToRecentGames(const QString &gameName, const QString &rawKey);

  bool m_running = false;
  QString m_currentGameName;
  qint64 m_currentGamePid = 0;
  // Stable match key: steam_app_<id>, executable path, Wine prefix +
  // game path, or a legacy reporter key when executable info is absent.
  QString m_currentGameKey;
  QStringList m_recentGames;
  // Raw keys parallel to m_recentGames (same order, capped together).
  QStringList m_recentKeys;
  // Restore-token keys (stable identities, or legacy display names).
  QSet<QString> m_tokenGames;

  QLocalServer *m_server;
  QMap<QLocalSocket *, qint64> m_activeConnections;
  QMap<qint64, QString> m_pidToGameName;
  // Raw match keys parallel to m_pidToGameName (cleaned at the same sites).
  QMap<qint64, QString> m_pidToRawKey;
  // Liveness watchdog for the tracked game (see onLivenessTimeout): a
  // single targeted kill(pid, 0) every few seconds — not a scan.
  QTimer *m_livenessTimer = nullptr;
  // Shim reporters that did not (yet) pass the confidence gate. Pruned
  // on STOP/disconnect; re-checked whenever window state changes so a
  // game that starts windowed still detects when it goes fullscreen.
  QMap<qint64, QString> m_pendingCandidates;
  // Reporter/window identity parallel to m_pendingCandidates. A Steam
  // resolved display name is not necessarily the key stored by the user.
  QMap<qint64, QString> m_pendingRawKeys;
  // Steam manifest name cache: appId -> display name. Validity is keyed
  // on the steamapps library path + libraryfolders.vdf mtime and the
  // matched manifest mtime, so library/name changes refresh automatically.
  // Only positive hits are cached: a missing manifest may appear later
  // without the vdf mtime changing (old-format vdfs).
  struct SteamNameCacheEntry {
    QString name;
    QString manifestPath;
    QDateTime modified;
  };
  QMap<uint, SteamNameCacheEntry> m_steamNameCache;
  QString m_steamLibCacheKey;
  // Per-PID looks-like-Steam verdicts (see processLooksLikeSteamGame):
  // pid -> (result, monotonic ms at check). Member-scoped so instances
  // (and tests) never share verdicts.
  QMap<qint64, QPair<bool, qint64>> m_steamCheckCache;
  static constexpr qint64 kCacheTtlMs = 5000;
  static constexpr int kCacheCap = 256;
  QElapsedTimer m_uptimeClock;

  WaylandWindowMonitor *m_monitor = nullptr;
  std::unique_ptr<GameWhitelist> m_whitelist;

  // Games-page list model (owned here so the pointer stays stable).
  std::unique_ptr<GameListModel> m_gamesModel;
  GameListFilterModel *m_gamesFilter = nullptr;
  void rebuildModels();
};

} // namespace TrophyBackend
