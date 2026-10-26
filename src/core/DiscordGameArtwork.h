/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QHash>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>

namespace TrophyBackend {

// Resolves public Steam game icons only. Never uploads local files or uses
// Steam credentials. The injected network manager must outlive this object.
class DiscordGameArtwork : public QObject {
  Q_OBJECT
public:
  explicit DiscordGameArtwork(QNetworkAccessManager &network, QObject *parent = nullptr);
  ~DiscordGameArtwork() override;
  void setGame(quint32 steamAppId);
  void rejectCurrentImage();
  QString imageUrl() const { return m_imageUrl; }
  static QString publicIconUrl(const QByteArray &html, quint32 steamAppId);

signals:
  void imageChanged();

private:
  friend class DiscordGameArtworkTest;
  void cancel();
  void request(const QUrl &url, bool imageProbe, quint64 generation);
  void complete(const QString &url);
  struct CacheEntry { QString url; qint64 expires; };
  QNetworkAccessManager &m_network;
  QPointer<QNetworkReply> m_reply;
  QTimer m_deadline;
  QElapsedTimer m_clock;
  QHash<quint32, CacheEntry> m_cache;
  quint32 m_appId = 0;
  quint64 m_generation = 0;
  QByteArray m_html;
  QString m_imageUrl;
};
} // namespace TrophyBackend
