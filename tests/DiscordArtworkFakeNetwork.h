/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTimer>
#include <cstring>
#include <gtest/gtest.h>

struct ArtworkResponse {
  int status = 200;
  QByteArray type = "text/html";
  QByteArray body;
  bool held = false;
};

class ArtworkReply : public QNetworkReply {
public:
  ArtworkReply(QNetworkAccessManager::Operation op, const QNetworkRequest &request,
               ArtworkResponse response, QObject *parent)
      : QNetworkReply(parent), m_response(std::move(response)) {
    setOperation(op);
    setRequest(request);
    setUrl(request.url());
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_response.status);
    setHeader(QNetworkRequest::ContentTypeHeader, m_response.type);
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    if (!m_response.held) QTimer::singleShot(0, this, [this] { release(); });
  }
  void abort() override {
    aborted = true;
    if (isFinished()) return;
    setError(OperationCanceledError, "Cancelled");
    setFinished(true);
    emit finished();
  }
  void release() {
    if (isFinished()) return;
    if (m_response.status >= 400) setError(ContentNotFoundError, "Missing image");
    emit readyRead();
    if (isFinished()) return; // Oversize processing may have cancelled us.
    setFinished(true);
    emit finished();
  }
  bool isSequential() const override { return true; }
  qint64 bytesAvailable() const override {
    return m_response.body.size() - m_offset + QNetworkReply::bytesAvailable();
  }
  bool aborted = false;
protected:
  qint64 readData(char *data, qint64 maxSize) override {
    const auto count = qMin(maxSize, qint64(m_response.body.size()) - m_offset);
    if (count <= 0) return -1;
    std::memcpy(data, m_response.body.constData() + m_offset, count);
    m_offset += count;
    return count;
  }
private:
  ArtworkResponse m_response;
  qint64 m_offset = 0;
};

class ArtworkNetwork : public QNetworkAccessManager {
public:
  QList<ArtworkResponse> responses;
  QList<QNetworkRequest> requests;
  QList<Operation> operations;
  QList<QPointer<ArtworkReply>> replies;
protected:
  QNetworkReply *createRequest(Operation op, const QNetworkRequest &request,
                               QIODevice *) override {
    requests << request;
    operations << op;
    ArtworkResponse response;
    if (responses.isEmpty()) {
      ADD_FAILURE() << "Unexpected artwork network request: " << request.url().toString().toStdString();
      response.status = 404;
    } else {
      response = responses.takeFirst();
    }
    auto *reply = new ArtworkReply(op, request, response, this);
    replies << reply;
    return reply;
  }
};

inline QString artworkHash(quint32 appId) {
  return QString::number(appId, 16).rightJustified(40, '0');
}
inline QString hubArtworkUrl(quint32 appId = 1422450) {
  return QStringLiteral("https://shared.fastly.steamstatic.com/community_assets/images/apps/%1/%2.jpg")
      .arg(appId).arg(artworkHash(appId));
}
inline QString publicArtworkUrl(quint32 appId = 1422450) {
  return QStringLiteral("https://avatars.steamstatic.com/%1_full.jpg").arg(artworkHash(appId));
}
inline QByteArray publicArtworkHtml(quint32 appId = 1422450) {
  return "<div class=\"apphub_AppIcon\"><img src=\"" + hubArtworkUrl(appId).toUtf8() + "\"></div>";
}
