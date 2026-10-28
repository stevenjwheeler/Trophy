/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include "DiscordGameArtwork.h"

namespace TrophyBackend {

// Owner-thread, asynchronous Discord desktop RPC. No SDK or account tokens;
// optional public artwork lookups are gated by presence/game-name preferences.
class DiscordPresence : public QObject {
  Q_OBJECT
public:
  explicit DiscordPresence(QObject *parent = nullptr);
  // Optional injected transport for isolated tests; it must outlive this service.
  explicit DiscordPresence(QNetworkAccessManager &network, QObject *parent = nullptr);
  ~DiscordPresence() override;
  void setPreferences(bool enabled, bool showGameNames, bool showWhenIdle);
  void setCaptureState(bool recording, const QString &recordingGame,
                       bool replayActive, const QString &replayGame,
                       quint32 recordingSteamAppId = 0, quint32 replaySteamAppId = 0);
  void shutdown();

private:
  DiscordPresence(QNetworkAccessManager *network, QObject *parent);
  friend class DiscordPresenceTest;
  static QStringList socketPaths();
  QJsonObject activity() const;
  bool wanted() const;
  void refresh();
  void connectNext();
  void retry();
  void readFrames();
  void publish();
  void sendFrame(quint32 opcode, const QByteArray &payload);
  void clearAndDisconnect();
  void refreshArtwork();

  QNetworkAccessManager m_network;
  DiscordGameArtwork m_artwork;
  QLocalSocket m_socket;
  QTimer m_retryTimer, m_timeout, m_updateTimer;
  QStringList m_paths;
  int m_nextPath = 0;
  QByteArray m_input;
  QString m_pendingNonce;
  QJsonObject m_lastActivity;
  bool m_ready = false, m_enabled = false, m_showGameNames = true;
  bool m_showWhenIdle = false, m_recording = false, m_replay = false;
  bool m_stopped = false;
  QString m_recordingGame, m_replayGame;
  qint64 m_recordingStart = 0;
  quint32 m_recordingAppId = 0, m_replayAppId = 0;
};
} // namespace TrophyBackend
