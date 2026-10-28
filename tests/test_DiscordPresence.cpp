/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/DiscordPresence.h"
#include "DiscordArtworkFakeNetwork.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalServer>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <gtest/gtest.h>
#include <memory>

namespace TrophyBackend {
class DiscordPresenceTest : public testing::Test {
protected:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "discord-test";
      static char *argv[] = {name, nullptr};
      static QCoreApplication app(argc, argv);
    }
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(server.listen(dir.path() + "/ipc"));
    network = std::make_unique<ArtworkNetwork>();
    presence = std::make_unique<DiscordPresence>(*network);
    EXPECT_EQ(presence->m_updateTimer.interval(), 5000);
    presence->m_updateTimer.setInterval(25);
    presence->m_paths = {server.fullServerName()}; // Never contact real Discord.
  }
  void TearDown() override { presence.reset(); }
  bool waitFor(const std::function<bool()> &predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 2500) QTest::qWait(5);
    return predicate();
  }
  QByteArray frame(quint32 opcode, const QByteArray &payload) {
    QByteArray result(8, '\0');
    qToLittleEndian(opcode, result.data());
    qToLittleEndian(quint32(payload.size()), result.data() + 4);
    return result + payload;
  }
  void send(quint32 opcode, const QJsonObject &object) {
    peer->write(frame(opcode, QJsonDocument(object).toJson(QJsonDocument::Compact)));
    peer->flush();
  }
  QJsonObject receive(quint32 expected = 1) {
    if (!waitFor([&] {
          buffer += peer->readAll();
          return buffer.size() >= 8 && buffer.size() >= 8 + qFromLittleEndian<quint32>(buffer.constData() + 4);
        })) {
      ADD_FAILURE() << "No complete IPC frame";
      return {};
    }
    EXPECT_EQ(qFromLittleEndian<quint32>(buffer.constData()), expected);
    const auto size = qFromLittleEndian<quint32>(buffer.constData() + 4);
    const auto result = QJsonDocument::fromJson(buffer.mid(8, size)).object();
    buffer.remove(0, 8 + size);
    return result;
  }
  void open() {
    ASSERT_TRUE(waitFor([&] { return server.hasPendingConnections(); }));
    peer.reset(server.nextPendingConnection());
    const auto hello = receive(0);
    EXPECT_EQ(hello["client_id"].toString(), "1564273097949974568");
    EXPECT_EQ(hello["v"].toInt(), 1);
    send(1, {{"cmd", "DISPATCH"}, {"evt", "READY"}});
  }
  QJsonObject activity(const QJsonObject &message) { return message["args"].toObject()["activity"].toObject(); }
  void ack(const QJsonObject &message) { send(1, {{"cmd", "SET_ACTIVITY"}, {"nonce", message["nonce"]}}); }
  QTemporaryDir dir;
  QLocalServer server;
  std::unique_ptr<QLocalSocket> peer;
  std::unique_ptr<ArtworkNetwork> network;
  std::unique_ptr<DiscordPresence> presence;
  QByteArray buffer;

  // Friend access stays in the fixture, not the generated TEST_F subclass.
  QJsonObject current() { return presence->activity(); }
  bool retryActive() { return presence->m_retryTimer.isActive(); }
  void expireTimeout() {
    presence->m_timeout.stop();
    QMetaObject::invokeMethod(&presence->m_timeout, "timeout");
  }
  void reconnectNow() {
    presence->m_retryTimer.stop();
    QMetaObject::invokeMethod(&presence->m_retryTimer, "timeout");
  }
  void prependMissingSocket() { presence->m_paths.prepend(dir.path() + "/missing"); }
};

TEST_F(DiscordPresenceTest, DefaultsAndHiddenIdleNeverConnect) {
  presence->setCaptureState(true, "Private game", false, {});
  QTest::qWait(30);
  EXPECT_FALSE(server.hasPendingConnections());
  presence->setCaptureState(false, {}, false, {});
  presence->setPreferences(true, true, false);
  QTest::qWait(30);
  EXPECT_FALSE(server.hasPendingConnections());
}

TEST_F(DiscordPresenceTest, TrophyTooltipIncludesVersionForLargeAndSmallIcons) {
  const auto previous = QCoreApplication::applicationVersion();
  QCoreApplication::setApplicationVersion("v0.9.7-test");
  presence->setPreferences(true, true, true);
  EXPECT_EQ(current()["assets"].toObject()["large_text"].toString(), "Trophy v0.9.7-test");
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml()}
                     << ArtworkResponse{200, "image/jpeg", {}};
  presence->setCaptureState(true, "Deadlock", false, {}, 1422450);
  const bool ready = waitFor([&] {
    return current()["assets"].toObject()["large_image"] == publicArtworkUrl();
  });
  EXPECT_TRUE(ready);
  const auto assets = current()["assets"].toObject();
  EXPECT_EQ(assets["large_text"].toString(), "Deadlock");
  EXPECT_EQ(assets["small_text"].toString(), "Trophy v0.9.7-test");
  QCoreApplication::setApplicationVersion("");
  presence->setCaptureState(false, {}, false, {});
  EXPECT_EQ(current()["assets"].toObject()["large_text"].toString(), "Trophy");
  QCoreApplication::setApplicationVersion(previous);
}

TEST_F(DiscordPresenceTest, PublicGameArtWithTrophyBadgeAndPrivacyFallback) {
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml()}
                     << ArtworkResponse{200, "image/jpeg", {}};
  presence->setCaptureState(true, "Deadlock", false, {}, 1422450);
  EXPECT_TRUE(network->requests.isEmpty()); // Disabled means no web requests.
  EXPECT_EQ(current()["assets"].toObject()["large_image"].toString(), "trophy");
  presence->setPreferences(true, true, false);
  ASSERT_TRUE(waitFor([&] { return current()["assets"].toObject()["large_image"] == publicArtworkUrl(); }));
  open();
  const auto message = receive();
  auto assets = activity(message)["assets"].toObject();
  EXPECT_EQ(assets["large_image"].toString(), publicArtworkUrl());
  EXPECT_EQ(assets["small_image"].toString(), "trophy");
  EXPECT_EQ(assets["large_text"].toString(), "Deadlock");
  ack(message);
  presence->setPreferences(true, false, false);
  assets = activity(receive())["assets"].toObject();
  EXPECT_EQ(assets["large_image"].toString(), "trophy");
  EXPECT_FALSE(assets.contains("small_image"));
  EXPECT_EQ(network->requests.size(), 2);
}

TEST_F(DiscordPresenceTest, TrophyOnlyForIdleUnknownAndFailedGameImages) {
  presence->setCaptureState(true, "Non-Steam game", false, {});
  presence->setPreferences(true, true, true);
  EXPECT_TRUE(network->requests.isEmpty());
  EXPECT_EQ(current()["assets"].toObject()["large_image"].toString(), "trophy");
  EXPECT_FALSE(current()["assets"].toObject().contains("small_image"));
  network->responses << ArtworkResponse{404, "text/html", {}};
  presence->setCaptureState(false, {}, true, "Missing art", 0, 570);
  ASSERT_TRUE(waitFor([&] { return network->responses.isEmpty(); }));
  QTest::qWait(20);
  EXPECT_EQ(current()["assets"].toObject()["large_image"].toString(), "trophy");
  EXPECT_FALSE(current()["assets"].toObject().contains("small_image"));
  presence->setCaptureState(false, {}, false, {});
  EXPECT_EQ(current()["assets"].toObject()["large_image"].toString(), "trophy");
  EXPECT_FALSE(current()["assets"].toObject().contains("small_image"));
}

TEST_F(DiscordPresenceTest, PrivacyDisableCancelsLookupAndRejectsLateResult) {
  presence->setPreferences(true, true, false);
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(), true};
  presence->setCaptureState(true, "Deadlock", false, {}, 1422450);
  auto reply = network->replies.last();
  presence->setPreferences(true, false, false);
  ASSERT_TRUE(reply);
  EXPECT_TRUE(reply->aborted);
  reply->release();
  EXPECT_EQ(current()["assets"].toObject()["large_image"].toString(), "trophy");
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(), true};
  presence->setPreferences(true, true, false);
  reply = network->replies.last();
  presence->setPreferences(false, true, false);
  ASSERT_TRUE(reply);
  EXPECT_TRUE(reply->aborted);
  reply->release();
  EXPECT_EQ(current()["assets"].toObject()["large_image"].toString(), "trophy");
}

TEST_F(DiscordPresenceTest, RecordingToReplaySwitchAndRpcImageRejection) {
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml()}
                     << ArtworkResponse{200, "image/jpeg", {}};
  presence->setPreferences(true, true, false);
  presence->setCaptureState(true, "Deadlock", true, "Dota", 1422450, 570);
  ASSERT_TRUE(waitFor([&] { return current()["assets"].toObject()["large_image"] == publicArtworkUrl(); }));
  open();
  auto message = receive();
  ack(message);
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(570)}
                     << ArtworkResponse{200, "image/jpeg", {}};
  presence->setCaptureState(false, {}, true, "Dota", 0, 570);
  // Never keep the predecessor's icon while the successor lookup runs.
  EXPECT_EQ(current()["assets"].toObject()["large_image"].toString(), "trophy");
  ASSERT_TRUE(waitFor([&] { return current()["assets"].toObject()["large_image"] == publicArtworkUrl(570); }));
  message = receive();
  EXPECT_EQ(activity(message)["assets"].toObject()["large_image"].toString(), publicArtworkUrl(570));
  send(1, {{"evt", "ERROR"}, {"nonce", message["nonce"]}});
  ASSERT_TRUE(waitFor([&] { return peer->state() == QLocalSocket::UnconnectedState; }));
  reconnectNow();
  buffer.clear();
  open();
  const auto assets = activity(receive())["assets"].toObject();
  EXPECT_EQ(assets["large_image"].toString(), "trophy");
  EXPECT_FALSE(assets.contains("small_image"));
}

TEST_F(DiscordPresenceTest, OldRpcImageFailureDoesNotRejectSuccessorArtwork) {
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml()}
                     << ArtworkResponse{200, "image/jpeg", {}};
  presence->setPreferences(true, true, false);
  presence->setCaptureState(true, "Deadlock", false, {}, 1422450);
  ASSERT_TRUE(waitFor([&] { return current()["assets"].toObject()["large_image"] == publicArtworkUrl(); }));
  open();
  const auto old = receive(); // Hold the previous game's acknowledgement.
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(570)}
                     << ArtworkResponse{200, "image/jpeg", {}};
  presence->setCaptureState(false, {}, true, "Dota", 0, 570);
  ASSERT_TRUE(waitFor([&] { return current()["assets"].toObject()["large_image"] == publicArtworkUrl(570); }));
  send(1, {{"evt", "ERROR"}, {"nonce", old["nonce"]}});
  ASSERT_TRUE(waitFor([&] { return peer->state() == QLocalSocket::UnconnectedState; }));
  reconnectNow();
  buffer.clear();
  open();
  EXPECT_EQ(activity(receive())["assets"].toObject()["large_image"].toString(), publicArtworkUrl(570));
}

TEST_F(DiscordPresenceTest, RecordingWinsAndPrivacyPreservesTimestamp) {
  presence->setCaptureState(true, "Deadlock", true, "Other game");
  presence->setPreferences(true, true, false);
  open();
  const auto first = receive();
  EXPECT_EQ(first["cmd"].toString(), "SET_ACTIVITY");
  EXPECT_EQ(activity(first)["details"].toString(), "Recording");
  EXPECT_EQ(activity(first)["state"].toString(), "Deadlock");
  EXPECT_GT(activity(first)["timestamps"].toObject()["start"].toInteger(), 0);
  ack(first);
  presence->setPreferences(true, false, false);
  const auto next = receive();
  EXPECT_FALSE(activity(next).contains("state"));
  EXPECT_EQ(activity(next)["timestamps"], activity(first)["timestamps"]);
  ack(next);
  presence->setCaptureState(false, {}, true, "Other game");
  const auto replay = receive();
  EXPECT_EQ(activity(replay)["details"].toString(), "Instant replay ready");
  EXPECT_FALSE(activity(replay).contains("timestamps"));
  EXPECT_FALSE(activity(replay).contains("state"));
}

TEST_F(DiscordPresenceTest, IdleClearingDisableAndShutdown) {
  presence->setPreferences(true, true, true);
  open();
  auto message = receive();
  EXPECT_EQ(activity(message)["details"].toString(), "Ready to record");
  ack(message);
  presence->setPreferences(true, true, false);
  message = receive();
  EXPECT_TRUE(message["args"].toObject()["activity"].isNull());
  EXPECT_FALSE(retryActive());
  presence->setPreferences(true, true, true);
  buffer.clear();
  open();
  receive();
  presence->setPreferences(false, true, true);
  EXPECT_TRUE(receive()["args"].toObject()["activity"].isNull());
  presence->shutdown();
  presence->setPreferences(true, true, true);
  QTest::qWait(30);
  EXPECT_FALSE(server.hasPendingConnections());
}

TEST_F(DiscordPresenceTest, FragmentedReadyPingAndCoalescedFrames) {
  presence->setPreferences(true, true, true);
  ASSERT_TRUE(waitFor([&] { return server.hasPendingConnections(); }));
  peer.reset(server.nextPendingConnection());
  receive(0);
  const auto ready = frame(1, R"({"cmd":"DISPATCH","evt":"READY"})");
  peer->write(ready.left(5));
  peer->flush();
  QTest::qWait(20);
  EXPECT_EQ(peer->bytesAvailable(), 0);
  peer->write(ready.mid(5) + frame(3, "{\"ping\":1}"));
  peer->flush();
  receive();
  EXPECT_EQ(receive(4)["ping"].toInt(), 1);
}

TEST_F(DiscordPresenceTest, MalformedOversizedAndTimeoutReconnect) {
  presence->setPreferences(true, true, true);
  open();
  receive();
  peer->write(frame(1, "not json"));
  peer->flush();
  ASSERT_TRUE(waitFor([&] { return peer->state() == QLocalSocket::UnconnectedState; }));
  EXPECT_TRUE(retryActive());
  expireTimeout(); // Force retry immediately without waiting for the backoff.
  presence->setPreferences(false, true, true);
  presence->setPreferences(true, true, true);
  buffer.clear();
  open();
  receive();
  QByteArray header(8, '\0');
  qToLittleEndian(quint32(1), header.data());
  qToLittleEndian(quint32(65537), header.data() + 4);
  peer->write(header);
  peer->flush();
  EXPECT_TRUE(waitFor([&] { return peer->state() == QLocalSocket::UnconnectedState; }));
}

TEST_F(DiscordPresenceTest, PendingAcknowledgementCoalescesLatestState) {
  presence->setPreferences(true, true, true);
  open();
  const auto first = receive();
  presence->setCaptureState(true, "One", false, {});
  presence->setCaptureState(true, "Two", false, {});
  QTest::qWait(1100);
  EXPECT_EQ(peer->bytesAvailable(), 0);
  ack(first);
  const auto latest = receive();
  EXPECT_EQ(activity(latest)["state"].toString(), "Two");
  ack(latest);
  QTest::qWait(1100);
  EXPECT_EQ(peer->bytesAvailable(), 0);
}

TEST_F(DiscordPresenceTest, MissingSocketThenHandshakeTimeoutAndReconnect) {
  prependMissingSocket();
  presence->setPreferences(true, true, true);
  ASSERT_TRUE(waitFor([&] { return server.hasPendingConnections(); }));
  peer.reset(server.nextPendingConnection());
  receive(0); // Hold READY, then fire the production watchdog.
  expireTimeout();
  EXPECT_TRUE(waitFor([&] { return peer->state() == QLocalSocket::UnconnectedState; }));
  reconnectNow();
  buffer.clear();
  open();
  auto message = receive();
  EXPECT_EQ(activity(message)["details"].toString(), "Ready to record");
  // An unacknowledged SET_ACTIVITY must also retire the connection.
  expireTimeout();
  EXPECT_TRUE(waitFor([&] { return peer->state() == QLocalSocket::UnconnectedState; }));
}

TEST_F(DiscordPresenceTest, RpcErrorAndDiscordRestartRepublishLatestState) {
  presence->setCaptureState(false, {}, true, "Deadlock");
  presence->setPreferences(true, true, false);
  open();
  auto message = receive();
  send(1, {{"evt", "ERROR"}, {"nonce", message["nonce"]}});
  EXPECT_TRUE(waitFor([&] { return peer->state() == QLocalSocket::UnconnectedState; }));
  presence->setCaptureState(true, "New game", false, {});
  reconnectNow();
  buffer.clear();
  open();
  message = receive();
  EXPECT_EQ(activity(message)["details"].toString(), "Recording");
  EXPECT_EQ(activity(message)["state"].toString(), "New game");
  ack(message);
  peer->abort();
  ASSERT_TRUE(waitFor([&] { return retryActive(); }));
  reconnectNow();
  buffer.clear();
  open();
  EXPECT_EQ(activity(receive())["timestamps"], activity(message)["timestamps"]);
}
} // namespace TrophyBackend
