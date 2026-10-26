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
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQuickImageProvider>
#include <QProcess>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <gtest/gtest.h>
#include <memory>

#include "core/StorageManager.h"
#include "library/Recording.h"
#include "library/RecordingLibrary.h"
#include "library/PreviewCache.h"

using namespace TrophyBackend;

// UI tests use their already-small fixture as the cached result. Held replies
// exercise cancellation/staleness without transcoding or touching host files.
class CardPreviewCacheFake : public QObject {
  Q_OBJECT
public:
  bool hold = false;
  QList<QPair<QUrl, int>> requests;
  QList<int> cancelled;
  QHash<QUrl, QUrl> results;
  void deliver(const QUrl &source, int ticket, const QUrl &preview) {
    emit previewFramesReady(source, ticket, preview, preview.isEmpty() ? QString() : QString("fixture"),
                            preview.isEmpty() ? 0 : 270, 15);
  }
  Q_INVOKABLE void retainFrames(const QString &) {}
  Q_INVOKABLE void releaseFrames(const QString &) {}
  Q_INVOKABLE void requestPreview(const QUrl &source, int ticket) {
    requests.append({source, ticket});
    if (!hold) QTimer::singleShot(0, this, [this, source, ticket] {
      deliver(source, ticket, results.value(source, source));
    });
  }
  Q_INVOKABLE void cancelPreview(int ticket) { cancelled.append(ticket); }
signals:
  void previewFramesReady(const QUrl &source, int ticket, const QUrl &preview, const QString &key, int count, int fps);
};

class CardFrameProvider : public QQuickImageProvider {
public:
  CardFrameProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
  QImage requestImage(const QString &id, QSize *size, const QSize &) override {
    QImage image(160, 90, QImage::Format_RGB32);
    image.fill(QColor::fromHsv((id.section('/', -1).toInt() * 10) % 360, 200, 200));
    if (size) *size = image.size();
    return image;
  }
};

class CardManagerFake : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool isRecording READ isRecording NOTIFY changed)
  Q_PROPERTY(QString currentGameName MEMBER gameName NOTIFY changed)
  Q_PROPERTY(qint64 recordingDuration MEMBER duration NOTIFY changed)
  Q_PROPERTY(qint64 recordingSize MEMBER size NOTIFY changed)
public:
  bool isRecording() const { return live; }
  bool live = false;
  QString gameName = "Manual Recording";
  qint64 duration = 65000;
  qint64 size = 1048576;
signals:
  void changed();
};

class CardIconFake : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject *gamesModel READ gamesModel CONSTANT)
public:
  QObject *gamesModel() { return &model; }
  QStandardItemModel model;
  QString iconPath;
  Q_INVOKABLE QUrl iconSourceForGame(const QString &name) const {
    return name == "Game" ? QUrl::fromLocalFile(iconPath) : QUrl();
  }
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
  Q_INVOKABLE void openShareExport(const QString &path) { sharedPath = path; }
  QString sharedPath;
  QString playedPath;
  int rangeIndex = -1;
  Q_INVOKABLE void openFolder(const QString &) {}
  Q_INVOKABLE void confirmDelete(const QString &, const QString &) {}
  Q_INVOKABLE void requestRename(const QString &, const QString &) {}
  Q_INVOKABLE void toggleCaptureProtection(const QString &path) { protectionPath = path; }
  QString protectionPath;
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
  CardPreviewCacheFake previewFake;
  QQmlEngine engine;
  QQuickWindow window;
  QStringList scriptErrors;
  std::unique_ptr<QObject> previewService;

  void SetUp() override {
    ASSERT_TRUE(tempDir.isValid());
    QObject::connect(
        &engine, &QQmlEngine::warnings, &engine,
        [this](const QList<QQmlError> &errors) {
          for (const auto &error : errors) {
            if (error.description().contains("ReferenceError") ||
                error.description().contains("TypeError") ||
                error.description().contains("recursive rearrange", Qt::CaseInsensitive) ||
                error.description().contains("Binding loop", Qt::CaseInsensitive))
              scriptErrors.append(error.toString());
          }
        });
    engine.rootContext()->setContextProperty("recordingManager", &manager);
    engine.rootContext()->setContextProperty("window", &windowFake);
    engine.rootContext()->setContextProperty("previewCache", &previewFake);
    engine.addImageProvider("hoverpreview", new CardFrameProvider);
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
  void exerciseHoverPreview(bool list);
  void exerciseFailedPreview(bool list);
  void exerciseProtection(bool list);
  void createPreviewService() {
    QString qmlPath = CARD_QML_FILE;
    qmlPath.replace("RecordingCard.qml", "HoverPreview.qml");
    QQmlComponent component(&engine, QUrl::fromLocalFile(qmlPath));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    previewService.reset(component.createWithInitialProperties(
        {{"parkingItem", QVariant::fromValue(window.contentItem())}}));
    ASSERT_NE(previewService, nullptr);
    qobject_cast<QQuickItem *>(previewService.get())->setParentItem(window.contentItem());
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
  auto *share = card->findChild<QQuickItem *>("gridShareExport");
  ASSERT_NE(share, nullptr);
  QSignalSpy shareSpy(card.get(), SIGNAL(shareRequested()));
  QTest::mouseMove(&window, QPoint(150, 90));
  QTest::qWait(250);
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      share->mapToScene(QPointF(share->width() / 2, share->height() / 2)).toPoint());
  EXPECT_EQ(shareSpy.count(), 1);
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

void RecordingCardBindingTest::exerciseProtection(bool list) {
  const auto path = createVideo("Protected_Game.mkv");
  RecordingLibrary library(tempDir.path());
  engine.rootContext()->setContextProperty("recordingLibrary", &library);
  QString qmlPath = CARD_QML_FILE;
  if (list) qmlPath.replace("RecordingCard.qml", "RecordingListItem.qml");
  QQmlComponent component(&engine, QUrl::fromLocalFile(qmlPath));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> card(component.create());
  ASSERT_NE(card, nullptr);
  auto *item = qobject_cast<QQuickItem *>(card.get());
  ASSERT_NE(item, nullptr);
  item->setParentItem(window.contentItem());
  item->setWidth(list ? 720 : 320);
  card->setProperty("recording", QVariant::fromValue(library.getRecording(path)));
  auto *lock = card->findChild<QQuickItem *>("captureProtectionButton");
  auto *selection = card->findChild<QQuickItem *>(list ? "listSelection" : "gridSelection");
  ASSERT_NE(lock, nullptr);
  ASSERT_NE(selection, nullptr);
  QTest::mouseMove(&window, QPoint(950, 700));
  ASSERT_TRUE(QTest::qWaitFor([&] { return !lock->isVisible(); }));
  QTest::mouseMove(&window, QPoint(150, 60));
  ASSERT_TRUE(QTest::qWaitFor([&] { return item->property("hovered").toBool() && lock->isVisible(); }));
  if (list) ASSERT_TRUE(QTest::qWaitFor([&] { return item->property("protectionReveal").toDouble() >= 0.999; }));
  EXPECT_EQ(lock->width(), selection->width());
  EXPECT_EQ(lock->height(), selection->height());
  EXPECT_DOUBLE_EQ(QQmlProperty::read(lock, "background.radius").toDouble(), lock->width() / 2);
  EXPECT_EQ(QQmlProperty::read(lock, "background.color"), selection->property("color"));
  EXPECT_EQ(QQmlProperty::read(lock, "background.border.color"), QQmlProperty::read(selection, "border.color"));
  const auto lockPos = lock->mapToItem(item, QPointF());
  const auto selectPos = selection->mapToItem(item, QPointF());
  if (list) {
    EXPECT_NEAR(lockPos.x(), selectPos.x(), 0.5);
    EXPECT_GT(lockPos.y(), selectPos.y() + selection->height());
  } else {
    EXPECT_NEAR(lockPos.y(), selectPos.y(), 0.5);
    EXPECT_NEAR(lockPos.x(), selectPos.y(), 0.5); // Same inset, opposite corners.
    EXPECT_LT(lockPos.x() + lock->width(), selectPos.x());
  }
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      lock->mapToScene(QPointF(lock->width() / 2, lock->height() / 2)).toPoint());
  EXPECT_EQ(windowFake.protectionPath, path);
  EXPECT_TRUE(windowFake.selectedRecordings().isEmpty());
  EXPECT_TRUE(windowFake.playedPath.isEmpty());
  ASSERT_TRUE(library.setProtected(path, true));
  windowFake.toggleSelection(path);
  QCoreApplication::processEvents();
  EXPECT_EQ(QQmlProperty::read(lock, "background.color"), selection->property("color"));
  QTest::mouseMove(&window, QPoint(950, 700));
  ASSERT_TRUE(QTest::qWaitFor([&] { return !item->property("hovered").toBool(); }));
  EXPECT_TRUE(lock->isVisible());
  ASSERT_TRUE(library.setProtected(path, false));
  ASSERT_TRUE(QTest::qWaitFor([&] { return !lock->isVisible(); }));
  EXPECT_TRUE(selection->isVisible()); // Selection mode must not reveal an off lock.
  if (list) {
    auto *slot = card->findChild<QQuickItem *>("listSelectionSlot");
    ASSERT_NE(slot, nullptr);
    EXPECT_NEAR(slot->width(), 30, 0.5); // Selected row still needs its checkbox.
    windowFake.clearSelection();
    ASSERT_TRUE(QTest::qWaitFor([&] { return slot->width() < 0.1; }));
  }
}

TEST_F(RecordingCardBindingTest, GridPadlockMatchesSelectionAndStaysVisibleOnlyWhenProtected) {
  exerciseProtection(false);
}
TEST_F(RecordingCardBindingTest, ListPadlockMatchesSelectionAndCollapsesWhenUnprotected) {
  exerciseProtection(true);
}

void RecordingCardBindingTest::exerciseHoverPreview(bool list) {
  createPreviewService();
  const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
  if (ffmpeg.isEmpty()) GTEST_SKIP() << "ffmpeg CLI unavailable";
  const QString path = tempDir.filePath("Hover # % Game.mkv");
  QProcess generator;
  generator.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i",
                          "testsrc2=size=160x90:rate=12", "-f", "lavfi", "-i",
                          "sine=frequency=440:sample_rate=48000", "-t", "18",
                          "-c:v", "mpeg4", "-c:a", "pcm_s16le", "-g", "12", path});
  ASSERT_TRUE(generator.waitForFinished(15000));
  ASSERT_EQ(generator.exitCode(), 0) << generator.readAllStandardError().toStdString();
  RecordingLibrary lib(tempDir.path());
  engine.rootContext()->setContextProperty("recordingLibrary", &lib);
  QString qmlPath = CARD_QML_FILE;
  if (list) qmlPath.replace("RecordingCard.qml", "RecordingListItem.qml");
  QQmlComponent component(&engine, QUrl::fromLocalFile(qmlPath));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> card(component.create());
  ASSERT_NE(card, nullptr);
  auto *item = qobject_cast<QQuickItem *>(card.get());
  item->setParentItem(window.contentItem());
  item->setWidth(list ? 720 : 320);
  card->setProperty("previewController", QVariant::fromValue(previewService.get()));
  card->setProperty("recording", QVariant::fromValue(lib.getRecording(path)));
  QTest::mouseMove(&window, QPoint(700, 500));
  QTest::mouseMove(&window, QPoint(100, 80));
  ASSERT_TRUE(QTest::qWaitFor([&] { return card->property("hovered").toBool(); }));
  QObject *startTimer = previewService->findChild<QObject *>("hoverPreviewStart");
  ASSERT_NE(startTimer, nullptr);
  EXPECT_EQ(startTimer->property("interval").toInt(), 50);
  QObject *sharedPlayer = previewService.get();
  ASSERT_NE(sharedPlayer, nullptr);
  EXPECT_EQ(previewService->findChild<QObject *>("hoverPreviewPlayer"), nullptr);
  ASSERT_TRUE(QTest::qWaitFor([&] { return sharedPlayer->property("source").toUrl().toLocalFile() == path; }));
  QTest::mouseMove(&window, QPoint(700, 500));
  QTest::qWait(200);
  EXPECT_FALSE(card->property("previewArmed").toBool());
  QTest::mouseMove(&window, QPoint(100, 80));
  ASSERT_TRUE(QTest::qWaitFor([&] { return card->property("previewVisible").toBool(); }, 10000));
  QObject *preview = card->property("currentPreview").value<QObject *>();
  ASSERT_NE(preview, nullptr);
  EXPECT_TRUE(preview->property("visible").toBool())
      << "Decoded frames must have a visible scene-graph subtree, not just a live sink";
  ASSERT_TRUE(QTest::qWaitFor([&] { return preview->property("opacity").toDouble() > 0.99; }, 1000));
  EXPECT_GT(preview->property("loopStart").toInt(), 0);
  EXPECT_LE(preview->property("loopStart").toInt(), 12000);
  // Boundary checks avoid a flaky probabilistic assertion about two random
  // hovers choosing different positions.
  auto chooseStart = [&](int duration, double fraction) {
    QVariant result;
    EXPECT_TRUE(QMetaObject::invokeMethod(preview, "chooseStart",
        Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, QVariant(duration)),
        Q_ARG(QVariant, QVariant(fraction))));
    return result.toInt();
  };
  EXPECT_EQ(chooseStart(0, 0.5), 0);
  EXPECT_EQ(chooseStart(2000, 0), 1);
  EXPECT_EQ(chooseStart(2000, 0.999999), 1000);
  EXPECT_EQ(chooseStart(60000, 0), 1);
  EXPECT_EQ(chooseStart(60000, 0.999999), 54000);
  QObject *player = previewService.get();
  ASSERT_NE(player, nullptr);
  EXPECT_EQ(player, sharedPlayer);
  EXPECT_NE(previewService->findChild<QObject *>("hoverPreviewImage"), nullptr);
  EXPECT_EQ(player->property("source").toUrl().toLocalFile(), path);
  // Keep EOF beyond the observation window: a short source could hide a
  // broken range seek by looping at EndOfMedia instead.
  preview->setProperty("loopStart", 1000);
  ASSERT_TRUE(QMetaObject::invokeMethod(player, "seek", Q_ARG(QVariant, QVariant(1000))));
  // Observe the six-second section wrap, not a full-file endless play.
  bool wrapped = false;
  qint64 previous = player->property("position").toLongLong();
  ASSERT_TRUE(QTest::qWaitFor([&] {
    const qint64 position = player->property("position").toLongLong();
    wrapped = wrapped || previous - position > 3000;
    previous = position;
    return wrapped;
  }, 10000));
  if (list) {
    card->setProperty("live", true);
    EXPECT_FALSE(card->property("previewArmed").toBool());
    EXPECT_FALSE(player->property("playing").toBool());
    EXPECT_FALSE(card->property("previewEligible").toBool());
    card->setProperty("live", false);
    QTest::mouseMove(&window, QPoint(700, 500));
    QTest::mouseMove(&window, QPoint(100, 80));
    ASSERT_TRUE(QTest::qWaitFor([&] { return card->property("previewVisible").toBool(); }, 10000));
  }
  card->setProperty("previewAllowed", false); // scrolling/navigation/full player
  EXPECT_FALSE(card->property("previewArmed").toBool());
  EXPECT_FALSE(player->property("playing").toBool());
  EXPECT_FALSE(card->property("previewVisible").toBool());
  card->setProperty("previewAllowed", true);
  ASSERT_TRUE(QTest::qWaitFor([&] { return card->property("previewVisible").toBool(); }, 10000));
  windowFake.toggleSelection(path);
  EXPECT_FALSE(card->property("previewArmed").toBool());
  windowFake.clearSelection();
  ASSERT_TRUE(QTest::qWaitFor([&] { return card->property("previewVisible").toBool(); }, 10000));
  card->setProperty("recording", QVariant());
  EXPECT_FALSE(card->property("previewVisible").toBool());
  EXPECT_FALSE(card->property("previewArmed").toBool());
  card->setProperty("recording", QVariant::fromValue(lib.getRecording(path)));
  ASSERT_TRUE(QTest::qWaitFor([&] { return card->property("previewVisible").toBool(); }, 10000));
  item->setVisible(false);
  EXPECT_FALSE(card->property("previewArmed").toBool());
  item->setVisible(true);
  QTest::mouseMove(&window, QPoint(700, 500));
  QTest::mouseMove(&window, QPoint(100, 80));
  ASSERT_TRUE(QTest::qWaitFor([&] { return card->property("previewVisible").toBool(); }, 10000));
  QTest::mouseMove(&window, QPoint(700, 500));
  ASSERT_TRUE(QTest::qWaitFor([&] { return !card->property("hovered").toBool(); }));
  EXPECT_FALSE(card->property("previewArmed").toBool());
  EXPECT_EQ(previewService.get(), sharedPlayer);
  ASSERT_TRUE(QTest::qWaitFor([&] { return sharedPlayer->property("source").toUrl().isEmpty(); }, 3000));
  card.reset();
  waitForProbes();
}

TEST_F(RecordingCardBindingTest, HoverPreviewIsShortDelayedSharedRandomSilentAndRetired) {
  exerciseHoverPreview(false);
}

TEST_F(RecordingCardBindingTest, ListHoverPreviewIsShortDelayedSharedRandomSilentAndRetired) {
  exerciseHoverPreview(true);
}

TEST_F(RecordingCardBindingTest, RapidGridListHandoffsReuseOneAnimationAndIgnoreOldReleases) {
  createPreviewService();
  const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
  if (ffmpeg.isEmpty()) GTEST_SKIP() << "ffmpeg CLI unavailable";
  const QString first = tempDir.filePath("First.mkv");
  const QString second = tempDir.filePath("Second.mkv");
  QProcess generator;
  generator.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i",
      "testsrc2=size=160x90:rate=12", "-t", "3", "-c:v", "mpeg4", first});
  ASSERT_TRUE(generator.waitForFinished(15000));
  ASSERT_EQ(generator.exitCode(), 0);
  ASSERT_TRUE(QFile::copy(first, second));
  RecordingLibrary lib(tempDir.path());
  engine.rootContext()->setContextProperty("recordingLibrary", &lib);
  auto createCard = [&](bool list, const QString &path) {
    QString qmlPath = CARD_QML_FILE;
    if (list) qmlPath.replace("RecordingCard.qml", "RecordingListItem.qml");
    QQmlComponent component(&engine, QUrl::fromLocalFile(qmlPath));
    std::unique_ptr<QObject> object(component.create());
    if (object) {
      auto *item = qobject_cast<QQuickItem *>(object.get());
      item->setParentItem(window.contentItem());
      item->setWidth(list ? 720 : 320);
      item->setY(list ? 320 : 0);
      object->setProperty("previewController", QVariant::fromValue(previewService.get()));
      object->setProperty("recording", QVariant::fromValue(lib.getRecording(path)));
    }
    return object;
  };
  auto grid = createCard(false, first);
  auto row = createCard(true, second);
  ASSERT_NE(grid, nullptr);
  ASSERT_NE(row, nullptr);
  QTest::mouseMove(&window, QPoint(900, 700));
  QTest::mouseMove(&window, QPoint(100, 80));
  ASSERT_TRUE(QTest::qWaitFor([&] { return grid->property("previewVisible").toBool(); }, 10000));
  auto *oldHost = previewService->property("target").value<QQuickItem *>();
  ASSERT_NE(oldHost, nullptr);
  QPointer<QObject> player = previewService.get();
  ASSERT_NE(player, nullptr);
  QSignalSpy sourceChanges(player, SIGNAL(sourceChanged()));
  ASSERT_TRUE(sourceChanges.isValid());
  for (int i = 0; i < 8; ++i) {
    QTest::mouseMove(&window, i % 2 ? QPoint(100, 80) : QPoint(100, 360));
    QTest::qWait(20);
  }
  EXPECT_EQ(sourceChanges.count(), 0); // 20ms passes must not load another video
  EXPECT_EQ(previewService.get(), player.data());
  EXPECT_EQ(player->property("source").toUrl().toLocalFile(), first);
  QTest::mouseMove(&window, QPoint(100, 360));
  ASSERT_TRUE(QTest::qWaitFor([&] { return row->property("previewVisible").toBool(); }, 10000));
  EXPECT_EQ(player->property("source").toUrl().toLocalFile(), second);
  EXPECT_EQ(previewService.get(), player.data());
  auto *sharedSurface = qobject_cast<QQuickItem *>(previewService.get());
  EXPECT_EQ(sharedSurface->width(), 128);
  EXPECT_EQ(sharedSurface->height(), 72);
  EXPECT_FALSE(grid->property("previewVisible").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(previewService.get(), "release",
      Q_ARG(QVariant, QVariant::fromValue(oldHost))));
  EXPECT_TRUE(row->property("previewVisible").toBool());
  grid.reset();
  EXPECT_TRUE(row->property("previewVisible").toBool());
  row.reset(); // active visual parent can die without destroying the player
  EXPECT_FALSE(player.isNull());
  EXPECT_EQ(previewService->property("target").value<QQuickItem *>(), nullptr);
  EXPECT_EQ(qobject_cast<QQuickItem *>(previewService.get())->parentItem(), window.contentItem());
  EXPECT_FALSE(player->property("playing").toBool());
  auto pending = createCard(true, first);
  ASSERT_NE(pending, nullptr);
  QTest::mouseMove(&window, QPoint(900, 700));
  QTest::mouseMove(&window, QPoint(100, 360));
  ASSERT_TRUE(QTest::qWaitFor([&] { return pending->property("hovered").toBool(); }));
  pending.reset();
  const int changesAfterDeletion = sourceChanges.count();
  QTest::qWait(200);
  EXPECT_EQ(sourceChanges.count(), changesAfterDeletion); // no delayed load after deletion
  ASSERT_TRUE(QTest::qWaitFor([&] { return player->property("source").toUrl().isEmpty(); }, 3000));
  EXPECT_EQ(previewService.get(), player.data());
  waitForProbes();
}

TEST_F(RecordingCardBindingTest, CachedPreviewRejectsLateResultsAndNeverOpensOriginal) {
  createPreviewService();
  previewFake.hold = true;
  const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
  if (ffmpeg.isEmpty()) GTEST_SKIP();
  const QString cached = tempDir.filePath("Cached.mp4");
  QProcess generator;
  generator.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i",
      "testsrc2=size=160x90:rate=15", "-t", "2", "-c:v", "mpeg4", cached});
  ASSERT_TRUE(generator.waitForFinished(10000));
  ASSERT_EQ(generator.exitCode(), 0);
  QQuickItem first(window.contentItem()), second(window.contentItem());
  first.setSize(QSizeF(320, 180)); second.setSize(QSizeF(320, 180));
  const QUrl oldSource = QUrl::fromLocalFile(tempDir.filePath("Original 1.mkv"));
  const QUrl newSource = QUrl::fromLocalFile(tempDir.filePath("Original 2.mkv"));
  auto request = [&](QQuickItem *host, const QUrl &url) {
    EXPECT_TRUE(QMetaObject::invokeMethod(previewService.get(), "request",
        Q_ARG(QVariant, QVariant::fromValue(host)), Q_ARG(QVariant, QVariant(url))));
  };
  QObject *player = previewService.get();
  ASSERT_NE(player, nullptr);
  request(&first, oldSource);
  ASSERT_TRUE(QTest::qWaitFor([&] { return previewFake.requests.size() == 1; }));
  const int staleTicket = previewFake.requests.first().second;
  EXPECT_TRUE(player->property("source").toUrl().isEmpty());
  request(&second, newSource);
  ASSERT_TRUE(QTest::qWaitFor([&] { return previewFake.requests.size() == 2; }));
  const int currentTicket = previewFake.requests.last().second;
  EXPECT_TRUE(previewFake.cancelled.contains(staleTicket));
  previewFake.deliver(oldSource, staleTicket, QUrl::fromLocalFile(cached));
  previewFake.deliver(oldSource, currentTicket, QUrl::fromLocalFile(cached));
  EXPECT_TRUE(player->property("source").toUrl().isEmpty());
  EXPECT_FALSE(previewService->property("frameReady").toBool());
  previewFake.deliver(newSource, currentTicket, QUrl::fromLocalFile(cached));
  ASSERT_TRUE(QTest::qWaitFor([&] { return previewService->property("frameReady").toBool(); }, 10000));
  EXPECT_EQ(player->property("source").toUrl().toLocalFile(), cached);
  EXPECT_EQ(previewService->property("target").value<QQuickItem *>(), &second);
  EXPECT_EQ(previewService->property("fadeDuration").toInt(), 180);
  EXPECT_LT(previewService->property("opacity").toDouble(), 0.99);
  ASSERT_TRUE(QTest::qWaitFor([&] { return previewService->property("opacity").toDouble() > 0.99; }, 1000));
  ASSERT_TRUE(QMetaObject::invokeMethod(previewService.get(), "release",
      Q_ARG(QVariant, QVariant::fromValue(&second))));
  EXPECT_FALSE(previewService->property("visible").toBool());
  EXPECT_DOUBLE_EQ(previewService->property("opacity").toDouble(), 0);
}

TEST_F(RecordingCardBindingTest, RealCacheRendersPreparedFramesRatherThanOpeningOriginal) {
  createPreviewService();
  const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
  if (ffmpeg.isEmpty()) GTEST_SKIP();
  const QString original = tempDir.filePath("Original # % Video.mkv");
  QProcess generator;
  generator.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i",
      "testsrc2=size=640x360:rate=30", "-t", "9", "-c:v", "mpeg4", original});
  ASSERT_TRUE(generator.waitForFinished(10000));
  ASSERT_EQ(generator.exitCode(), 0);
  PreviewCache realCache;
  engine.removeImageProvider("hoverpreview");
  engine.addImageProvider("hoverpreview", realCache.createImageProvider());
  previewService->setProperty("cache", QVariant::fromValue(&realCache));
  QQuickItem host(window.contentItem());
  host.setSize(QSizeF(320, 180));
  ASSERT_TRUE(QMetaObject::invokeMethod(previewService.get(), "request",
      Q_ARG(QVariant, QVariant::fromValue(&host)),
      Q_ARG(QVariant, QVariant(QUrl::fromLocalFile(original)))));
  ASSERT_TRUE(QTest::qWaitFor([&] { return previewService->property("frameReady").toBool(); }, 15000));
  QObject *player = previewService.get();
  ASSERT_NE(player, nullptr);
  const QString loaded = player->property("source").toUrl().toLocalFile();
  EXPECT_NE(loaded, original);
  EXPECT_EQ(QFileInfo(loaded).absolutePath(), tempDir.filePath(".thumbnails"));
  EXPECT_TRUE(QFileInfo(loaded).fileName().contains(".preview-v1-"));
  EXPECT_EQ(player->findChild<QObject *>("hoverPreviewPlayer"), nullptr);
  EXPECT_EQ(previewService->property("targetSource").toUrl().toLocalFile(), original);
  ASSERT_TRUE(QMetaObject::invokeMethod(previewService.get(), "release",
      Q_ARG(QVariant, QVariant::fromValue(&host))));
}

TEST_F(RecordingCardBindingTest, MissingCachedPreviewKeepsThumbnailWithoutOpeningOriginal) {
  createPreviewService();
  const QUrl original = QUrl::fromLocalFile(tempDir.filePath("No preview # %.mkv"));
  previewFake.results.insert(original, QUrl());
  QQuickItem host(window.contentItem());
  host.setSize(QSizeF(320, 180));
  ASSERT_TRUE(QMetaObject::invokeMethod(previewService.get(), "request",
      Q_ARG(QVariant, QVariant::fromValue(&host)), Q_ARG(QVariant, QVariant(original))));
  ASSERT_TRUE(QTest::qWaitFor([&] {
    return previewService->property("failedTarget").value<QQuickItem *>() == &host;
  }));
  QObject *player = previewService.get();
  ASSERT_NE(player, nullptr);
  EXPECT_TRUE(player->property("source").toUrl().isEmpty());
  EXPECT_FALSE(previewService->property("visible").toBool());
  EXPECT_DOUBLE_EQ(previewService->property("opacity").toDouble(), 0);
}

void RecordingCardBindingTest::exerciseFailedPreview(bool list) {
  createPreviewService();
  const QString path = createVideo("Broken.mkv");
  previewFake.results.insert(QUrl::fromLocalFile(path), QUrl());
  RecordingLibrary lib(tempDir.path());
  engine.rootContext()->setContextProperty("recordingLibrary", &lib);
  QString qmlPath = CARD_QML_FILE;
  if (list) qmlPath.replace("RecordingCard.qml", "RecordingListItem.qml");
  QQmlComponent component(&engine, QUrl::fromLocalFile(qmlPath));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> card(component.create());
  ASSERT_NE(card, nullptr);
  auto *item = qobject_cast<QQuickItem *>(card.get());
  item->setParentItem(window.contentItem());
  item->setWidth(list ? 720 : 320);
  card->setProperty("previewController", QVariant::fromValue(previewService.get()));
  card->setProperty("recording", QVariant::fromValue(lib.getRecording(path)));
  QTest::mouseMove(&window, QPoint(700, 500));
  QTest::mouseMove(&window, QPoint(100, 80));
  ASSERT_TRUE(QTest::qWaitFor([&] { return card->property("previewFailed").toBool(); }, 10000));
  EXPECT_FALSE(card->property("previewVisible").toBool());
  EXPECT_FALSE(card->property("previewArmed").toBool());
  QObject *player = previewService.get();
  ASSERT_NE(player, nullptr);
  const int requestsBeforeRetry = previewFake.requests.size();
  QTest::qWait(200);
  EXPECT_EQ(previewFake.requests.size(), requestsBeforeRetry); // no retry storm
  QTest::mouseMove(&window, QPoint(700, 500));
  ASSERT_TRUE(QTest::qWaitFor([&] { return !card->property("previewFailed").toBool(); }));
  QTest::mouseMove(&window, QPoint(100, 80));
  ASSERT_TRUE(QTest::qWaitFor([&] { return card->property("previewFailed").toBool(); }, 10000));
  EXPECT_EQ(previewFake.requests.size(), requestsBeforeRetry + 1);
  // Changing the delegate's source retires its old attempt even while hovered.
  card->setProperty("recording", QVariant());
  EXPECT_FALSE(card->property("previewArmed").toBool());
  EXPECT_FALSE(card->property("previewFailed").toBool());
  card.reset();
  waitForProbes();
}

TEST_F(RecordingCardBindingTest, FailedPreviewFallsBackUntilNextHover) {
  exerciseFailedPreview(false);
}

TEST_F(RecordingCardBindingTest, FailedListPreviewFallsBackUntilNextHover) {
  exerciseFailedPreview(true);
}

TEST_F(RecordingCardBindingTest, LiveGridIconFollowsCurrentGameAndModelRefresh) {
  CardManagerFake manager;
  manager.gameName = "Game";
  CardIconFake detector;
  detector.iconPath = tempDir.filePath("live-app.png");
  QImage image(48, 48, QImage::Format_ARGB32);
  image.fill(Qt::green);
  ASSERT_TRUE(image.save(detector.iconPath));
  engine.rootContext()->setContextProperty("recordingManager", &manager);
  engine.rootContext()->setContextProperty("gameDetector", &detector);

  QQmlComponent component(&engine, QUrl::fromLocalFile(CARD_QML_FILE).resolved(
      QUrl("ActiveRecordingCard.qml")));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> card(component.create());
  ASSERT_NE(card, nullptr);
  auto *item = qobject_cast<QQuickItem *>(card.get());
  ASSERT_NE(item, nullptr);
  item->setParentItem(window.contentItem());
  item->setWidth(340);
  item->setHeight(280);
  auto *icon = card->findChild<QQuickItem *>("liveGridGameIcon");
  ASSERT_NE(icon, nullptr);
  for (int n = 0; n < 100 && !icon->isVisible(); ++n)
    QTest::qWait(10);
  ASSERT_TRUE(icon->isVisible());
  QTest::qWait(50);
  EXPECT_EQ(icon->width(), 40);
  EXPECT_EQ(icon->height(), 40);

  manager.gameName = "Other";
  emit manager.changed();
  QTest::qWait(50);
  EXPECT_FALSE(icon->isVisible());

  detector.iconPath = tempDir.filePath("missing.png");
  manager.gameName = "Game";
  emit manager.changed();
  QTest::qWait(100);
  EXPECT_FALSE(icon->isVisible());
  detector.iconPath = tempDir.filePath("live-app.png");
  detector.model.clear();
  for (int n = 0; n < 100 && !icon->isVisible(); ++n)
    QTest::qWait(10);
  EXPECT_TRUE(icon->isVisible());
}

TEST_F(RecordingCardBindingTest, ExplicitCardDependenciesOverrideCreationContextAndStayReactive) {
  const QString path = createVideo("Game_2026-01-15_20-30-00.mkv");
  RecordingLibrary library(tempDir.path());
  CardWindowFake explicitWindow;
  for (const QString &file : {QString("RecordingCard.qml"), QString("RecordingListItem.qml")}) {
    QQmlComponent component(&engine, QUrl::fromLocalFile(CARD_QML_FILE).resolved(QUrl(file)));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> card(component.createWithInitialProperties({
        {"recording", QVariant::fromValue(library.getRecording(path))},
        {"library", QVariant::fromValue(&library)},
        {"hostWindow", QVariant::fromValue(&explicitWindow)}}));
    ASSERT_NE(card, nullptr) << component.errorString().toStdString();
    auto *item = qobject_cast<QQuickItem *>(card.get());
    ASSERT_NE(item, nullptr);
    item->setParentItem(window.contentItem());
    item->setWidth(680);
    explicitWindow.toggleSelection(path);
    EXPECT_TRUE(card->property("isSelected").toBool());
    windowFake.toggleSelection(path); // The old context must not take control.
    explicitWindow.clearSelection();
    EXPECT_FALSE(card->property("isSelected").toBool());
    auto *lock = card->findChild<QObject *>("captureProtectionButton");
    ASSERT_NE(lock, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(lock, "clicked"));
    EXPECT_EQ(explicitWindow.protectionPath, path);
    EXPECT_TRUE(windowFake.protectionPath.isEmpty());
    windowFake.clearSelection();
  }
  waitForProbes();
}

TEST_F(RecordingCardBindingTest, AppIconsAppearOnlyWhenLoadedInBothViews) {
  const QString path = createVideo("Game_2026-01-15_20-30-00.mkv");
  const QString otherPath = createVideo("Other_2026-01-15_20-30-00.mkv");
  const QString manualPath = createVideo("Manual_Recording_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  CardIconFake detector;
  detector.iconPath = tempDir.filePath("app.png");
  QImage iconImage(48, 48, QImage::Format_ARGB32);
  iconImage.fill(Qt::green);
  ASSERT_TRUE(iconImage.save(detector.iconPath));
  engine.rootContext()->setContextProperty("recordingLibrary", &lib);
  engine.rootContext()->setContextProperty("gameDetector", &detector);

  for (const auto &file : {QString("RecordingCard.qml"), QString("RecordingListItem.qml")}) {
    QQmlComponent component(&engine, QUrl::fromLocalFile(CARD_QML_FILE).resolved(QUrl(file)));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> card(component.create());
    ASSERT_NE(card, nullptr);
    auto *item = qobject_cast<QQuickItem *>(card.get());
    ASSERT_NE(item, nullptr);
    item->setParentItem(window.contentItem());
    item->setWidth(680);
    auto *icon = card->findChild<QQuickItem *>(
        file == "RecordingCard.qml" ? "gridGameIcon" : "listGameIcon");
    ASSERT_NE(icon, nullptr);
    EXPECT_FALSE(icon->isVisible());
    card->setProperty("recording", QVariant::fromValue(lib.getRecording(path)));
    for (int n = 0; n < 100 && !icon->isVisible(); ++n)
      QTest::qWait(10);
    ASSERT_TRUE(icon->isVisible());
    QTest::qWait(50); // Let the layout reclaim the newly visible icon slot.
    EXPECT_EQ(icon->width(), file == "RecordingCard.qml" ? 40 : 24);
    qreal detailsWidthWithIcon = 0;
    if (file == "RecordingCard.qml") {
      auto *date = card->findChild<QQuickItem *>("gridDate");
      ASSERT_NE(date, nullptr);
      detailsWidthWithIcon = date->width();
      EXPECT_LE(date->mapToItem(icon, QPointF(date->width(), 0)).x(), 0);
    } else {
      auto *thumbnail = card->findChild<QQuickItem *>("listThumbnail");
      ASSERT_NE(thumbnail, nullptr);
      EXPECT_EQ(icon->parentItem(), thumbnail);
      EXPECT_EQ(icon->x(), 6);
      EXPECT_EQ(icon->y(), 6);
    }
    card->setProperty("recording", QVariant::fromValue(lib.getRecording(otherPath)));
    QTest::qWait(50);
    EXPECT_FALSE(icon->isVisible());
    if (file == "RecordingCard.qml")
      EXPECT_GT(card->findChild<QQuickItem *>("gridDate")->width(), detailsWidthWithIcon);
    // Failed image loads must also disappear, not leave a placeholder.
    detector.iconPath = tempDir.filePath("missing.png");
    card->setProperty("recording", QVariant::fromValue(lib.getRecording(path)));
    QTest::qWait(100);
    EXPECT_FALSE(icon->isVisible());
    detector.iconPath = tempDir.filePath("app.png");
    detector.model.clear(); // A refreshed app list retries the icon lookup.
    for (int n = 0; n < 100 && !icon->isVisible(); ++n)
      QTest::qWait(10);
    EXPECT_TRUE(icon->isVisible());
    card->setProperty("recording", QVariant::fromValue(lib.getRecording(manualPath)));
    for (int n = 0; n < 100 && icon->property("status").toInt() != 1; ++n)
      QTest::qWait(10);
    EXPECT_EQ(icon->property("source").toUrl(), QUrl("qrc:/icons/manual-recording.svg"));
    EXPECT_TRUE(icon->isVisible());
    QTest::qWait(50);
    EXPECT_EQ(icon->width(), file == "RecordingCard.qml" ? 40 : 24);
    if (file == "RecordingListItem.qml") {
      EXPECT_EQ(icon->x(), 6);
      EXPECT_EQ(icon->y(), 6);
    }
  }
  waitForProbes();
}

TEST_F(RecordingCardBindingTest, CaptureHealthBelongsToStatusCard) {
  for (const auto *name : {"recordingLibrary", "settingsManager", "storageManager",
                           "replayManager", "activityFeed", "gameDetector", "audioMixer", "app"})
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
  QTest::qWait(50);
  auto *card = page->findChild<QQuickItem *>("captureStatusCard");
  auto *statusTimer = page->findChild<QObject *>("captureStatusTimer");
  ASSERT_NE(statusTimer, nullptr);
  statusTimer->setProperty("running", false); // snapshots are injected by this fixture
  auto *settings = page->findChild<QQuickItem *>("recordingSettingsCard");
  auto *health = page->findChild<QQuickItem *>("captureHealthStatus");
  auto *bookmark = page->findChild<QQuickItem *>("addRecordingBookmark");
  auto *infoButton = page->findChild<QQuickItem *>("captureInfoButton");
  auto *infoDialog = page->findChild<QObject *>("captureInfoDialog");
  ASSERT_NE(infoButton, nullptr);
  ASSERT_NE(infoDialog, nullptr);
  EXPECT_FALSE(infoButton->isEnabled());
  EXPECT_FALSE(infoButton->isVisible());
  ASSERT_NE(card, nullptr);
  ASSERT_NE(settings, nullptr);
  ASSERT_NE(health, nullptr);
  ASSERT_NE(bookmark, nullptr);
  EXPECT_TRUE(card->isAncestorOf(health));
  EXPECT_FALSE(settings->isAncestorOf(health));
  EXPECT_FALSE(health->isVisible());
  EXPECT_NEAR(card->height(), 80, 1);
  const qreal settingsHeight = settings->height();

  for (const QString &mode : {QStringLiteral("recording"), QStringLiteral("replay")}) {
    QVariantMap status{{"mode", mode}, {"hasVideo", true},
                       {"audio0", "capturing"}, {"audio1", "included-in-desktop"},
                       {"audio2", "capturing"}, {"desktopFallback", true},
                       {"bufferSeconds", 12}, {"bufferTargetSeconds", 30}};
    page->setProperty("captureStatus", status);
    QTest::qWait(50);
    EXPECT_TRUE(infoButton->isEnabled());
    EXPECT_TRUE(infoButton->isVisible());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                     infoButton->mapToScene(QPointF(infoButton->width() / 2,
                                                    infoButton->height() / 2)).toPoint());
    QTest::qWait(50);
    EXPECT_TRUE(infoDialog->property("visible").toBool());
    EXPECT_GT(infoDialog->property("height").toDouble(), 300);
    // Live values are rebound, including a segment/session replacement.
    status["deliveredFrames"] = 123;
    status["actualCaptureFps"] = 59.3;
    status["bitrateKbps"] = 12000;
    status["actualOutputKbps"] = 13000;
    status["rateControlMode"] = "VBR";
    status["transport"] = "shm";
    status["sourceLabel"] = "Selected window";
    status["detectedGameKey"] = "steam_app_1422450";
    status["detectedSteamAppId"] = "1422450";
    status["detectedGamePid"] = 12345;
    status["audio2Signal"] = QVariantMap{
        {"received", true}, {"muted", false}, {"peak", 0.5}, {"rms", 0.25},
        {"peakHold", 1.25}, {"clippingRisk", true}, {"fullScaleSamplesTotal", 3},
        {"lastBufferAgeMs", 25}};
    status["audioStreams"] = QVariantList{QVariantMap{
        {"stream", 3}, {"codec", "AAC"}, {"sampleRate", 48000}, {"channels", 2}}};
    status["session"] = mode + "-next.mkv";
    page->setProperty("captureStatus", status);
    QQmlExpression statValue(qmlContext(page.get()), page.get(),
                            "captureInfoValue('deliveredFrames')");
    EXPECT_EQ(statValue.evaluate().toString(), "123");
    QQmlExpression unknownValue(qmlContext(page.get()), page.get(),
                               "captureInfoValue('encoderDrops')");
    EXPECT_EQ(unknownValue.evaluate().toString(), QString::fromUtf8("–"));
    std::function<QQuickItem *(QQuickItem *, const QString &)> findVisual;
    findVisual = [&](QQuickItem *parent, const QString &name) -> QQuickItem * {
      if (!parent) return nullptr;
      if (parent->objectName() == name || parent->property("rowName").toString() == name)
        return parent;
      for (auto *child : parent->childItems())
        if (auto *found = findVisual(child, name)) return found;
      return nullptr;
    };
    auto *content = infoDialog->property("contentItem").value<QQuickItem *>();
    auto *header = infoDialog->property("header").value<QQuickItem *>();
    auto *footer = infoDialog->property("footer").value<QQuickItem *>();
    auto *closeButton = findVisual(footer, "captureInfoCloseButton");
    ASSERT_NE(footer, nullptr);
    ASSERT_NE(closeButton, nullptr);
    const auto closeTopLeft = closeButton->mapToItem(footer, QPointF());
    EXPECT_GE(closeTopLeft.y(), 20);
    EXPECT_GE(footer->width() - closeTopLeft.x() - closeButton->width(), 20);
    EXPECT_GE(footer->height() - closeTopLeft.y() - closeButton->height(), 20);
    ASSERT_NE(content, nullptr);
    auto *flickable = content->property("contentItem").value<QQuickItem *>();
    ASSERT_NE(flickable, nullptr);
    QTest::qWait(20);
    EXPECT_GT(flickable->property("contentHeight").toDouble(),
              flickable->height() + 200);
    flickable->setProperty("contentY", 200);
    status["actualCaptureFps"] = 58.7;
    page->setProperty("captureStatus", status);
    QTest::qWait(20);
    EXPECT_NEAR(flickable->property("contentY").toDouble(), 200, 1);
    auto *captureRate = findVisual(content, "Actual capture frame rate");
    ASSERT_NE(captureRate, nullptr);
    EXPECT_EQ(captureRate->property("rowValue").toString(), "58.7 fps");
    auto *targetBitrate = findVisual(content, "Target video bitrate");
    auto *currentBitrate = findVisual(content, "Actual output bitrate (A/V payload)");
    ASSERT_NE(targetBitrate, nullptr);
    ASSERT_NE(currentBitrate, nullptr);
    EXPECT_EQ(targetBitrate->property("rowValue").toString(), QString::fromUtf8("12000 kbps · VBR"));
    EXPECT_EQ(currentBitrate->property("rowValue").toString(), QString::fromUtf8("13000 kbps · VBR"));
    auto *transport = findVisual(content, "Delivered transport");
    ASSERT_NE(transport, nullptr);
    EXPECT_EQ(transport->property("rowValue").toString(), "Shared memory (SHM)");
    for (int tabIndex : {1, 2, 0}) {
      auto *tab = findVisual(header, QStringLiteral("captureInfoTab%1").arg(tabIndex));
      ASSERT_NE(tab, nullptr);
      QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
                       tab->mapToScene(QPointF(tab->width() / 2, tab->height() / 2)).toPoint());
      QTest::qWait(10);
      EXPECT_EQ(infoDialog->property("section").toInt(), tabIndex);
      EXPECT_TRUE(tab->property("checked").toBool());
      EXPECT_NEAR(flickable->property("contentY").toDouble(), 0, 1);
      if (tabIndex == 1) {
        auto *audioStream = findVisual(content, "Audio stream 3");
        ASSERT_NE(audioStream, nullptr);
        EXPECT_EQ(audioStream->property("rowValue").toString(),
                  QString::fromUtf8("AAC · 48000 Hz · 2 channels"));
        auto *micPanel = findVisual(content, "captureAudioSignal2");
        auto *voicePanel = findVisual(content, "captureAudioSignal1");
        ASSERT_NE(micPanel, nullptr);
        ASSERT_NE(voicePanel, nullptr);
        auto *peak = findVisual(micPanel, QString::fromUtf8("Peak (≈1 s)"));
        auto *rms = findVisual(micPanel, QString::fromUtf8("RMS (≈1 s)"));
        auto *receipt = findVisual(micPanel, "Last audio buffer");
        auto *voiceReceipt = findVisual(voicePanel, "Last audio buffer");
        ASSERT_NE(peak, nullptr); ASSERT_NE(rms, nullptr);
        ASSERT_NE(receipt, nullptr); ASSERT_NE(voiceReceipt, nullptr);
        EXPECT_EQ(peak->property("rowValue").toString(), "-6.0 dBFS");
        EXPECT_EQ(rms->property("rowValue").toString(), "-12.0 dBFS");
        EXPECT_EQ(receipt->property("rowValue").toString(), "25 ms ago");
        EXPECT_EQ(voiceReceipt->property("rowValue").toString(), "No separate source");
        auto audioSnapshot = status;
        auto micSignal = status["audio2Signal"].toMap();
        micSignal["peak"] = 0.0;
        micSignal["rms"] = 0.0;
        micSignal["muted"] = true;
        audioSnapshot["audio2Signal"] = micSignal;
        page->setProperty("audioSignalSnapshot", audioSnapshot);
        QTest::qWait(10);
        EXPECT_EQ(findVisual(content, "captureAudioSignal2"), micPanel);
        EXPECT_EQ(peak->property("rowValue").toString(), QString::fromUtf8("−∞ dBFS"));
        micSignal.remove("peak"); micSignal.remove("rms");
        micSignal["lastBufferAgeMs"] = 2000;
        audioSnapshot["audio2Signal"] = micSignal;
        page->setProperty("audioSignalSnapshot", audioSnapshot);
        QTest::qWait(10);
        EXPECT_EQ(peak->property("rowValue").toString(), QString::fromUtf8("–"));
        EXPECT_EQ(receipt->property("rowValue").toString(), QString::fromUtf8("2.0 s ago · no recent buffers"));
        // Restore before the next tab/session fixture iteration.
        page->setProperty("audioSignalSnapshot", status);
      } else if (tabIndex == 2) {
        auto *source = findVisual(content, "Portal stream label");
        ASSERT_NE(source, nullptr);
        EXPECT_EQ(source->property("rowValue").toString(), "Selected window");
        auto *steamId = findVisual(content, "Steam App ID");
        ASSERT_NE(steamId, nullptr);
        EXPECT_EQ(steamId->property("rowValue").toString(), "1422450");
        auto *gamePid = findVisual(content, "Detected game PID (at capture start)");
        ASSERT_NE(gamePid, nullptr);
        EXPECT_EQ(gamePid->property("rowValue").toString(), "12345");
        EXPECT_EQ(findVisual(content, "DVR session duration") != nullptr, mode == "replay");
      }
    }
    ASSERT_TRUE(QMetaObject::invokeMethod(infoDialog, "close"));
    QTest::qWait(50);
    EXPECT_TRUE(health->isVisible());
    EXPECT_EQ(bookmark->isVisible(), mode == "recording");
    EXPECT_GT(card->height(), 80);
    const QPointF top = health->mapToItem(card, QPointF());
    EXPECT_GE(top.y(), 80);
    EXPECT_LE(top.y() + health->height(), card->height());
    EXPECT_GT(health->width(), 200);
    EXPECT_NEAR(settings->height(), settingsHeight, 1);
    auto *primary = page->findChild<QQuickItem *>("primaryAudioMeter");
    auto *voice = page->findChild<QQuickItem *>("voiceAudioMeter");
    ASSERT_NE(primary, nullptr); ASSERT_NE(voice, nullptr);
    EXPECT_EQ(primary->property("label").toString(), "System");
    EXPECT_FALSE(voice->isVisible());
    auto *mic = page->findChild<QQuickItem *>("micAudioMeter");
    ASSERT_NE(mic, nullptr);
    EXPECT_TRUE(mic->isVisible());
    EXPECT_EQ(primary->parentItem(), mic->parentItem());
    EXPECT_NEAR(mic->y(), primary->y() + primary->height(), 1);
  }
  auto appAudio = page->property("captureStatus").toMap();
  appAudio["desktopFallback"] = false;
  page->setProperty("captureStatus", appAudio);
  QTest::qWait(50);
  EXPECT_EQ(page->findChild<QQuickItem *>("primaryAudioMeter")->property("label").toString(), "Game");
  EXPECT_TRUE(page->findChild<QQuickItem *>("voiceAudioMeter")->isVisible());
  appAudio["gameName"] = "Deadlock";
  appAudio["audio1Name"] = "Discord";
  appAudio["audio2Name"] = "USB Microphone";
  appAudio["audio0"] = "capturing";
  appAudio["audio1"] = "capturing";
  appAudio["audio2"] = "capturing";
  page->setProperty("captureStatus", appAudio);
  QTest::qWait(50);
  const auto healthText = [&](int role) {
    QQmlExpression expression(qmlContext(page.get()), page.get(),
                              QStringLiteral("audioHealthText(%1)").arg(role));
    return expression.evaluate().toString();
  };
  EXPECT_EQ(healthText(0), "Connected to Deadlock");
  EXPECT_EQ(healthText(1), "Connected to Discord");
  // The device enumerator is a null context property here, so the mic falls
  // back to the source's own display name rather than a generic "Connected".
  EXPECT_EQ(healthText(2), "Connected to USB Microphone");
  auto *primary = page->findChild<QQuickItem *>("primaryAudioMeter");
  auto *voice = page->findChild<QQuickItem *>("voiceAudioMeter");
  auto *mic = page->findChild<QQuickItem *>("micAudioMeter");
  EXPECT_NEAR(voice->y(), primary->y() + primary->height(), 1);
  EXPECT_NEAR(mic->y(), voice->y() + voice->height(), 1);
  QTest::qWait(260);
  const qreal expandedHeight = card->height();
  ASSERT_TRUE(QMetaObject::invokeMethod(infoDialog, "open"));
  EXPECT_TRUE(infoDialog->property("visible").toBool());
  page->setProperty("captureStatus", QVariantMap{});
  EXPECT_FALSE(infoButton->isEnabled());
  EXPECT_FALSE(infoButton->isVisible());
  EXPECT_FALSE(infoDialog->property("visible").toBool());
  QTest::qWait(120);
  EXPECT_TRUE(health->isVisible());
  EXPECT_FALSE(health->isEnabled());
  EXPECT_GT(card->height(), 80);
  EXPECT_LT(card->height(), expandedHeight);
  EXPECT_EQ(page->property("healthStatus").toMap().value("mode").toString(), "replay");
  QTest::qWait(260);
  EXPECT_FALSE(health->isVisible());
  EXPECT_NEAR(card->height(), 80, 1);
}

TEST_F(RecordingCardBindingTest, CaptureInfoRowsSettleAcrossLiveUpdatesAndResizing) {
  for (const auto *name : {"recordingLibrary", "settingsManager", "storageManager",
                           "replayManager", "activityFeed", "gameDetector", "audioMixer", "app"})
    engine.rootContext()->setContextProperty(name, QVariant::fromValue<QObject *>(nullptr));
  const auto url = QUrl::fromLocalFile(CARD_QML_FILE).resolved(QUrl("../pages/DashboardPage.qml"));
  QQmlComponent component(&engine, url);
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> page(component.create());
  ASSERT_NE(page, nullptr);
  auto *item = qobject_cast<QQuickItem *>(page.get());
  ASSERT_NE(item, nullptr);
  item->setParentItem(window.contentItem());
  item->setSize(QSizeF(1280, 800));
  auto *timer = page->findChild<QObject *>("captureStatusTimer");
  ASSERT_NE(timer, nullptr);
  timer->setProperty("running", false);
  auto *dialog = page->findChild<QObject *>("captureInfoDialog");
  ASSERT_NE(dialog, nullptr);
  QVariantMap status{{"mode", "recording"}, {"session", "layout-fixture"},
                     {"hasVideo", true}, {"encoder", "h264_nvenc"},
                     {"inputWidth", 1920}, {"inputHeight", 1080},
                     {"outputWidth", 1920}, {"outputHeight", 1080},
                     {"actualCaptureFps", 59.9}, {"actualEncodeFps", 59.8},
                     {"audioStreams", QVariantList{QVariantMap{{"stream", 1}, {"codec", "AAC"},
                       {"sampleRate", 48000}, {"channels", 2}}}},
                     {"sourceLabel", QString(180, 'W')},
                     {"detectedGameKey", QString(200, 'x')}};
  page->setProperty("captureStatus", status);
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "open"));
  for (int section : {0, 1, 2, 0}) {
    dialog->setProperty("section", section);
    for (int width : {1280, 720, 480, 900, 1280}) {
      window.resize(width, 800);
      item->setSize(QSizeF(width, 800));
      status["actualOutputKbps"] = width * 17;
      status["encoder"] = width % 3 ? "h264_nvenc" : QString(140, 'E');
      status["audioStreams"] = QVariantList{QVariantMap{{"stream", 1},
        {"codec", width % 3 ? "AAC" : QString(120, 'A')}, {"sampleRate", 48000}, {"channels", 2}}};
      page->setProperty("captureStatus", status);
      QTest::qWait(35);
      auto *content = dialog->property("contentItem").value<QQuickItem *>();
      ASSERT_NE(content, nullptr);
      int rows = 0;
      std::function<void(QQuickItem *)> inspect = [&](QQuickItem *parent) {
        if (parent->property("rowName").isValid() && parent->isVisible()) {
          ++rows;
          EXPECT_GT(parent->width(), 100);
          EXPECT_GT(parent->height(), 10);
          EXPECT_LE(parent->width(), content->width() + 1);
          const QSizeF settled = parent->size();
          QCoreApplication::processEvents();
          EXPECT_EQ(parent->size(), settled);
          for (auto *child : parent->childItems())
            if (child->isVisible()) {
              EXPECT_GE(child->y(), 0);
              EXPECT_LE(child->y() + child->height(), parent->height() + 1);
            }
        }
        for (auto *child : parent->childItems()) inspect(child);
      };
      inspect(content);
      EXPECT_GT(rows, 0);
    }
  }
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
  QTest::qWait(50);
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "open"));
  QTest::qWait(50);
}

TEST_F(RecordingCardBindingTest, DashboardActivityDurationIncludesHours) {
  for (const auto *name : {"recordingLibrary", "settingsManager", "storageManager",
                           "replayManager", "activityFeed", "gameDetector", "audioMixer", "app"})
    engine.rootContext()->setContextProperty(name, QVariant::fromValue<QObject *>(nullptr));
  const auto dashboardUrl = QUrl::fromLocalFile(CARD_QML_FILE).resolved(
      QUrl("../pages/DashboardPage.qml"));
  QQmlComponent component(&engine, dashboardUrl);
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> page(component.create());
  ASSERT_NE(page, nullptr) << component.errorString().toStdString();
  for (const auto &[seconds, expected] : QList<QPair<int, QString>>{
           {0, "00:00"}, {65, "1:05"}, {3599, "59:59"}, {3600, "1:00:00"},
           {3723, "1:02:03"}, {36000, "10:00:00"}}) {
    QQmlExpression expression(engine.rootContext(), page.get(),
                              QStringLiteral("formatDuration(%1)").arg(seconds));
    EXPECT_EQ(expression.evaluate().toString(), expected);
    EXPECT_FALSE(expression.hasError());
  }
}

TEST_F(RecordingCardBindingTest, RecordingPillDurationIncludesHours) {
  // Exercise the actual binding without starting the application's services.
  const auto mainUrl = QUrl::fromLocalFile(CARD_QML_FILE).resolved(QUrl("../main.qml"));
  QFile file(mainUrl.toLocalFile());
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const QString source = QString::fromUtf8(file.readAll());
  const auto start = source.indexOf("var durMs =");
  ASSERT_GE(start, 0);
  const auto end = source.indexOf('}', start);
  ASSERT_GT(end, start);
  QString binding = source.mid(start, end - start);
  binding.replace("window.recordingManager", "recordingManager");
  for (const auto &[ms, expected] : QList<QPair<qint64, QString>>{
           {0, "0:00"}, {65999, "1:05"}, {3599000, "59:59"}, {3600000, "1:00:00"},
           {3723000, "1:02:03"}, {36000000, "10:00:00"}}) {
    manager.duration = ms;
    QQmlExpression expression(engine.rootContext(), nullptr,
                              "(function() {" + binding + "})()");
    EXPECT_EQ(expression.evaluate().toString(), expected);
    EXPECT_FALSE(expression.hasError()) << expression.error().toString().toStdString();
  }
}

TEST_F(RecordingCardBindingTest, DashboardStorageAndActivityIgnoreLibraryFilters) {
  createVideo("Game_2026-01-15_20-30-00.mkv");
  createVideo("Other_2026-01-15_20-31-00.mkv");
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
  auto *count = page->findChild<QObject *>("storageRecordingCount");
  auto *activity = page->findChild<QObject *>("recentActivityModel");
  ASSERT_NE(count, nullptr);
  ASSERT_NE(activity, nullptr);
  const auto totalText = count->property("text").toString();
  EXPECT_TRUE(totalText.startsWith("2"));
  ASSERT_EQ(activity->property("count").toInt(), 2);
  lib.setSearchQuery("no matching capture");
  lib.setKindFilter(RecordingLibrary::NoCaptures);
  lib.setProtectedOnly(true);
  lib.setSortOrder(RecordingLibrary::NameZA);
  QCoreApplication::processEvents();
  EXPECT_EQ(lib.count(), 0);
  EXPECT_EQ(count->property("text").toString(), totalText);
  EXPECT_EQ(activity->property("count").toInt(), 2);
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

  QPointer<QObject> sharedPreview = page->findChild<QObject *>("libraryHoverPreview");
  ASSERT_NE(sharedPreview, nullptr);
  QPointer<QObject> sharedPlayer = sharedPreview->findChild<QObject *>("hoverPreviewImage");
  ASSERT_NE(sharedPlayer, nullptr);
  EXPECT_EQ(activeView()->property("previewController").value<QObject *>(), sharedPreview.data());

  windowFake.toggleSelection(path);
  auto *listButton = page->findChild<QObject *>("libraryListButton");
  ASSERT_NE(listButton, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(listButton, "clicked"));
  QTest::qWait(100);
  EXPECT_TRUE(page->property("libraryListView").toBool());
  EXPECT_EQ(page->findChild<QObject *>("hoverPreviewImage"), sharedPlayer.data());
  ASSERT_NE(activeView(), nullptr);
  EXPECT_EQ(activeView()->property("count").toInt(), 1);
  EXPECT_EQ(activeView()->property("previewController").value<QObject *>(), sharedPreview.data());
  EXPECT_TRUE(windowFake.selectedRecordings().contains(path));
  EXPECT_TRUE(QMetaObject::invokeMethod(page.get(), "scrollToRecording", Q_ARG(QVariant, 0)));

  auto *gridButton = page->findChild<QObject *>("libraryGridButton");
  ASSERT_NE(gridButton, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(gridButton, "clicked"));
  QTest::qWait(100);
  EXPECT_FALSE(page->property("libraryListView").toBool());
  EXPECT_EQ(page->findChildren<QObject *>("hoverPreviewImage").size(), 1);
  EXPECT_EQ(page->findChild<QObject *>("hoverPreviewImage"), sharedPlayer.data());
  EXPECT_EQ(page->findChild<QObject *>("hoverPreviewPlayer"), nullptr);
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
  for (int n = 0; n < 100 && filterPopup->property("visible").toBool(); ++n)
    QTest::qWait(10);
  EXPECT_FALSE(filterPopup->property("visible").toBool());
  EXPECT_TRUE(filterButton->property("checked").toBool());
  page.reset();
  waitForProbes();
}

TEST_F(RecordingCardBindingTest, LiveListRowMatchesSavedRowsAndUpdatesStats) {
  createVideo("Game_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  engine.rootContext()->setContextProperty("recordingLibrary", &lib);
  QQmlComponent component(&engine, QUrl::fromLocalFile(CARD_QML_FILE).resolved(
      QUrl("RecordingListView.qml")));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> view(component.create());
  ASSERT_NE(view, nullptr);
  auto *item = qobject_cast<QQuickItem *>(view.get());
  ASSERT_NE(item, nullptr);
  item->setParentItem(window.contentItem());
  item->setSize(QSizeF(680, 400));
  QTest::qWait(100);
  EXPECT_EQ(view->property("count").toInt(), 1);
  EXPECT_EQ(view->property("contentY").toDouble(), 0);
  // Start while the existing list is visible, without recreating it.
  manager.live = true;
  emit manager.changed();
  QTest::qWait(100);
  EXPECT_EQ(view->property("count").toInt(), 2);
  auto *content = view->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(content, nullptr);
  QQuickItem *row = nullptr;
  QQuickItem *savedRow = nullptr;
  for (auto *child : content->childItems()) {
    if (child->objectName() == "liveListRow")
      row = child;
    if (child->objectName() == "savedListRow")
      savedRow = child;
  }
  ASSERT_NE(row, nullptr);
  ASSERT_NE(savedRow, nullptr);
  EXPECT_EQ(savedRow->property("cardIndex").toInt(), 0);
  EXPECT_GE(row->mapToItem(item, QPointF()).y(), 0);
  EXPECT_LE(row->mapToItem(item, QPointF(0, row->height())).y(), item->height());
  EXPECT_TRUE(row->isVisible());
  EXPECT_EQ(row->height(), 92);
  EXPECT_EQ(row->width(), 680);
  auto *thumbnail = row->findChild<QQuickItem *>("listThumbnail");
  auto *title = row->findChild<QQuickItem *>("listTitle");
  auto *metadata = row->findChild<QQuickItem *>("listMetadata");
  auto *selection = row->findChild<QQuickItem *>("listSelection");
  auto *actions = row->findChild<QQuickItem *>("listActions");
  ASSERT_NE(thumbnail, nullptr);
  ASSERT_NE(title, nullptr);
  ASSERT_NE(metadata, nullptr);
  ASSERT_NE(selection, nullptr);
  ASSERT_NE(actions, nullptr);
  EXPECT_EQ(thumbnail->size(), QSizeF(128, 72));
  EXPECT_EQ(title->property("text").toString(), "Manual Recording");
  EXPECT_EQ(metadata->property("text").toString(), QString::fromUtf8("• 1:05 • 1.0 MB"));
  EXPECT_EQ(row->findChild<QObject *>("listType")->property("text").toString(), QString::fromUtf8("● REC"));
  windowFake.toggleSelection("/other.mkv");
  QTest::mouseMove(&window, QPoint(340, 46));
  QTest::qWait(250);
  EXPECT_FALSE(selection->isVisible());
  EXPECT_FALSE(actions->isEnabled());
  EXPECT_TRUE(QMetaObject::invokeMethod(row, "activate", Q_ARG(QVariant, 0)));
  EXPECT_TRUE(windowFake.playedPath.isEmpty());
  manager.duration = 125000;
  emit manager.changed();
  EXPECT_TRUE(metadata->property("text").toString().contains("2:05"));
  manager.live = false;
  emit manager.changed();
  QTest::qWait(50);
  EXPECT_EQ(view->property("count").toInt(), 1);
  view.reset();
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
  EXPECT_NEAR(thumbnail->x() - restingThumbnailX, 30, 0.5); // Stacked controls share one gutter.
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
  EXPECT_GE(actions->width(), 152); // Play / Export to share / Rename / Delete.
  auto *share = row->findChild<QQuickItem *>("listShareExport");
  ASSERT_NE(share, nullptr);
  QTest::mouseMove(&window, actions->mapToScene(QPointF(10, 10)).toPoint());
  QTest::qWait(250);
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      share->mapToScene(QPointF(share->width() / 2, share->height() / 2)).toPoint());
  EXPECT_EQ(windowFake.sharedPath, path);
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
