#include <QColor>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
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
  EXPECT_EQ(color("background"), QColor("#1a1a2e"));
  probe->setProperty("preferences", QVariant::fromValue(static_cast<QObject *>(&settings)));
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
  EXPECT_EQ(color("background"), QColor("#1a1a2e"));
  EXPECT_EQ(color("accent"), QColor("#e94560"));
  EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
