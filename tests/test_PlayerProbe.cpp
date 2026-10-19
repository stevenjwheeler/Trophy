/* Real PlayerPage engine load with controlled probe delivery, no media I/O. */
#include <QGuiApplication>
#include <QColor>
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
#include <gtest/gtest.h>
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
  PlayerLibraryFake() {
    // Returned QObject pointers default to JS ownership; this is a member,
    // not a heap object the engine may collect.
    QQmlEngine::setObjectOwnership(&entry, QQmlEngine::CppOwnership);
  }
  bool clipSaving() const { return false; }
  // Keep pipelines idle: this suite tests the page's probe lifecycle, not
  // decoding, and never passes the fake file paths to FFmpeg.
  Q_INVOKABLE QUrl localFileUrl(const QString &) const { return {}; }
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

class PlayerProbeTest : public ::testing::Test {
protected:
  PlayerWaveformFake waveform;
  PlayerLibraryFake library;
  PlayerFilmstripFake filmstrip;
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
