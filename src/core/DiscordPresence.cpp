/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "DiscordPresence.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QSignalBlocker>
#include <QUuid>
#include <QtEndian>

namespace TrophyBackend {
namespace {
constexpr quint32 MaxPayload = 64 * 1024;
constexpr auto ApplicationId = "1564273097949974568";
}

DiscordPresence::DiscordPresence(QObject *parent)
    : DiscordPresence(nullptr, parent) {}

DiscordPresence::DiscordPresence(QNetworkAccessManager &network, QObject *parent)
    : DiscordPresence(&network, parent) {}

DiscordPresence::DiscordPresence(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_artwork(network ? *network : m_network), m_paths(socketPaths()) {
  connect(&m_artwork, &DiscordGameArtwork::imageChanged, this, [this] {
    if (wanted() && m_ready && !m_updateTimer.isActive()) m_updateTimer.start();
  });
  m_socket.setReadBufferSize(MaxPayload + 8);
  for (auto *timer : {&m_retryTimer, &m_timeout, &m_updateTimer})
    timer->setSingleShot(true);
  // Presence is state-driven, not a per-second timer update. Discord renders
  // elapsed time from the single start timestamp; coalesce rapid UI changes.
  m_updateTimer.setInterval(5000);
  connect(&m_retryTimer, &QTimer::timeout, this, &DiscordPresence::connectNext);
  connect(&m_timeout, &QTimer::timeout, this, &DiscordPresence::retry);
  connect(&m_updateTimer, &QTimer::timeout, this, &DiscordPresence::publish);
  connect(&m_socket, &QLocalSocket::connected, this, [this] {
    sendFrame(0, QJsonDocument(QJsonObject{{"v", 1}, {"client_id", ApplicationId}})
                     .toJson(QJsonDocument::Compact));
    m_timeout.start(15000);
  });
  connect(&m_socket, &QLocalSocket::readyRead, this, &DiscordPresence::readFrames);
  connect(&m_socket, &QLocalSocket::disconnected, this, &DiscordPresence::retry);
  connect(&m_socket, &QLocalSocket::errorOccurred, this,
          [this](QLocalSocket::LocalSocketError) { retry(); });
}

DiscordPresence::~DiscordPresence() { shutdown(); }

QStringList DiscordPresence::socketPaths() {
  QStringList roots;
  for (const auto *key : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
    const auto path = qEnvironmentVariable(key);
    if (QDir::isAbsolutePath(path)) roots << QDir::cleanPath(path);
  }
  roots << "/tmp";
  roots.removeDuplicates();
  QStringList paths;
  for (const auto &root : roots) {
    for (const auto &subdir : {QString(), QStringLiteral("/app/com.discordapp.Discord")})
      for (int i = 0; i < 10; ++i)
        paths << root + subdir + "/discord-ipc-" + QString::number(i);
  }
  return paths;
}

void DiscordPresence::setPreferences(bool enabled, bool names, bool idle) {
  m_enabled = enabled;
  m_showGameNames = names;
  m_showWhenIdle = idle;
  refreshArtwork();
  refresh();
}

void DiscordPresence::setCaptureState(bool recording, const QString &game,
                                      bool replay, const QString &replayGame,
                                      quint32 recordingAppId, quint32 replayAppId) {
  if (recording && !m_recording)
    m_recordingStart = QDateTime::currentSecsSinceEpoch();
  m_recording = recording;
  m_recordingGame = game;
  m_replay = replay;
  m_replayGame = replayGame;
  m_recordingAppId = recordingAppId;
  m_replayAppId = replayAppId;
  refreshArtwork();
  refresh();
}

void DiscordPresence::refreshArtwork() {
  m_artwork.setGame(wanted() && m_showGameNames
      ? (m_recording ? m_recordingAppId : (m_replay ? m_replayAppId : 0)) : 0);
}

QJsonObject DiscordPresence::activity() const {
  QJsonObject result;
  if (m_recording) {
    result["details"] = "Recording";
    result["timestamps"] = QJsonObject{{"start", m_recordingStart}};
  } else if (m_replay) {
    result["details"] = "Instant replay ready";
  } else {
    result["details"] = "Ready to record";
  }
  const auto game = (m_recording ? m_recordingGame : m_replayGame).simplified();
  if (m_showGameNames && (m_recording || m_replay) && !game.isEmpty())
    result["state"] = game.left(128);
  const auto version = QCoreApplication::applicationVersion().trimmed();
  const auto trophyTooltip = version.isEmpty()
      ? QStringLiteral("Trophy") : QStringLiteral("Trophy %1").arg(version);
  QJsonObject assets{{"large_image", "trophy"}, {"large_text", trophyTooltip}};
  if (m_showGameNames && (m_recording || m_replay) && !m_artwork.imageUrl().isEmpty()) {
    assets["large_image"] = m_artwork.imageUrl();
    assets["large_text"] = game.isEmpty() ? QStringLiteral("Game") : game.left(128);
    assets["small_image"] = "trophy";
    assets["small_text"] = trophyTooltip;
  }
  result["assets"] = assets;
  return result;
}

bool DiscordPresence::wanted() const {
  return !m_stopped && m_enabled && (m_recording || m_replay || m_showWhenIdle);
}

void DiscordPresence::refresh() {
  if (!wanted()) {
    clearAndDisconnect();
  } else if (m_ready) {
    if (!m_updateTimer.isActive()) m_updateTimer.start();
  } else if (m_socket.state() == QLocalSocket::UnconnectedState && !m_retryTimer.isActive()) {
    m_retryTimer.start(0);
  }
}

void DiscordPresence::connectNext() {
  if (!wanted() || m_paths.isEmpty()) return;
  m_timeout.start(2000);
  const auto path = m_paths.at(m_nextPath);
  m_nextPath = (m_nextPath + 1) % m_paths.size();
  m_socket.connectToServer(path);
}

void DiscordPresence::retry() {
  m_ready = false;
  m_timeout.stop();
  m_updateTimer.stop();
  m_pendingNonce.clear();
  m_lastActivity = {};
  m_input.clear();
  // abort() may emit disconnected synchronously; suppress recursive retry.
  const QSignalBlocker blocker(&m_socket);
  m_socket.abort();
  if (wanted()) m_retryTimer.start(m_nextPath == 0 ? 5000 : 50);
}

void DiscordPresence::sendFrame(quint32 opcode, const QByteArray &payload) {
  QByteArray frame(8, '\0');
  qToLittleEndian(opcode, frame.data());
  qToLittleEndian(quint32(payload.size()), frame.data() + 4);
  frame += payload;
  m_socket.write(frame);
}

void DiscordPresence::readFrames() {
  m_input += m_socket.readAll();
  while (m_input.size() >= 8) {
    const auto opcode = qFromLittleEndian<quint32>(m_input.constData());
    const auto size = qFromLittleEndian<quint32>(m_input.constData() + 4);
    if (size > MaxPayload) { retry(); return; }
    if (m_input.size() < 8 + size) return;
    const auto payload = m_input.mid(8, size);
    m_input.remove(0, 8 + size);
    if (opcode == 3) { sendFrame(4, payload); continue; }
    if (opcode == 4) continue;
    if (opcode != 1) { retry(); return; }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) { retry(); return; }
    const auto message = doc.object();
    if (!m_ready && message["cmd"] == "DISPATCH" && message["evt"] == "READY") {
      m_ready = true;
      m_timeout.stop();
      publish();
    } else if (!m_pendingNonce.isEmpty() && message["nonce"].toString() == m_pendingNonce) {
      if (message["evt"] == "ERROR") {
        // A Discord proxy can reject an image that our public probe accepted.
        // Retry with Trophy alone rather than repeatedly sending broken art.
        const auto rejected = m_lastActivity["assets"].toObject()["large_image"].toString();
        if (rejected.startsWith("https://") && rejected == m_artwork.imageUrl())
          m_artwork.rejectCurrentImage();
        retry();
        return;
      }
      m_pendingNonce.clear();
      m_timeout.stop();
      if (activity() != m_lastActivity) m_updateTimer.start();
    }
  }
}

void DiscordPresence::publish() {
  if (!wanted() || !m_ready || !m_pendingNonce.isEmpty()) return;
  const auto next = activity();
  if (next == m_lastActivity) return;
  m_lastActivity = next;
  m_pendingNonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QJsonObject args{{"pid", QCoreApplication::applicationPid()}, {"activity", next}};
  sendFrame(1, QJsonDocument(QJsonObject{{"cmd", "SET_ACTIVITY"}, {"args", args},
                                       {"nonce", m_pendingNonce}}).toJson(QJsonDocument::Compact));
  m_timeout.start(15000);
}

void DiscordPresence::clearAndDisconnect() {
  m_retryTimer.stop();
  m_timeout.stop();
  m_updateTimer.stop();
  if (m_ready && m_socket.state() == QLocalSocket::ConnectedState) {
    sendFrame(1, QJsonDocument(QJsonObject{{"cmd", "SET_ACTIVITY"},
        {"args", QJsonObject{{"pid", QCoreApplication::applicationPid()}, {"activity", QJsonValue::Null}}},
        {"nonce", QUuid::createUuid().toString(QUuid::WithoutBraces)}}).toJson(QJsonDocument::Compact));
    m_socket.flush(); // Nonblocking; disconnect also clears Discord's connection-owned activity.
  }
  m_ready = false;
  m_pendingNonce.clear();
  m_lastActivity = {};
  m_input.clear();
  const QSignalBlocker blocker(&m_socket);
  m_socket.abort();
}

void DiscordPresence::shutdown() {
  m_stopped = true;
  m_artwork.setGame(0);
  clearAndDisconnect();
}
} // namespace TrophyBackend
