/* Real PlayerPage engine load with controlled probe delivery, no media I/O. */
#include <QGuiApplication>
#include <QColor>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QJSValue>
#include <QQmlProperty>
#include <QTest>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#include <QSignalSpy>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <tuple>

class PlayerWaveformFake : public QObject {
  Q_OBJECT
public:
  int requests = 0;
  Q_INVOKABLE void requestAudioTrackCount(const QString &, int) { ++requests; }
  Q_INVOKABLE void requestPeaks(const QString &, int) {}
  Q_INVOKABLE QString trackLabel(int, int) const { return "Audio"; }
signals:
  void audioTrackCountReady(const QString &filePath, int requestId, int count);
  void peaksReady(const QString &filePath, int track, const QVariantList &peaks);
};

class PlayerLibraryFake : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool clipSaving READ clipSaving CONSTANT)
public:
  QQmlPropertyMap entry;
  bool hasEntry = false;
  bool realMedia = false;
  QString bookmarkedPath;
  qint64 bookmarkedPosition = -1;
  int removedIndex = -1;
  Q_INVOKABLE bool removeBookmark(const QString &, int index) {
    removedIndex = index;
    auto marks = entry.value("bookmarks").toList();
    if (index < 0 || index >= marks.size()) return false;
    marks.removeAt(index);
    entry.insert("bookmarks", marks);
    return true;
  }
  Q_INVOKABLE bool addBookmark(const QString &path, qint64 ms) {
    bookmarkedPath = path;
    bookmarkedPosition = ms;
    auto marks = entry.value("bookmarks").toList();
    marks.append(QVariantMap{{"positionMs", ms}});
    entry.insert("bookmarks", marks);
    return true;
  }
  PlayerLibraryFake() {
    // Returned QObject pointers default to JS ownership; this is a member,
    // not a heap object the engine may collect.
    QQmlEngine::setObjectOwnership(&entry, QQmlEngine::CppOwnership);
  }
  bool clipSaving() const { return false; }
  // Keep pipelines idle: this suite tests the page's probe lifecycle, not
  // decoding, and never passes the fake file paths to FFmpeg.
  Q_INVOKABLE QUrl localFileUrl(const QString &path) const {
    return realMedia || path.endsWith(".png") ? QUrl::fromLocalFile(path) : QUrl{};
  }
  Q_INVOKABLE QString localFilePath(const QUrl &url) const {
    return url.isLocalFile() ? url.toLocalFile() : QString{};
  }
  // The file-info dialog looks the open path up in the library; unknown
  // here, so its rows fall back to path-derived text.
  Q_INVOKABLE QObject *getRecording(const QString &) { return hasEntry ? &entry : nullptr; }
signals:
  void clipSaved(const QString &path, const QString &game, double seconds);
  void clipSaveFailed(const QString &reason);
};

class PlayerFilmstripFake : public QObject {
  Q_OBJECT
public:
  Q_INVOKABLE void requestThumbs(const QString &, int) {}
signals:
  void filmstripReady(const QString &path, const QStringList &thumbs);
};

class PlayerGameDetectorFake : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject *gamesModel READ gamesModel CONSTANT)
public:
  QString iconUrl;
  QObject *gamesModel() const { return nullptr; }
  Q_INVOKABLE QString iconSourceForGame(const QString &name) const {
    return name == "Game" ? iconUrl : QString{};
  }
};

class PlayerProbeTest : public ::testing::Test {
protected:
  PlayerWaveformFake waveform;
  PlayerLibraryFake library;
  PlayerFilmstripFake filmstrip;
  PlayerGameDetectorFake detector;
  QQmlEngine engine;
  QQuickWindow window;
  std::unique_ptr<QQuickItem> page;
  QStringList scriptErrors;

  void SetUp() override {
    QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                     [this](const QList<QQmlError> &errors) {
      for (const auto &error : errors) {
        if (error.description().contains("ReferenceError") ||
            error.description().contains("TypeError"))
          scriptErrors.append(error.toString());
      }
    });
    engine.rootContext()->setContextProperty("waveformCache", &waveform);
    engine.rootContext()->setContextProperty("recordingLibrary", &library);
    engine.rootContext()->setContextProperty("filmstripCache", &filmstrip);
    engine.rootContext()->setContextProperty("gameDetector", &detector);
    QQmlComponent component(&engine, QUrl::fromLocalFile(PLAYER_QML_FILE));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    QObject *created = component.create();
    ASSERT_NE(created, nullptr) << component.errorString().toStdString();
    page.reset(qobject_cast<QQuickItem *>(created));
    ASSERT_NE(page, nullptr);
    page->setParentItem(window.contentItem());
    page->setWidth(1024);
    page->setHeight(720);
  }
  void TearDown() override {
    page.reset();
    QCoreApplication::processEvents();
    EXPECT_TRUE(scriptErrors.isEmpty()) << scriptErrors.join('\n').toStdString();
    // scriptErrors is destroyed before engine; don't retain a lambda into
    // that member while the engine tears down its remaining components.
    QObject::disconnect(&engine, nullptr, &engine, nullptr);
  }
  void open(const QString &path) {
    ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "open",
        Q_ARG(QVariant, QVariant(path)), Q_ARG(QVariant, QVariant("Test game")),
        Q_ARG(QVariant, QVariant("2026-10-13"))));
  }
  void close() { ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "close")); }
  int requestId() { return page->property("audioProbeId").toInt(); }
  int count() { return page->property("trackCount").toInt(); }
  bool pending() { return page->property("audioProbePending").toBool(); }
  QQuickItem *findVisual(QQuickItem *item, const QString &name) {
    if (item->objectName() == name) return item;
    for (auto *child : item->childItems()) if (auto *found = findVisual(child, name)) return found;
    return nullptr;
  }
};

TEST_F(PlayerProbeTest, SpeedPopupAlignsTrackNotchesAndLabels) {
  open("/fake/game.mkv");
  window.resize(1024, 720);
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  for (const auto *name : {"playerSpeedButton", "playerSeekBackButton", "playerSeekForwardButton"}) {
    auto *button = findVisual(page.get(), name);
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->implicitWidth(), 36);
    EXPECT_EQ(button->implicitHeight(), 36);
  }
  auto *popup = page->findChild<QObject *>("playerSpeedPopup");
  ASSERT_NE(popup, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(popup, "open"));
  ASSERT_TRUE(QTest::qWaitFor([&] { return popup->property("opened").toBool(); }));
  auto *slider = page->findChild<QQuickItem *>("playerSpeedSlider");
  auto *content = qvariant_cast<QQuickItem *>(popup->property("contentItem"));
  ASSERT_NE(content, nullptr);
  ASSERT_NE(slider, nullptr);
  EXPECT_EQ(page->property("playbackRate").toDouble(), 1.0);
  for (int i = 0; i < 6; ++i) {
    auto *dot = findVisual(slider, "playerSpeedNotch" + QString::number(i));
    auto *label = findVisual(content, "playerSpeedLabel" + QString::number(i));
    ASSERT_NE(dot, nullptr);
    ASSERT_NE(label, nullptr);
    EXPECT_NEAR(dot->y() + dot->height() / 2, slider->height() / 2, 0.5);
    EXPECT_NEAR(dot->mapToScene(QPointF(dot->width() / 2, 0)).x(),
                label->mapToScene(QPointF(label->width() / 2, 0)).x(), 0.5);
  }
  ASSERT_TRUE(QMetaObject::invokeMethod(popup, "close"));
}

TEST_F(PlayerProbeTest, SnapshotChoosesDestinationAndCancelWritesNothing) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString source = dir.filePath("Game # 100%.mkv");
  open(source);
  window.resize(1024, 720);
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  auto *dialog = page->findChild<QObject *>("playerSnapshotDialog");
  auto *video = page->findChild<QObject *>("playerVideo");
  ASSERT_NE(dialog, nullptr);
  ASSERT_NE(video, nullptr);
  QSignalSpy saved(video, SIGNAL(snapshotSaved(QString,bool)));

  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "snapshotCurrent"));
  ASSERT_TRUE(QTest::qWaitFor([&] { return dialog->property("visible").toBool(); }));
  const auto suggested = dialog->property("selectedFile").toUrl();
  EXPECT_TRUE(suggested.toLocalFile().startsWith(dir.filePath("Game # 100%_snapshot_")));
  EXPECT_TRUE(suggested.toLocalFile().endsWith(".png"));
  EXPECT_FALSE(QFileInfo::exists(suggested.toLocalFile()));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "reject"));
  EXPECT_EQ(saved.count(), 0);
  EXPECT_TRUE(page->property("pendingSnapshot").isNull());
  EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).isEmpty());

  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "snapshotCurrent"));
  ASSERT_TRUE(QTest::qWaitFor([&] { return dialog->property("visible").toBool(); }));
  const QString destination = dir.filePath("Chosen # 50% café.png");
  ASSERT_TRUE(dialog->setProperty("selectedFile", QUrl::fromLocalFile(destination)));
  // Drive the acceptance handler with a chosen URL; the offscreen dialog's
  // filename editor otherwise overwrites programmatic selections on accept.
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "accepted"));
  ASSERT_EQ(saved.count(), 1);
  EXPECT_EQ(saved.at(0).at(0).toString(), destination);
  EXPECT_TRUE(saved.at(0).at(1).toBool());
  EXPECT_FALSE(QImage(destination).isNull());
  EXPECT_FALSE(QFileInfo::exists(suggested.toLocalFile()));
}

TEST_F(PlayerProbeTest, SnapshotCloseDiscardsFrameAndSaveFailureIsReported) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  open(dir.filePath("game.mkv"));
  window.resize(1024, 720);
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  auto *dialog = page->findChild<QObject *>("playerSnapshotDialog");
  auto *video = page->findChild<QObject *>("playerVideo");
  ASSERT_NE(dialog, nullptr);
  ASSERT_NE(video, nullptr);
  QSignalSpy saved(video, SIGNAL(snapshotSaved(QString,bool)));
  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "snapshotCurrent"));
  ASSERT_TRUE(QTest::qWaitFor([&] { return dialog->property("visible").toBool(); }));
  close();
  EXPECT_FALSE(dialog->property("visible").toBool());
  EXPECT_TRUE(page->property("pendingSnapshot").isNull());
  EXPECT_EQ(saved.count(), 0);

  open(dir.filePath("other.mkv"));
  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "snapshotCurrent"));
  ASSERT_TRUE(QTest::qWaitFor([&] { return dialog->property("visible").toBool(); }));
  const QString destination = dir.filePath("missing/image.png");
  ASSERT_TRUE(dialog->setProperty("selectedFile", QUrl::fromLocalFile(destination)));
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "accepted"));
  ASSERT_EQ(saved.count(), 1);
  EXPECT_EQ(saved.at(0).at(0).toString(), destination);
  EXPECT_FALSE(saved.at(0).at(1).toBool());
  EXPECT_FALSE(QFileInfo::exists(destination));

  // An already queued grab must not reopen the dialog after navigation.
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "snapshotCurrent"));
  close();
  QTest::qWait(50);
  EXPECT_FALSE(dialog->property("visible").toBool());
  EXPECT_TRUE(page->property("pendingSnapshot").isNull());
  EXPECT_EQ(saved.count(), 1);
}

TEST_F(PlayerProbeTest, HeaderIconSitsLeftOfTitleAndCollapsesWhenUnavailable) {
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  QImage image(40, 40, QImage::Format_RGB32);
  image.fill(Qt::green);
  const QString iconPath = dir.filePath("game.png");
  ASSERT_TRUE(image.save(iconPath));
  detector.iconUrl = QUrl::fromLocalFile(iconPath).toString();
  library.hasEntry = true;
  library.entry.insert("gameName", "Game");
  open("/fake/game.mkv");
  window.resize(1024, 720);
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  auto *icon = findVisual(page.get(), "playerGameIcon");
  auto *title = findVisual(page.get(), "playerTitleBlock");
  ASSERT_NE(icon, nullptr);
  ASSERT_NE(title, nullptr);
  ASSERT_TRUE(QTest::qWaitFor([&] { return icon->isVisible() && icon->width() == 40; }));
  // Image readiness can precede the RowLayout's next polish. In particular,
  // the title can still occupy its no-icon position when visibility flips.
  // Settle that layout before capturing the baseline for the collapse check.
  icon->parentItem()->ensurePolished();
  ASSERT_TRUE(QTest::qWaitFor([&] {
    return icon->x() > 0 && icon->x() + icon->width() <= title->x();
  })) << "Visible game icon was not laid out before the title";
  EXPECT_EQ(icon->height(), 40);
  EXPECT_LE(icon->x() + icon->width(), title->x());
  EXPECT_NEAR(icon->y() + icon->height() / 2, title->y() + title->height() / 2, 1);
  const qreal withIcon = title->x();

  library.entry.insert("gameName", "Missing icon");
  ASSERT_TRUE(QTest::qWaitFor([&] { return !icon->isVisible(); }))
      << "Game icon stayed visible after its source was removed";
  EXPECT_TRUE(icon->property("source").toUrl().isEmpty());
  ASSERT_TRUE(QTest::qWaitFor([&] { return title->x() < withIcon; }))
      << "Title did not reclaim the hidden icon's space: before=" << withIcon
      << ", after=" << title->x();
  close();
  library.hasEntry = false;
  open("/fake/unknown.mkv");
  EXPECT_FALSE(icon->isVisible());
}

TEST_F(PlayerProbeTest, SamePathReopenRejectsOldProbe) {
  open("/fake/same.mkv");
  const int oldId = requestId();
  ASSERT_TRUE(pending());
  close();
  open("/fake/same.mkv");
  ASSERT_NE(requestId(), oldId);
  emit waveform.audioTrackCountReady("/fake/same.mkv", oldId, 3);
  EXPECT_TRUE(pending());
  EXPECT_EQ(count(), -1);
  emit waveform.audioTrackCountReady("/fake/same.mkv", requestId(), 0);
  EXPECT_FALSE(pending());
  EXPECT_EQ(count(), 0);
  EXPECT_EQ(waveform.requests, 2);
}

TEST_F(PlayerProbeTest, BoundFilmstripDelegatesKeepIndicesAcrossRefreshZoomAndReopen) {
  window.show();
  const QString path = "/fake/filmstrip.mkv";
  for (int reopen = 0; reopen < 2; ++reopen) {
    open(path);
    page->setVisible(true); // main.qml reveals the page after open(), including reopen.
    auto *lane = findVisual(page.get(), "playerVideoLane");
    ASSERT_NE(lane, nullptr);
    lane->setProperty("playDuration", 60000);
    lane->setProperty("viewStartMs", 0);
    lane->setProperty("viewSpanMs", 60000);
    emit filmstrip.filmstripReady(path, {"/fake/one.jpg", "/fake/two.jpg", "/fake/three.jpg"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return findVisual(lane, "filmstripThumb2") != nullptr; }));
    for (int index = 0; index < 3; ++index) {
      auto *thumb = findVisual(lane, QString("filmstripThumb%1").arg(index));
      ASSERT_NE(thumb, nullptr);
      EXPECT_EQ(thumb->property("index").toInt(), index);
      EXPECT_EQ(thumb->property("thumbStart").toDouble(), index * 20000);
      EXPECT_EQ(thumb->property("thumbSpan").toDouble(), 20000);
    }
    lane->setProperty("viewStartMs", 30000);
    lane->setProperty("viewSpanMs", 30000);
    QCoreApplication::processEvents();
    EXPECT_FALSE(findVisual(lane, "filmstripThumb0")->isVisible());
    EXPECT_TRUE(QTest::qWaitFor([&] { return findVisual(lane, "filmstripThumb2")->isVisible(); }));
    emit filmstrip.filmstripReady(path, {"/fake/replacement.jpg"});
    ASSERT_TRUE(QTest::qWaitFor([&] { return findVisual(lane, "filmstripThumb1") == nullptr; }));
    auto *replacement = findVisual(lane, "filmstripThumb0");
    ASSERT_NE(replacement, nullptr);
    EXPECT_EQ(replacement->property("modelData").toString(), "/fake/replacement.jpg");
    EXPECT_EQ(replacement->property("thumbSpan").toDouble(), 60000);
    close();
  }
}

TEST_F(PlayerProbeTest, SeekAfterEndResumesButPausedSeekStaysPaused) {
  const QString ffmpeg = QStandardPaths::findExecutable("ffmpeg");
  if (ffmpeg.isEmpty()) GTEST_SKIP() << "ffmpeg fixture tool unavailable";
  QTemporaryDir dir;
  ASSERT_TRUE(dir.isValid());
  const QString path = dir.filePath("end.mkv");
  QProcess generate;
  generate.start(ffmpeg, {"-v", "error", "-f", "lavfi", "-i",
      "testsrc2=size=160x90:rate=15", "-t", "1", "-c:v", "mpeg4", path});
  ASSERT_TRUE(generate.waitForFinished(15000));
  ASSERT_EQ(generate.exitCode(), 0) << generate.readAllStandardError().toStdString();
  library.realMedia = true;
  window.resize(1024, 720);
  window.show();
  ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
  open(path);
  emit waveform.audioTrackCountReady(path, requestId(), 0);
  auto *video = page->findChild<QObject *>("playerVideo");
  ASSERT_NE(video, nullptr);
  auto waitFor = [](auto predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 10000) QTest::qWait(20);
    return predicate();
  };
  ASSERT_TRUE(waitFor([&] { return video->property("atEnd").toBool(); }));
  auto *output = video->findChild<QObject *>("playerVideoOutput");
  ASSERT_NE(output, nullptr);
  EXPECT_EQ(output->property("endOfStreamPolicy").toInt(), 1);
  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "seekAll", Q_ARG(QVariant, QVariant(200))));
  ASSERT_TRUE(waitFor([&] { return video->property("playing").toBool(); }));
  EXPECT_FALSE(video->property("atEnd").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "pauseAll"));
  ASSERT_TRUE(waitFor([&] { return !video->property("playing").toBool(); }));
  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "seekAll", Q_ARG(QVariant, QVariant(100))));
  QTest::qWait(150);
  EXPECT_FALSE(video->property("playing").toBool());
  close();
}

TEST_F(PlayerProbeTest, BookmarkListReactsAndRangeClampsAtFileEdges) {
  library.entry.insert("bookmarks", QVariantList{QVariantMap{{"positionMs", 12000}}});
  library.entry.insert("protectedFromCleanup", true);
  library.entry.insert("fileName", "Game.mkv");
  library.entry.insert("gameName", "Game");
  library.entry.insert("duration", 60);
  for (const auto &key : {"encoderName", "directory", "videoCodec", "audioCodec", "trophyVersion"})
    library.entry.insert(key, QString());
  library.hasEntry = true;
  open("/fake/bookmarked.mkv");
  QCoreApplication::processEvents();
  const auto list = page->property("bookmarks");
  const auto values = list.canConvert<QJSValue>() ? list.value<QJSValue>().toVariant().toList() : list.toList();
  ASSERT_EQ(values.size(), 1);
  auto *button = page->findChild<QObject *>("playerBookmarksButton");
  ASSERT_NE(button, nullptr);
  EXPECT_TRUE(button->property("enabled").toBool());
  EXPECT_EQ(page->findChild<QObject *>("playerProtectionButton")->property("checked").toBool(), true);
  window.resize(1024, 720);
  window.show();
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
  QCoreApplication::processEvents();
  auto *dialog = page->findChild<QObject *>("playerBookmarksDialog");
  ASSERT_NE(dialog, nullptr);
  EXPECT_GT(dialog->property("implicitHeight").toDouble(), 100);
  auto *content = dialog->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(content, nullptr);
  auto *row = findVisual(content, "bookmarkMomentRow");
  ASSERT_NE(row, nullptr);
  EXPECT_GT(row->width(), 450);
  EXPECT_GT(row->height(), 55);
  auto *icon = findVisual(row, "bookmarkMomentIcon");
  ASSERT_NE(icon, nullptr);
  EXPECT_EQ(QQmlProperty::read(icon, "icon.color").value<QColor>(), QColor("#FFD54F"));
  auto *header = dialog->property("header").value<QObject *>();
  ASSERT_NE(header, nullptr);
  EXPECT_EQ(header->findChild<QObject *>("bookmarkMomentCount")->property("text").toString(), "1 moment");
  auto *remove = findVisual(row, "bookmarkRemoveButton");
  ASSERT_NE(remove, nullptr);
  remove->forceActiveFocus();
  EXPECT_EQ(remove->opacity(), 1);
  // Real row pointer activation closes the dialog (no separate Jump button).
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      row->mapToScene(QPointF(70, row->height() / 2)).toPoint());
  EXPECT_FALSE(dialog->property("visible").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "close"));
  library.entry.insert("bookmarks", QVariantList{});
  QCoreApplication::processEvents();
  EXPECT_FALSE(button->property("enabled").toBool());
  for (const auto &[ms, expectedStart, expectedEnd] :
       {std::tuple{1000, 0, 16000}, std::tuple{59000, 44000, 60000},
        std::tuple{90000, 45000, 60000}}) {
    QVariant returned;
    ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "bookmarkClipRange",
        Q_RETURN_ARG(QVariant, returned), Q_ARG(QVariant, QVariant(ms)),
        Q_ARG(QVariant, QVariant(60000))));
    const auto range = returned.canConvert<QJSValue>() ? returned.value<QJSValue>().toVariant().toList() : returned.toList();
    ASSERT_EQ(range.size(), 2);
    EXPECT_EQ(range[0].toInt(), expectedStart);
    EXPECT_EQ(range[1].toInt(), expectedEnd);
  }
  close();
}

TEST_F(PlayerProbeTest, TimelineAddsBookmarkAndRefreshesMarkers) {
  library.entry.insert("bookmarks", QVariantList{});
  library.entry.insert("fileName", "Game.mkv");
  library.entry.insert("gameName", "Game");
  library.entry.insert("duration", 60);
  library.hasEntry = true;
  window.resize(1024, 720); window.show();
  open("/fake/add-bookmark.mkv");
  auto *add = page->findChild<QQuickItem *>("playerAddBookmarkButton");
  ASSERT_NE(add, nullptr);
  EXPECT_FALSE(add->property("enabled").toBool()); // No decoded timeline yet.
  // This suite deliberately does not decode media; enable the control to
  // exercise a real pointer click at the initial playhead (zero).
  add->setProperty("enabled", true);
  ASSERT_TRUE(QTest::qWaitFor([&] { return add->isVisible() && add->width() > 0; }));
  QTest::qWait(50); // Let the newly opened tracks layout polish before hit testing.
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      add->mapToScene(QPointF(add->width() / 2, add->height() / 2)).toPoint());
  EXPECT_EQ(library.bookmarkedPath, "/fake/add-bookmark.mkv");
  EXPECT_EQ(library.bookmarkedPosition, 0);
  auto *list = page->findChild<QObject *>("playerBookmarksButton");
  ASSERT_NE(list, nullptr);
  EXPECT_TRUE(list->property("enabled").toBool());
  auto *lane = page->findChild<QQuickItem *>("playerVideoLane");
  ASSERT_NE(lane, nullptr);
  EXPECT_NE(findVisual(lane, "timelineBookmark"), nullptr);
  close();
}

TEST_F(PlayerProbeTest, MomentsSortChronologicallyAndDeleteOriginalEntry) {
  library.entry.insert("bookmarks", QVariantList{
      QVariantMap{{"positionMs", 2000}}, QVariantMap{{"positionMs", 10000}},
      QVariantMap{{"positionMs", 5000}}});
  library.entry.insert("fileName", "Game.mkv");
  library.entry.insert("gameName", "Game");
  library.entry.insert("duration", 60);
  library.hasEntry = true;
  window.resize(1024, 720); window.show();
  open("/fake/sorted.mkv");
  auto *button = page->findChild<QObject *>("playerBookmarksButton");
  ASSERT_NE(button, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
  QTest::qWait(50);
  auto *dialog = page->findChild<QObject *>("playerBookmarksDialog");
  ASSERT_NE(dialog, nullptr);
  auto *content = dialog->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(content, nullptr);
  auto *first = findVisual(content, "bookmarkMomentRow");
  ASSERT_NE(first, nullptr);
  QList<QQuickItem *> rows;
  for (auto *child : first->parentItem()->childItems())
    if (child->objectName() == "bookmarkMomentRow") rows.append(child);
  std::sort(rows.begin(), rows.end(), [](auto *a, auto *b) { return a->y() < b->y(); });
  ASSERT_EQ(rows.size(), 3);
  const QList<int> expected{2000, 5000, 10000};
  for (int i = 0; i < rows.size(); ++i) {
    const auto value = rows[i]->property("modelData");
    const auto data = value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
    EXPECT_EQ(data.value("positionMs").toInt(), expected[i]);
    auto *remove = findVisual(rows[i], "bookmarkRemoveButton");
    ASSERT_NE(remove, nullptr);
    EXPECT_TRUE(remove->isVisible());
    EXPECT_EQ(remove->opacity(), 1);
  }
  auto *removeMiddle = findVisual(rows[1], "bookmarkRemoveButton");
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      removeMiddle->mapToScene(QPointF(removeMiddle->width() / 2, removeMiddle->height() / 2)).toPoint());
  EXPECT_EQ(library.removedIndex, 2);
  const auto remaining = library.entry.value("bookmarks").toList();
  ASSERT_EQ(remaining.size(), 2);
  EXPECT_EQ(remaining[0].toMap().value("positionMs").toInt(), 2000);
  EXPECT_EQ(remaining[1].toMap().value("positionMs").toInt(), 10000);
  EXPECT_TRUE(dialog->property("visible").toBool());
  close();
}

TEST_F(PlayerProbeTest, ClosedPageIgnoresLateResult) {
  open("/fake/closed.mkv");
  const int oldId = requestId();
  close();
  emit waveform.audioTrackCountReady("/fake/closed.mkv", oldId, 3);
  EXPECT_EQ(count(), 0);
  EXPECT_FALSE(pending());
  EXPECT_TRUE(page->property("filePath").toString().isEmpty());
}

TEST_F(PlayerProbeTest, YellowTimelineBookmarksClickExactTimeAndFollowZoom) {
  library.entry.insert("bookmarks", QVariantList{QVariantMap{{"positionMs", 12000}}});
  library.entry.insert("fileName", "Game.mkv");
  library.entry.insert("gameName", "Game");
  library.entry.insert("duration", 60);
  for (const auto &key : {"encoderName", "directory", "videoCodec", "audioCodec", "trophyVersion"})
    library.entry.insert(key, QString());
  library.hasEntry = true;
  window.resize(1024, 720); window.show();
  open("/fake/markers.mkv");
  emit waveform.audioTrackCountReady("/fake/markers.mkv", requestId(), 0);
  auto *lane = page->findChild<QQuickItem *>("playerVideoLane");
  ASSERT_NE(lane, nullptr);
  // No media decoding in this suite; control the lane's file-clock window.
  lane->setProperty("viewSpanMs", 60000);
  lane->setProperty("playDuration", 60000);
  QCoreApplication::processEvents();
  auto *marker = findVisual(lane, "timelineBookmark");
  ASSERT_NE(marker, nullptr);
  ASSERT_TRUE(QTest::qWaitFor([&] { return marker->isVisible() && marker->height() > 0; }));
  EXPECT_EQ(QQmlProperty::read(marker, "icon.color").value<QColor>(), QColor("#FFD54F"));
  QSignalSpy jumps(lane, SIGNAL(bookmarkActivated(double)));
  QSignalSpy scrubs(lane, SIGNAL(seekRequested(double)));
  ASSERT_TRUE(jumps.isValid()); ASSERT_TRUE(scrubs.isValid());
  auto click = [&](QQuickItem *target) {
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
        target->mapToScene(QPointF(target->width() / 2, target->height() - 3)).toPoint());
    QCoreApplication::processEvents();
  };
  click(marker);
  ASSERT_EQ(jumps.count(), 1);
  EXPECT_EQ(jumps.last()[0].toDouble(), 12000);
  EXPECT_TRUE(scrubs.isEmpty()); // The scrub area must not steal marker clicks.
  lane->setProperty("viewStartMs", 20000);
  EXPECT_FALSE(marker->isVisible());
  lane->setProperty("viewStartMs", 10000);
  lane->setProperty("viewSpanMs", 30000);
  QCoreApplication::processEvents();
  EXPECT_TRUE(marker->isVisible());
  EXPECT_NEAR(marker->x() + marker->width() / 2, 2000.0 / 30000 * marker->parentItem()->width(), 0.5);
  click(marker);
  ASSERT_EQ(jumps.count(), 2);
  EXPECT_EQ(jumps.last()[0].toDouble(), 12000);
  auto *area = marker->parentItem();
  lane->setProperty("viewStartMs", 0); lane->setProperty("viewSpanMs", 60000);
  library.entry.insert("bookmarks", QVariantList{QVariantMap{{"positionMs", 0}}, QVariantMap{{"positionMs", 60000}}});
  QCoreApplication::processEvents();
  int markers = 0;
  for (auto *child : area->childItems()) {
    if (child->objectName() != "timelineBookmark") continue;
    ++markers;
    EXPECT_TRUE(child->isVisible());
    EXPECT_GE(child->x(), 0);
    EXPECT_LE(child->x() + child->width(), area->width() + 0.1);
  }
  EXPECT_EQ(markers, 2);
  close();
}

TEST_F(PlayerProbeTest, WrongPathIgnoredAndFailureKeepsZeroAudioLanes) {
  open("/fake/current.mkv");
  emit waveform.audioTrackCountReady("/fake/other.mkv", requestId(), 3);
  EXPECT_TRUE(pending());
  emit waveform.audioTrackCountReady("/fake/current.mkv", requestId(), -1);
  EXPECT_FALSE(pending());
  EXPECT_EQ(count(), -1);
  EXPECT_EQ(page->property("audioLanes").toInt(), 0);
}

TEST_F(PlayerProbeTest, BookmarkLinesAlignAndJumpAcrossAllAudioLanes) {
  library.entry.insert("bookmarks", QVariantList{QVariantMap{{"positionMs", 12000}}});
  library.entry.insert("fileName", "Game.mkv"); library.entry.insert("gameName", "Game");
  library.entry.insert("duration", 60);
  for (const auto &key : {"encoderName", "directory", "videoCodec", "audioCodec", "trophyVersion"}) library.entry.insert(key, QString());
  library.hasEntry = true;
  window.resize(1024, 720); window.show();
  open("/fake/all-tracks.mkv");
  emit waveform.audioTrackCountReady("/fake/all-tracks.mkv", requestId(), 3);
  for (int track = 0; track < 3; ++track) emit waveform.peaksReady("/fake/all-tracks.mkv", track, {});
  QList<QQuickItem *> lanes;
  lanes.append(findVisual(page.get(), "playerVideoLane"));
  for (int track = 0; track < 3; ++track) lanes.append(findVisual(page.get(), "playerAudioLane" + QString::number(track)));
  for (auto *lane : lanes) {
    ASSERT_NE(lane, nullptr);
    lane->setProperty("viewSpanMs", 60000); lane->setProperty("playDuration", 60000);
  }
  QCoreApplication::processEvents();
  double markerX = -1;
  for (auto *lane : lanes) {
    auto *marker = findVisual(lane, "timelineBookmark");
    ASSERT_NE(marker, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return marker->isVisible() && marker->height() > 0; }));
    const double x = marker->mapToScene(QPointF(marker->width() / 2, 0)).x();
    if (markerX < 0) markerX = x;
    EXPECT_NEAR(x, markerX, 0.5);
    EXPECT_EQ(QQmlProperty::read(marker, "icon.color").value<QColor>(), QColor("#FFD54F"));
    QSignalSpy jumps(lane, SIGNAL(bookmarkActivated(double)));
    QSignalSpy scrubs(lane, SIGNAL(seekRequested(double)));
    ASSERT_TRUE(jumps.isValid()); ASSERT_TRUE(scrubs.isValid());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
        marker->mapToScene(QPointF(marker->width() / 2, marker->height() - 3)).toPoint());
    ASSERT_EQ(jumps.count(), 1); EXPECT_EQ(jumps[0][0].toDouble(), 12000);
    EXPECT_TRUE(scrubs.isEmpty());
    lane->setProperty("viewStartMs", 20000); EXPECT_FALSE(marker->isVisible());
    lane->setProperty("viewStartMs", 10000); lane->setProperty("viewSpanMs", 30000);
    EXPECT_TRUE(marker->isVisible());
  }
  close();
}

TEST_F(PlayerProbeTest, SystemOnlyHistoryHidesVoiceWithoutRenumberingMic) {
  library.entry.insert("bookmarks", QVariantList{});
  library.entry.insert("fileName", "System.mkv"); library.entry.insert("gameName", "Game");
  library.entry.insert("duration", 60);
  for (const auto &key : {"encoderName", "directory", "videoCodec", "audioCodec", "trophyVersion"}) library.entry.insert(key, QString());
  QVariantMap routing{{"v", 1}, {"system", true}, {"game", false}, {"voice", false},
      {"separate", true}, {"primary", 0}, {"voiceTrack", 1}, {"micTrack", 2}};
  library.entry.insert("audioRouting", routing);
  library.hasEntry = true;
  open("/fake/system.mkv");
  emit waveform.audioTrackCountReady("/fake/system.mkv", requestId(), 3);
  QCoreApplication::processEvents();
  EXPECT_EQ(page->property("hiddenVoiceTrack").toInt(), 1);
  EXPECT_EQ(findVisual(page.get(), "playerAudioLane1"), nullptr);
  auto *system = findVisual(page.get(), "playerAudioLane0");
  auto *mic = findVisual(page.get(), "playerAudioLane2");
  ASSERT_NE(system, nullptr); ASSERT_NE(mic, nullptr);
  EXPECT_EQ(system->property("headerText").toString(), "System");
  EXPECT_EQ(mic->property("trackIndex").toInt(), 2);
  EXPECT_EQ(mic->property("headerText").toString(), "Mic");
  auto *pipelines = page->findChild<QObject *>("playerAudioPipelines");
  ASSERT_NE(pipelines, nullptr);
  EXPECT_EQ(pipelines->property("count").toInt(), 2);
  QObject *pipeline = nullptr;
  ASSERT_TRUE(QMetaObject::invokeMethod(pipelines, "objectAt", Q_RETURN_ARG(QObject *, pipeline), Q_ARG(int, 1)));
  ASSERT_NE(pipeline, nullptr); EXPECT_EQ(pipeline->property("modelData").toInt(), 2);
  page->setProperty("mutedTracks", QVariantList{2});
  QCoreApplication::processEvents();
  auto *output = pipeline->property("audioOutput").value<QObject *>();
  ASSERT_NE(output, nullptr); EXPECT_TRUE(output->property("muted").toBool());
  routing["game"] = true; routing["voice"] = true;
  library.entry.insert("audioRouting", routing);
  QCoreApplication::processEvents();
  auto *voice = findVisual(page.get(), "playerAudioLane1");
  ASSERT_NE(voice, nullptr);
  EXPECT_EQ(voice->property("headerText").toString(), "Voice");
  EXPECT_EQ(findVisual(page.get(), "playerAudioLane0")->property("headerText").toString(), "Game / System");
  EXPECT_EQ(pipelines->property("count").toInt(), 3);
  close();
}

TEST_F(PlayerProbeTest, DiscoveredTracksGrowCardFloorAndKeepLargerHeight) {
  open("/fake/tracks.mkv");
  const double before = page->property("minTracksHeight").toDouble();
  emit waveform.audioTrackCountReady("/fake/tracks.mkv", requestId(), 3);
  QCoreApplication::processEvents();
  EXPECT_EQ(page->property("audioLanes").toInt(), 3);
  const double floor = page->property("minTracksHeight").toDouble();
  EXPECT_GT(floor, before);
  EXPECT_GE(page->property("tracksHeight").toDouble(), floor);

  page->setProperty("tracksHeight", floor + 50);
  emit waveform.audioTrackCountReady("/fake/tracks.mkv", requestId(), 3);
  EXPECT_DOUBLE_EQ(page->property("tracksHeight").toDouble(), floor + 50);
  // End with the player closed like every sibling: tearing down live
  // pipelines with the page (instead of stopping them first) crashes
  // on machines without a working audio backend.
  close();
  EXPECT_TRUE(page->property("filePath").toString().isEmpty());
  EXPECT_EQ(count(), 0);
}

TEST(AudioLevelMeterTest, PollingDoesNotEmitMuteIntentButUserToggleDoes) {
  QQmlEngine engine;
  QQmlComponent component(&engine, QUrl::fromLocalFile(AUDIO_METER_QML_FILE));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  std::unique_ptr<QObject> meter(component.create());
  ASSERT_NE(meter, nullptr) << component.errorString().toStdString();
  QSignalSpy intent(meter.get(), SIGNAL(muteToggled(bool)));
  ASSERT_TRUE(intent.isValid());
  meter->setProperty("muted", true);
  meter->setProperty("muted", false);
  EXPECT_EQ(intent.count(), 0);
  ASSERT_TRUE(QMetaObject::invokeMethod(meter.get(), "toggleMute"));
  EXPECT_EQ(intent.count(), 1);
  EXPECT_TRUE(intent.at(0).first().toBool());
  meter->setProperty("muted", false);
  EXPECT_EQ(intent.count(), 1);
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  // This probe never decodes media. Prevent Qt Multimedia's backend discovery
  // from initializing host CUDA/VA-API drivers merely by creating MediaPlayer.
  // Qt documents ',' as an empty device list (an empty value uses defaults).
  qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
  qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", ",");
  QGuiApplication app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "test_PlayerProbe.moc"
