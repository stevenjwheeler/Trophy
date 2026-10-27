/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "UpdateInstaller.h"

#include <QCoreApplication>
#include <QDebug>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>

#include <cstdio>

namespace TrophyBackend {

UpdateInstaller::UpdateInstaller(QObject *parent)
    : QObject(parent), m_network(this) {
  m_timeout.setSingleShot(true);
  m_timeout.setInterval(300000);
  connect(&m_timeout, &QTimer::timeout, this, [this]() {
    if (m_reply) {
      m_reply->abort();
    }
  });
}

void UpdateInstaller::setStatus(const QString &s) {
  if (m_status != s) {
    m_status = s;
    emit statusChanged();
  }
}

void UpdateInstaller::setBusy(bool b) {
  if (m_busy != b) {
    m_busy = b;
    emit busyChanged();
  }
}

void UpdateInstaller::fail(const QString &msg) {
  setStatus(msg);
  setBusy(false);
  if (m_reply) {
    m_reply->abort();
    m_reply->deleteLater();
    m_reply = nullptr;
  }
  m_timeout.stop();
  // Clean up partial download.
  if (!m_tempPath.isEmpty())
    QFile::remove(m_tempPath);
  emit finished(false, msg);
}

void UpdateInstaller::installAppImage(const QString &url, const QString &digest,
                                     const QString &targetPath) {
  if (m_busy) {
    emit finished(false, QStringLiteral("An update is already in progress."));
    return;
  }
  if (m_recording) {
    fail(QStringLiteral("Recording in progress; update aborted."));
    return;
  }
  if (m_forceFail) {
    fail(QStringLiteral("Forced failure (TROPHY_FORCE_FAIL_UPDATE)."));
    return;
  }
  if (targetPath.isEmpty()) {
    qWarning("UpdateInstaller: no AppImage target path (APPIMAGE env unset); "
             "cannot self-update. asset=%s",
             url.toUtf8().constData());
    emit finished(false, QStringLiteral("No AppImage path available."));
    return;
  }
  qDebug("UpdateInstaller: starting AppImage update url=%s target=%s",
         url.toUtf8().constData(), targetPath.toUtf8().constData());
  if (url.isEmpty()) {
    emit finished(false, QStringLiteral("No update asset URL."));
    return;
  }

  setBusy(true);
  m_progress = 0;
  m_total = 0;
  emit progressChanged();
  m_targetPath = targetPath;
  m_digest = digest;
  setStatus(QStringLiteral("Downloading update…"));

  // Temp file in the same directory as the target for an atomic rename.
  const QFileInfo tfi(targetPath);
  const QString dir = tfi.absolutePath();
  const QString tempName =
      tfi.fileName() + QStringLiteral(".update-") +
      QString::number(QDateTime::currentMSecsSinceEpoch());
  m_tempPath = dir + QLatin1Char('/') + tempName;

  QNetworkRequest req{QUrl(url)};
  req.setHeader(QNetworkRequest::UserAgentHeader, "Trophy UpdateInstaller");
  m_reply = m_network.get(req);
  connect(m_reply, &QNetworkReply::downloadProgress, this,
          [this](qint64 recv, qint64 total) {
            m_progress = recv;
            m_total = total;
            emit progressChanged();
          });
  connect(m_reply, &QNetworkReply::finished, this, [this]() {
    m_timeout.stop();
    QPointer<QNetworkReply> reply = m_reply;
    m_reply = nullptr;
    if (!reply)
      return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
      fail(QStringLiteral("Download failed: %1")
               .arg(reply->errorString()));
      return;
    }

    // Write body to temp file.
    const QByteArray data = reply->readAll();
    QFile out(m_tempPath);
    if (!out.open(QIODevice::WriteOnly) ||
        out.write(data) != data.size() || !out.flush()) {
      fail(QStringLiteral("Failed to write update file."));
      return;
    }
    out.close();

    // Verify digest if provided.
    if (!m_digest.isEmpty()) {
      // GitHub digest is "sha256:<hex>".
      QString expected = m_digest;
      if (expected.startsWith(QStringLiteral("sha256:"), Qt::CaseInsensitive))
        expected = expected.mid(7);
      const QByteArray actual =
          QCryptographicHash::hash(data, QCryptographicHash::Sha256)
              .toHex();
      if (!expected.isEmpty() &&
          !QString::fromLatin1(actual).compare(expected,
                                               Qt::CaseInsensitive) == 0) {
        fail(QStringLiteral("Checksum mismatch; update aborted."));
        return;
      }
    }

    // Make executable.
    QFile::setPermissions(m_tempPath, QFileDevice::ReadOwner |
                                          QFileDevice::WriteOwner |
                                          QFileDevice::ExeOwner |
                                          QFileDevice::ReadGroup |
                                          QFileDevice::ExeGroup |
                                          QFileDevice::ReadOther |
                                          QFileDevice::ExeOther);

    setStatus(QStringLiteral("Installing update…"));
    spawnHelperAndQuit(m_tempPath, m_targetPath);
  });
  m_timeout.start();
}

void UpdateInstaller::simulate() {
  if (m_busy) {
    emit finished(false, QStringLiteral("An update is already in progress."));
    return;
  }
  if (m_recording) {
    fail(QStringLiteral("Recording in progress; update aborted."));
    return;
  }
  if (m_forceFail) {
    fail(QStringLiteral("Forced failure (TROPHY_FORCE_FAIL_UPDATE)."));
    return;
  }
  setBusy(true);
  m_progress = 0;
  m_total = 100;
  emit progressChanged();
  setStatus(QStringLiteral("Downloading update…"));

  // Drive progress with a repeating timer.
  QTimer *t = new QTimer(this);
  t->setInterval(120);
  connect(t, &QTimer::timeout, this, [this, t]() {
    m_progress += 8;
    if (m_progress >= m_total) {
      m_progress = m_total;
      emit progressChanged();
      t->stop();
      t->deleteLater();
      setStatus(QStringLiteral("Installing update…"));
      QTimer::singleShot(600, this, [this]() {
        setBusy(false);
        setStatus(QStringLiteral("Update complete (simulated)."));
        emit finished(true, QStringLiteral("Update simulated successfully."));
      });
    } else {
      emit progressChanged();
    }
  });
  t->start();
}

namespace {
// Shell-quote a path so it survives the generated helper script even
// when it contains spaces, quotes or other metacharacters.
QString shellQuote(const QString &s) {
  QString out = QStringLiteral("'");
  for (const QChar &c : s) {
    if (c == QLatin1Char('\''))
      out += QStringLiteral("'\\''");
    else
      out += c;
  }
  out += QLatin1Char('\'');
  return out;
}
} // namespace

void UpdateInstaller::spawnHelperAndQuit(const QString &newFile,
                                        const QString &targetPath) {
  // Write a helper script that waits for this process to exit, then
  // swaps the files and relaunches.
  const QString helperPath =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
      QStringLiteral("/trophy-update-helper.sh");
  QFile helper(helperPath);
  if (!helper.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    fail(QStringLiteral("Failed to create update helper."));
    return;
  }
  const QByteArray script = QByteArray(
      "#!/bin/sh\n"
      "PARENT=") +
      QByteArray::number(QCoreApplication::applicationPid()) +
      QByteArray("\nNEW=") +
      shellQuote(newFile).toUtf8() +
      QByteArray("\nTARGET=") +
      shellQuote(targetPath).toUtf8() +
      QByteArray(
          "\n"
          "while kill -0 \"$PARENT\" 2>/dev/null; do sleep 0.2; done\n"
          "if [ ! -w \"$(dirname \"$TARGET\")\" ]; then\n"
          "  echo 'Trophy update: target directory not writable; run from a user-writable location.' >&2\n"
          "  rm -f \"$0\"\n"
          "  exit 1\n"
          "fi\n"
          "cp -f \"$TARGET\" \"$TARGET.old\" 2>/dev/null\n"
          "if mv -f \"$NEW\" \"$TARGET\"; then\n"
          "  chmod +x \"$TARGET\"\n"
          "  if [ -x \"$TARGET\" ]; then\n"
          "    rm -f \"$TARGET.old\"\n"
          "  else\n"
          "    mv -f \"$TARGET.old\" \"$TARGET\"\n"
          "  fi\n"
          "else\n"
          "  mv -f \"$TARGET.old\" \"$TARGET\" 2>/dev/null\n"
          "fi\n"
          "\"$TARGET\" &\n"
          "rm -f \"$0\"\n");
  helper.write(script);
  helper.close();
  QFile::setPermissions(helperPath, QFileDevice::ReadOwner |
                                        QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner |
                                        QFileDevice::ReadGroup |
                                        QFileDevice::ExeGroup |
                                        QFileDevice::ReadOther |
                                        QFileDevice::ExeOther);

  // Launch detached.
  QProcess::startDetached(helperPath, QStringList());

  setBusy(false);
  setStatus(QStringLiteral("Restarting to apply update…"));
  emit finished(true, QStringLiteral("Update installed; restarting."));
  QCoreApplication::quit();
}

} // namespace TrophyBackend