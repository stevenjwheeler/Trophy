/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "DiscordGameArtwork.h"
#include <QNetworkReply>
#include <QRegularExpression>

namespace TrophyBackend {
namespace {
constexpr qsizetype MaxHtml = 256 * 1024;
}

DiscordGameArtwork::DiscordGameArtwork(QNetworkAccessManager &network, QObject *parent)
    : QObject(parent), m_network(network) {
  m_clock.start();
  m_deadline.setSingleShot(true);
  m_deadline.setInterval(10000);
  connect(&m_deadline, &QTimer::timeout, this, [this] {
    cancel();
    complete({});
  });
}

DiscordGameArtwork::~DiscordGameArtwork() { cancel(); }

QString DiscordGameArtwork::publicIconUrl(const QByteArray &html, quint32 appId) {
  if (!appId || html.size() > MaxHtml) return {};
  // Only the game's hub icon, never avatars, screenshots or arbitrary URLs.
  static const QRegularExpression icon(
      R"re(<div\s+class=["']apphub_AppIcon["'][^>]*>\s*<img\s+[^>]*src=["']([^"']+)["'])re",
      QRegularExpression::CaseInsensitiveOption);
  const auto match = icon.match(QString::fromUtf8(html));
  if (!match.hasMatch()) return {};
  const QUrl url(match.captured(1));
  const QRegularExpression path(QStringLiteral(
      "^/(?:community_assets/images/apps|steamcommunity/public/images/apps)/%1/[a-fA-F0-9]{40}\\.(?:jpg|png)$").arg(appId));
  if (url.scheme() != "https" || !url.host().endsWith(".steamstatic.com") ||
      !url.userInfo().isEmpty() || url.port() != -1 || url.hasQuery() || url.hasFragment() ||
      !path.match(url.path()).hasMatch()) return {};
  // Hub/launcher icons are only 32×32. The same content hash identifies
  // Steam's 184×184 avatar variant; probe that public URL before using it.
  const auto hash = url.fileName().section('.', 0, 0);
  return QStringLiteral("https://avatars.steamstatic.com/%1_full.jpg").arg(hash);
}

void DiscordGameArtwork::cancel() {
  ++m_generation;
  m_deadline.stop();
  if (m_reply) {
    auto *reply = m_reply.data();
    m_reply.clear();
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }
  m_html.clear();
}

void DiscordGameArtwork::setGame(quint32 appId) {
  if (appId == m_appId) return;
  cancel();
  m_appId = appId;
  m_imageUrl.clear();
  emit imageChanged();
  if (!appId) return;
  const auto found = m_cache.constFind(appId);
  if (found != m_cache.cend() && found->expires > m_clock.elapsed()) {
    m_imageUrl = found->url;
    emit imageChanged();
    return;
  }
  m_deadline.start();
  request(QUrl(QStringLiteral("https://steamcommunity.com/app/%1").arg(appId)), false, m_generation);
}

void DiscordGameArtwork::request(const QUrl &url, bool probe, quint64 generation) {
  QNetworkRequest request(url);
  request.setTransferTimeout(5000);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
  request.setRawHeader("User-Agent", "Trophy-DiscordPresence/1.0");
  auto *reply = probe ? m_network.head(request) : m_network.get(request);
  m_reply = reply;
  reply->setReadBufferSize(MaxHtml + 1);
  connect(reply, &QNetworkReply::readyRead, this, [this, reply, generation, probe] {
    if (generation != m_generation || m_reply != reply || probe) return;
    m_html += reply->read(MaxHtml + 1 - m_html.size());
    if (m_html.size() > MaxHtml) {
      cancel();
      complete({});
    }
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, generation, probe, url] {
    if (generation != m_generation || m_reply != reply) return;
    m_reply.clear();
    reply->deleteLater();
    const bool ok = reply->error() == QNetworkReply::NoError &&
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
    if (probe) {
      const auto type = reply->header(QNetworkRequest::ContentTypeHeader).toString();
      complete(ok && type.startsWith("image/", Qt::CaseInsensitive) ? url.toString() : QString());
    } else {
      m_html += reply->read(MaxHtml + 1 - m_html.size());
      const auto icon = ok ? publicIconUrl(m_html, m_appId) : QString();
      m_html.clear();
      if (icon.isEmpty()) complete({});
      else this->request(QUrl(icon), true, generation);
    }
  });
}

void DiscordGameArtwork::complete(const QString &url) {
  m_deadline.stop();
  if (!m_appId) return;
  if (m_cache.size() >= 64) m_cache.clear();
  m_cache.insert(m_appId, {url, m_clock.elapsed() + (url.isEmpty() ? 300000 : 3600000)});
  m_imageUrl = url;
  emit imageChanged();
}

void DiscordGameArtwork::rejectCurrentImage() {
  cancel();
  complete({});
}
} // namespace TrophyBackend
