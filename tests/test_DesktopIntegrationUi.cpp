/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Trophy - Game recording and clipping
 *
 * The Settings "Desktop Integration" card must stay hidden outside an
 * AppImage, and must reflect registration/entry state live so the fallback
 * dialog policy and the visible status never disagree.
 */

#include "settings/SettingsManager.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

#include <memory>

class IntegrationUiApp : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool vulkanLayerRegistered READ no CONSTANT)
  Q_PROPERTY(bool glShimAvailable READ no CONSTANT)
  Q_PROPERTY(bool trayAvailable READ no CONSTANT)
  Q_PROPERTY(bool canRestartTrophy READ canRestartTrophy WRITE setCanRestartTrophy NOTIFY stateChanged)

public:
  bool no() const { return false; }
  bool canRestartTrophy() const { return m_canRestart; }
  void setCanRestartTrophy(bool value) {
    if (m_canRestart == value)
      return;
    m_canRestart = value;
    emit stateChanged();
  }
  Q_INVOKABLE void restartTrophy() { ++restarts; }
  Q_INVOKABLE bool isEncoderAvailable(const QString &) const { return true; }
  int restarts = 0;

signals:
  void stateChanged();

private:
  bool m_canRestart = true;
};

// Stand-in for DesktopIntegration: same properties/signal shape.
class FakeIntegration : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool isAppImage READ isAppImage WRITE setAppImage NOTIFY stateChanged)
  Q_PROPERTY(bool isIntegrated READ isIntegrated WRITE setIntegrated NOTIFY stateChanged)
  Q_PROPERTY(bool isRegistered READ isRegistered WRITE setRegistered NOTIFY stateChanged)
  Q_PROPERTY(bool restartRequired READ restartRequired WRITE setRestartRequired NOTIFY stateChanged)
  Q_PROPERTY(QString appImagePath READ appImagePath CONSTANT)
  Q_PROPERTY(QString managedEntry READ managedEntry CONSTANT)
  Q_PROPERTY(QString statusMessage READ statusMessage WRITE setStatusMessage NOTIFY statusChanged)
  Q_PROPERTY(bool statusIsError READ statusIsError WRITE setStatusIsError NOTIFY statusChanged)

public:
  bool isAppImage() const { return m_isAppImage; }
  void setAppImage(bool value) {
    if (m_isAppImage == value)
      return;
    m_isAppImage = value;
    emit stateChanged();
  }
  bool isIntegrated() const { return m_isIntegrated; }
  void setIntegrated(bool value) {
    if (m_isIntegrated == value)
      return;
    m_isIntegrated = value;
    emit stateChanged();
  }
  bool isRegistered() const { return m_isRegistered; }
  QString managedEntry() const { return QString(); }
  QString appImagePath() const {
    return QStringLiteral("/home/deck/Applications/Trophy-0.9.7-x86_64.AppImage");
  }
  void setRegistered(bool value) {
    if (m_isRegistered == value)
      return;
    m_isRegistered = value;
    emit stateChanged();
  }
  bool restartRequired() const { return m_restartRequired; }
  void setRestartRequired(bool value) {
    if (m_restartRequired == value)
      return;
    m_restartRequired = value;
    emit stateChanged();
  }
  QString statusMessage() const { return m_statusMessage; }
  void setStatusMessage(const QString &value) {
    if (m_statusMessage == value)
      return;
    m_statusMessage = value;
    emit statusChanged();
  }
  bool statusIsError() const { return m_statusIsError; }
  void setStatusIsError(bool value) {
    if (m_statusIsError == value)
      return;
    m_statusIsError = value;
    emit statusChanged();
  }
  Q_INVOKABLE bool integrate() {
    ++integrations;
    setStatusMessage(QStringLiteral("Desktop entry installed."));
    setStatusIsError(false);
    setIntegrated(true);
    setRegistered(true);
    return true;
  }

  int integrations = 0;

signals:
  void stateChanged();
  void statusChanged();

private:
  bool m_isAppImage = false;
  bool m_isIntegrated = false;
  bool m_isRegistered = false;
  bool m_restartRequired = false;
  QString m_statusMessage;
  bool m_statusIsError = false;
};

TEST(DesktopIntegrationUiTest, CardIsAppImageOnlyAndTracksRegistrationAndEntryState) {
  QTemporaryDir config;
  ASSERT_TRUE(config.isValid());
  const auto oldConfig = qgetenv("XDG_CONFIG_HOME");
  qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
  {
    TrophyBackend::SettingsManager settings;
    IntegrationUiApp app;
    FakeIntegration integration;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("settingsManager", &settings);
    engine.rootContext()->setContextProperty("app", &app);
    engine.rootContext()->setContextProperty("desktopIntegration", &integration);
    for (const auto *name : {"recordingManager", "replayManager", "storageManager",
                             "audioDeviceEnumerator", "windowMonitor", "globalShortcuts",
                             "hardwareInfo"})
      engine.rootContext()->setContextProperty(name, static_cast<QObject *>(nullptr));

    QStringList errors;
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [&](const QList<QQmlError> &warnings) {
                       for (const auto &warning : warnings)
                         errors << warning.toString();
                     });

    QQmlComponent component(&engine, QUrl::fromLocalFile(SETTINGS_QML_FILE));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> page(component.create());
    ASSERT_NE(page, nullptr) << component.errorString().toStdString();

    auto *item = qobject_cast<QQuickItem *>(page.get());
    ASSERT_NE(item, nullptr);
    QQuickWindow window;
    item->setParentItem(window.contentItem());
    item->setWidth(1024);
    item->setHeight(720);
    window.show();
    QCoreApplication::processEvents();

    auto *card = page->findChild<QObject *>("desktopIntegrationCard");
    auto *status = page->findChild<QObject *>("desktopIntegrationStatus");
    auto *button = page->findChild<QObject *>("desktopIntegrationButton");
    auto *location = page->findChild<QObject *>("desktopIntegrationLocation");
    auto *restartRow = page->findChild<QObject *>("desktopIntegrationRestartRow");
    auto *restartButton = page->findChild<QObject *>("desktopIntegrationRestartButton");
    auto *message = page->findChild<QObject *>("desktopIntegrationMessage");
    ASSERT_NE(card, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(button, nullptr);
    ASSERT_NE(location, nullptr);
    ASSERT_NE(restartRow, nullptr);
    ASSERT_NE(restartButton, nullptr);
    ASSERT_NE(message, nullptr);

    // A native/portable install must not show the section at all.
    EXPECT_FALSE(card->property("visible").toBool());

    // ...while an AppImage gets it, and the state labels follow the backend.
    integration.setAppImage(true);
    QCoreApplication::processEvents();
    EXPECT_TRUE(card->property("visible").toBool());
    EXPECT_EQ(status->property("text").toString(), QStringLiteral("Not registered"));
    // The section must say where the running AppImage actually is.
    EXPECT_EQ(location->property("text").toString(),
              QStringLiteral("/home/deck/Applications/Trophy-0.9.7-x86_64.AppImage"));
    EXPECT_TRUE(location->property("readOnly").toBool());
    EXPECT_EQ(button->property("text").toString(), QStringLiteral("Integrate"));

    integration.setRegistered(true);
    integration.setIntegrated(true);
    QCoreApplication::processEvents();
    EXPECT_EQ(status->property("text").toString(), QStringLiteral("Registered"));
    EXPECT_EQ(button->property("text").toString(), QStringLiteral("Repair"));

    // The button drives the backend and its result is surfaced (previously
    // integrate() failures were silent). The message row is bound to the
    // status text, so an empty message also clears the visible line.
    EXPECT_TRUE(message->property("text").toString().isEmpty());
    ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
    QCoreApplication::processEvents();
    EXPECT_EQ(integration.integrations, 1);
    EXPECT_EQ(message->property("text").toString(),
              QStringLiteral("Desktop entry installed."));
    integration.setStatusMessage(QString());
    QCoreApplication::processEvents();
    EXPECT_TRUE(message->property("text").toString().isEmpty());

    for (const QString &error : errors)
      EXPECT_FALSE(error.contains("ReferenceError") || error.contains("TypeError") ||
                   error.contains("desktopIntegration", Qt::CaseInsensitive))
          << error.toStdString();
  }
  if (oldConfig.isNull())
    qunsetenv("XDG_CONFIG_HOME");
  else
    qputenv("XDG_CONFIG_HOME", oldConfig);
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  QGuiApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "test_DesktopIntegrationUi.moc"
