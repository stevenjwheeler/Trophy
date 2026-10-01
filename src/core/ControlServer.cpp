/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControlServer.h"

#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

namespace TrophyBackend {

namespace {
const char *kSocketName = "trophy-control.sock";
} // namespace

QString ControlServer::defaultSocketPath() {
  return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
         "/" + QString::fromLatin1(kSocketName);
}

ControlServer::ControlServer(QObject *parent)
    : ControlServer(defaultSocketPath(), parent) {}

ControlServer::ControlServer(const QString &socketPath, QObject *parent)
    : QObject(parent), m_socketPath(socketPath) {
  m_server = new QLocalServer(this);
  connect(m_server, &QLocalServer::newConnection, this,
          &ControlServer::onNewConnection);
}

ControlServer::~ControlServer() { close(); }

bool ControlServer::isListening() const {
  return m_server && m_server->isListening();
}

bool ControlServer::listen() {
  if (isListening())
    return true;
  // Drop a stale socket file from a crashed run, then listen. A live
  // instance would have answered sendCommand first, so removing here
  // cannot steal another instance's socket.
  QLocalServer::removeServer(m_socketPath);
  if (!m_server->listen(m_socketPath)) {
    qWarning() << "Control socket failed to listen:" << m_server->errorString();
    return false;
  }
  qInfo() << "Control socket listening on" << m_socketPath;
  return true;
}

void ControlServer::close() {
  if (m_server && m_server->isListening()) {
    m_server->close();
    QLocalServer::removeServer(m_socketPath);
  }
}

bool ControlServer::sendCommand(const QString &command, int timeoutMs) {
  return sendCommandTo(defaultSocketPath(), command, timeoutMs);
}

bool ControlServer::sendCommandTo(const QString &socketPath,
                                  const QString &command, int timeoutMs) {
  QLocalSocket socket;
  socket.connectToServer(socketPath);
  if (!socket.waitForConnected(timeoutMs))
    return false;
  socket.write((command.trimmed() + "\n").toUtf8());
  if (!socket.waitForBytesWritten(timeoutMs))
    return false;
  socket.disconnectFromServer();
  return true;
}

void ControlServer::onNewConnection() {
  QLocalSocket *client = m_server ? m_server->nextPendingConnection() : nullptr;
  if (!client)
    return;
  connect(client, &QLocalSocket::readyRead, this, [this, client]() {
    while (client->canReadLine()) {
      handleLine(QString::fromUtf8(client->readLine()).trimmed());
    }
  });
  connect(client, &QLocalSocket::disconnected, client,
          &QLocalSocket::deleteLater);
  // In case the command already arrived with the connection.
  if (client->canReadLine()) {
    while (client->canReadLine()) {
      handleLine(QString::fromUtf8(client->readLine()).trimmed());
    }
  }
}

void ControlServer::handleLine(const QString &line) {
  if (line == QLatin1String("toggle-record")) {
    emit toggleRecordingRequested();
  } else if (line == QLatin1String("save-clip")) {
    emit saveClipRequested();
  } else if (line == QLatin1String("show-window")) {
    emit showWindowRequested();
  } else if (!line.isEmpty()) {
    qWarning() << "Control socket ignoring unknown command:" << line;
  }
}

} // namespace TrophyBackend
