/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/DiscordGameArtwork.h"
#include "DiscordArtworkFakeNetwork.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <memory>

namespace TrophyBackend {

class DiscordGameArtworkTest : public testing::Test {
protected:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "artwork-test";
      static char *argv[] = {name, nullptr};
      static QCoreApplication app(argc, argv);
    }
    network = std::make_unique<ArtworkNetwork>();
    artwork = std::make_unique<DiscordGameArtwork>(*network);
  }
  bool waitFor(const std::function<bool()> &predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 2000) QTest::qWait(5);
    return predicate();
  }
  void success(quint32 id = 1422450) {
    network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(id)}
                       << ArtworkResponse{200, "image/jpeg", {}};
  }
  void expireDeadline() {
    artwork->m_deadline.stop();
    QMetaObject::invokeMethod(&artwork->m_deadline, "timeout");
  }
  std::unique_ptr<ArtworkNetwork> network;
  std::unique_ptr<DiscordGameArtwork> artwork;
};

TEST_F(DiscordGameArtworkTest, OnlyAcceptsPublicIconForExactGame) {
  EXPECT_EQ(DiscordGameArtwork::publicIconUrl(publicArtworkHtml(), 1422450), publicArtworkUrl());
  EXPECT_TRUE(DiscordGameArtwork::publicIconUrl(publicArtworkHtml(), 730).isEmpty());
  for (const auto &url : {QString("file:///tmp/icon.jpg"),
                          QString("https://steamstatic.com.attacker.test/icon.jpg"),
                          hubArtworkUrl() + "?token=secret",
                          QString(hubArtworkUrl()).replace("https://", "http://"),
                          QString(hubArtworkUrl()).replace("shared.fastly", "user:password@shared.fastly"),
                          QString(hubArtworkUrl()).replace(".com/", ".com:443/")}) {
    const auto html = "<div class='apphub_AppIcon'><img src='" + url.toUtf8() + "'></div>";
    EXPECT_TRUE(DiscordGameArtwork::publicIconUrl(html, 1422450).isEmpty());
  }
  EXPECT_TRUE(DiscordGameArtwork::publicIconUrl("<img src='" + hubArtworkUrl().toUtf8() + "'>", 1422450).isEmpty());
}

TEST_F(DiscordGameArtworkTest, ValidatesImageBeforePublishingAndCachesResult) {
  success();
  artwork->setGame(1422450);
  EXPECT_TRUE(artwork->imageUrl().isEmpty());
  ASSERT_TRUE(waitFor([&] { return !artwork->imageUrl().isEmpty(); }));
  EXPECT_EQ(artwork->imageUrl(), publicArtworkUrl());
  ASSERT_EQ(network->requests.size(), 2);
  EXPECT_EQ(network->operations[0], QNetworkAccessManager::GetOperation);
  EXPECT_EQ(network->operations[1], QNetworkAccessManager::HeadOperation);
  EXPECT_EQ(network->requests[0].url().toString(), "https://steamcommunity.com/app/1422450");
  EXPECT_EQ(network->requests[1].url().toString(), publicArtworkUrl());
  EXPECT_TRUE(network->requests[1].url().path().endsWith("_full.jpg"));
  for (const auto &request : network->requests) {
    EXPECT_EQ(request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt(), QNetworkRequest::Manual);
    EXPECT_EQ(request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt(), QNetworkRequest::Manual);
    EXPECT_EQ(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), QNetworkRequest::ManualRedirectPolicy);
  }
  artwork->setGame(0);
  artwork->setGame(1422450);
  EXPECT_EQ(artwork->imageUrl(), publicArtworkUrl());
  EXPECT_EQ(network->requests.size(), 2);
}

TEST_F(DiscordGameArtworkTest, MissingInvalidRedirectedAndNonImageStayEmpty) {
  quint32 id = 100;
  for (const auto &response : {ArtworkResponse{404, "text/html", "missing"},
                               ArtworkResponse{302, "text/html", "redirect"},
                               ArtworkResponse{200, "text/html", "no icon"},
                               ArtworkResponse{200, "text/html", QByteArray(262145, 'x')}}) {
    network->responses << response;
    artwork->setGame(id++);
    ASSERT_TRUE(waitFor([&] { return network->responses.isEmpty(); }));
    QTest::qWait(10);
    EXPECT_TRUE(artwork->imageUrl().isEmpty());
  }
  for (const auto &response : {ArtworkResponse{404, "image/jpeg", {}},
                               ArtworkResponse{200, "text/html", {}},
                               ArtworkResponse{302, "image/jpeg", {}}}) {
    network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(id)} << response;
    artwork->setGame(id++);
    ASSERT_TRUE(waitFor([&] { return network->responses.isEmpty(); }));
    QTest::qWait(10);
    EXPECT_TRUE(artwork->imageUrl().isEmpty());
  }
  const auto count = network->requests.size();
  artwork->setGame(0);
  artwork->setGame(id - 1);
  EXPECT_EQ(network->requests.size(), count); // Negative results are cached too.
}

TEST_F(DiscordGameArtworkTest, CancelSwitchAndShutdownRetireReplies) {
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(), true};
  artwork->setGame(1422450);
  auto old = network->replies.last();
  success(730);
  artwork->setGame(730);
  ASSERT_TRUE(old);
  EXPECT_TRUE(old->aborted);
  old->release();
  ASSERT_TRUE(waitFor([&] { return artwork->imageUrl() == publicArtworkUrl(730); }));
  artwork->setGame(0);
  EXPECT_TRUE(artwork->imageUrl().isEmpty());
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(570), true};
  artwork->setGame(570);
  auto held = network->replies.last();
  artwork.reset();
  ASSERT_TRUE(held);
  EXPECT_TRUE(held->aborted);
}

TEST_F(DiscordGameArtworkTest, RejectionFallsBackAndDoesNotImmediatelyRetry) {
  success();
  artwork->setGame(1422450);
  ASSERT_TRUE(waitFor([&] { return !artwork->imageUrl().isEmpty(); }));
  artwork->rejectCurrentImage();
  EXPECT_TRUE(artwork->imageUrl().isEmpty());
  artwork->setGame(0);
  artwork->setGame(1422450);
  EXPECT_TRUE(artwork->imageUrl().isEmpty());
  EXPECT_EQ(network->requests.size(), 2);
}

TEST_F(DiscordGameArtworkTest, PendingImageProbeIsCancelledOnSwitch) {
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml()}
                     << ArtworkResponse{200, "image/jpeg", {}, true};
  artwork->setGame(1422450);
  ASSERT_TRUE(waitFor([&] { return network->requests.size() == 2; }));
  auto probe = network->replies.last();
  artwork->setGame(0);
  ASSERT_TRUE(probe);
  EXPECT_TRUE(probe->aborted);
  probe->release();
  EXPECT_TRUE(artwork->imageUrl().isEmpty());
}

TEST_F(DiscordGameArtworkTest, DeadlineAbortsMetadataAndImageProbe) {
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(), true};
  artwork->setGame(1422450);
  auto held = network->replies.last();
  expireDeadline();
  ASSERT_TRUE(held);
  EXPECT_TRUE(held->aborted);
  EXPECT_TRUE(artwork->imageUrl().isEmpty());
  network->responses << ArtworkResponse{200, "text/html", publicArtworkHtml(570)}
                     << ArtworkResponse{200, "image/jpeg", {}, true};
  artwork->setGame(570);
  ASSERT_TRUE(waitFor([&] { return network->requests.size() == 3; }));
  held = network->replies.last();
  expireDeadline();
  ASSERT_TRUE(held);
  EXPECT_TRUE(held->aborted);
  EXPECT_TRUE(artwork->imageUrl().isEmpty());
}
} // namespace TrophyBackend
