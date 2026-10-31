/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "settings/SettingsManager.h"
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>
#include <memory>

class PresenceUiApp : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool vulkanLayerRegistered READ no CONSTANT)
  Q_PROPERTY(bool trayAvailable READ no CONSTANT)
public:
  bool no() const { return false; }
  Q_INVOKABLE bool isEncoderAvailable(const QString &) const { return true; }
};

TEST(DiscordPresenceUiTest, DefaultsEnablementUserIntentAndReset) {
  QTemporaryDir config;
  ASSERT_TRUE(config.isValid());
  const auto oldConfig = qgetenv("XDG_CONFIG_HOME");
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  {
    TrophyBackend::SettingsManager settings;
    PresenceUiApp app;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("settingsManager", &settings);
    engine.rootContext()->setContextProperty("app", &app);
    for (const auto *name : {"recordingManager", "replayManager", "storageManager",
                             "audioDeviceEnumerator", "windowMonitor", "globalShortcuts", "hardwareInfo"})
      engine.rootContext()->setContextProperty(name, static_cast<QObject *>(nullptr));
    QStringList errors;
    QObject::connect(&engine, &QQmlEngine::warnings, &engine, [&](const QList<QQmlError> &warnings) {
      for (const auto &warning : warnings) errors << warning.toString();
    });
    QQmlComponent component(&engine, QUrl::fromLocalFile(SETTINGS_QML_FILE));
    std::unique_ptr<QObject> page(component.create());
    ASSERT_NE(page, nullptr) << component.errorString().toStdString();
    auto *master = page->findChild<QObject *>("discordPresenceSwitch");
    auto *names = page->findChild<QObject *>("discordGameNamesSwitch");
    auto *idle = page->findChild<QObject *>("discordIdleSwitch");
    ASSERT_NE(master, nullptr);
    ASSERT_NE(names, nullptr);
    ASSERT_NE(idle, nullptr);
    auto *namesRow = page->findChild<QObject *>("discordGameNamesRow");
    auto *idleRow = page->findChild<QObject *>("discordIdleRow");
    ASSERT_NE(namesRow, nullptr);
    ASSERT_NE(idleRow, nullptr);
    EXPECT_DOUBLE_EQ(namesRow->property("opacity").toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(idleRow->property("opacity").toDouble(), 0.5);
    EXPECT_FALSE(master->property("checked").toBool());
    EXPECT_TRUE(names->property("checked").toBool());
    EXPECT_FALSE(idle->property("checked").toBool());
    EXPECT_FALSE(names->property("enabled").toBool());
    EXPECT_FALSE(idle->property("enabled").toBool());
    ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "scrollToSection",
        Q_ARG(QVariant, QVariant("Discord Rich Presence"))));
    // Explicit activation writes preferences; normal binding refreshes do not.
    master->setProperty("checked", true);
    ASSERT_TRUE(QMetaObject::invokeMethod(master, "toggled"));
    EXPECT_TRUE(settings.discordPresenceEnabled());
    EXPECT_DOUBLE_EQ(namesRow->property("opacity").toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(idleRow->property("opacity").toDouble(), 1.0);
    EXPECT_TRUE(names->property("enabled").toBool());
    names->setProperty("checked", false);
    ASSERT_TRUE(QMetaObject::invokeMethod(names, "toggled"));
    idle->setProperty("checked", true);
    ASSERT_TRUE(QMetaObject::invokeMethod(idle, "toggled"));
    EXPECT_FALSE(settings.discordShowGameNames());
    EXPECT_TRUE(settings.discordShowWhenIdle());
    settings.setDiscordPresenceEnabled(false);
    EXPECT_DOUBLE_EQ(namesRow->property("opacity").toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(idleRow->property("opacity").toDouble(), 0.5);
    EXPECT_FALSE(master->property("checked").toBool());
    EXPECT_FALSE(names->property("enabled").toBool());
    EXPECT_FALSE(names->property("checked").toBool()); // Disabled retains preference.
    EXPECT_TRUE(idle->property("checked").toBool());
    settings.resetToDefaults();
    EXPECT_FALSE(master->property("checked").toBool());
    EXPECT_TRUE(names->property("checked").toBool());
    EXPECT_FALSE(idle->property("checked").toBool());
    for (const auto &error : errors)
      EXPECT_FALSE(error.contains("ReferenceError") || error.contains("TypeError") ||
                   error.contains("discord", Qt::CaseInsensitive)) << error.toStdString();
  }
  if (oldConfig.isNull()) qunsetenv("XDG_CONFIG_HOME");
  else qputenv("XDG_CONFIG_HOME", oldConfig);
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  QGuiApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "test_DiscordPresenceUi.moc"
