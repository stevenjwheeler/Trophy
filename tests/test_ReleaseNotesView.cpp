/*
 * Trophy - Unit Tests
 * ReleaseNotesView: real QML engine load of the updater notes panel.
 */

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <gtest/gtest.h>

#include <memory>

class ReleaseNotesViewTest : public ::testing::Test {
protected:
  QQmlEngine engine;
  QQuickWindow window;
  std::unique_ptr<QObject> view;
  QStringList warnings;

  void SetUp() override {
    window.resize(640, 520);
    window.show();
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [this](const QList<QQmlError> &list) {
                       for (const auto &warning : list)
                         warnings.append(warning.toString());
                     });

    QQmlComponent component(&engine,
                            QUrl::fromLocalFile(RELEASE_NOTES_QML_FILE));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    view.reset(component.create());
    ASSERT_NE(view, nullptr) << component.errorString().toStdString();

    auto *panel = qobject_cast<QQuickItem *>(view.get());
    ASSERT_NE(panel, nullptr);
    panel->setParentItem(window.contentItem());
    panel->setWidth(600);
    QCoreApplication::processEvents();
  }

  void TearDown() override {
    view.reset();
    QCoreApplication::processEvents();
    for (const auto &warning : warnings) {
      EXPECT_FALSE(warning.contains(QStringLiteral("ReferenceError")) ||
                   warning.contains(QStringLiteral("TypeError")))
          << warning.toStdString();
    }
    QObject::disconnect(&engine, nullptr, &engine, nullptr);
  }

  QQuickItem *panel() { return qobject_cast<QQuickItem *>(view.get()); }

  QQuickItem *findItem(const QString &name) {
    return panel() ? panel()->findChild<QQuickItem *>(name) : nullptr;
  }

  void setNotes(const QString &html, int maximumHeight = 320) {
    view->setProperty("maximumHeight", maximumHeight);
    view->setProperty("html", html);
    QCoreApplication::processEvents();
  }
};

TEST_F(ReleaseNotesViewTest, LongNotesClampAndScroll) {
  QString html;
  for (int i = 0; i < 40; ++i) {
    html += QStringLiteral(
                "<p style=\"margin-bottom:8px\">Line %1 with enough text "
                "to wrap inside the updater dialog.</p>")
                .arg(i);
  }

  setNotes(html, 180);
  EXPECT_LE(panel()->implicitHeight(), 180.0);
  EXPECT_GT(panel()->implicitHeight(), 100.0);

  auto *flickable = findItem(QStringLiteral("releaseNotesFlickable"));
  ASSERT_NE(flickable, nullptr);
  EXPECT_GT(flickable->property("contentHeight").toDouble(),
            flickable->property("height").toDouble());
  EXPECT_TRUE(flickable->property("interactive").toBool());
}

TEST_F(ReleaseNotesViewTest, ShortNotesFitWithoutAScrollBar) {
  setNotes(QStringLiteral("<p>Only one short line.</p>"), 320);
  EXPECT_GT(panel()->implicitHeight(), 0.0);
  EXPECT_LT(panel()->implicitHeight(), 60.0);

  auto *flickable = findItem(QStringLiteral("releaseNotesFlickable"));
  ASSERT_NE(flickable, nullptr);
  EXPECT_LE(flickable->property("contentHeight").toDouble(),
            flickable->property("height").toDouble() + 1.0);
  EXPECT_FALSE(flickable->property("interactive").toBool());
}

TEST_F(ReleaseNotesViewTest, RichTextIsUsedAndLinksAreForwarded) {
  setNotes(QStringLiteral(
      "<p><b>Bold</b> and <a href=\"https://example.com\">Trophy</a></p>"));

  auto *text = findItem(QStringLiteral("releaseNotesText"));
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(text->property("textFormat").toInt(), int(Qt::RichText));
  EXPECT_TRUE(text->property("text").toString().contains(QStringLiteral("<b>")));

  QSignalSpy spy(view.get(), SIGNAL(linkActivated(QString)));
  ASSERT_TRUE(spy.isValid());
  ASSERT_TRUE(QMetaObject::invokeMethod(
      text, "linkActivated", Qt::DirectConnection,
      Q_ARG(QString, QStringLiteral("https://example.com"))));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.at(0).at(0).toString(),
            QStringLiteral("https://example.com"));
}

TEST_F(ReleaseNotesViewTest, RealClickActivatesLink) {
  setNotes(QStringLiteral(
      "<p><a href=\"https://example.com/x\">LINK</a></p>"), 320);
  auto *text = findItem(QStringLiteral("releaseNotesText"));
  ASSERT_NE(text, nullptr);
  ASSERT_GT(text->implicitHeight(), 8.0);

  QSignalSpy spy(view.get(), SIGNAL(linkActivated(QString)));
  ASSERT_TRUE(spy.isValid());
  const QPoint point = text->mapToScene(QPointF(8, text->implicitHeight() / 2))
                           .toPoint();
  QTest::mouseMove(&window, point);
  QCoreApplication::processEvents();
  EXPECT_EQ(text->property("hoveredLink").toString(),
            QStringLiteral("https://example.com/x"));
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, point);
  QCoreApplication::processEvents();
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.at(0).at(0).toString(),
            QStringLiteral("https://example.com/x"));
}

TEST_F(ReleaseNotesViewTest, TextKeepsRoomForTheScrollBarGutter) {
  setNotes(QStringLiteral("<p>short</p>"), 320);
  auto *flickable = findItem(QStringLiteral("releaseNotesFlickable"));
  auto *text = findItem(QStringLiteral("releaseNotesText"));
  ASSERT_NE(flickable, nullptr);
  ASSERT_NE(text, nullptr);
  EXPECT_NEAR(text->width(), flickable->width() - 12.0, 1.0);
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  QGuiApplication application(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
