/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "settings/SettingsManager.h"
#include <QFont>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <gtest/gtest.h>
#include <memory>

static QQuickItem *findItem(QQuickItem *root, const QString &name) {
  if (root->objectName() == name)
    return root;
  for (auto *child : root->childItems())
    if (auto *item = findItem(child, name))
      return item;
  return nullptr;
}

static int missingLaunchers(QQuickItem *root) {
  int count = root->property("text").toString() == "Not detected" ? 1 : 0;
  for (auto *child : root->childItems())
    count += missingLaunchers(child);
  return count;
}

static bool containsText(QQuickItem *root, const QString &text) {
  if (root->property("text").toString() == text)
    return true;
  for (auto *child : root->childItems())
    if (containsText(child, text))
      return true;
  return false;
}

class LibraryUiBackend : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList gameLibraries MEMBER rows NOTIFY changed)
  Q_PROPERTY(bool librariesScanning MEMBER scanning NOTIFY changed)
public:
  QVariantList rows;
  bool scanning = false;
  int rescans = 0;
  QString automaticLauncher;
  bool automatic = true;
  Q_INVOKABLE void setLibraryAutomatic(const QString &id, bool enabled) {
    automaticLauncher = id;
    automatic = enabled;
    for (auto &value : rows) {
      auto row = value.toMap();
      if (row.value("id").toString() == id) {
        row["automatic"] = enabled;
        value = row;
      }
    }
    emit changed();
  }
  Q_INVOKABLE void rescanGameLibraries() {
    ++rescans;
    scanning = true;
    emit changed();
  }
signals:
  void changed();
};

class LibraryUiApp : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool trayAvailable READ no CONSTANT)
  Q_PROPERTY(bool runWithoutTraySupported READ no CONSTANT)
  Q_PROPERTY(bool canRestartTrophy READ no CONSTANT)
  Q_PROPERTY(bool useNativeDialogs READ no CONSTANT)
public:
  bool no() const { return false; }
  Q_INVOKABLE bool isEncoderAvailable(const QString &) const { return true; }
};

class SetupUiAudio : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList audioApps MEMBER apps NOTIFY changed)
  Q_PROPERTY(QVariantList inputDevices READ inputs CONSTANT)
  Q_PROPERTY(QVariantList outputDevices READ inputs CONSTANT)
public:
  QVariantList apps;
  QVariantList inputs() const { return {}; }
  int refreshes = 0;
  Q_INVOKABLE void refreshAudioApps() { ++refreshes; }
signals:
  void changed();
};

class SetupUiLibrary : public QObject {
  Q_OBJECT
public:
  Q_INVOKABLE QUrl localFileUrl(const QString &path) { return QUrl::fromLocalFile(path); }
};

class SetupUiHardware : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool nvencAvailable READ no CONSTANT)
  Q_PROPERTY(bool vaapiAvailable READ no CONSTANT)
  Q_PROPERTY(bool dmaBufAvailable READ no CONSTANT)
  Q_PROPERTY(bool hasNvidiaGpu READ no CONSTANT)
  Q_PROPERTY(bool hasAmdGpu READ no CONSTANT)
  Q_PROPERTY(bool hasIntelGpu READ no CONSTANT)
  Q_PROPERTY(QStringList gpuDescriptions READ descriptions CONSTANT)
  Q_PROPERTY(QString recommendedEncoderName READ encoder CONSTANT)
  Q_PROPERTY(QString recommendedEncoderDisplayName READ encoder CONSTANT)
public:
  bool no() const { return false; }
  QStringList descriptions() const { return {}; }
  QString encoder() const { return "x264"; }
};

class SetupUiShortcuts : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool available READ no CONSTANT)
  Q_PROPERTY(bool bound READ no CONSTANT)
  Q_PROPERTY(QVariantMap boundShortcuts READ shortcuts CONSTANT)
  Q_PROPERTY(QString lastError READ error CONSTANT)
public:
  int requests = 0;
  bool no() const { return false; }
  QVariantMap shortcuts() const { return {}; }
  QString error() const { return {}; }
  Q_INVOKABLE void ensureSetup() { ++requests; }
};

static bool visibleText(QQuickItem *root, const QString &text) {
  if (!root->isVisible())
    return false;
  if (root->property("text").toString() == text)
    return true;
  for (auto *child : root->childItems())
    if (visibleText(child, text))
      return true;
  return false;
}

TEST(SetupWizardUiTest, SevenPagesNavigateFinishAndSummarizeLibrariesWithoutRefresh) {
  TrophyBackend::SettingsManager settings;
  settings.setVoiceChatApp("discord");
  LibraryUiApp app;
  SetupUiAudio audio;
  SetupUiLibrary library;
  SetupUiHardware hardware;
  SetupUiShortcuts shortcuts;
  LibraryUiBackend detector;
  detector.scanning = true;
  QQmlEngine engine;
  engine.rootContext()->setContextProperty("settingsManager", &settings);
  engine.rootContext()->setContextProperty("app", &app);
  engine.rootContext()->setContextProperty("audioDeviceEnumerator", &audio);
  engine.rootContext()->setContextProperty("recordingLibrary", &library);
  engine.rootContext()->setContextProperty("hardwareInfo", &hardware);
  engine.rootContext()->setContextProperty("globalShortcuts", &shortcuts);
  engine.rootContext()->setContextProperty("gameDetector", &detector);
  for (const auto *name : {"recordingManager", "storageManager"})
    engine.rootContext()->setContextProperty(name, static_cast<QObject *>(nullptr));
  QStringList errors;
  QObject::connect(&engine, &QQmlEngine::warnings, &engine,
      [&](const QList<QQmlError> &warnings) {
        for (const auto &warning : warnings)
          errors.append(warning.toString());
      });
  QQmlComponent component(&engine, QUrl::fromLocalFile(WIZARD_QML_FILE));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> wizard(component.create());
  ASSERT_NE(wizard, nullptr) << component.errorString().toStdString();
  auto *item = qobject_cast<QQuickItem *>(wizard.get());
  ASSERT_NE(item, nullptr);
  QQuickWindow window;
  window.resize(900, 760);
  item->setParentItem(window.contentItem());
  window.show();
  QTest::qWait(300);
  auto *stack = findItem(item, "setupWizardSteps");
  auto *next = findItem(item, "wizardNextButton");
  auto *back = findItem(item, "wizardBackButton");
  ASSERT_NE(stack, nullptr);
  ASSERT_NE(next, nullptr);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(wizard->property("totalSteps").toInt(), 7);
  EXPECT_EQ(stack->property("count").toInt(), 7);
  EXPECT_FALSE(containsText(item, "Game Detection Setup"));
  EXPECT_FALSE(containsText(item, "Refresh"));
  const QStringList titles{"Let's Get Started", "Storage Settings", "Audio Configuration",
                           "Game Libraries", "Your Hardware", "Keyboard Shortcuts", "You're All Set!"};
  for (int step = 0; step < titles.size(); ++step) {
    EXPECT_EQ(stack->property("currentIndex").toInt(), step);
    EXPECT_TRUE(visibleText(item, titles[step])) << titles[step].toStdString();
    if (step == 2) {
      auto *voice = findItem(item, "wizardVoiceAppPicker");
      ASSERT_NE(voice, nullptr);
      audio.apps = {QVariantMap{{"name", "Discord live"}, {"binary", "discord"}}};
      emit audio.changed();
      QCoreApplication::processEvents();
      EXPECT_EQ(voice->property("displayText").toString(), "Discord live");
      audio.apps.clear();
      emit audio.changed();
      QCoreApplication::processEvents();
      EXPECT_EQ(voice->property("displayText").toString(), "discord");
      EXPECT_EQ(settings.voiceChatApp(), "discord");
    }
    if (step == 3) {
      EXPECT_TRUE(next->isEnabled()); // Discovery must not block navigation.
      EXPECT_TRUE(visibleText(item, "Scanning local game libraries…"));
      auto *count = findItem(item, "wizardLibraryGameCount");
      ASSERT_NE(count, nullptr);
      EXPECT_EQ(count->property("text").toString(), QString::fromUtf8("…"));
      const auto font = count->property("font").value<QFont>();
      EXPECT_EQ(font.pixelSize(), 80);
      EXPECT_EQ(font.weight(), QFont::ExtraBold);
      detector.scanning = false;
      emit detector.changed();
      QCoreApplication::processEvents();
      EXPECT_EQ(count->property("text").toString(), "0");
      EXPECT_TRUE(visibleText(item, "No supported launcher games found. You can still add games manually."));
      const QVariantMap sharedGame{{"name", "Shared Game"}, {"directory", "/games/shared"}};
      detector.rows = {
          QVariantMap{{"id", "steam"}, {"name", "Steam"}, {"detected", true},
              {"games", QVariantList{sharedGame, QVariantMap{{"name", "Other Game"}, {"directory", "/games/other"}}}}},
          QVariantMap{{"id", "heroic"}, {"name", "Heroic"}, {"detected", true},
              {"games", QVariantList{sharedGame}}},
          QVariantMap{{"id", "lutris"}, {"name", "Lutris"}, {"detected", true}, {"games", QVariantList{}}},
          QVariantMap{{"id", "nile"}, {"name", "Nile"}, {"detected", false}, {"games", QVariantList{}}}};
      emit detector.changed();
      QTest::qWait(50);
      EXPECT_EQ(count->property("text").toString(), "2");
      EXPECT_TRUE(visibleText(item, "Games found in your launcher libraries"));
      for (const auto *id : {"steam", "heroic", "lutris"}) {
        auto *icon = findItem(item, QString("wizardLauncherIcon_") + id);
        ASSERT_NE(icon, nullptr);
        EXPECT_TRUE(icon->isVisible());
        EXPECT_EQ(icon->property("status").toInt(), 1);
      }
      EXPECT_EQ(findItem(item, "wizardLauncherIcon_nile"), nullptr);
      EXPECT_EQ(detector.rescans, 0);
      ASSERT_TRUE(QMetaObject::invokeMethod(back, "clicked"));
      QCoreApplication::processEvents();
      EXPECT_EQ(stack->property("currentIndex").toInt(), 2);
      ASSERT_TRUE(QMetaObject::invokeMethod(next, "clicked"));
      QCoreApplication::processEvents();
    }
    ASSERT_TRUE(QMetaObject::invokeMethod(next, "clicked"));
    QCoreApplication::processEvents();
  }
  EXPECT_TRUE(settings.setupCompleted());
  EXPECT_EQ(settings.wizardStep(), 0);
  EXPECT_EQ(shortcuts.requests, 0);
  EXPECT_EQ(audio.refreshes, 0);
  for (const auto &error : errors)
    EXPECT_FALSE(error.contains("ReferenceError") || error.contains("TypeError") ||
                 error.contains("Binding loop") || error.contains("recursive rearrange"))
        << error.toStdString();
}

TEST(GameLibrariesUiTest, ListsMissingLaunchersShowsGamesAndRequestsRefresh) {
  TrophyBackend::SettingsManager settings;
  settings.setVoiceChatApp("discord");
  LibraryUiApp app;
  SetupUiAudio audio;
  LibraryUiBackend backend;
  const QStringList ids{"steam", "heroic", "legendary", "nile", "lutris", "faugus"};
  for (const auto &id : ids)
    backend.rows.append(QVariantMap{{"id", id}, {"name", id},
        {"automatic", true}, {"paths", QStringList{}}, {"locations", QVariantList{}},
        {"detected", id == "steam"}, {"count", id == "steam" ? 2 : 0},
        {"games", id == "steam" ? QVariantList{QVariantMap{{"name", "First Game"}},
                                             QVariantMap{{"name", "Second Game"}}}
                               : QVariantList{}}});
  auto steam = backend.rows[0].toMap();
  auto steamGames = steam.value("games").toList();
  for (int i = 3; i <= 52; ++i)
    steamGames.append(QVariantMap{{"name", QString("Game %1").arg(i)}});
  steam["games"] = steamGames;
  steam["count"] = 52;
  backend.rows[0] = steam;
  QQmlEngine engine;
  engine.rootContext()->setContextProperty("settingsManager", &settings);
  engine.rootContext()->setContextProperty("gameDetector", &backend);
  engine.rootContext()->setContextProperty("app", &app);
  engine.rootContext()->setContextProperty("audioDeviceEnumerator", &audio);
  for (const auto *name : {"recordingManager", "replayManager", "storageManager",
                          "globalShortcuts", "hardwareInfo",
                          "desktopIntegration"})
    engine.rootContext()->setContextProperty(name, static_cast<QObject *>(nullptr));
  QStringList errors;
  QObject::connect(&engine, &QQmlEngine::warnings, &engine,
      [&](const QList<QQmlError> &warnings) {
        for (const auto &warning : warnings)
          errors.append(warning.toString());
      });
  QQmlComponent component(&engine, QUrl::fromLocalFile(SETTINGS_QML_FILE));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> page(component.create());
  ASSERT_NE(page, nullptr) << component.errorString().toStdString();
  auto *item = qobject_cast<QQuickItem *>(page.get());
  ASSERT_NE(item, nullptr);
  QQuickWindow window;
  window.resize(1024, 720);
  item->setParentItem(window.contentItem());
  item->setSize(QSizeF(1024, 720));
  window.show();
  auto *card = page->findChild<QObject *>("gameDetectionCard");
  ASSERT_NE(card, nullptr);
  card->setProperty("expanded", true);
  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "scrollToSection",
      Q_ARG(QVariant, QVariant(QStringLiteral("Game Libraries")))));
  QTest::qWait(500);
  EXPECT_FALSE(containsText(item, "Refresh"));
  auto *voice = findItem(item, "settingsVoiceAppPicker");
  EXPECT_TRUE(containsText(item, "If a game is not recognized or does not use a supported launcher, start the game, then use Add Game in the Games tab to add it manually.\n\nDetected means local metadata was found, not that the launcher is running. An empty game list can mean no supported launch tasks were found. Trophy does not launch games or modify these files, and listing a game here does not authorize recording."));
  ASSERT_NE(voice, nullptr);
  audio.apps = {QVariantMap{{"name", "Discord live"}, {"binary", "discord"}}};
  emit audio.changed();
  QCoreApplication::processEvents();
  EXPECT_EQ(voice->property("displayText").toString(), "Discord live");
  EXPECT_EQ(settings.voiceChatApp(), "discord");
  EXPECT_EQ(audio.refreshes, 0);
  QMap<QString, QQuickItem *> cards;
  for (auto *candidate : page->findChildren<QQuickItem *>()) {
    const auto title = candidate->property("sectionTitle").toString();
    if (!title.isEmpty())
      cards.insert(title, candidate);
  }
  const QStringList order{"Video Recording", "Audio Recording", "Background Clips",
      "Game Detection & Libraries", "Keyboard Shortcuts", "Storage Management",
      "System Tray", "Notifications", "Discord Rich Presence", "Customization"};
  for (int i = 1; i < order.size(); ++i) {
    ASSERT_TRUE(cards.contains(order[i - 1]));
    ASSERT_TRUE(cards.contains(order[i]));
    EXPECT_LT(cards[order[i - 1]]->y(), cards[order[i]]->y())
        << order[i - 1].toStdString() << " should precede " << order[i].toStdString();
  }
  EXPECT_FALSE(cards.contains("Game Libraries"));
  auto *libraries = findItem(item, "gameLibrariesSection");
  ASSERT_NE(libraries, nullptr);
  EXPECT_TRUE(containsText(libraries, "Game Libraries"));
  EXPECT_FALSE(libraries->property("expanded").isValid());
  auto *detection = qobject_cast<QQuickItem *>(card);
  ASSERT_NE(detection, nullptr);
  auto *ancestor = libraries->parentItem();
  while (ancestor && ancestor != detection)
    ancestor = ancestor->parentItem();
  EXPECT_EQ(ancestor, detection);
  EXPECT_LT(libraries->mapToItem(detection, QPointF()).y(), detection->height());
  EXPECT_TRUE(QMetaObject::invokeMethod(page.get(), "scrollToSection",
      Q_ARG(QVariant, QVariant(QStringLiteral("Game Detection & Libraries")))));
  QCoreApplication::processEvents();
  EXPECT_TRUE(card->property("expanded").toBool());
  auto *table = findItem(item, "gameLibrariesTable");
  ASSERT_NE(table, nullptr);
  EXPECT_LT(table->height(), 410);
  auto *refresh = page->findChild<QObject *>("rescanGameLibrariesButton");
  ASSERT_NE(refresh, nullptr);
  EXPECT_TRUE(refresh->property("enabled").toBool());
  for (const auto &id : ids) {
    auto *icon = findItem(item, id + "LibraryIcon");
    ASSERT_NE(icon, nullptr);
    EXPECT_EQ(icon->width(), 24);
    EXPECT_EQ(icon->height(), 24);
    EXPECT_EQ(icon->property("status").toInt(), 1); // Image.Ready
    EXPECT_TRUE(icon->property("source").toUrl().toString().startsWith("qrc:/icons/"));
    auto *dropdown = findItem(item, id + "DetectedGamesDropdown");
    ASSERT_NE(dropdown, nullptr) << id.toStdString() << "\n" << errors.join('\n').toStdString();
    auto *indicator = findItem(item, id + "LibraryStatusIndicator");
    ASSERT_NE(indicator, nullptr);
    EXPECT_EQ(indicator->width(), 8);
    EXPECT_EQ(indicator->property("color").value<QColor>().alpha(), id == "steam" ? 255 : 0);
    auto *popup = dropdown->findChild<QObject *>(id + "DetectedGamesPopup");
    ASSERT_NE(popup, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(dropdown, "clicked"));
    QTest::qWait(180);
    EXPECT_TRUE(popup->property("visible").toBool());
    auto *popupContent = popup->property("contentItem").value<QQuickItem *>();
    ASSERT_NE(popupContent, nullptr);
    auto *list = findItem(popupContent, id + "DetectedGamesList");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->property("count").toInt(), id == "steam" ? 52 : 0);
    EXPECT_TRUE(containsText(popupContent, id == "steam" ? "First Game" : "No usable games detected"));
    if (id == "steam") {
      EXPECT_TRUE(containsText(popupContent, "Second Game"));
      EXPECT_LE(list->height(), 260);
      ASSERT_TRUE(QMetaObject::invokeMethod(list, "positionViewAtEnd"));
      QTest::qWait(100);
      EXPECT_TRUE(containsText(popupContent, "Game 52"));
    }
    QTest::keyClick(&window, Qt::Key_Escape);
    QTest::qWait(180);
    EXPECT_FALSE(popup->property("visible").toBool());
    auto *locations = findItem(item, id + "LibraryLocationsButton");
    ASSERT_NE(locations, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(locations, "clicked"));
    QTest::qWait(180);
    auto *locationPopup = locations->findChild<QObject *>(id + "LibraryLocationsPopup");
    ASSERT_NE(locationPopup, nullptr);
    EXPECT_TRUE(locationPopup->property("visible").toBool());
    auto *locationContent = locationPopup->property("contentItem").value<QQuickItem *>();
    ASSERT_NE(locationContent, nullptr);
    if (id == "steam") {
      auto *automatic = findItem(locationContent, id + "LibraryAutomaticSwitch");
      ASSERT_NE(automatic, nullptr);
      automatic->setProperty("checked", false);
      ASSERT_TRUE(QMetaObject::invokeMethod(automatic, "toggled"));
      QCoreApplication::processEvents();
      EXPECT_EQ(backend.automaticLauncher, "steam");
      EXPECT_FALSE(backend.automatic);
      EXPECT_TRUE(locationPopup->property("visible").toBool());
    }
    QMetaObject::invokeMethod(locationPopup, "close");
  }
  EXPECT_EQ(missingLaunchers(item), 5);
  ASSERT_TRUE(QMetaObject::invokeMethod(refresh, "clicked"));
  QCoreApplication::processEvents();
  EXPECT_EQ(backend.rescans, 1);
  EXPECT_FALSE(refresh->property("enabled").toBool());
  backend.scanning = false;
  auto heroic = backend.rows[1].toMap();
  heroic["detected"] = true;
  heroic["count"] = 1;
  heroic["games"] = QVariantList{QVariantMap{{"name", "Newly Installed Game"}}};
  backend.rows[1] = heroic;
  emit backend.changed();
  QCoreApplication::processEvents();
  auto *updated = findItem(item, "heroicDetectedGamesDropdown");
  ASSERT_NE(updated, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(updated, "clicked"));
  QTest::qWait(180);
  auto *updatedPopup = updated->findChild<QObject *>("heroicDetectedGamesPopup");
  ASSERT_NE(updatedPopup, nullptr);
  auto *updatedContent = updatedPopup->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(updatedContent, nullptr);
  auto *updatedList = findItem(updatedContent, "heroicDetectedGamesList");
  ASSERT_NE(updatedList, nullptr);
  EXPECT_EQ(updatedList->property("count").toInt(), 1);
  EXPECT_TRUE(containsText(updatedContent, "Newly Installed Game"));
  EXPECT_TRUE(updatedPopup->property("visible").toBool());
  QMetaObject::invokeMethod(updatedPopup, "close");
  EXPECT_TRUE(refresh->property("enabled").toBool());
  item->setWidth(640);
  window.resize(640, 720);
  QTest::qWait(100);
  auto *statusHeader = findItem(table, "libraryStatusColumnHeader");
  ASSERT_NE(statusHeader, nullptr);
  for (const auto &id : ids) {
    auto *button = findItem(item, id + "LibraryLocationsButton");
    ASSERT_NE(button, nullptr);
    EXPECT_LE(button->mapToItem(table, QPointF(button->width(), 0)).x(), table->width());
    auto *indicator = findItem(table, id + "LibraryStatusIndicator");
    ASSERT_NE(indicator, nullptr);
    EXPECT_NEAR(indicator->mapToItem(table, QPointF()).x(),
                statusHeader->mapToItem(table, QPointF()).x(), 1);
  }
  if (!qEnvironmentVariableIsEmpty("TROPHY_LIBRARY_UI_SCREENSHOT"))
    window.grabWindow().save(qEnvironmentVariable("TROPHY_LIBRARY_UI_SCREENSHOT"));
  for (const auto &error : errors)
    EXPECT_FALSE(error.contains("ReferenceError") || error.contains("TypeError") ||
                 error.contains("Binding loop") || error.contains("recursive rearrange"))
        << error.toStdString();
}

TEST(InformationUiTest, DescribesCurrentFeaturesAndCreditsActualIntegrations) {
  QQmlEngine engine;
  engine.rootContext()->setContextProperty("recordingManager", static_cast<QObject *>(nullptr));
  QStringList warnings;
  QObject::connect(&engine, &QQmlEngine::warnings, &engine,
      [&](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
          warnings.append(error.toString());
      });
  QQmlComponent component(&engine, QUrl::fromLocalFile(INFO_QML_FILE));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> page(component.create());
  ASSERT_NE(page, nullptr) << component.errorString().toStdString();
  QString text;
  for (auto *child : page->findChildren<QObject *>()) {
    text += child->property("text").toString() + '\n';
    text += child->property("description").toString() + '\n';
    text += child->property("title").toString() + '\n';
  }
  for (const auto *expected : {"Process-based detection", "recording requires authorization",
       "desktop fallback may include voice", "bookmark a full recording",
        "install when you choose", "Steam, Heroic and Lutris icons", "Papirus",
        "Legendary", "Nile", "Felix Ribeiro", "CC BY 4.0"})
    EXPECT_TRUE(text.contains(expected)) << expected;
  for (const auto *obsolete : {"KDE KWin", "Hyprland", "Sway", "GNOME / Mutter",
        "Automatic Updates", "save recent moments anytime", "SQLite", "LibYAML",
        "Khronos", "Vulkan-Headers", "linuxdeploy"})
    EXPECT_FALSE(text.contains(obsolete)) << obsolete;
  EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  QGuiApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#include "test_GameLibrariesUi.moc"
