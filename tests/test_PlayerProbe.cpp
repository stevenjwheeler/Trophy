/* Real PlayerPage engine load with controlled probe delivery, no media I/O. */
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#include <QSignalSpy>
#include <gtest/gtest.h>
#include <memory>

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
  bool clipSaving() const { return false; }
  // Keep pipelines idle: this suite tests the page's probe lifecycle, not
  // decoding, and never passes the fake file paths to FFmpeg.
  Q_INVOKABLE QUrl localFileUrl(const QString &) const { return {}; }
  // The file-info dialog looks the open path up in the library; unknown
  // here, so its rows fall back to path-derived text.
  Q_INVOKABLE QObject *getRecording(const QString &) const { return nullptr; }
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

TEST_F(PlayerProbeTest, ClosedPageIgnoresLateResult) {
  open("/fake/closed.mkv");
  const int oldId = requestId();
  close();
  emit waveform.audioTrackCountReady("/fake/closed.mkv", oldId, 3);
  EXPECT_EQ(count(), 0);
  EXPECT_FALSE(pending());
  EXPECT_TRUE(page->property("filePath").toString().isEmpty());
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
