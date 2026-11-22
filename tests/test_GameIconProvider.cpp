/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "detection/GameIconProvider.h"
#include "detection/GameListModel.h"
#include "detection/DefaultAppCatalog.h"

#include <QDir>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QSignalSpy>
#include <QPointer>
#include <QJSValue>
#include <gtest/gtest.h>
#include <functional>
#include <memory>

using namespace TrophyBackend;

namespace {
class GameIconTest : public testing::Test {
protected:
  QTemporaryDir temp;
  QString root() const { return temp.path(); }
  template<class Predicate> bool waitUntil(Predicate condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 2000)
      QTest::qWait(1);
    return condition();
  }
  void write(const QString &relative, const QByteArray &bytes) {
    const QString path = root() + '/' + relative;
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
  }
  void icon(const QString &relative, QColor color = Qt::green) {
    const QString path = root() + '/' + relative;
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QImage image(64, 64, QImage::Format_ARGB32);
    image.fill(color);
    ASSERT_TRUE(image.save(path));
  }
  QString id(const QString &key, uint appId = 0) {
    GameListEntry entry;
    entry.key = key;
    entry.steamAppId = appId;
    GameListModel model;
    model.setEntries({entry});
    const QUrl url = model.data(model.index(0), GameListModel::IconSourceRole).toUrl();
    EXPECT_EQ(url.scheme(), "image");
    EXPECT_EQ(url.host(), "gameicon");
    return url.path().mid(1);
  }
  QImage load(GameIconProvider &provider, const QString &key, uint appId = 0) {
    QSize size;
    const auto image = provider.requestImage(id(key, appId), &size, {40, 40});
    EXPECT_EQ(size, image.size());
    // The provider's transparent miss sentinel preserves UI fallback state.
    return image.size() == QSize(1, 1) ? QImage() : image;
  }
  QString cacheFile() const { return root() + "/cache/game-icons.json"; }
  void expireSources() {
    QFile file(cacheFile());
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    auto document = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    auto entries = document.value("entries").toObject();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
      auto entry = it.value().toObject();
      entry.insert("discovered", "0");
      it.value() = entry;
    }
    document.insert("entries", entries);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_GT(file.write(QJsonDocument(document).toJson()), 0);
  }
};

TEST_F(GameIconTest, SteamIconWithoutThemeIndex) {
  icon("icons/hicolor/48x48/apps/steam_icon_1422450.png");
  GameIconProvider provider({root()}, {}, "absent", {});
  const auto image = load(provider, "steam_app_1422450", 1422450);
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.size(), QSize(40, 40));
  EXPECT_EQ(image.pixelColor(20, 20), QColor(Qt::green));
  EXPECT_TRUE(provider.flags().testFlag(QQmlImageProviderBase::ForceAsynchronousImageLoading));
}

TEST_F(GameIconTest, LegacySteamCacheAndCorruptThemeFallback) {
  write("icons/hicolor/48x48/apps/steam_icon_42.png", "not an image");
  icon("steam/appcache/librarycache/42_icon.jpg");
  GameIconProvider provider({root()}, {}, "", {root() + "/steam"});
  EXPECT_FALSE(load(provider, "steam_app_42", 42).isNull());
}

TEST_F(GameIconTest, HashedPerAppSteamIconIsFoundAndCached) {
  const QString artwork = "steam/appcache/librarycache/1659040/" +
      QString(40, 'a') + ".jpg";
  icon(artwork, Qt::cyan);
  {
    GameIconProvider provider({root()}, {}, "", {root() + "/steam"}, cacheFile());
    const auto image = load(provider, "steam_app_1659040", 1659040);
    ASSERT_FALSE(image.isNull());
    EXPECT_GT(image.pixelColor(20, 20).green(), 240);
    EXPECT_GT(image.pixelColor(20, 20).blue(), 240);
  }
  QFile cache(cacheFile());
  ASSERT_TRUE(cache.open(QIODevice::ReadOnly));
  const auto entries = QJsonDocument::fromJson(cache.readAll()).object()["entries"].toObject();
  EXPECT_EQ(entries.value(id("steam_app_1659040", 1659040)).toObject()["path"].toString(),
            root() + "/" + artwork);
  GameIconProvider reopened({root()}, {}, "", {root() + "/steam"}, cacheFile());
  EXPECT_FALSE(load(reopened, "steam_app_1659040", 1659040).isNull());
}

TEST_F(GameIconTest, ModernSteamLookupDoesNotUseCapsulesOrOtherAppArtwork) {
  icon("steam/appcache/librarycache/43/" + QString(40, 'a') + ".png");
  icon("steam/appcache/librarycache/42/" + QString(40, 'b') + "/icon.png");
  write("steam/appcache/librarycache/42/" + QString(40, 'c') + ".jpg", "corrupt");
  QImage banner(256, 128, QImage::Format_RGB32);
  banner.fill(Qt::red);
  ASSERT_TRUE(banner.save(root() + "/steam/appcache/librarycache/42/" + QString(40, 'd') + ".png"));
  GameIconProvider provider({root()}, {}, "", {root() + "/steam"});
  EXPECT_TRUE(load(provider, "steam_app_42", 42).isNull());
}

TEST_F(GameIconTest, OldPolicyCachedSteamMissDoesNotHideModernIcon) {
  icon("steam/appcache/librarycache/42/" + QString(40, 'a') + ".png");
  QJsonArray context{2, "", QJsonArray{root()},
                     QJsonArray{root() + "/icons", root() + "/pixmaps"},
                     QJsonArray{root() + "/steam"}};
  for (const auto &app : defaultAppCatalog())
    context.append(QJsonArray{app.key, app.artwork,
                             QJsonArray::fromStringList(app.aliases),
                             QJsonArray::fromStringList(app.icons)});
  const auto fingerprint = QString::fromLatin1(QCryptographicHash::hash(
      QJsonDocument(context).toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256).toHex());
  write("cache/game-icons.json", QJsonDocument(QJsonObject{
      {"fingerprint", fingerprint},
      {"entries", QJsonObject{{id("steam_app_42", 42), QJsonObject{
          {"path", ""}, {"discovered", QString::number(QDateTime::currentMSecsSinceEpoch())}}}}}
  }).toJson());
  GameIconProvider provider({root()}, {}, "", {root() + "/steam"}, cacheFile());
  EXPECT_FALSE(load(provider, "steam_app_42", 42).isNull());
}

TEST_F(GameIconTest, MissingValidIconReturnsTransparentSentinel) {
  GameIconProvider provider({root()}, {}, "", {});
  QSize size;
  const auto image = provider.requestImage(id("steam_app_42", 42), &size, {40, 40});
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(size, QSize(1, 1));
  EXPECT_EQ(image.pixelColor(0, 0).alpha(), 0);
}

TEST_F(GameIconTest, DesktopIdAndWindowClassUseLocalTheme) {
  write("applications/org.example.Game.desktop",
        "[Desktop Entry]\nIcon=example-game\nStartupWMClass=ExampleGame\n");
  icon("icons/hicolor/48x48/apps/example-game.png");
  GameIconProvider provider({root()}, {}, "", {});
  EXPECT_FALSE(load(provider, "org.example.game").isNull());
  EXPECT_FALSE(load(provider, "examplegame").isNull());
}

TEST_F(GameIconTest, NativeExecutableMatchesFullDesktopExecOnly) {
  write("applications/game.desktop",
        "[Desktop Entry]\nIcon=example\nExec=\"/games/A Game/game\" --launch %U\n");
  icon("icons/hicolor/48x48/apps/example.png");
  GameIconProvider provider({root()}, {}, "", {});
  EXPECT_FALSE(load(provider, "exe:/games/a game/game").isNull());
  EXPECT_TRUE(load(provider, "exe:/another/game").isNull());
}

TEST_F(GameIconTest, AbsoluteDesktopIconPreservesSpecialCharacters) {
  icon("art/雪 #100%.png");
  write("applications/game.desktop", QByteArray("[Desktop Entry]\nIcon=") +
        (root() + "/art/雪 #100%.png").toUtf8() + "\nStartupWMClass=雪 #100%\n");
  GameIconProvider provider({root()}, {}, "", {}, cacheFile());
  EXPECT_FALSE(load(provider, QString::fromUtf8("雪 #100%")).isNull());
  ASSERT_TRUE(QFile::remove(root() + "/applications/game.desktop"));
  GameIconProvider reopened({root()}, {}, "", {}, cacheFile());
  EXPECT_FALSE(load(reopened, QString::fromUtf8("雪 #100%")).isNull());
}

TEST_F(GameIconTest, InheritedThemeAndUnthemedIcon) {
  write("icons/custom/index.theme", "[Icon Theme]\nInherits=parent\n");
  write("icons/parent/index.theme", "[Icon Theme]\nDirectories=apps\n");
  icon("icons/parent/apps/example.png");
  icon("pixmaps/plain.png");
  GameIconProvider provider({root()}, {}, "custom", {});
  EXPECT_FALSE(load(provider, "example").isNull());
  EXPECT_FALSE(load(provider, "plain").isNull());
}

TEST_F(GameIconTest, EveryDefaultHasBundledArtworkWithNoInstalledApps) {
  GameIconProvider provider({root()}, {}, "absent", {});
  for (const auto &app : defaultAppCatalog()) {
    SCOPED_TRACE(app.key.toStdString());
    const QImage image = load(provider, app.key);
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.size(), QSize(40, 40));
    for (const QString &alias : app.aliases)
      EXPECT_FALSE(load(provider, alias).isNull()) << alias.toStdString();
  }
  EXPECT_TRUE(load(provider, "com.obsproject.studio.some-game").isNull());
  EXPECT_TRUE(QFile::exists(":/icons/default-apps/README.md"));
  EXPECT_TRUE(QFile::exists(":/icons/default-apps/LICENSE"));
}

TEST_F(GameIconTest, InstalledReverseDnsDesktopIconWinsOverBundle) {
  write("applications/com.obsproject.Studio.desktop",
        "[Desktop Entry]\nIcon=custom-obs\n");
  icon("icons/hicolor/48x48/apps/custom-obs.png", Qt::magenta);
  GameIconProvider provider({root()}, {}, "", {});
  const auto image = load(provider, "obs");
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.pixelColor(20, 20), QColor(Qt::magenta));
}

TEST_F(GameIconTest, InstalledIconAliasWinsWithoutDesktopEntry) {
  icon("icons/hicolor/48x48/apps/microsoft-edge.png", Qt::cyan);
  GameIconProvider provider({root()}, {}, "", {});
  const auto image = load(provider, "msedge");
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.pixelColor(20, 20), QColor(Qt::cyan));
}

TEST_F(GameIconTest, ZoomActionCannotReplaceMeetingsApplicationIcon) {
  write("icons/custom/index.theme",
        "[Icon Theme]\nDirectories=actions/16,apps/48\n"
        "[actions/16]\nContext=Actions\n[apps/48]\nContext=Applications\n");
  icon("icons/custom/actions/16/zoom.png", Qt::magenta);
  icon("icons/custom/apps/48/Zoom.png", Qt::cyan);
  GameIconProvider provider({root()}, {}, "custom", {});
  const auto image = load(provider, "zoom");
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.pixelColor(20, 20), QColor(Qt::cyan));
}

TEST_F(GameIconTest, ZoomUsesBundleWhenOnlyToolbarMagnifierIsInstalled) {
  write("icons/custom/index.theme",
        "[Icon Theme]\nDirectories=actions/16\n[actions/16]\nContext=Actions\n");
  icon("icons/custom/actions/16/zoom.png", Qt::magenta);
  GameIconProvider provider({root()}, {}, "custom", {});
  GameIconProvider empty({root() + "/empty"}, {}, "", {});
  const auto image = load(provider, "zoom");
  ASSERT_FALSE(image.isNull());
  EXPECT_TRUE(image == load(empty, "zoom"));
}

TEST_F(GameIconTest, PreviousLookupPolicyCannotReuseCachedActionIcon) {
  write("icons/custom/index.theme",
        "[Icon Theme]\nDirectories=actions/16\n[actions/16]\nContext=Actions\n");
  icon("icons/custom/actions/16/zoom.png", Qt::magenta);
  // Reproduce the old cache context, differing ONLY in lookup-policy version.
  QJsonArray context{1, "custom", QJsonArray{root()},
                     QJsonArray{root() + "/icons", root() + "/pixmaps"}, QJsonArray{}};
  for (const auto &app : defaultAppCatalog())
    context.append(QJsonArray{app.key, app.artwork,
                             QJsonArray::fromStringList(app.aliases),
                             QJsonArray::fromStringList(app.icons)});
  const QString fingerprint = QString::fromLatin1(QCryptographicHash::hash(
      QJsonDocument(context).toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256).toHex());
  const QFileInfo action(root() + "/icons/custom/actions/16/zoom.png");
  write("cache/game-icons.json", QJsonDocument(QJsonObject{
      {"fingerprint", fingerprint},
      {"entries", QJsonObject{{id("zoom"), QJsonObject{
          {"path", action.absoluteFilePath()},
          {"bytes", QString::number(action.size())},
          {"modified", QString::number(action.lastModified().toMSecsSinceEpoch())},
          {"discovered", QString::number(QDateTime::currentMSecsSinceEpoch())}}}}}}).toJson());
  GameIconProvider provider({root()}, {}, "custom", {}, cacheFile());
  GameIconProvider empty({root() + "/empty"}, {}, "", {});
  EXPECT_TRUE(load(provider, "zoom") == load(empty, "zoom"));
}

TEST_F(GameIconTest, ApplicationsContextWorksOutsideConventionalAppsDirectory) {
  write("icons/custom/index.theme",
        "[Icon Theme]\nDirectories=products\n[products]\nContext=Applications\n");
  icon("icons/custom/products/game.png", Qt::cyan);
  GameIconProvider provider({root()}, {}, "custom", {});
  const auto image = load(provider, "game");
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.pixelColor(20, 20), QColor(Qt::cyan));
}

TEST_F(GameIconTest, PersistentSourceSkipsDesktopDiscoveryAcrossProvidersAndSizes) {
  write("applications/game.desktop", "[Desktop Entry]\nIcon=unrelated-icon-name\n");
  icon("icons/hicolor/48x48/apps/unrelated-icon-name.png", Qt::magenta);
  {
    GameIconProvider first({root()}, {}, "", {}, cacheFile());
    ASSERT_FALSE(load(first, "game").isNull());
  }
  ASSERT_TRUE(QFile::exists(cacheFile()));
  // Without the source cache, lookup cannot connect this key to the icon.
  ASSERT_TRUE(QFile::remove(root() + "/applications/game.desktop"));
  GameIconProvider reopened({root()}, {}, "", {}, cacheFile());
  QSize size;
  const auto image = reopened.requestImage(id("game"), &size, {80, 80});
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.pixelColor(20, 20), QColor(Qt::magenta));
  EXPECT_EQ(size, QSize(80, 80));
}

TEST_F(GameIconTest, DeletedSourceRediscoveredAndCacheRepaired) {
  write("applications/game.desktop", "[Desktop Entry]\nIcon=old\n");
  icon("icons/hicolor/48x48/apps/old.png", Qt::magenta);
  {
    GameIconProvider first({root()}, {}, "", {}, cacheFile());
    ASSERT_FALSE(load(first, "game").isNull());
  }
  ASSERT_TRUE(QFile::remove(root() + "/icons/hicolor/48x48/apps/old.png"));
  write("applications/game.desktop", "[Desktop Entry]\nIcon=new\n");
  icon("icons/hicolor/48x48/apps/new.png", Qt::cyan);
  {
    GameIconProvider reopened({root()}, {}, "", {}, cacheFile());
    const auto image = load(reopened, "game");
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.pixelColor(20, 20), QColor(Qt::cyan));
  }
  ASSERT_TRUE(QFile::remove(root() + "/applications/game.desktop"));
  GameIconProvider repaired({root()}, {}, "", {}, cacheFile());
  EXPECT_FALSE(load(repaired, "game").isNull());
}

TEST_F(GameIconTest, ChangedOrCorruptSourceCannotKeepStaleSelection) {
  write("applications/game.desktop", "[Desktop Entry]\nIcon=old\n");
  icon("icons/hicolor/48x48/apps/old.png");
  {
    GameIconProvider first({root()}, {}, "", {}, cacheFile());
    ASSERT_FALSE(load(first, "game").isNull());
  }
  write("icons/hicolor/48x48/apps/old.png", "corrupt");
  write("applications/game.desktop", "[Desktop Entry]\nIcon=new\n");
  icon("icons/hicolor/48x48/apps/new.png", Qt::cyan);
  GameIconProvider reopened({root()}, {}, "", {}, cacheFile());
  const auto image = load(reopened, "game");
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.pixelColor(20, 20), QColor(Qt::cyan));
}

TEST_F(GameIconTest, ThemeAndSearchRootsInvalidatePersistentSelections) {
  icon("icons/first/48x48/apps/game.png", Qt::magenta);
  icon("icons/second/48x48/apps/game.png", Qt::cyan);
  {
    GameIconProvider first({root()}, {}, "first", {}, cacheFile());
    ASSERT_FALSE(load(first, "game").isNull());
  }
  {
    GameIconProvider second({root()}, {}, "second", {}, cacheFile());
    const auto image = load(second, "game");
    ASSERT_FALSE(image.isNull());
    EXPECT_EQ(image.pixelColor(20, 20), QColor(Qt::cyan));
  }
  GameIconProvider otherRoots({root() + "/empty"}, {}, "second", {}, cacheFile());
  EXPECT_TRUE(load(otherRoots, "game").isNull());
}

TEST_F(GameIconTest, ExpiredFallbackAndMissDiscoverNewlyInstalledIcons) {
  {
    GameIconProvider first({root()}, {}, "", {}, cacheFile());
    ASSERT_FALSE(load(first, "obs").isNull()); // bundled fallback
    ASSERT_TRUE(load(first, "new-game").isNull());
  }
  icon("icons/hicolor/48x48/apps/obs.png", Qt::magenta);
  icon("icons/hicolor/48x48/apps/new-game.png", Qt::cyan);
  {
    GameIconProvider cached({root()}, {}, "", {}, cacheFile());
    EXPECT_TRUE(load(cached, "new-game").isNull()); // short-lived negative cache
  }
  expireSources();
  GameIconProvider expired({root()}, {}, "", {}, cacheFile());
  const auto obs = load(expired, "obs");
  const auto game = load(expired, "new-game");
  ASSERT_FALSE(obs.isNull());
  ASSERT_FALSE(game.isNull());
  EXPECT_EQ(obs.pixelColor(20, 20), QColor(Qt::magenta));
  EXPECT_EQ(game.pixelColor(20, 20), QColor(Qt::cyan));
}

TEST_F(GameIconTest, CorruptOrUnwritableCacheDoesNotHideIcons) {
  icon("icons/hicolor/48x48/apps/game.png");
  write("cache/game-icons.json", "not JSON");
  {
    GameIconProvider corrupt({root()}, {}, "", {}, cacheFile());
    EXPECT_FALSE(load(corrupt, "game").isNull());
  }
  write("cache/game-icons.json", QByteArray(513 * 1024, 'x'));
  {
    GameIconProvider oversized({root()}, {}, "", {}, cacheFile());
    EXPECT_FALSE(load(oversized, "game").isNull());
  }
  write("not-a-directory", "file");
  GameIconProvider unwritable({root()}, {}, "", {}, root() + "/not-a-directory/cache.json");
  EXPECT_FALSE(load(unwritable, "game").isNull());
}

TEST_F(GameIconTest, MissingAndCorruptImagesLeaveFallbackAvailable) {
  icon("icons/hicolor/48x48/apps/some-other-game.png");
  write("icons/hicolor/48x48/apps/broken.png", "broken");
  GameIconProvider provider({root()}, {}, "", {});
  EXPECT_TRUE(load(provider, "missing").isNull());
  EXPECT_TRUE(load(provider, "broken").isNull());
  EXPECT_TRUE(load(provider, "../some-other-game").isNull());
  // Positive results and misses are retained across delegate/model rebuilds.
  icon("icons/hicolor/48x48/apps/missing.png");
  EXPECT_TRUE(load(provider, "missing").isNull());
}

TEST_F(GameIconTest, EmptyOrMalformedRequestsDoNotMatchDesktopDefaults) {
  write("applications/game.desktop", "[Desktop Entry]\nIcon=example\n");
  icon("icons/hicolor/48x48/apps/example.png");
  GameIconProvider provider({root()}, {}, "", {});
  QSize size;
  EXPECT_TRUE(provider.requestImage("0/", &size, {40, 40}).isNull());
  EXPECT_TRUE(provider.requestImage("invalid", &size, {40, 40}).isNull());
  EXPECT_TRUE(provider.requestImage("invalid/", &size, {40, 40}).isNull());
}

TEST_F(GameIconTest, QmlImageLoadsThroughRegisteredProvider) {
  icon("icons/hicolor/48x48/apps/steam_icon_42.png");
  QQmlEngine engine;
  engine.addImageProvider("gameicon", new GameIconProvider({root()}, {}, "", {}));
  QQmlComponent component(&engine);
  component.setData("import QtQuick\nImage { sourceSize: Qt.size(40,40); asynchronous: true }", QUrl());
  std::unique_ptr<QObject> object(component.create());
  ASSERT_NE(object, nullptr) << component.errorString().toStdString();
  object->setProperty("source", QUrl("image://gameicon/" + id("steam_app_42", 42)));
  for (int i = 0; i < 100 && object->property("status").toInt() != 1; ++i)
    QTest::qWait(10);
  EXPECT_EQ(object->property("status").toInt(), 1); // Image.Ready
  EXPECT_EQ(object->property("sourceSize").toSize(), QSize(40, 40));
}

TEST_F(GameIconTest, RecordingMissingArtworkIsQuietAndKeepsFallbackVisible) {
  QQmlEngine engine;
  engine.addImageProvider("gameicon", new GameIconProvider({root()}, {}, "", {}));
  QSignalSpy warnings(&engine, &QQmlEngine::warnings);
  const auto path = QFileInfo(QString::fromUtf8(GAMES_QML_FILE)).dir()
      .absoluteFilePath("../views/RecordingGameIcon.qml");
  QQmlComponent component(&engine, QUrl::fromLocalFile(path));
  std::unique_ptr<QObject> object(component.create());
  ASSERT_NE(object, nullptr) << component.errorString().toStdString();
  object->setProperty("width", 40);
  object->setProperty("height", 40);
  object->setProperty("source", QUrl("image://gameicon/" + id("steam_app_42", 42)));
  ASSERT_TRUE(waitUntil([&] { return object->property("status").toInt() == 1; }));
  EXPECT_FALSE(object->property("hasArtwork").toBool());
  EXPECT_FALSE(object->property("visible").toBool());
  EXPECT_TRUE(warnings.isEmpty());
}

TEST_F(GameIconTest, WindowIconSourcesNormalizeAndEncodeWithoutChangingIdentity) {
  EXPECT_EQ(gameIconSource("  FIREFOX.EXE  "), gameIconSource("firefox"));
  EXPECT_EQ(gameIconSource("steam_app_42"), gameIconSource("steam_app_42", 42));
  EXPECT_TRUE(gameIconSource("").isEmpty());
  const QString key = QString::fromUtf8("org.example.é#app");
  const QUrl url = gameIconSource(key);
  EXPECT_EQ(QString::fromUtf8(QByteArray::fromBase64(
      url.path().section('/', 2).toLatin1(), QByteArray::Base64UrlEncoding)), key);
}

TEST_F(GameIconTest, AddGameDialogShowsProgramIconsAndRetainsLetterFallback) {
  GameListModel model;
  QQmlEngine engine;
  engine.addImageProvider("gameicon", new GameIconProvider({root()}, {}, "", {}));
  engine.rootContext()->setContextProperty("gameDetector", QVariantMap{
      {"gamesModel", QVariant::fromValue(static_cast<QObject *>(&model))},
      {"gameFilterText", ""}, {"showDefaultExclusions", false}});
  QQuickWindow window;
  QQmlComponent component(&engine, QUrl::fromLocalFile(GAMES_QML_FILE));
  std::unique_ptr<QObject> object(component.create());
  ASSERT_NE(object, nullptr) << component.errorString().toStdString();
  auto *page = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(page, nullptr);
  window.resize(1113, 700);
  page->setParentItem(window.contentItem());
  page->setSize({1113, 700});
  window.show();
  auto *dialog = object->findChild<QObject *>("addGameDialog");
  ASSERT_NE(dialog, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "open"));
  QTest::qWait(100);
  const QVariantList processes{
      QVariantMap{{"title", "Firefox process"},
                   {"likelyApplication", true},
                  {"path", "/usr/bin/firefox"},
                  {"pid", 10},
                  {"iconSource", gameIconSource("firefox")}},
      QVariantMap{{"title", "Missing program"},
                   {"likelyApplication", true},
                  {"path", "/games/missing-app"},
                  {"pid", 20},
                  {"iconSource", gameIconSource("missing-app")}}};
  ASSERT_TRUE(dialog->setProperty("allProcesses", processes));
  auto *content = dialog->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(content, nullptr);
  QList<QQuickItem *> images, letters;
  std::function<void(QQuickItem *)> collect = [&](QQuickItem *item) {
    if (item->objectName() == "pickerIcon") images.append(item);
    if (item->objectName() == "pickerIconFallback") letters.append(item);
    for (auto *child : item->childItems()) collect(child);
  };
  for (int i = 0; i < 100; ++i) {
    QTest::qWait(10);
    images.clear();
    letters.clear();
    collect(content);
    if (images.size() == 2 && images[0]->property("status").toInt() == 1 &&
        images[1]->property("status").toInt() == 1)
      break;
  }
  ASSERT_EQ(images.size(), 2);
  ASSERT_EQ(letters.size(), 2);
  EXPECT_TRUE(images[0]->isVisible());
  EXPECT_FALSE(letters[0]->isVisible());
  EXPECT_FALSE(images[1]->isVisible());
  EXPECT_TRUE(letters[1]->isVisible());
  EXPECT_EQ(letters[1]->property("text").toString(), "M");
}

TEST_F(GameIconTest, AddGameDefaultsToApplicationsAndAllModeRetainsUnknownProcesses) {
  GameListModel model;
  QQmlEngine engine;
  engine.addImageProvider("gameicon", new GameIconProvider({root()}, {}, "", {}));
  QQmlComponent fake(&engine);
  fake.setData(R"(import QtQml
QtObject {
  property var gamesModel
  property string gameFilterText: ""
  property bool showDefaultExclusions: false
  property var processes: []
  property var lastAdded: []
  property int refreshes: 0
  function runningProcesses() { refreshes++; return processes }
  function addProcessGame(pid, identity, display, block) {
    lastAdded = [pid, identity, display, block]; return true
  }
})", QUrl());
  const QVariantMap app{{"pid", 10}, {"identity", "10:100"}, {"title", "Game"},
                        {"path", "/games/game"}, {"likelyApplication", true}};
  const QVariantMap unknown{{"pid", 99}, {"identity", "99:200"}, {"title", "Unknown native"},
                            {"path", "/games/unlisted"}, {"likelyApplication", false}};
  const QVariantMap service{{"pid", 12}, {"identity", "12:300"}, {"title", "rclone"},
                            {"path", "/usr/bin/rclone"}, {"likelyApplication", false}};
  std::unique_ptr<QObject> detector(fake.createWithInitialProperties({
      {"gamesModel", QVariant::fromValue(static_cast<QObject *>(&model))},
      {"processes", QVariantList{app, service, unknown}}}));
  ASSERT_NE(detector, nullptr) << fake.errorString().toStdString();
  engine.rootContext()->setContextProperty("gameDetector", detector.get());
  QQuickWindow window;
  QQmlComponent component(&engine, QUrl::fromLocalFile(GAMES_QML_FILE));
  std::unique_ptr<QObject> object(component.create());
  ASSERT_NE(object, nullptr) << component.errorString().toStdString();
  auto *page = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(page, nullptr);
  page->setParentItem(window.contentItem());
  window.resize(1113, 700);
  page->setSize({1113, 700});
  window.show();
  auto *dialog = object->findChild<QObject *>("addGameDialog");
  ASSERT_NE(dialog, nullptr);
  const auto list = [](const QVariant &value) {
    return value.metaType() == QMetaType::fromType<QJSValue>()
        ? value.value<QJSValue>().toVariant().toList() : value.toList();
  };
  const auto shown = [&] { return list(dialog->property("shownProcesses")); };
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "open"));
  ASSERT_TRUE(waitUntil([&] { return dialog->property("opened").toBool(); }));
  EXPECT_FALSE(dialog->property("showAllProcesses").toBool());
  ASSERT_EQ(shown().size(), 1);
  EXPECT_EQ(shown()[0].toMap()["pid"].toInt(), 10);
  auto *toggle = object->findChild<QObject *>("showAllProcessesSwitch");
  ASSERT_NE(toggle, nullptr);
  toggle->setProperty("checked", true);
  ASSERT_TRUE(QMetaObject::invokeMethod(toggle, "toggled"));
  EXPECT_TRUE(dialog->property("showAllProcesses").toBool());
  EXPECT_EQ(shown().size(), 3);
  dialog->setProperty("searchQuery", "99");
  ASSERT_EQ(shown().size(), 1);
  EXPECT_EQ(shown()[0].toMap()["identity"].toString(), "99:200");
  dialog->setProperty("pickedProcess", shown()[0]);
  dialog->setProperty("showAllProcesses", false);
  EXPECT_TRUE(shown().isEmpty());
  EXPECT_TRUE(dialog->property("pickedProcess").isNull());
  dialog->setProperty("searchQuery", "/games/game");
  ASSERT_EQ(shown().size(), 1);
  dialog->setProperty("pickedProcess", shown()[0]);
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "addPicked"));
  const auto added = list(detector->property("lastAdded"));
  ASSERT_EQ(added.size(), 4);
  EXPECT_EQ(added[0].toInt(), 10);
  EXPECT_EQ(added[1].toString(), "10:100");
  EXPECT_FALSE(added[3].toBool()); // Watch retains its existing routing.
  ASSERT_TRUE(waitUntil([&] { return !dialog->property("visible").toBool(); }));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "open"));
  ASSERT_TRUE(waitUntil([&] { return dialog->property("opened").toBool(); }));
  EXPECT_FALSE(dialog->property("showAllProcesses").toBool());
  dialog->setProperty("showAllProcesses", true);
  dialog->setProperty("searchQuery", "unlisted");
  ASSERT_EQ(shown().size(), 1);
  dialog->setProperty("pickedProcess", shown()[0]);
  auto *block = object->findChild<QObject *>("blockProcessRadio");
  ASSERT_NE(block, nullptr);
  block->setProperty("checked", true);
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "addPicked"));
  const auto blocked = list(detector->property("lastAdded"));
  EXPECT_EQ(blocked[0].toInt(), 99);
  EXPECT_EQ(blocked[1].toString(), "99:200");
  EXPECT_TRUE(blocked[3].toBool());
  EXPECT_GE(detector->property("refreshes").toInt(), 2);
}

TEST_F(GameIconTest, RealGamesPageShowsIconAndKeepsMissingLetterTile) {
  icon("icons/hicolor/48x48/apps/steam_icon_42.png");
  GameListEntry found;
  found.key = "steam_app_42";
  found.display = "Found";
  found.steamAppId = 42;
  GameListEntry missing;
  missing.key = "steam_app_43";
  missing.display = "Missing";
  missing.steamAppId = 43;
  GameListEntry mpv;
  mpv.key = mpv.display = "mpv";
  mpv.isDefault = true;
  GameListEntry obs;
  obs.key = "obs";
  obs.display = "OBS";
  obs.isDefault = true;
  GameListModel model;
  model.setEntries({found, missing, mpv, obs});
  QQmlEngine engine;
  engine.addImageProvider("gameicon", new GameIconProvider({root()}, {}, "", {}));
  engine.rootContext()->setContextProperty("gameDetector", QVariantMap{
      {"gamesModel", QVariant::fromValue(static_cast<QObject *>(&model))},
      {"gameFilterText", ""}, {"showDefaultExclusions", false}});
  QQuickWindow window;
  QQmlComponent component(&engine, QUrl::fromLocalFile(GAMES_QML_FILE));
  std::unique_ptr<QObject> object(component.create());
  ASSERT_NE(object, nullptr) << component.errorString().toStdString();
  auto *page = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(page, nullptr);
  window.resize(1113, 700);
  page->setParentItem(window.contentItem());
  page->setSize({1113, 700});
  window.show();
  QList<QQuickItem *> images;
  QList<QQuickItem *> letters;
  QList<QQuickItem *> subtitles;
  // ListView delegates are visual descendants, not necessarily QObject children.
  std::function<void(QQuickItem *)> collect = [&](QQuickItem *item) {
    if (item->objectName() == "gameIcon") images.append(item);
    if (item->objectName() == "gameIconFallback") letters.append(item);
    if (item->objectName() == "gameEntrySubtitle") subtitles.append(item);
    for (auto *child : item->childItems()) collect(child);
  };
  for (int i = 0; i < 100; ++i) {
    QTest::qWait(10);
    images.clear();
    letters.clear();
    subtitles.clear();
    collect(page);
    if (images.size() == 4 && images[0]->property("status").toInt() == 1 &&
        images[1]->property("status").toInt() == 1 &&
        images[2]->property("status").toInt() == 1 &&
        images[3]->property("status").toInt() == 1)
      break;
  }
  ASSERT_EQ(images.size(), 4);
  ASSERT_EQ(letters.size(), 4);
  EXPECT_EQ(images[0]->property("status").toInt(), 1);
  EXPECT_TRUE(images[0]->isVisible());
  EXPECT_FALSE(letters[0]->isVisible());
  EXPECT_EQ(images[1]->property("status").toInt(), 1); // Ready, but no artwork.
  EXPECT_FALSE(images[1]->isVisible());
  EXPECT_TRUE(letters[1]->isVisible());
  EXPECT_EQ(letters[1]->property("text").toString(), "M");
  ASSERT_EQ(subtitles.size(), 4);
  EXPECT_TRUE(subtitles[2]->isVisible()); // mpv no longer loses its subtitle
  EXPECT_EQ(subtitles[2]->property("text").toString(), "Default exclusion · 2 identifiers");
  EXPECT_EQ(subtitles[3]->property("text").toString(), "Default exclusion · 4 identifiers");
  EXPECT_EQ(QQmlProperty(subtitles[3], "ToolTip.text", qmlContext(subtitles[3])).read().toString(),
            "obs\nobs64\nobs-studio\ncom.obsproject.studio");
}
TEST_F(GameIconTest, GamesAndPickerLayoutOwnIconAndDividerSizesAcrossResize) {
  GameListEntry first;
  first.key = first.display = "First";
  GameListEntry second;
  second.key = second.display = "Second";
  GameListModel model;
  model.setEntries({first, second});
  QQmlEngine engine;
  engine.addImageProvider("gameicon", new GameIconProvider({root()}, {}, "", {}));
  engine.rootContext()->setContextProperty("gameDetector", QVariantMap{
      {"gamesModel", QVariant::fromValue(static_cast<QObject *>(&model))},
      {"gameFilterText", ""}, {"showDefaultExclusions", false}});
  QQuickWindow window;
  QQmlComponent component(&engine, QUrl::fromLocalFile(GAMES_QML_FILE));
  std::unique_ptr<QObject> object(component.create());
  ASSERT_NE(object, nullptr) << component.errorString().toStdString();
  auto *page = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(page, nullptr);
  page->setParentItem(window.contentItem());
  window.show();
  auto *dialog = object->findChild<QObject *>("addGameDialog");
  ASSERT_NE(dialog, nullptr);
  auto *content = dialog->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(content, nullptr);
  auto items = [](QQuickItem *rootItem, const QString &name) {
    QList<QQuickItem *> found;
    std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
      if (item->objectName() == name) found.append(item);
      for (auto *child : item->childItems()) visit(child);
    };
    visit(rootItem);
    return found;
  };
  auto checkTiles = [&](QQuickItem *rootItem, const QString &name, int size) {
    ASSERT_TRUE(waitUntil([&] { return items(rootItem, name).size() == 2; }));
    for (auto *tile : items(rootItem, name)) {
      EXPECT_EQ(QQmlProperty(tile, "Layout.preferredWidth", qmlContext(tile)).read().toInt(), size);
      EXPECT_EQ(QQmlProperty(tile, "Layout.preferredHeight", qmlContext(tile)).read().toInt(), size);
      EXPECT_TRUE(waitUntil([&] { return tile->width() == size && tile->height() == size; }));
    }
  };
  const QVariantList processes{
      QVariantMap{
          {"title", "First process"}, {"path", "/games/first"}, {"pid", 10}, {"likelyApplication", true}},
      QVariantMap{
          {"title", "Second process"}, {"path", "/games/second"}, {"pid", 20}, {"likelyApplication", true}}};
  for (const QSize &size : {QSize(1113, 700), QSize(800, 650), QSize(1280, 800)}) {
    window.resize(size);
    page->setSize(size);
    checkTiles(page, "gameIconTile", 40);
    dialog->setProperty("width", size.width() > 1000 ? 620 : 520);
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "open"));
    ASSERT_TRUE(waitUntil([&] { return dialog->property("opened").toBool(); }));
    // A host without a census service leaves an empty model safely.
    EXPECT_TRUE(dialog->property("allProcesses").toList().isEmpty());
    for (const auto *name : {"gamePickerTopDivider", "gamePickerBottomDivider"}) {
      auto *divider = object->findChild<QQuickItem *>(name);
      ASSERT_NE(divider, nullptr);
      EXPECT_EQ(divider->implicitHeight(), 1);
      EXPECT_TRUE(waitUntil([&] { return divider->height() == 1 && divider->width() > 400; }));
    }
    ASSERT_TRUE(dialog->setProperty("allProcesses", processes));
    checkTiles(content, "pickerIconTile", 36);
    ASSERT_TRUE(dialog->setProperty("allProcesses", QVariantList{}));
    ASSERT_TRUE(waitUntil([&] { return items(content, "pickerIconTile").isEmpty(); }));
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
    ASSERT_TRUE(waitUntil([&] { return !dialog->property("visible").toBool(); }));
  }
}

TEST_F(GameIconTest, BoundPromptBlockPolesKeepLabelsStylingAndClickRouting) {
  GameListEntry entry;
  entry.key = entry.display = "Game";
  GameListModel model;
  model.setEntries({entry});
  QQmlEngine engine;
  engine.addImageProvider("gameicon", new GameIconProvider({root()}, {}, "", {}));
  QQmlComponent fake(&engine);
  fake.setData(R"(import QtQml
QtObject {
  property var gamesModel
  property string gameFilterText: ""
  property bool showDefaultExclusions: false
  property int promptCalls: 0
  property int blockCalls: 0
  property string lastKey: ""
  function unblockGame(key) { lastKey = key; promptCalls++; return true }
  function moveToBlacklist(key) { lastKey = key; blockCalls++; return true }
})", QUrl());
  std::unique_ptr<QObject> detector(fake.createWithInitialProperties({
      {"gamesModel", QVariant::fromValue(static_cast<QObject *>(&model))}}));
  ASSERT_NE(detector, nullptr) << fake.errorString().toStdString();
  engine.rootContext()->setContextProperty("gameDetector", detector.get());
  QStringList warnings;
  QObject::connect(&engine, &QQmlEngine::warnings, &engine,
      [&](const QList<QQmlError> &errors) {
        for (const auto &error : errors) warnings.append(error.toString());
      });
  QQuickWindow window;
  QQmlComponent component(&engine, QUrl::fromLocalFile(GAMES_QML_FILE));
  std::unique_ptr<QObject> object(component.create());
  ASSERT_NE(object, nullptr) << component.errorString().toStdString();
  auto *page = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(page, nullptr);
  window.resize(1113, 700);
  page->setParentItem(window.contentItem());
  page->setSize({1113, 700});
  window.show();
  std::function<QQuickItem *(QQuickItem *, const QString &)> find =
      [&](QQuickItem *item, const QString &name) -> QQuickItem * {
    if (item->objectName() == name) return item;
    for (auto *child : item->childItems())
      if (auto *found = find(child, name)) return found;
    return nullptr;
  };
  ASSERT_TRUE(waitUntil([&] { return find(page, "blockPole") != nullptr; }));
  auto *prompt = find(page, "promptPole");
  auto *block = find(page, "blockPole");
  ASSERT_NE(prompt, nullptr);
  ASSERT_NE(block, nullptr);
  ASSERT_TRUE(waitUntil([&] { return block->width() > 0 && block->isVisible(); }));
  auto label = [](QQuickItem *pole) -> QObject * {
    for (auto *child : pole->childItems())
      if (child->property("text").isValid()) return child;
    return nullptr;
  };
  ASSERT_NE(label(prompt), nullptr);
  ASSERT_NE(label(block), nullptr);
  EXPECT_EQ(label(prompt)->property("text").toString(), "Prompt");
  EXPECT_EQ(label(block)->property("text").toString(), "Deny");
  EXPECT_EQ(label(prompt)->property("color").value<QColor>(), QColor("#fbbf24"));
  const QColor active = prompt->property("color").value<QColor>();
  EXPECT_GT(active.alpha(), 0);
  EXPECT_EQ(block->property("color").value<QColor>().alpha(), 0);
  auto *list = find(page, "gamesList");
  ASSERT_NE(list, nullptr);
  const QPointF listPosition = list->mapToScene(QPointF());
  const QSizeF listSize = list->size();
  const double scrollPosition = list->property("contentY").toDouble();
  for (auto *pole : {block, prompt}) {
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
        pole->mapToScene(QPointF(pole->width() / 2, pole->height() / 2)).toPoint());
    QTest::qWait(20);
    EXPECT_EQ(list->mapToScene(QPointF()), listPosition);
    EXPECT_EQ(list->size(), listSize);
    EXPECT_EQ(list->property("contentY").toDouble(), scrollPosition);
  }
  EXPECT_EQ(detector->property("blockCalls").toInt(), 1);
  EXPECT_EQ(detector->property("promptCalls").toInt(), 1);
  EXPECT_EQ(detector->property("lastKey").toString(), "Game");
  ASSERT_TRUE(prompt->parentItem()->parentItem()->setProperty("blocked", true));
  EXPECT_EQ(prompt->property("color").value<QColor>().alpha(), 0);
  EXPECT_GT(block->property("color").value<QColor>().alpha(), 0);
  for (const auto &warning : warnings)
    EXPECT_FALSE(warning.contains("ReferenceError") || warning.contains("TypeError"))
        << warning.toStdString();
}

TEST_F(GameIconTest, GamesTableAlignsColumnsAndPreservesApprovalAndRuleActions) {
  GameListEntry ready, prompt, blocked;
  ready.key = "steam_app_42";
  ready.display = "Ready game";
  ready.hasToken = true;
  ready.tokenKey = "pinned-token-key";
  ready.steamAppId = 42;
  ready.section = "Custom Games";
  prompt.key = "wine:/very/long/prefix/game.exe";
  prompt.display = "Prompt game";
  prompt.running = true;
  prompt.section = "Custom Games";
  blocked.key = blocked.display = "Blocked game";
  blocked.listed = 2;
  blocked.library = blocked.section = "Steam";
  blocked.blockKey = "exe:/library-test/executable";
  GameListModel model;
  model.setEntries({ready, prompt, blocked});
  GameListFilterModel filtered;
  filtered.setSourceModel(&model);
  QQmlEngine engine;
  engine.addImageProvider("gameicon", new GameIconProvider({root()}, {}, "", {}));
  QQmlComponent fake(&engine);
  fake.setData(R"(import QtQml
QtObject {
  property var gamesModel
  property string gameFilterText: ""
  property bool showDefaultExclusions: false
  property string withdrawnKey: ""
  property bool resetModelOnWithdraw: false
  property string forgottenKey: ""
  property string deniedKey: ""
  property var unblockedKeys: []
  property var instances: []
  property string themeMode: "amoled"
  property string accentColor: "rose"
  function removeRestoreToken(key) {
    withdrawnKey = key
    if (resetModelOnWithdraw) gamesModel = null
  }
  function forgetGame(key) { forgottenKey = key }
  function moveToBlacklist(key) { deniedKey = key }
  function unblockGame(key) { unblockedKeys = unblockedKeys.concat([key]) }
})", QUrl());
  std::unique_ptr<QObject> detector(fake.createWithInitialProperties({
      {"gamesModel", QVariant::fromValue(static_cast<QObject *>(&filtered))}}));
  ASSERT_NE(detector, nullptr);
  engine.rootContext()->setContextProperty("gameDetector", detector.get());
  engine.rootContext()->setContextProperty("settingsManager", detector.get());
  QStringList warnings;
  QObject::connect(&engine, &QQmlEngine::warnings, &engine,
      [&](const QList<QQmlError> &errors) {
        for (const auto &error : errors) warnings.append(error.toString());
      });
  QQmlComponent component(&engine, QUrl::fromLocalFile(GAMES_QML_FILE));
  std::unique_ptr<QObject> object(component.create());
  ASSERT_NE(object, nullptr) << component.errorString().toStdString();
  QSignalSpy policyNotices(object.get(), SIGNAL(policyNoticeRequested(QString,QString,QString)));
  ASSERT_TRUE(policyNotices.isValid());
  auto *page = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(page, nullptr);
  QQuickWindow window;
  page->setParentItem(window.contentItem());
  window.show();
  auto items = [](QQuickItem *rootItem, const QString &name) {
    QList<QQuickItem *> found;
    std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
      if (item->objectName() == name) found.append(item);
      for (auto *child : item->childItems()) visit(child);
    };
    visit(rootItem);
    return found;
  };
  for (const QSize &size : {QSize(1113, 700), QSize(640, 650), QSize(1280, 800)}) {
    window.resize(size);
    page->setSize(size);
    ASSERT_TRUE(waitUntil([&] { return items(page, "gameEntryStatus").size() == 3; }));
    QTest::qWait(50);
    auto *statusHeader = items(page, "gamesStatusColumnHeader").value(0);
    auto *actionsHeader = items(page, "gamesActionsColumnHeader").value(0);
    ASSERT_NE(statusHeader, nullptr);
    ASSERT_NE(actionsHeader, nullptr);
    for (auto *status : items(page, "gameEntryStatus"))
      EXPECT_NEAR(status->mapToItem(page, QPointF()).x(),
                  statusHeader->mapToItem(page, QPointF()).x(), 1);
    for (auto *actions : items(page, "gameEntryActions")) {
      EXPECT_NEAR(actions->mapToItem(page, QPointF()).x(),
                  actionsHeader->mapToItem(page, QPointF()).x(), 1);
      EXPECT_LE(actions->mapToItem(page, QPointF(actions->width(), 0)).x(), size.width());
    }
    for (auto *status : items(page, "gameEntryStatusText"))
      EXPECT_FALSE(status->property("truncated").toBool());
    for (auto *withdraw : items(page, "withdrawGameApprovalButton")) {
      if (withdraw->isVisible())
        EXPECT_NEAR(withdraw->width(), page->property("actionsColumnWidth").toInt(), 1);
    }
  }
  const auto statuses = items(page, "gameEntryStatusText");
  ASSERT_EQ(statuses.size(), 3);
  EXPECT_EQ(statuses[0]->property("text").toString(), "Ready to capture");
  EXPECT_EQ(statuses[1]->property("text").toString(), "Waiting for approval");
  EXPECT_EQ(statuses[1]->property("color").value<QColor>(), QColor("#fbbf24"));
  EXPECT_NE(statuses[0]->property("color").value<QColor>(),
            statuses[1]->property("color").value<QColor>());
  EXPECT_EQ(statuses[2]->property("text").toString(), "Denied");
  QStringList headings;
  for (auto *heading : items(page, "gamesSectionHeading"))
    headings.append(heading->property("text").toString());
  EXPECT_TRUE(headings.contains("Steam (1)"));
  EXPECT_TRUE(headings.contains("Custom Games (2)"));
  ready.running = true;
  model.setEntries({ready, prompt, blocked});
  QCoreApplication::processEvents();
  EXPECT_EQ(statuses[0]->property("text").toString(), "Active");
  EXPECT_FALSE(items(page, "withdrawGameApprovalButton").value(0)->isVisible());
  prompt.running = false;
  model.setEntries({ready, prompt, blocked});
  QCoreApplication::processEvents();
  EXPECT_EQ(statuses[1]->property("text").toString(), "Waiting for launch");
  ready.running = false;
  model.setEntries({ready, prompt, blocked});
  QCoreApplication::processEvents();
  EXPECT_EQ(statuses[0]->property("text").toString(), "Ready to capture");
  for (auto *subtitle : items(page, "gameEntrySubtitle"))
    EXPECT_FALSE(subtitle->isVisible());
  const auto names = items(page, "gameEntryName");
  ASSERT_EQ(names.size(), 3);
  EXPECT_TRUE(QQmlProperty(names[1], "ToolTip.text", qmlContext(names[1])).read()
                  .toString().contains(prompt.key));
  auto *withdraw = items(page, "withdrawGameApprovalButton").value(0);
  ASSERT_NE(withdraw, nullptr);
  ASSERT_TRUE(withdraw->isVisible());
  ASSERT_TRUE(QMetaObject::invokeMethod(withdraw, "clicked"));
  EXPECT_EQ(detector->property("withdrawnKey").toString(), ready.tokenKey);
  ASSERT_EQ(policyNotices.size(), 1);
  EXPECT_TRUE(policyNotices.last()[1].toString().contains("Approval revoked"));
  EXPECT_TRUE(policyNotices.last()[2].toString().contains("Saved capture approval was removed"));
  const auto forget = items(page, "forgetGameButton");
  ASSERT_EQ(forget.size(), 3);
  EXPECT_FALSE(forget[0]->isVisible());
  EXPECT_FALSE(forget[2]->isVisible()); // Discovered library entries have no Remove.
  ASSERT_TRUE(QMetaObject::invokeMethod(forget[1], "clicked"));
  EXPECT_EQ(detector->property("forgottenKey").toString(), prompt.key);
  const auto denyPoles = items(page, "blockPole");
  ASSERT_EQ(denyPoles.size(), 3);
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      denyPoles[2]->mapToScene(QPointF(denyPoles[2]->width() / 2,
                                     denyPoles[2]->height() / 2)).toPoint());
  EXPECT_EQ(detector->property("deniedKey").toString(), blocked.blockKey);
  ASSERT_EQ(policyNotices.size(), 2);
  EXPECT_TRUE(policyNotices.last()[1].toString().startsWith("Deny"));
  EXPECT_TRUE(policyNotices.last()[2].toString().contains("files are unchanged"));
  const auto promptPoles = items(page, "promptPole");
  ASSERT_EQ(promptPoles.size(), 3);
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      promptPoles[2]->mapToScene(QPointF(promptPoles[2]->width() / 2,
                                       promptPoles[2]->height() / 2)).toPoint());
  ASSERT_EQ(policyNotices.size(), 3);
  EXPECT_TRUE(policyNotices.last()[1].toString().startsWith("Prompt"));
  EXPECT_TRUE(policyNotices.last()[2].toString().contains("does not start recording"));
  blocked.supportTool = true;
  blocked.hasToken = true;
  blocked.tokenKey = "old-helper-token";
  model.setEntries({ready, prompt, blocked});
  QCoreApplication::processEvents();
  EXPECT_EQ(statuses[2]->property("text").toString(), "Excluded");
  EXPECT_FALSE(denyPoles[2]->isVisible());
  EXPECT_FALSE(forget[2]->isVisible());
  EXPECT_FALSE(items(page, "withdrawGameApprovalButton").value(2)->isVisible());
  if (!qEnvironmentVariableIsEmpty("TROPHY_GAMES_UI_SCREENSHOT"))
    window.grabWindow().save(qEnvironmentVariable("TROPHY_GAMES_UI_SCREENSHOT"));
  auto *header = items(page, "gamesSection_Custom Games").value(0);
  ASSERT_NE(header, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(header, "clicked"));
  QTest::qWait(30);
  EXPECT_TRUE(filtered.collapsedSections().contains("Custom Games"));
  EXPECT_EQ(filtered.rowCount(), 2); // One representative plus Steam, not three full rows.
  EXPECT_EQ(filtered.matchingGameCount(), 3);
  EXPECT_EQ(filtered.sectionCount("Custom Games"), 2);
  header = items(page, "gamesSection_Custom Games").value(0);
  ASSERT_NE(header, nullptr); // Heading survives collapse.
  ASSERT_TRUE(QMetaObject::invokeMethod(header, "clicked"));
  QTest::qWait(30);
  EXPECT_FALSE(filtered.collapsedSections().contains("Custom Games"));
  EXPECT_EQ(filtered.rowCount(), 3);
  detector->setProperty("resetModelOnWithdraw", true);
  withdraw = items(page, "withdrawGameApprovalButton").value(0);
  ASSERT_NE(withdraw, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(withdraw, "clicked"));
  EXPECT_EQ(policyNotices.size(), 4); // Delivered even when the action destroys its row.
  EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}

TEST_F(GameIconTest, CollapsedLibrariesKeepOnlyOneRepresentativeAndFilterCountsStayAccurate) {
  QList<GameListEntry> entries;
  for (int i = 0; i < 1000; ++i) {
    GameListEntry entry;
    entry.key = entry.display = QString("Game %1").arg(i);
    entry.section = entry.library = "Steam";
    entries.append(entry);
  }
  GameListModel model;
  model.setEntries(entries);
  GameListFilterModel filtered;
  filtered.setShowDefaultEntries(false);
  filtered.setSourceModel(&model);
  filtered.toggleSection("Steam");
  EXPECT_EQ(filtered.rowCount(), 1);
  EXPECT_EQ(filtered.matchingGameCount(), 1000);
  EXPECT_EQ(filtered.sectionCount("Steam"), 1000);
  filtered.setFilterText("Game 99");
  EXPECT_EQ(filtered.rowCount(), 1);
  EXPECT_EQ(filtered.matchingGameCount(), 11);
  EXPECT_EQ(filtered.index(0, 0).data(GameListModel::KeyRole).toString(), "Game 99");
  entries[99].isDefault = true;
  model.setEntries(entries);
  EXPECT_EQ(filtered.rowCount(), 1);
  EXPECT_EQ(filtered.matchingGameCount(), 10);
  EXPECT_EQ(filtered.index(0, 0).data(GameListModel::KeyRole).toString(), "Game 990");
  filtered.toggleSection("Steam");
  EXPECT_EQ(filtered.rowCount(), 10);
  filtered.setFilterText("");
  EXPECT_EQ(filtered.rowCount(), 999);
  filtered.setShowDefaultEntries(true);
  EXPECT_EQ(filtered.rowCount(), 1000);
}

TEST_F(GameIconTest, StateOnlyGameUpdatesPreserveModelAndFilteredIndices) {
  GameListEntry first, second;
  first.key = first.display = "First";
  second.key = second.display = "Second";
  GameListModel model;
  model.setEntries({first, second});
  GameListFilterModel filtered;
  filtered.setSourceModel(&model);
  QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
  QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
  QSignalSpy filteredResets(&filtered, &QAbstractItemModel::modelReset);
  QPersistentModelIndex retained(filtered.index(1, 0));
  for (int state : {2, 0, 2, 0}) {
    second.listed = state;
    model.setEntries({first, second});
    EXPECT_TRUE(retained.isValid());
    EXPECT_EQ(retained.data(GameListModel::ListedRole).toInt(), state);
  }
  EXPECT_EQ(resets.size(), 0);
  EXPECT_EQ(filteredResets.size(), 0);
  EXPECT_EQ(changes.size(), 4);
  model.setEntries({first, second});
  EXPECT_EQ(changes.size(), 4); // Identical rebuilds are silent.
  second.hasToken = true;
  second.tokenKey = "Second";
  second.section = "Ready to capture";
  model.setEntries({first, second});
  EXPECT_EQ(retained.data(GameListModel::TokenKeyRole).toString(), "Second");
  EXPECT_EQ(resets.size(), 0);
  model.setEntries({second, first}); // Real reorder still resets safely.
  EXPECT_EQ(resets.size(), 1);
  EXPECT_FALSE(retained.isValid());
  model.setEntries({first});
  EXPECT_EQ(resets.size(), 2);
}

TEST_F(GameIconTest, PromptBlockRebuildKeepsScrolledFilteredPageAndDelegate) {
  QList<GameListEntry> entries;
  for (int i = 0; i < 40; ++i) {
    GameListEntry entry;
    entry.key = entry.display = QString("Game %1").arg(i, 2, 10, QLatin1Char('0'));
    entry.section = "Apps";
    entries.append(entry);
  }
  GameListModel model;
  model.setEntries(entries);
  GameListFilterModel filtered;
  filtered.setSourceModel(&model);
  QQmlEngine engine;
  engine.addImageProvider("gameicon", new GameIconProvider({root()}, {}, "", {}));
  engine.rootContext()->setContextProperty("gameDetector", QVariantMap{
      {"gamesModel", QVariant::fromValue(static_cast<QObject *>(&filtered))},
      {"gameFilterText", ""}, {"showDefaultExclusions", false}});
  QQuickWindow window;
  QQmlComponent component(&engine, QUrl::fromLocalFile(GAMES_QML_FILE));
  std::unique_ptr<QObject> object(component.create());
  ASSERT_NE(object, nullptr) << component.errorString().toStdString();
  auto *page = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(page, nullptr);
  window.resize(1113, 700);
  page->setParentItem(window.contentItem());
  page->setSize({1113, 700});
  window.show();
  auto *list = page->findChild<QQuickItem *>("gamesList");
  ASSERT_NE(list, nullptr);
  ASSERT_TRUE(waitUntil([&] { return list->property("contentHeight").toDouble() > 2000; }));
  list->setProperty("contentY", 700);
  QTest::qWait(50);
  const double scroll = list->property("contentY").toDouble();
  ASSERT_GT(scroll, 500);
  const QPointF position = list->mapToScene(QPointF());
  const QSizeF size = list->size();
  QPointer<QQuickItem> row;
  std::function<void(QQuickItem *)> findRow = [&](QQuickItem *item) {
    if (item->property("entryKey").toString() == "Game 12") row = item;
    for (auto *child : item->childItems()) findRow(child);
  };
  findRow(list);
  ASSERT_FALSE(row.isNull());
  QSignalSpy resets(&filtered, &QAbstractItemModel::modelReset);
  for (int state : {2, 0, 2, 0}) {
    entries[12].listed = state;
    model.setEntries(entries); // Same production rebuild entry point as detection actions.
    QTest::qWait(30);
    ASSERT_FALSE(row.isNull());
    EXPECT_EQ(row->property("entryListed").toInt(), state);
    EXPECT_EQ(list->property("contentY").toDouble(), scroll);
    EXPECT_EQ(list->mapToScene(QPointF()), position);
    EXPECT_EQ(list->size(), size);
  }
  EXPECT_EQ(resets.size(), 0);
}
} // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
