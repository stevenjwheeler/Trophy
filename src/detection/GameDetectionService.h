/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "GameListModel.h"
#include "ProcessGames.h"
#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QVariantMap>
#include <future>
#include <memory>

class RecordingManagerTest;
namespace TrophyBackend {
class GameWhitelist;
class AudioApplicationRegistry;
class GameDetectionService : public QObject {
  Q_OBJECT
  Q_PROPERTY(
      QString currentGameName READ currentGameName NOTIFY currentGameChanged)
  Q_PROPERTY(bool isGameRunning READ isGameRunning NOTIFY currentGameChanged)
  Q_PROPERTY(
      qint64 currentGamePid READ currentGamePid NOTIFY currentGameChanged)
  Q_PROPERTY(QStringList recentGames READ recentGames NOTIFY recentGamesChanged)
  Q_PROPERTY(QAbstractItemModel *gamesModel READ gamesModel CONSTANT)
  Q_PROPERTY(QString gameFilterText READ gameFilterText WRITE setGameFilterText
                 NOTIFY gameFilterTextChanged)
  Q_PROPERTY(bool showDefaultExclusions READ showDefaultExclusions WRITE
                 setShowDefaultExclusions NOTIFY showDefaultExclusionsChanged)
  Q_PROPERTY(QVariantList instances READ instances NOTIFY instancesChanged)
  Q_PROPERTY(QVariantList gameLibraries READ gameLibraries NOTIFY gameLibrariesChanged)
  Q_PROPERTY(bool librariesScanning READ librariesScanning NOTIFY librariesScanningChanged)
  Q_PROPERTY(bool gameLibrariesReady READ gameLibrariesReady NOTIFY gameLibrariesChanged)
public:
  QVariantList gameLibraries() const { return m_libraryStatus; }
  bool librariesScanning() const { return m_libraryRefresh || m_libraryScanning; }
  bool gameLibrariesReady() const { return m_librariesReady; }
  Q_INVOKABLE void rescanGameLibraries();
  Q_INVOKABLE void setLibraryAutomatic(const QString &id, bool automatic);
  Q_INVOKABLE void addLibraryLocation(const QString &id, const QUrl &directory);
  Q_INVOKABLE void removeLibraryLocation(const QString &id, const QString &directory);
  Q_INVOKABLE void resetLibraryLocations(const QString &id);
  explicit GameDetectionService(QObject *parent = nullptr);
  ~GameDetectionService() override;
  void start();
  void stop();
  void setAudioApplicationRegistry(AudioApplicationRegistry *registry);
  bool isGameRunning() const { return m_currentGamePid > 0; }
  // Process readiness only: the portal still owns actual window selection.
  bool isGamePayloadReady() const;
  QString currentGameName() const { return m_currentGameName; }
  qint64 currentGamePid() const { return m_currentGamePid; }
  QString currentGameKey() const { return m_currentGameKey; }
  QString currentGameSource() const;
  QStringList recentGames() const { return m_recentGames; }
  QList<GameInstance> games() const { return m_instances; }
  QVariantList instances() const;
  // Cached census only: listing never performs a blocking /proc scan on the UI.
  Q_INVOKABLE QVariantList runningProcesses() const;
  Q_INVOKABLE bool addProcessGame(qint64 pid, const QString &identity,
                                  const QString &display, bool block);
  Q_INVOKABLE bool selectInstance(const QString &id);
  QVariantMap captureIdentityInfo(const QString &name) const;
  QString captureKeyForGame(const QString &name) const {
    return name == m_currentGameName ? m_currentGameKey : name;
  }
  bool instanceAlive(const QString &id) const;
  // Census-settling evidence only: a runtime is never a live game member.
  bool instanceLaunchContextAlive(const QString &id) const;
  Q_INVOKABLE bool addToWhitelist(const QString &key, const QString &display);
  Q_INVOKABLE bool addToBlacklist(const QString &key);
  Q_INVOKABLE bool removeFromWhitelist(const QString &key);
  Q_INVOKABLE bool removeFromBlacklist(const QString &key);
  Q_INVOKABLE bool addSteamGame(uint appId, const QString &display);
  Q_INVOKABLE bool moveToWhitelist(const QString &key, const QString &display);
  Q_INVOKABLE bool moveToBlacklist(const QString &key);
  Q_INVOKABLE bool unblockGame(const QString &key);
  Q_INVOKABLE bool isBlocked(const QString &key) const;
  Q_INVOKABLE bool addCustomGame(const QString &key, const QString &display,
                                 bool block);
  Q_INVOKABLE bool forgetGame(const QString &key);
  QStringList whitelist() const;
  QStringList blacklist() const;
  QAbstractItemModel *gamesModel() const { return m_gamesFilter; }
  Q_INVOKABLE QUrl iconSourceForGame(const QString &name) const;
  QString gameFilterText() const;
  void setGameFilterText(const QString &text);
  void setTokenGames(const QStringList &games);
  bool showDefaultExclusions() const;
  void setShowDefaultExclusions(bool show);
  static uint steamAppIdFromAppId(const QString &key);
  static uint steamAppIdForPid(qint64 pid);
  static bool processExists(qint64 pid);
signals:
  // Selected capture target; all concurrent games are exposed in instances.
  void gameDetected(const QString &name, qint64 pid);
  void gameClosed(const QString &name);
  void currentGameChanged();
  void instancesChanged();
  void recentGamesChanged();
  void whitelistChanged();
  void blacklistChanged();
  void gameFilterTextChanged();
  void showDefaultExclusionsChanged();
  void tokenGamesChanged();
  void gameLibrariesChanged();
  void librariesScanningChanged();

private:
  friend class GameDetectionServiceTest;
  friend class DetectionFixture;
  friend class ApplicationAuthorizePromptTest;
  friend class ::RecordingManagerTest;
  void poll();
  void queueLibraryRefresh();
  void updateLibraryWatches(const QStringList &paths);
  void apply(const ProcessCensus::Snapshot &snapshot);
  // Values must outlive synchronous slots that rebuild the instance list.
  void updateCurrentGame(QString name, qint64 pid, QString key = {});
  void rebuildModels();
  void rulesChanged();
  static QSet<QString> desktopApplicationExecutables(const QStringList &dataRoots);
  QString getSteamAppName(uint id) const;
  bool m_running = false;
  QTimer m_timer;
  struct ScanResult {
    ProcessCensus::Snapshot processes;
    QList<LauncherGame> catalogue;
    QSet<QString> applicationExecutables;
    bool catalogueUpdated = false;
    QVariantList libraryStatus;
    unsigned libraryGeneration = 0;
    QStringList libraryWatchPaths;
    QVariantList audioStreams;
    quint64 audioRevision = 0;
  };
  void applyScanResult(ScanResult result);
  std::future<ScanResult> m_scan;
  QFileSystemWatcher m_libraryWatcher;
  QTimer m_libraryFallback, m_libraryDebounce;
  QVariantMap m_libraryPreferences;
  QVariantList m_libraryStatus;
  bool m_libraryRefresh = true, m_libraryScanning = false;
  bool m_librariesReady = false;
  unsigned m_libraryGeneration = 0;
  void saveLibraryPreferences(const QString &id, const QVariantMap &preference);
  AudioApplicationRegistry *m_audioRegistry = nullptr;
  QVariantList m_audioMetadata;
  quint64 m_audioRevision = 0;
  QVariantList m_audioStreams;
  ProcessCensus::Snapshot m_processes;
  QList<LauncherGame> m_catalogue;
  QSet<QString> m_applicationExecutables;
  QList<GameInstance> m_instances;
  QString m_selectedId;
  QString m_currentGameName, m_currentGameKey;
  qint64 m_currentGamePid = 0;
  QStringList m_recentGames, m_recentKeys;
  QSet<QString> m_tokenGames;
  std::unique_ptr<GameWhitelist> m_whitelist;
  std::unique_ptr<GameListModel> m_gamesModel;
  GameListFilterModel *m_gamesFilter = nullptr;
};
} // namespace TrophyBackend
