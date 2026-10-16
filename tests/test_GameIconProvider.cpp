/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "detection/GameIconProvider.h"
#include "detection/GameListModel.h"
#include "detection/DefaultAppCatalog.h"

#include <QDir>
#include <QCryptographicHash>
#include <QDateTime>
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
#include <gtest/gtest.h>
#include <functional>
#include <memory>

using namespace TrophyBackend;

namespace {
class GameIconTest : public testing::Test {
protected:
  QTemporaryDir temp;
  QString root() const { return temp.path(); }
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
    return image;
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
  engine.rootContext()->setContextProperty("windowService", QVariant::fromValue<QObject *>(nullptr));
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
  const QVariantList windows{
      QVariantMap{{"title", "Firefox window"}, {"class", "firefox"},
                  {"iconSource", gameIconSource("firefox")}},
      QVariantMap{{"title", "Missing program"}, {"class", "missing-app"},
                  {"iconSource", gameIconSource("missing-app")}}};
  ASSERT_TRUE(dialog->setProperty("allWindows", windows));
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
        images[1]->property("status").toInt() == 3)
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
        images[1]->property("status").toInt() == 3 &&
        images[2]->property("status").toInt() == 1 &&
        images[3]->property("status").toInt() == 1)
      break;
  }
  ASSERT_EQ(images.size(), 4);
  ASSERT_EQ(letters.size(), 4);
  EXPECT_EQ(images[0]->property("status").toInt(), 1);
  EXPECT_TRUE(images[0]->isVisible());
  EXPECT_FALSE(letters[0]->isVisible());
  EXPECT_EQ(images[1]->property("status").toInt(), 3); // Image.Error
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
} // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
