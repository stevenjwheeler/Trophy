#include <QColor>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QElapsedTimer>
#include <QTest>
#include <QSignalSpy>
#include "core/WindowChrome.h"
#include <gtest/gtest.h>
#include <memory>

TEST(ThemeTest, LiveModesAccentsAndDetachedDefaults) {
  QQmlPropertyMap settings;
  settings.insert("themeMode", "dark");
  settings.insert("accentColor", "rose");
  QQmlPropertyMap recordingState;
  recordingState.insert("currentGameName", "Test Game");
  recordingState.insert("recordingDuration", 8000);
  recordingState.insert("recordingSize", 1024);
  QQmlEngine engine;
  engine.rootContext()->setContextProperty("recordingManager", &recordingState);
  QStringList warnings;
  QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                   [&](const QList<QQmlError> &errors) {
    for (const auto &error : errors)
      warnings.append(error.toString());
  });
  QQmlComponent component(&engine);
  component.setData(R"(
import QtQuick
import "." as Styles
import "../controls" as Controls
import "../views" as Views
Item {
    id: root
    property var preferences: null
    property color background: Styles.Theme.backgroundColor
    property color surface: Styles.Theme.surfaceColor
    property color card: Styles.Theme.cardColor
    property color accent: Styles.Theme.accentColor
    property color primary: Styles.Theme.primaryColor
    function gradientHead() { return Styles.Theme.primaryGradient.stops[0].color; }
    property color recording: Styles.Theme.recordingColor
    property color error: Styles.Theme.errorColor
    Binding { target: Styles.Theme; property: "settings"; value: root.preferences }
    Controls.AccentColorPicker {
        selectedKey: root.preferences ? root.preferences.accentColor : "rose"
        onColorSelected: function(key) {
            if (root.preferences)
                root.preferences.accentColor = key;
        }
    }
    Views.ActiveRecordingCard { width: 340; height: 248 }
}
)", QUrl::fromLocalFile(QString(THEME_STYLES_DIR) + "/ThemeProbe.qml"));
  std::unique_ptr<QObject> probe(component.create());
  ASSERT_NE(probe, nullptr) << component.errorString().toStdString();
  auto color = [&](const char *name) { return probe->property(name).value<QColor>(); };
  EXPECT_EQ(color("background"), QColor("#000000"));
  EXPECT_EQ(color("accent"), QColor("#e94560"));
  probe->setProperty("preferences", QVariant::fromValue(static_cast<QObject *>(&settings)));
  EXPECT_EQ(color("background"), QColor("#1a1a2e"));
  // Repeater delegates belong to the visual tree, not necessarily the
  // QObject ownership tree used by findChild().
  auto findSwatch = [](auto &&self, QQuickItem *item, const QString &name) -> QQuickItem * {
    if (item->objectName() == name)
      return item;
    for (auto *child : item->childItems()) {
      if (auto *found = self(self, child, name))
        return found;
    }
    return nullptr;
  };
  auto *blue = findSwatch(findSwatch, qobject_cast<QQuickItem *>(probe.get()), "accentSwatch_blue");
  auto *rose = findSwatch(findSwatch, qobject_cast<QQuickItem *>(probe.get()), "accentSwatch_rose");
  ASSERT_NE(blue, nullptr);
  ASSERT_NE(rose, nullptr);
  EXPECT_TRUE(rose->property("selected").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(blue, "clicked"));
  EXPECT_EQ(settings.value("accentColor").toString(), "blue");
  EXPECT_TRUE(blue->property("selected").toBool());
  EXPECT_FALSE(rose->property("selected").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(blue, "clicked"));
  EXPECT_TRUE(blue->property("selected").toBool());
  settings.insert("themeMode", "amoled");
  EXPECT_EQ(color("background"), QColor("#000000"));
  EXPECT_EQ(color("surface"), QColor("#000000"));
  EXPECT_LT(color("card").lightness(), 20);
  settings.insert("themeMode", "charcoal");
  EXPECT_EQ(color("background"), QColor("#18181b"));
  const QColor recording = color("recording");
  const QColor error = color("error");
  for (const auto &[key, hex] : {std::pair{"blue", "#397bdc"},
                                {"purple", "#8055cc"}, {"teal", "#167e78"},
                                {"orange", "#f97316"}, {"cyan", "#0891b2"},
                                {"pink", "#db2777"}, {"rose", "#e94560"}}) {
    settings.insert("accentColor", key);
    QCoreApplication::processEvents();
    EXPECT_EQ(color("accent"), QColor(hex));
    EXPECT_EQ(color("primary"), color("accent"));
    QVariant gradientHead;
    ASSERT_TRUE(QMetaObject::invokeMethod(probe.get(), "gradientHead",
                                         Q_RETURN_ARG(QVariant, gradientHead)));
    EXPECT_EQ(gradientHead.value<QColor>(), color("accent"));
    EXPECT_EQ(color("recording"), recording);
    EXPECT_EQ(color("error"), error);
    for (const auto *name : {"liveRecordingDot", "liveRecordingBadge", "liveRecordingStatus"}) {
      auto *indicator = findSwatch(findSwatch, qobject_cast<QQuickItem *>(probe.get()), name);
      ASSERT_NE(indicator, nullptr);
      EXPECT_EQ(indicator->property("color").value<QColor>(), color("accent"));
    }
    auto *swatch = findSwatch(findSwatch, qobject_cast<QQuickItem *>(probe.get()),
                             QString("accentSwatch_") + key);
    ASSERT_NE(swatch, nullptr);
    EXPECT_TRUE(swatch->property("selected").toBool());
  }
  probe->setProperty("preferences", QVariant::fromValue(static_cast<QObject *>(nullptr)));
  EXPECT_EQ(color("background"), QColor("#000000"));
  EXPECT_EQ(color("accent"), QColor("#e94560"));
  EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}

TEST(ThemeTest, SettingsSectionsAnimateAndReverseWithoutLayoutJumps) {
  QQmlEngine engine;
  QQmlComponent component(&engine);
  component.setData(R"(
import QtQuick
import QtQuick.Layouts
import "../controls" as Controls
Item {
    id: root
    width: 600; height: 800
    property bool sawIntermediate: false
    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        Controls.SettingsSectionCard {
            id: panel
            objectName: "panel"
            sectionTitle: "Animated settings"
            description: "A test panel"
            Rectangle { implicitHeight: 180; Layout.fillWidth: true }
            onRevealProgressChanged: {
                if (revealProgress > 0 && revealProgress < 1)
                    root.sawIntermediate = true;
            }
        }
        Rectangle { objectName: "nextPanel"; implicitHeight: 50; Layout.fillWidth: true }
    }
}
)", QUrl::fromLocalFile(QString(THEME_STYLES_DIR) + "/PanelProbe.qml"));
  std::unique_ptr<QObject> probe(component.create());
  ASSERT_NE(probe, nullptr) << component.errorString().toStdString();
  QQuickWindow window;
  window.resize(640, 800);
  qobject_cast<QQuickItem *>(probe.get())->setParentItem(window.contentItem());
  window.show();
  QTest::qWait(30);
  auto *panel = probe->findChild<QQuickItem *>("panel");
  auto *next = probe->findChild<QQuickItem *>("nextPanel");
  ASSERT_NE(panel, nullptr);
  ASSERT_NE(next, nullptr);
  const qreal collapsedHeight = panel->height();
  const qreal collapsedNextY = next->y();
  auto settled = [&](double target) {
    QElapsedTimer timer;
    timer.start();
    while (qAbs(panel->property("revealProgress").toDouble() - target) > 0.001 && timer.elapsed() < 2000)
      QTest::qWait(10);
    QTest::qWait(20); // Allow the layout to polish its final geometry.
    return qAbs(panel->property("revealProgress").toDouble() - target) < 0.001;
  };
  panel->setProperty("expanded", true);
  EXPECT_LT(panel->property("revealProgress").toDouble(), 1);
  EXPECT_TRUE(settled(1));
  EXPECT_TRUE(probe->property("sawIntermediate").toBool());
  EXPECT_GT(panel->height(), collapsedHeight + 170);
  EXPECT_GT(next->y(), collapsedNextY + 170);
  panel->setProperty("expanded", false);
  EXPECT_GT(panel->property("revealProgress").toDouble(), 0);
  EXPECT_TRUE(settled(0));
  EXPECT_NEAR(panel->height(), collapsedHeight, 1);
  EXPECT_NEAR(next->y(), collapsedNextY, 1);
  panel->setProperty("expanded", true);
  QTest::qWait(50);
  panel->setProperty("expanded", false);
  EXPECT_TRUE(settled(0));
  EXPECT_NEAR(panel->height(), collapsedHeight, 1);
}

TEST(ThemeTest, WindowCornersFollowStateAndFullSpanSizes) {
  using TrophyBackend::WindowChrome;
  const QSize screen(1920, 1080);
  const QSize available(1920, 1040);
  EXPECT_FALSE(WindowChrome::shouldSquare(Qt::WindowNoState, {1200, 800}, screen, available));
  EXPECT_TRUE(WindowChrome::shouldSquare(Qt::WindowMaximized, {1200, 800}, screen, available));
  EXPECT_TRUE(WindowChrome::shouldSquare(Qt::WindowFullScreen, {1200, 800}, screen, available));
  EXPECT_TRUE(WindowChrome::shouldSquare(Qt::WindowNoState, {960, 1040}, screen, available));
  EXPECT_TRUE(WindowChrome::shouldSquare(Qt::WindowNoState, {960, 1080}, screen, available));
  EXPECT_TRUE(WindowChrome::shouldSquare(Qt::WindowNoState, {1920, 520}, screen, available));
  EXPECT_TRUE(WindowChrome::shouldSquare(Qt::WindowNoState, {960, 1038}, screen, available));
  EXPECT_FALSE(WindowChrome::shouldSquare(Qt::WindowNoState, {960, 1000}, screen, available));
  // Do not invent a tiled state for a floating window sized like a quarter tile.
  EXPECT_FALSE(WindowChrome::shouldSquare(Qt::WindowNoState, {960, 520}, screen, available));
  EXPECT_FALSE(WindowChrome::shouldSquare(Qt::WindowNoState, {1200, 800}, {}, {}));
  EXPECT_TRUE(WindowChrome::shouldSquare(Qt::WindowNoState, {1280, 1000}, {1280, 1440}, {1280, 1400}));
}

TEST(ThemeTest, WindowCornersRestoreAndDetachFromOldWindows) {
  using TrophyBackend::WindowChrome;
  QWindow first;
  first.resize(400, 300);
  WindowChrome chrome;
  QSignalSpy changed(&chrome, &WindowChrome::squareCornersChanged);
  chrome.setWindow(&first);
  EXPECT_FALSE(chrome.squareCorners());
  first.setWindowState(Qt::WindowMaximized);
  EXPECT_TRUE(chrome.squareCorners());
  first.setWindowState(Qt::WindowFullScreen);
  EXPECT_EQ(changed.count(), 1);
  first.setWindowState(Qt::WindowNoState);
  EXPECT_FALSE(chrome.squareCorners());
  EXPECT_EQ(changed.count(), 2);
  auto second = std::make_unique<QWindow>();
  second->resize(400, 300);
  chrome.setWindow(second.get());
  first.setWindowState(Qt::WindowMaximized);
  EXPECT_FALSE(chrome.squareCorners());
  second->setWindowState(Qt::WindowMaximized);
  EXPECT_TRUE(chrome.squareCorners());
  second.reset();
  EXPECT_EQ(chrome.window(), nullptr);
  EXPECT_FALSE(chrome.squareCorners());
  chrome.setWindow(&first);
  EXPECT_TRUE(chrome.squareCorners());
  chrome.setWindow(nullptr);
  EXPECT_FALSE(chrome.squareCorners());
}

TEST(ThemeTest, WindowCornerStateReachesQmlThemeAndRestores) {
  qmlRegisterType<TrophyBackend::WindowChrome>("ChromeTest", 1, 0, "WindowChrome");
  QQmlEngine engine;
  QQmlComponent component(&engine);
  component.setData(R"(
import QtQuick
import ChromeTest 1.0
import "." as Styles
Window {
    id: host
    width: 400; height: 300
    property int frameRadius: Styles.Theme.windowRadius
    WindowChrome { id: chrome; targetWindow: host }
    Binding {
        target: Styles.Theme
        property: "squareWindowCorners"
        value: chrome.squareCorners
    }
}
)", QUrl::fromLocalFile(QString(THEME_STYLES_DIR) + "/ChromeProbe.qml"));
  std::unique_ptr<QObject> probe(component.create());
  ASSERT_NE(probe, nullptr) << component.errorString().toStdString();
  auto *window = qobject_cast<QWindow *>(probe.get());
  ASSERT_NE(window, nullptr);
  EXPECT_EQ(probe->property("frameRadius").toInt(), 12);
  window->setWindowState(Qt::WindowMaximized);
  EXPECT_EQ(probe->property("frameRadius").toInt(), 0);
  window->setWindowState(Qt::WindowFullScreen);
  EXPECT_EQ(probe->property("frameRadius").toInt(), 0);
  window->setWindowState(Qt::WindowNoState);
  EXPECT_EQ(probe->property("frameRadius").toInt(), 12);
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
