/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "troubleshooters/DmaBufTroubleshooter.h"
#include "encoding/EncoderFactory.h"
#include "settings/SettingsManager.h"
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
extern "C" {
#include <libavutil/pixfmt.h>
}
#if defined(HAVE_GBM) && defined(HAVE_LIBDRM)
#include <gbm.h>
#include <libdrm/drm_fourcc.h>
#endif

using namespace TrophyBackend;

class TroubleshooterUiApp : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool trayAvailable READ no CONSTANT)
  Q_PROPERTY(bool runWithoutTraySupported READ no CONSTANT)
  Q_PROPERTY(bool canRestartTrophy READ no CONSTANT)
  Q_PROPERTY(bool useNativeDialogs READ no CONSTANT)
public:
  bool no() const { return false; }
  Q_INVOKABLE bool isEncoderAvailable(const QString &) const { return true; }
};

namespace {
DmaBufTestEvidence working() {
  DmaBufTestEvidence e;
  e.dmaFrames = e.singlePlane = e.egl = e.accepted = e.decoded = 30;
  e.encoder = "test hardware";
  return e;
}
QString status(const DmaBufTestEvidence &e) { return e.report()["status"].toString(); }

class FakeCapture : public IVideoCaptureSource {
public:
  CaptureConfig config;
  FrameCallback callback;
  std::atomic<bool> prepared{false}, finished{false};
  std::atomic<bool> finishEntered{false};
  std::shared_future<void> finishGate;
  bool initialized = false, started = false;
  bool initialize(const CaptureConfig &value) override {
    config = value; initialized = true; return true;
  }
  bool startCapture() override { started = true; emit captureStarted(); return true; }
  void setFrameCallback(FrameCallback value) override { callback = std::move(value); }
  void stopCapture() override { prepareStopCapture(); finishStopCapture(); }
  void prepareStopCapture() override { prepared = true; }
  void finishStopCapture() override {
    EXPECT_TRUE(prepared);
    EXPECT_FALSE(bool(callback));
    finishEntered = true;
    if (finishGate.valid()) finishGate.wait();
    finished = true;
  }
  bool isCapturing() const override { return started && !prepared; }
  CaptureCapabilities capabilities() const override { return {}; }
  QSize nativeResolution() const override { return {320, 240}; }
  QString name() const override { return "fake"; }
};
}

TEST(DmaBufEvidenceTest, RequiresDecodedFramesAndActualGpuImport) {
  EXPECT_EQ(status({}), "inconclusive");
  auto e = working();
  EXPECT_EQ(status(e), "passed");
  e.egl = 0;
  EXPECT_EQ(status(e), "inconclusive");
  e.egl = 30;
  e.singlePlane = e.egl = 0;
  EXPECT_EQ(status(e), "inconclusive");
}
TEST(DmaBufEvidenceTest, MultiPlaneAndDirectMappingCanPassIndependently) {
  auto e = working();
  e.multiPlane = e.singlePlane; e.singlePlane = 0;
  e.vpp = e.egl; e.egl = 0;
  EXPECT_EQ(status(e), "passed");
  e.singlePlane = e.multiPlane; e.multiPlane = 0;
  e.direct = e.vpp; e.vpp = 0;
  EXPECT_EQ(status(e), "passed");
}
TEST(DmaBufEvidenceTest, FallbackNeverPassesEvenAfterSomeGpuFrames) {
  auto e = working();
  e.shmFrames = 1;
  EXPECT_EQ(status(e), "fallback");
  e.shmFrames = 0; e.fallback = 1;
  EXPECT_EQ(status(e), "fallback");
}
TEST(DmaBufEvidenceTest, MissingDecodedPictureOrDroppedFrameFails) {
  auto e = working();
  --e.decoded;
  EXPECT_EQ(status(e), "failed");
  e = working(); e.dropped = 1;
  EXPECT_EQ(status(e), "failed");
}
TEST(DmaBufEvidenceTest, ErrorsOverrideSuccessAndFewFramesStayInconclusive) {
  auto e = working(); e.error = "native error";
  EXPECT_EQ(status(e), "failed");
  EXPECT_EQ(e.report()["summary"].toString(), "native error");
  e = working(); e.accepted = e.decoded = e.singlePlane = 2;
  EXPECT_EQ(status(e), "inconclusive");
}

namespace TrophyBackend {
class DmaBufTroubleshooterTest : public ::testing::Test {
protected:
  DmaBufTroubleshooter tester;
  FakeCapture *capture = nullptr;
  void SetUp() override {
    tester.m_captureFactory = [this] {
      auto source = std::make_unique<FakeCapture>();
      capture = source.get();
      return source;
    };
    // The real worker is used. With no DMA-BUF input it never opens a device.
  }
  void settle() {
    for (int i = 0; tester.busy() && i < 200; ++i) QTest::qWait(10);
    ASSERT_FALSE(tester.busy());
  }
  void expire() { tester.m_deadline.stop(); tester.stop(); }
  bool queueEmpty() {
    std::lock_guard lock(tester.m_run->mutex);
    return tester.m_run->frames.empty();
  }
  quint64 generation() const { return tester.m_generation; }
  void postCompletion(quint64 old) {
    QMetaObject::invokeMethod(&tester, [this, old] {
      if (tester.m_generation == old) tester.stop();
    }, Qt::QueuedConnection);
  }
  void holdProbe() {
    tester.m_probe = [](DmaBufTroubleshooter::Run &run, const std::function<void()> &) {
      std::unique_lock lock(run.mutex);
      run.wake.wait(lock, [&] { return run.stopping; });
    };
  }
  DmaBufLayout queuedLayout() {
    std::lock_guard lock(tester.m_run->mutex);
    return tester.m_run->frames.empty() ? DmaBufLayout{} :
        tester.m_run->frames.front().dmaBufLayout();
  }
  DmaBufTestEvidence probeFrames(const VideoFrame &frame) {
    DmaBufTroubleshooter::Run run;
    for (int i = 0; i < 90; ++i) run.frames.push_back(frame);
    bool complete = false;
    DmaBufTroubleshooter::probe(run, [&] { complete = true; });
    EXPECT_TRUE(complete);
    return run.evidence;
  }
  bool hardwareAvailable() {
    return EncoderFactory::isAvailable(IEncoder::EncoderType::NVENC_H264) ||
        EncoderFactory::isAvailable(IEncoder::EncoderType::VAAPI_H264);
  }
};

TEST_F(DmaBufTroubleshooterTest, FreshEphemeralGrantAndNoSettingsChanges) {
  tester.start();
  ASSERT_TRUE(tester.busy());
  ASSERT_NE(capture, nullptr);
  EXPECT_TRUE(capture->config.useDmaBuf);
  EXPECT_FALSE(capture->config.persistPermission);
  EXPECT_TRUE(capture->config.waylandRestoreToken.isEmpty());
  EXPECT_FALSE(capture->config.captureMouseCursor);
  EXPECT_EQ(capture->config.targetFps, 30);
  tester.cancel(); settle();
  EXPECT_EQ(tester.result()["status"].toString(), "cancelled");
}
TEST_F(DmaBufTroubleshooterTest, BusyStartDoesNotReplaceAndUnavailableDoesNotStart) {
  tester.setAvailable(false);
  tester.start();
  EXPECT_FALSE(tester.busy());
  tester.setAvailable(true);
  tester.start();
  auto *original = capture;
  tester.start();
  EXPECT_EQ(capture, original);
  tester.setAvailable(false);
  settle();
  EXPECT_EQ(tester.result()["status"].toString(), "cancelled");
}
TEST_F(DmaBufTroubleshooterTest, ShmIsCountedWithoutRetainingBorrowedPixels) {
  tester.start();
  VideoFrame frame{};
  frame.data = reinterpret_cast<const uint8_t *>(uintptr_t(1)); // Must not dereference.
  capture->callback(frame);
  EXPECT_TRUE(queueEmpty());
  expire(); settle();
  EXPECT_EQ(tester.result()["shmFrames"].toInt(), 1);
  EXPECT_EQ(tester.result()["status"].toString(), "fallback");
}
TEST_F(DmaBufTroubleshooterTest, PortalFailureRetiresAndRetryWorks) {
  tester.start();
  emit capture->captureError("portal refused");
  settle();
  EXPECT_EQ(tester.result()["status"].toString(), "failed");
  EXPECT_EQ(tester.result()["summary"].toString(), "portal refused");
  tester.start();
  EXPECT_TRUE(tester.busy());
  tester.cancel(); settle();
}
TEST_F(DmaBufTroubleshooterTest, OldWorkerCompletionCannotStopRetry) {
  tester.start();
  const auto old = generation();
  tester.cancel(); settle();
  tester.start();
  postCompletion(old);
  QCoreApplication::processEvents();
  EXPECT_TRUE(capture->isCapturing());
  tester.cancel(); settle();
}
TEST_F(DmaBufTroubleshooterTest, DestructionSettlesOwnedWorker) {
  tester.start();
  // Fixture destruction exercises synchronous final lifetime barrier.
}
TEST_F(DmaBufTroubleshooterTest, KeepsSourceAndBusyStateUntilNativeTeardownSettles) {
  tester.start();
  std::promise<void> release;
  capture->finishGate = release.get_future().share();
  tester.cancel();
  for (int i = 0; !capture->finishEntered && i < 100; ++i) QTest::qWait(5);
  EXPECT_TRUE(capture->finishEntered);
  EXPECT_TRUE(tester.busy());
  auto *original = capture;
  tester.start();
  EXPECT_EQ(capture, original);
  release.set_value();
  settle();
}
TEST_F(DmaBufTroubleshooterTest, MultiPlaneQueueIsBoundedAndReleasesEveryHandleAndLease) {
  holdProbe();
  tester.start();
  QTemporaryFile file;
  ASSERT_TRUE(file.open());
  ASSERT_TRUE(file.resize(65536));
  VideoFrame frame{};
  frame.isDmaBuf = true;
  frame.width = frame.height = 16;
  frame.dmaBufPlanes.count = 2;
  frame.dmaBufPlanes.planes[0] = {file.handle(), 64, 0, 65536};
  frame.dmaBufPlanes.planes[1] = {file.handle(), 64, 32768, 65536};
  auto lease = std::make_shared<int>(1);
  std::weak_ptr<int> weak = lease;
  frame.bufferLease = lease;
  for (int i = 0; i < 50; ++i) capture->callback(frame);
  const auto retained = queuedLayout();
  EXPECT_EQ(retained.count, 2u);
  EXPECT_NE(retained.planes[0].fd, file.handle());
  EXPECT_NE(retained.planes[0].fd, retained.planes[1].fd);
  frame.bufferLease.reset();
  lease.reset();
  EXPECT_FALSE(weak.expired());
  tester.cancel(); settle();
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(fcntl(retained.planes[0].fd, F_GETFD), -1);
  EXPECT_EQ(fcntl(retained.planes[1].fd, F_GETFD), -1);
}
TEST_F(DmaBufTroubleshooterTest, NativeProbeDecodesUploadedFramesWithoutClaimingDmaBufSuccess) {
  if (!hardwareAvailable()) GTEST_SKIP() << "No native H.264 hardware encoder";
  auto pixels = std::make_shared<std::vector<uint8_t>>(320 * 240 * 4, 96);
  VideoFrame frame{};
  frame.width = 320; frame.height = 240; frame.stride = 320 * 4;
  frame.pixelFormat = AV_PIX_FMT_BGR0;
  frame.cpuStorage = pixels;
  frame.data = pixels->data();
  const auto e = probeFrames(frame);
  EXPECT_TRUE(e.error.isEmpty()) << e.error.toStdString();
  EXPECT_EQ(e.accepted, 90);
  EXPECT_EQ(e.decoded, e.accepted);
  EXPECT_EQ(e.fallback, 90);
  EXPECT_EQ(status(e), "fallback");
}
#if defined(HAVE_GBM) && defined(HAVE_LIBDRM)
TEST_F(DmaBufTroubleshooterTest, NativeProbeVerifiesRealSinglePlaneDmaBuf) {
  if (!hardwareAvailable()) GTEST_SKIP() << "No native H.264 hardware encoder";
  QFile node("/dev/dri/renderD128");
  if (!node.open(QIODevice::ReadWrite)) GTEST_SKIP() << "No render node";
  std::unique_ptr<gbm_device, decltype(&gbm_device_destroy)>
      device(gbm_create_device(node.handle()), gbm_device_destroy);
  if (!device) GTEST_SKIP() << "GBM unavailable";
  std::unique_ptr<gbm_bo, decltype(&gbm_bo_destroy)> bo(
      gbm_bo_create(device.get(), 320, 240, GBM_FORMAT_XRGB8888,
                    GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR), gbm_bo_destroy);
  if (!bo || gbm_bo_get_plane_count(bo.get()) != 1)
    GTEST_SKIP() << "Single-plane linear allocation unavailable";
  uint32_t stride = 0;
  void *mapping = nullptr;
  auto *pixels = static_cast<uint8_t *>(gbm_bo_map(bo.get(), 0, 0, 320, 240,
      GBM_BO_TRANSFER_WRITE, &stride, &mapping));
  if (!pixels) GTEST_SKIP() << "Linear allocation cannot be filled";
  for (int row = 0; row < 240; ++row)
    memset(pixels + row * stride, 96, 320 * 4);
  gbm_bo_unmap(bo.get(), mapping);
  VideoFrame frame{};
  frame.width = 320; frame.height = 240; frame.pixelFormat = AV_PIX_FMT_BGR0;
  frame.isDmaBuf = true;
  frame.drmFormat = DRM_FORMAT_XRGB8888;
  frame.drmModifier = gbm_bo_get_modifier(bo.get());
  frame.dmaBufFd = gbm_bo_get_fd(bo.get());
  ASSERT_GE(frame.dmaBufFd, 0);
  frame.dmaBufStride = int(gbm_bo_get_stride(bo.get()));
  frame.dmaBufOffset = int(gbm_bo_get_offset(bo.get(), 0));
  const int exported = frame.dmaBufFd;
  const bool retained = frame.retainDmaBuf();
  close(exported);
  ASSERT_TRUE(retained);
  const auto e = probeFrames(frame);
  EXPECT_TRUE(e.error.isEmpty()) << e.error.toStdString();
  EXPECT_EQ(e.singlePlane, 90);
  EXPECT_EQ(e.accepted, 90);
  EXPECT_EQ(e.decoded, 90);
  EXPECT_EQ(status(e), "passed") << e.report()["summary"].toString().toStdString();
}
#endif
TEST_F(DmaBufTroubleshooterTest, WizardLoadsStartsCancelsAndReportsWithoutPortal) {
  QQmlEngine engine;
  QStringList warnings;
  QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                   [&](const QList<QQmlError> &items) {
    for (const auto &item : items) warnings << item.toString();
  });
  QQmlComponent component(&engine, QUrl::fromLocalFile(DMABUF_DIALOG_FILE));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  QQuickWindow window;
  window.resize(800, 800);
  window.show();
  auto dialog = std::unique_ptr<QObject>(component.createWithInitialProperties({
      {"parent", QVariant::fromValue(window.contentItem())},
      {"tester", QVariant::fromValue<QObject *>(&tester)}}));
  ASSERT_NE(dialog, nullptr) << component.errorString().toStdString();
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog.get(), "open"));
  QTest::qWait(250);
  auto *start = dialog->findChild<QObject *>("startDmaBufTest");
  ASSERT_NE(start, nullptr);
  EXPECT_TRUE(start->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(start, "clicked"));
  EXPECT_TRUE(tester.busy());
  ASSERT_TRUE(QMetaObject::invokeMethod(dialog.get(), "close"));
  settle();
  EXPECT_EQ(tester.result()["status"].toString(), "cancelled");
  EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}
TEST_F(DmaBufTroubleshooterTest, SettingsSectionNavigationOpensWizardAndTracksCaptureAvailability) {
  SettingsManager settings;
  TroubleshooterUiApp app;
  QQmlEngine engine;
  engine.rootContext()->setContextProperty("settingsManager", &settings);
  engine.rootContext()->setContextProperty("app", &app);
  engine.rootContext()->setContextProperty("dmaBufTroubleshooter", &tester);
  for (const auto *name : {"recordingManager", "replayManager", "storageManager",
       "audioDeviceEnumerator", "globalShortcuts", "hardwareInfo", "gameDetector"})
    engine.rootContext()->setContextProperty(name, static_cast<QObject *>(nullptr));
  QStringList warnings;
  QObject::connect(&engine, &QQmlEngine::warnings, &engine,
                   [&](const QList<QQmlError> &items) {
    for (const auto &item : items) warnings << item.toString();
  });
  QQmlComponent component(&engine, QUrl::fromLocalFile(SETTINGS_QML_FILE));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  QQuickWindow window;
  window.resize(1024, 720);
  auto page = std::unique_ptr<QObject>(component.create());
  ASSERT_NE(page, nullptr) << component.errorString().toStdString();
  auto *item = qobject_cast<QQuickItem *>(page.get());
  ASSERT_NE(item, nullptr);
  item->setParentItem(window.contentItem());
  item->setSize(QSizeF(1024, 720));
  window.show();
  ASSERT_TRUE(QMetaObject::invokeMethod(page.get(), "scrollToSection",
      Q_ARG(QVariant, QVariant(QStringLiteral("Troubleshooter")))));
  QTest::qWait(500);
  auto *section = page->findChild<QObject *>("troubleshooterSection");
  ASSERT_NE(section, nullptr);
  EXPECT_TRUE(section->property("expanded").toBool());
  auto *open = page->findChild<QObject *>("openDmaBufTester");
  ASSERT_NE(open, nullptr);
  tester.setAvailable(false);
  ASSERT_TRUE(QMetaObject::invokeMethod(open, "clicked"));
  QTest::qWait(250);
  auto *wizard = page->findChild<QObject *>("dmaBufTestWizard");
  ASSERT_NE(wizard, nullptr);
  EXPECT_TRUE(wizard->property("visible").toBool());
  auto *start = wizard->findChild<QObject *>("startDmaBufTest");
  ASSERT_NE(start, nullptr);
  EXPECT_FALSE(start->property("enabled").toBool());
  tester.setAvailable(true);
  EXPECT_TRUE(start->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(start, "clicked"));
  EXPECT_TRUE(tester.busy());
  ASSERT_TRUE(QMetaObject::invokeMethod(wizard, "close"));
  settle();
  EXPECT_TRUE(warnings.isEmpty()) << warnings.join('\n').toStdString();
}
} // namespace TrophyBackend

int main(int argc, char **argv) {
  QTemporaryDir home;
  qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  QGuiApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#include "test_DmaBufTroubleshooter.moc"
