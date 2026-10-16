/* Headless engine load of the real RecordingCard: rebinding `recording`
 * across an app-wired rename must keep the thumbnail. The library adopts
 * renames in place (same object), but the fallback remove + add path
 * swaps objects — either way the card must follow to the live recording.
 */
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <gtest/gtest.h>
#include <memory>

#include "core/StorageManager.h"
#include "library/Recording.h"
#include "library/RecordingLibrary.h"

using namespace TrophyBackend;

class CardManagerFake : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool isRecording READ isRecording CONSTANT)
public:
  bool isRecording() const { return false; }
};

class CardWindowFake : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList selectedRecordings READ selectedRecordings NOTIFY
                 selectedRecordingsChanged)
  Q_PROPERTY(int lastSelectedIndex READ lastSelectedIndex WRITE
                  setLastSelectedIndex NOTIFY lastSelectedIndexChanged)
  Q_PROPERTY(QObject *storageManager READ storageManager CONSTANT)
public:
  QObject *storageManager() const { return nullptr; }
  QVariantList selectedRecordings() const { return m_selected; }
  int lastSelectedIndex() const { return m_idx; }
  void setLastSelectedIndex(int i) {
    m_idx = i;
    emit lastSelectedIndexChanged();
  }
  Q_INVOKABLE void toggleSelection(const QString &path) {
    if (m_selected.contains(path))
      m_selected.removeAll(path);
    else
      m_selected.append(path);
    emit selectedRecordingsChanged();
  }
  Q_INVOKABLE void shiftSelectRange(int index) { rangeIndex = index; }
  Q_INVOKABLE void clearSelection() {
    m_selected.clear();
    setLastSelectedIndex(-1);
    emit selectedRecordingsChanged();
  }
  Q_INVOKABLE void openPlayer(const QString &path) { playedPath = path; }
  QString playedPath;
  int rangeIndex = -1;
  Q_INVOKABLE void openFolder(const QString &) {}
  Q_INVOKABLE void confirmDelete(const QString &, const QString &) {}
  Q_INVOKABLE void requestRename(const QString &, const QString &) {}
signals:
  void selectedRecordingsChanged();
  void lastSelectedIndexChanged();

private:
  int m_idx = -1;
  QVariantList m_selected;
};

class RecordingCardBindingTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;
  CardManagerFake manager;
  CardWindowFake windowFake;
  QQmlEngine engine;
  QQuickWindow window;
  QStringList scriptErrors;

  void SetUp() override {
    ASSERT_TRUE(tempDir.isValid());
    QObject::connect(
        &engine, &QQmlEngine::warnings, &engine,
        [this](const QList<QQmlError> &errors) {
          for (const auto &error : errors) {
            if (error.description().contains("ReferenceError") ||
                error.description().contains("TypeError"))
              scriptErrors.append(error.toString());
          }
        });
    engine.rootContext()->setContextProperty("recordingManager", &manager);
    engine.rootContext()->setContextProperty("window", &windowFake);
    window.resize(1024, 768);
    window.show();
    EXPECT_TRUE(QTest::qWaitForWindowExposed(&window, 5000));
  }

  void TearDown() override {
    QThreadPool::globalInstance()->waitForDone(5000);
    EXPECT_TRUE(scriptErrors.isEmpty())
        << scriptErrors.join('\n').toStdString();
    QObject::disconnect(&engine, nullptr, &engine, nullptr);
  }

  QString createVideo(const QString &name) {
    const QString path = tempDir.path() + "/" + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(8192, '\0'));
    f.close();
    return path;
  }

  void createThumb(const QString &baseName) {
    QDir(tempDir.path()).mkpath(".thumbnails");
    const QString path = tempDir.path() + "/.thumbnails/" + baseName + ".jpg";
    QImage img(320, 180, QImage::Format_RGB888);
    img.fill(Qt::red);
    EXPECT_TRUE(img.save(path, "JPEG"));
  }

  void waitForProbes() {
    QThreadPool::globalInstance()->waitForDone(10000);
    QCoreApplication::processEvents();
  }
};

TEST_F(RecordingCardBindingTest, RebindAcrossRenameKeepsThumbnail) {
  const QString oldPath = createVideo("Old_Game_2026-01-15_20-30-00.mkv");
  createThumb("Old_Game_2026-01-15_20-30-00");

  StorageManager storage(tempDir.path());
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 1);
  engine.rootContext()->setContextProperty("recordingLibrary", &lib);
  QObject::connect(&storage, &StorageManager::recordingRenamed, &lib,
                   &RecordingLibrary::onRecordingRenamed);
  QObject::connect(&storage, &StorageManager::recordingRenamed, &lib,
                   &RecordingLibrary::refresh);

  QQmlComponent cardComp(&engine, QUrl::fromLocalFile(CARD_QML_FILE));
  ASSERT_TRUE(cardComp.isReady()) << cardComp.errorString().toStdString();
  QObject *created = cardComp.create();
  ASSERT_NE(created, nullptr) << cardComp.errorString().toStdString();
  QQuickItem *card = qobject_cast<QQuickItem *>(created);
  ASSERT_NE(card, nullptr);
  card->setParentItem(window.contentItem());
  card->setWidth(280);
  card->setHeight(200);

  Recording *before = lib.getRecording(oldPath);
  ASSERT_NE(before, nullptr);
  card->setProperty("recording", QVariant::fromValue(before));
  QCoreApplication::processEvents();
  QTest::qWait(100);
  QCoreApplication::processEvents();
  QObject *bound = card->property("recording").value<QObject *>();
  ASSERT_EQ(bound, before);
  EXPECT_FALSE(bound->property("thumbnailPath").toString().isEmpty());

  const QString newPath = storage.renameRecording(oldPath, "New Title");
  ASSERT_FALSE(newPath.isEmpty());
  lib.refresh();
  waitForProbes();

  // Whatever object the library holds after the rename (adopted in
  // place or recreated via the fallback path), the card follows it
  // and keeps a resolvable thumbnail.
  Recording *after = lib.getRecording(newPath);
  ASSERT_NE(after, nullptr);
  card->setProperty("recording", QVariant::fromValue(after));
  QCoreApplication::processEvents();
  QTest::qWait(100);
  QCoreApplication::processEvents();
  QObject *rebound = card->property("recording").value<QObject *>();
  ASSERT_EQ(rebound, after);
  EXPECT_EQ(rebound->property("filePath").toString(), newPath);
  const QString thumbAfter = rebound->property("thumbnailPath").toString();
  EXPECT_FALSE(thumbAfter.isEmpty());
  if (!thumbAfter.isEmpty())
    EXPECT_TRUE(QFile::exists(thumbAfter));
}

TEST_F(RecordingCardBindingTest, GridDetailsStayCompactWithoutShrinkingThumbnail) {
  const QString path = createVideo("Game_2026-01-15_20-30-00.mkv");
  const QString clipPath = createVideo("Game_Replay.mkv");
  RecordingLibrary lib(tempDir.path());
  engine.rootContext()->setContextProperty("recordingLibrary", &lib);
  QQmlComponent component(&engine, QUrl::fromLocalFile(CARD_QML_FILE));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> card(component.create());
  ASSERT_NE(card, nullptr);
  auto *item = qobject_cast<QQuickItem *>(card.get());
  ASSERT_NE(item, nullptr);
  item->setParentItem(window.contentItem());
  item->setWidth(320);
  card->setProperty("recording", QVariant::fromValue(lib.getRecording(path)));
  QTest::qWait(100);
  auto *thumbnail = card->findChild<QQuickItem *>("gridThumbnail");
  auto *metadata = card->findChild<QQuickItem *>("gridMetadata");
  auto *type = card->findChild<QQuickItem *>("gridType");
  auto *date = card->findChild<QQuickItem *>("gridDate");
  ASSERT_NE(thumbnail, nullptr);
  ASSERT_NE(metadata, nullptr);
  ASSERT_NE(type, nullptr);
  ASSERT_NE(date, nullptr);
  EXPECT_EQ(item->height(), 248);
  EXPECT_EQ(thumbnail->height(), 180);
  EXPECT_EQ(type->property("text").toString(), "Recording");
  EXPECT_TRUE(metadata->property("text").toString().startsWith("• "));
  const qreal metadataBottom = metadata->mapToItem(item, QPointF(0, metadata->height())).y();
  const qreal dateTop = date->mapToItem(item, QPointF()).y();
  EXPECT_GE(dateTop, metadataBottom);
  EXPECT_LE(dateTop - metadataBottom, 4);
  EXPECT_LE(dateTop + date->height(), item->height() - 6);

  card->setProperty("recording", QVariant::fromValue(lib.getRecording(clipPath)));
  QCoreApplication::processEvents();
  EXPECT_EQ(type->property("text").toString(), "Clip");
  EXPECT_TRUE(metadata->property("text").toString().startsWith("• "));
  card.reset();
  waitForProbes();
}

TEST_F(RecordingCardBindingTest, DashboardSwitchesLibraryViewWithoutLosingSelection) {
  const QString path = createVideo("Game_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  engine.rootContext()->setContextProperty("recordingLibrary", &lib);
  for (const auto *name : {"settingsManager", "storageManager", "replayManager",
                           "activityFeed", "gameDetector", "audioMixer", "app"})
    engine.rootContext()->setContextProperty(name, QVariant::fromValue<QObject *>(nullptr));

  const auto dashboardUrl = QUrl::fromLocalFile(CARD_QML_FILE).resolved(
      QUrl("../pages/DashboardPage.qml"));
  QQmlComponent component(&engine, dashboardUrl);
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> page(component.create());
  ASSERT_NE(page, nullptr) << component.errorString().toStdString();
  auto *item = qobject_cast<QQuickItem *>(page.get());
  ASSERT_NE(item, nullptr);
  item->setParentItem(window.contentItem());
  item->setSize(QSizeF(1280, 800));
  QTest::qWait(100);
  auto *loader = page->findChild<QObject *>("libraryView");
  ASSERT_NE(loader, nullptr);
  auto activeView = [&]() { return loader->property("item").value<QObject *>(); };
  ASSERT_NE(activeView(), nullptr);
  EXPECT_EQ(activeView()->property("count").toInt(), 1);

  windowFake.toggleSelection(path);
  auto *listButton = page->findChild<QObject *>("libraryListButton");
  ASSERT_NE(listButton, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(listButton, "clicked"));
  QTest::qWait(100);
  EXPECT_TRUE(page->property("libraryListView").toBool());
  ASSERT_NE(activeView(), nullptr);
  EXPECT_EQ(activeView()->property("count").toInt(), 1);
  EXPECT_TRUE(windowFake.selectedRecordings().contains(path));
  EXPECT_TRUE(QMetaObject::invokeMethod(page.get(), "scrollToRecording", Q_ARG(QVariant, 0)));

  auto *gridButton = page->findChild<QObject *>("libraryGridButton");
  ASSERT_NE(gridButton, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(gridButton, "clicked"));
  QTest::qWait(100);
  EXPECT_FALSE(page->property("libraryListView").toBool());
  EXPECT_EQ(activeView()->property("count").toInt(), 1);
  EXPECT_TRUE(windowFake.selectedRecordings().contains(path));
  auto *filterButton = page->findChild<QObject *>("libraryFilterButton");
  auto *filterPopup = page->findChild<QObject *>("libraryFilterPopup");
  ASSERT_NE(filterButton, nullptr);
  ASSERT_NE(filterPopup, nullptr);
  auto *filterItem = qobject_cast<QQuickItem *>(filterButton);
  auto *listItem = qobject_cast<QQuickItem *>(listButton);
  ASSERT_NE(filterItem, nullptr);
  ASSERT_NE(listItem, nullptr);
  EXPECT_GE(filterItem->mapToScene(QPointF()).x() -
                listItem->mapToScene(QPointF(listItem->width(), 0)).x(), 16);
  EXPECT_FALSE(filterPopup->property("visible").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(filterButton, "clicked"));
  QTest::qWait(100);
  EXPECT_TRUE(filterPopup->property("visible").toBool());
  EXPECT_GT(filterPopup->property("height").toDouble(), 100);
  // Repeater delegates belong to the visual tree, not necessarily the
  // popup's QObject ownership tree.
  auto *popupContent = filterPopup->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(popupContent, nullptr);
  QObject *showRecordings = nullptr;
  QObject *showClips = nullptr;
  for (auto *child : popupContent->childItems()) {
    if (child->objectName() == "libraryShowRecordings")
      showRecordings = child;
    if (child->objectName() == "libraryShowClips")
      showClips = child;
  }
  auto *sort = page->findChild<QObject *>("librarySortOrder");
  ASSERT_NE(showRecordings, nullptr);
  ASSERT_NE(showClips, nullptr);
  ASSERT_NE(sort, nullptr);
  EXPECT_TRUE(showRecordings->property("checked").toBool());
  EXPECT_TRUE(showClips->property("checked").toBool());
  showRecordings->setProperty("checked", false);
  ASSERT_TRUE(QMetaObject::invokeMethod(showRecordings, "toggled"));
  QCoreApplication::processEvents();
  EXPECT_EQ(lib.kindFilter(), RecordingLibrary::ClipsOnly);
  EXPECT_EQ(activeView()->property("count").toInt(), 0);
  EXPECT_TRUE(windowFake.selectedRecordings().isEmpty());

  // Changing layout retains the filter; both views share the backend.
  ASSERT_TRUE(QMetaObject::invokeMethod(listButton, "clicked"));
  QCoreApplication::processEvents();
  ASSERT_NE(activeView(), nullptr);
  EXPECT_EQ(activeView()->property("count").toInt(), 0);
  showClips->setProperty("checked", false);
  ASSERT_TRUE(QMetaObject::invokeMethod(showClips, "toggled"));
  EXPECT_EQ(lib.kindFilter(), RecordingLibrary::NoCaptures);
  showRecordings->setProperty("checked", true);
  ASSERT_TRUE(QMetaObject::invokeMethod(showRecordings, "toggled"));
  EXPECT_EQ(lib.kindFilter(), RecordingLibrary::RecordingsOnly);
  showClips->setProperty("checked", true);
  ASSERT_TRUE(QMetaObject::invokeMethod(showClips, "toggled"));
  EXPECT_EQ(lib.kindFilter(), RecordingLibrary::AllCaptures);
  QCoreApplication::processEvents();
  EXPECT_EQ(activeView()->property("count").toInt(), 1);
  auto *direction = page->findChild<QObject *>("librarySortDirection");
  ASSERT_NE(direction, nullptr);
  EXPECT_EQ(sort->property("count").toInt(), 4);
  const QList<int> descendingSorts = {
      RecordingLibrary::NewestFirst, RecordingLibrary::LargestFirst,
      RecordingLibrary::LongestFirst, RecordingLibrary::NameZA};
  const QList<int> ascendingSorts = {
      RecordingLibrary::OldestFirst, RecordingLibrary::SmallestFirst,
      RecordingLibrary::ShortestFirst, RecordingLibrary::NameAZ};
  for (int i = 0; i < descendingSorts.size(); ++i) {
    sort->setProperty("currentIndex", i);
    ASSERT_TRUE(QMetaObject::invokeMethod(sort, "activated", Q_ARG(int, i)));
    EXPECT_EQ(lib.sortOrder(), descendingSorts[i]);
    EXPECT_EQ(direction->property("text").toString(), QString::fromUtf8("↓"));
    ASSERT_TRUE(QMetaObject::invokeMethod(direction, "clicked"));
    EXPECT_EQ(lib.sortOrder(), ascendingSorts[i]);
    EXPECT_EQ(sort->property("currentIndex").toInt(), i);
    EXPECT_EQ(direction->property("text").toString(), QString::fromUtf8("↑"));
    ASSERT_TRUE(QMetaObject::invokeMethod(direction, "clicked"));
    EXPECT_EQ(lib.sortOrder(), descendingSorts[i]);
  }
  // Field changes also preserve ascending, and external updates sync both controls.
  lib.setSortOrder(RecordingLibrary::OldestFirst);
  EXPECT_EQ(sort->property("currentIndex").toInt(), 0);
  sort->setProperty("currentIndex", 2);
  ASSERT_TRUE(QMetaObject::invokeMethod(sort, "activated", Q_ARG(int, 2)));
  EXPECT_EQ(lib.sortOrder(), RecordingLibrary::ShortestFirst);
  ASSERT_TRUE(QMetaObject::invokeMethod(filterPopup, "close"));
  QTest::qWait(100);
  EXPECT_FALSE(filterPopup->property("visible").toBool());
  EXPECT_TRUE(filterButton->property("checked").toBool());
  page.reset();
  waitForProbes();
}

TEST_F(RecordingCardBindingTest, ListRowPlaybackAndSelectionUseLibraryIndices) {
  const QString path = createVideo("Game_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  engine.rootContext()->setContextProperty("recordingLibrary", &lib);
  QQmlComponent component(&engine, QUrl::fromLocalFile(CARD_QML_FILE).resolved(
      QUrl("RecordingListItem.qml")));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> row(component.create());
  ASSERT_NE(row, nullptr);
  row->setProperty("recording", QVariant::fromValue(lib.getRecording(path)));
  row->setProperty("cardIndex", 3);
  auto *rowItem = qobject_cast<QQuickItem *>(row.get());
  ASSERT_NE(rowItem, nullptr);
  rowItem->setParentItem(window.contentItem());
  rowItem->setSize(QSizeF(680, 92));
  QTest::mouseMove(&window, QPoint(900, 500));
  QTest::qWait(200);
  auto *thumbnail = row->findChild<QQuickItem *>("listThumbnail");
  auto *type = row->findChild<QQuickItem *>("listType");
  auto *selection = row->findChild<QQuickItem *>("listSelection");
  auto *actions = row->findChild<QQuickItem *>("listActions");
  ASSERT_NE(thumbnail, nullptr);
  ASSERT_NE(type, nullptr);
  ASSERT_NE(selection, nullptr);
  ASSERT_NE(actions, nullptr);
  EXPECT_GE(type->mapToItem(thumbnail, QPointF()).x(), thumbnail->width());
  EXPECT_FALSE(selection->isVisible());
  const qreal restingThumbnailX = thumbnail->x();
  QTest::mouseMove(&window, QPoint(340, 46));
  QTest::qWait(250);
  EXPECT_TRUE(selection->isVisible());
  EXPECT_NEAR(thumbnail->x() - restingThumbnailX, 30, 0.5);
  EXPECT_LT(selection->mapToItem(thumbnail, QPointF(selection->width(), 0)).x(), 0);
  QTest::mouseMove(&window, QPoint(900, 500));
  QTest::qWait(250);
  EXPECT_FALSE(selection->isVisible());
  EXPECT_NEAR(thumbnail->x(), restingThumbnailX, 0.5);
  // Selecting a different row reveals this row too, without hover.
  windowFake.toggleSelection("/other-recording.mkv");
  QTest::qWait(250);
  EXPECT_TRUE(selection->isVisible());
  EXPECT_NEAR(thumbnail->x() - restingThumbnailX, 30, 0.5);
  windowFake.toggleSelection("/other-recording.mkv");
  EXPECT_GE(actions->width(), 152);
  // No labels/badges may obscure the preview: metadata is outside it.
  for (auto *child : thumbnail->findChildren<QObject *>())
    EXPECT_LT(child->metaObject()->indexOfProperty("text"), 0);
  auto activate = [&](int modifiers) {
    EXPECT_TRUE(QMetaObject::invokeMethod(row.get(), "activate", Q_ARG(QVariant, modifiers)));
  };
  activate(0);
  EXPECT_EQ(windowFake.playedPath, path);
  activate(Qt::ControlModifier);
  EXPECT_TRUE(row->property("isSelected").toBool());
  EXPECT_EQ(windowFake.lastSelectedIndex(), 3);
  activate(Qt::ShiftModifier);
  EXPECT_EQ(windowFake.rangeIndex, 3);
  activate(0);
  EXPECT_FALSE(row->property("isSelected").toBool());
  row->setProperty("recording", QVariant::fromValue<QObject *>(nullptr));
  activate(0);
  row.reset();
  waitForProbes();
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  QGuiApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "test_RecordingCardBinding.moc"
