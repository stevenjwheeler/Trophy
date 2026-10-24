#include <gtest/gtest.h>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlExpression>
#include <QJSValue>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTest>
#include <QSignalSpy>
#include <QFile>
#include <memory>

class ExportUiFake : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy MEMBER active NOTIFY changed)
  Q_PROPERTY(bool inspecting READ inspecting NOTIFY changed)
  Q_PROPERTY(QString sourcePath MEMBER source NOTIFY changed)
  Q_PROPERTY(QVariantMap info MEMBER data NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString resultPath READ resultPath NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY changed)
  Q_PROPERTY(QVariantList jobs MEMBER queueRows NOTIFY changed)
public:
  bool active = false;
  bool scanning = false;
  QString result;
  int inspections = 0, exports = 0, releases = 0, height = 0;
  qint64 bytes = -1;
  QString source, destination;
  QVariantMap data;
  QVariantList selected;
  QVariantList queueRows;
  QVariantList batchCaptures;
  QHash<QString, QVariantMap> captureData;
  QString heldProbe;
  QString cancelledJob;
  QStringList copiedSources;
  bool copyAccepted = true;
  bool watermark = false;
  bool inspecting() const { return scanning; }
  QString error() const { return {}; }
  QString resultPath() const { return result; }
  double progress() const { return 0.25; }
  Q_INVOKABLE bool inspect(const QString &path) {
    ++inspections; source = path; result.clear();
    scanning = path == heldProbe;
    data = {{"tracks", QVariantList{QVariantMap{{"index", 1}, {"label", "Game"}}, QVariantMap{{"index", 2}, {"label", "Voice"}}}}};
    if (captureData.contains(path)) data = captureData.value(path);
    emit changed(); return true;
  }
  Q_INVOKABLE bool start(const QString &folder, int h, qint64 b, const QVariantList &tracks, bool mark) {
    watermark = mark;
    ++exports; destination = folder; height = h; bytes = b; selected = tracks; active = true; emit changed(); return true;
  }
  Q_INVOKABLE bool startBatch(const QString &folder, int h, qint64 b, const QVariantList &captures, bool mark) {
    watermark = mark;
    ++exports; destination = folder; height = h; bytes = b; batchCaptures = captures; active = true; emit changed(); return true;
  }
  Q_INVOKABLE void cancel() { active = false; emit changed(); }
  Q_INVOKABLE void release() { ++releases; }
  Q_INVOKABLE void cancelJob(const QString &id) { cancelledJob = id; }
  Q_INVOKABLE bool copyOriginals(const QStringList &sources, const QString &folder) {
    copiedSources = sources; destination = folder; return copyAccepted;
  }
  Q_INVOKABLE QUrl resultFolder() const { return QUrl::fromLocalFile("/fake"); }
  Q_INVOKABLE QString localFolderPath(const QUrl &url) const {
    return url.isLocalFile() ? url.toLocalFile() : QString{};
  }
signals:
  void changed();
  void jobSettled(const QString &id, const QString &sourcePath, const QString &resultPath,
                  const QString &error, bool cancelled, bool copyOriginal = false);
};
class UndoUiFake : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList pending READ pending NOTIFY pendingChanged)
public:
  bool present = true;
  double progress = 0;
  int undos = 0;
  QVariantList pending() const { return present ? QVariantList{status("one")} : QVariantList{}; }
  Q_INVOKABLE QVariantMap status(const QString &) const {
    return {{"id", "one"}, {"count", 2}, {"progress", progress}, {"secondsLeft", 10 - int(progress * 10)}, {"committing", false}};
  }
  Q_INVOKABLE bool undo(const QString &id) {
    if (id != "one") return false;
    ++undos; present = false; emit pendingChanged(); return true;
  }
signals:
  void pendingChanged();
};
class SharingUiTest : public testing::Test {
protected:
  ExportUiFake exporter;
  UndoUiFake queue;
  QQmlEngine engine;
  QQuickWindow window;
  QStringList errors;
  std::unique_ptr<QObject> object;
  void SetUp() override {
    window.resize(1100, 800); window.show();
    QObject::connect(&engine, &QQmlEngine::warnings, &engine, [this](const QList<QQmlError> &warnings) {
      for (const auto &warning : warnings) errors.append(warning.toString());
    });
  }
  void TearDown() override {
    object.reset(); QCoreApplication::processEvents();
    EXPECT_TRUE(errors.isEmpty()) << errors.join('\n').toStdString();
    QObject::disconnect(&engine, nullptr, &engine, nullptr);
  }
  template<class F> bool wait(F condition) {
    QElapsedTimer time; time.start();
    while (!condition() && time.elapsed() < 2000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return condition();
  }
  QQuickItem *findItem(QQuickItem *item, const QString &name) {
    if (!item) return nullptr;
    if (item->objectName() == name) return item;
    for (auto *child : item->childItems()) if (auto *found = findItem(child, name)) return found;
    return nullptr;
  }
  void create(const QString &file, QVariantMap properties) {
    properties["parent"] = QVariant::fromValue(window.contentItem());
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString(SHARING_QML_DIR) + "/" + file));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    object.reset(component.createWithInitialProperties(properties));
    ASSERT_TRUE(bool(object)) << component.errorString().toStdString();
  }
};
TEST_F(SharingUiTest, ProtectedDeleteWarningWrapsAndCollapsesWhenUnused) {
  create("../controls/TrophyDialog.qml", {{"title", "Delete Recording"},
      {"message", "Delete this capture?"}, {"warningMessage", "This capture is protected from auto-cleanup. Deleting it manually will still permanently remove it after the Undo period."}});
  ASSERT_TRUE(object);
  ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "open"));
  auto *box = object->findChild<QQuickItem *>("dialogWarningBox");
  auto *label = object->findChild<QQuickItem *>("dialogWarningText");
  ASSERT_TRUE(box);
  ASSERT_TRUE(label);
  ASSERT_TRUE(wait([&] { return box->isVisible() && label->height() > 0; }));
  EXPECT_GT(box->height(), label->height());
  EXPECT_GT(label->property("lineCount").toInt(), 1);
  const double warningHeight = object->property("implicitHeight").toDouble();
  object->setProperty("warningMessage", "");
  ASSERT_TRUE(wait([&] { return !box->isVisible() && object->property("implicitHeight").toDouble() < warningHeight; }));
  object->setProperty("warningMessage", "Protected captures selected: 2.");
  EXPECT_TRUE(wait([&] { return box->isVisible(); }));
}

TEST_F(SharingUiTest, ProtectedDeletesCannotSkipConfirmation) {
  QFile source(QString(SHARING_QML_DIR) + "/../main.qml");
  ASSERT_TRUE(source.open(QIODevice::ReadOnly));
  const QString qml = QString::fromUtf8(source.readAll());
  // Exercise the actual main-window functions without starting application services.
  auto function = [&](const QString &name) {
    const auto start = qml.indexOf("    function " + name + "(");
    const auto end = qml.indexOf("\n    }", start);
    return qml.mid(start, end + 6 - start);
  };
  auto result = engine.evaluate(QStringLiteral(R"JS(
    var queued = 0;
    var selectedRecordings = ["plain"];
    var deleteDialog = { opened: 0, open: function() { ++this.opened; } };
    var bulkDeleteDialog = { opened: 0, open: function() { ++this.opened; } };
    var window = {
      skipDeleteConfirmation: true, storageManager: {},
      recordingLibrary: { getRecording: function(path) {
        return path === "missing" ? null : { protectedFromCleanup: path === "protected" };
      } },
      queueDeletion: function() { ++queued; }
    };
    function performDeleteSelected() { ++queued; }
  )JS") + function("protectedCaptureCount") + function("confirmDelete") + function("deleteSelected") + QStringLiteral(R"JS(
    confirmDelete("plain", "Plain");
    if (queued !== 1 || deleteDialog.opened !== 0) throw Error("plain skip");
    confirmDelete("protected", "Protected");
    if (queued !== 1 || deleteDialog.opened !== 1 || !deleteDialog.captureProtected) throw Error("protected single");
    deleteSelected();
    if (queued !== 2 || bulkDeleteDialog.opened !== 0) throw Error("plain bulk skip");
    selectedRecordings = ["plain", "protected", "protected", "missing"];
    deleteSelected();
    if (queued !== 2 || bulkDeleteDialog.opened !== 1 || bulkDeleteDialog.protectedCount !== 2) throw Error("protected bulk");
    window.skipDeleteConfirmation = false;
    confirmDelete("plain", "Plain");
    if (deleteDialog.opened !== 2 || deleteDialog.captureProtected) throw Error("stale single warning");
    selectedRecordings = ["plain"];
    deleteSelected();
    if (bulkDeleteDialog.opened !== 2 || bulkDeleteDialog.protectedCount !== 0) throw Error("stale bulk warning");
    true;
  )JS"));
  ASSERT_FALSE(result.isError()) << result.toString().toStdString();
  EXPECT_TRUE(result.toBool());
}

TEST_F(SharingUiTest, VoiceAppSurvivesOfflineStartupRefreshAndLateLaunch) {
  create("../controls/VoiceAppComboBox.qml", {{"savedApp", "Discord"}});
  ASSERT_TRUE(object);
  QSignalSpy selected(object.get(), SIGNAL(appSelected(QString)));
  ASSERT_TRUE(wait([&] { return object->property("currentIndex").toInt() == 1; }));
  EXPECT_EQ(object->property("displayText").toString(), "Discord");
  auto refresh = [&](const QVariantList &apps, int index, const QString &label) {
    object->setProperty("apps", apps);
    EXPECT_TRUE(wait([&] { return object->property("currentIndex").toInt() == index &&
                                  object->property("displayText").toString() == label; }));
    EXPECT_EQ(object->property("savedApp").toString(), "Discord");
    EXPECT_TRUE(selected.isEmpty());
  };
  const QVariantMap discord{{"name", "Discord Voice"}, {"binary", "discord"}};
  const QVariantMap other{{"name", "Music"}, {"binary", "music"}};
  refresh({other}, 2, "Discord");
  // Same count/index, different model: ComboBox must still restore selection.
  refresh({QVariantMap{{"name", "Other"}, {"binary", "other"}}}, 2, "Discord");
  refresh({other, discord}, 2, "Discord Voice");
  refresh({discord, other}, 1, "Discord Voice");
  refresh({}, 1, "Discord");
  refresh({}, 1, "Discord");
  // Reopening the picker before Discord starts also preserves the choice.
  object.reset();
  create("../controls/VoiceAppComboBox.qml", {{"savedApp", "Discord"}});
  ASSERT_TRUE(object);
  ASSERT_TRUE(wait([&] { return object->property("currentIndex").toInt() == 1; }));
  EXPECT_EQ(object->property("displayText").toString(), "Discord");
}

TEST_F(SharingUiTest, VoiceAppOnlyExplicitValidActivationChangesPreference) {
  create("../controls/VoiceAppComboBox.qml", {{"savedApp", "Discord"},
      {"apps", QVariantList{QVariantMap{{"name", "No identity"}, {"binary", ""}},
                            QVariantMap{{"name", "TeamSpeak"}, {"binary", "teamspeak"}}}}});
  ASSERT_TRUE(object);
  QSignalSpy selected(object.get(), SIGNAL(appSelected(QString)));
  auto activate = [&](int index) {
    ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "activated", Q_ARG(int, index)));
  };
  activate(3); // Retained offline Discord entry.
  activate(1); // A transient entry without a usable binary must not erase Discord.
  EXPECT_TRUE(selected.isEmpty());
  activate(2);
  ASSERT_EQ(selected.size(), 1);
  EXPECT_EQ(selected.takeFirst().at(0).toString(), "teamspeak");
  object->setProperty("savedApp", "teamspeak");
  ASSERT_TRUE(wait([&] { return object->property("currentIndex").toInt() == 2; }));
  activate(0);
  ASSERT_EQ(selected.size(), 1);
  EXPECT_TRUE(selected.takeFirst().at(0).toString().isEmpty());
  object->setProperty("savedApp", "");
  ASSERT_TRUE(wait([&] { return object->property("currentIndex").toInt() == 0; }));
  EXPECT_EQ(object->property("displayText").toString(), "None");
}

TEST_F(SharingUiTest, DmaBufOptionsDisableForSoftwareWithoutChangingSavedPreference) {
  auto read = [](const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString{};
  };
  const auto settings = read(QString(SHARING_QML_DIR) + "/../pages/SettingsPage.qml");
  const auto wizard = read(QString(SHARING_QML_DIR) + "/SetupWizard.qml");
  ASSERT_FALSE(settings.isEmpty()); ASSERT_FALSE(wizard.isEmpty());
  auto block = [](const QString &source, int start) {
    if (start < 0) return QString{};
    int depth = 0;
    for (auto i = source.indexOf('{', start); i < source.size(); ++i) {
      if (source[i] == '{') ++depth;
      if (source[i] == '}' && --depth == 0) return source.mid(start, i - start + 1);
    }
    return QString{};
  };
  const auto rowType = block(settings, settings.indexOf("component SettingRow:"));
  ASSERT_FALSE(rowType.isEmpty());
  for (bool isWizard : {false, true}) {
    const auto &source = isWizard ? wizard : settings;
    const QString prefix = isWizard ? "wizard" : "settings";
    SCOPED_TRACE(prefix.toStdString());
    const auto marker = source.indexOf("objectName: \"" + prefix + "DmaBufOption\"");
    ASSERT_GE(marker, 0);
    const auto row = block(source, source.lastIndexOf(isWizard ? "RowLayout {" : "SettingRow {", marker));
    ASSERT_FALSE(row.isEmpty());
    QQmlComponent component(&engine);
    component.setData((QStringLiteral(R"(
      import QtQuick
      import QtQuick.Controls
      import QtQuick.Layouts
      import "../controls"
      import "../styles"
      Item {
        width: 640; height: 200
        QtObject { id: settingsManager; objectName: "preferences"
          property string encoder: "nvenc_h264"
          property bool useDmaBuf: true
        }
        QtObject { id: recordingManager; objectName: "recorder"
          property bool isRecording: false
        }
        QtObject { id: hardwareInfo; objectName: "hardware"
          property string recommendedEncoderName: "nvenc_h264"
          property bool dmaBufAvailable: true
        }
    )") + rowType + "\nColumnLayout { width: parent.width\n" + row + "\n}}" ).toUtf8(),
        QUrl::fromLocalFile(QString(SHARING_QML_DIR) + "/DmaBufOptionsProbe.qml"));
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    object.reset(component.createWithInitialProperties({{"parent", QVariant::fromValue(window.contentItem())}}));
    ASSERT_TRUE(object) << component.errorString().toStdString();
    auto *preferences = object->findChild<QObject *>("preferences");
    auto *recorder = object->findChild<QObject *>("recorder");
    auto *hardware = object->findChild<QObject *>("hardware");
    auto *option = object->findChild<QQuickItem *>(prefix + "DmaBufOption");
    auto *toggle = object->findChild<QQuickItem *>(prefix + "DmaBufSwitch");
    ASSERT_NE(preferences, nullptr); ASSERT_NE(recorder, nullptr); ASSERT_NE(hardware, nullptr);
    ASSERT_NE(option, nullptr); ASSERT_NE(toggle, nullptr);
    QSignalSpy writes(preferences, SIGNAL(useDmaBufChanged()));
    ASSERT_TRUE(writes.isValid());
    ASSERT_TRUE(wait([&] { return toggle->width() > 0 && toggle->isEnabled(); }));
    auto click = [&] {
      QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
          toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint());
    };
    preferences->setProperty("encoder", "x264");
    ASSERT_TRUE(wait([&] { return !toggle->isEnabled(); }));
    EXPECT_DOUBLE_EQ(option->opacity(), 0.5);
    click();
    EXPECT_TRUE(preferences->property("useDmaBuf").toBool());
    EXPECT_TRUE(toggle->property("checked").toBool());
    EXPECT_TRUE(writes.isEmpty());
    preferences->setProperty("encoder", "auto");
    ASSERT_TRUE(wait([&] { return toggle->isEnabled(); }));
    hardware->setProperty("recommendedEncoderName", "x264");
    ASSERT_TRUE(wait([&] { return !toggle->isEnabled(); }));
    EXPECT_TRUE(writes.isEmpty());
    hardware->setProperty("recommendedEncoderName", "vaapi_h264");
    ASSERT_TRUE(wait([&] { return toggle->isEnabled(); }));
    preferences->setProperty("encoder", "vaapi_h264");
    recorder->setProperty("isRecording", true);
    ASSERT_TRUE(wait([&] { return !toggle->isEnabled(); }));
    click();
    EXPECT_TRUE(writes.isEmpty());
    recorder->setProperty("isRecording", false);
    ASSERT_TRUE(wait([&] { return toggle->isEnabled(); }));
    click();
    ASSERT_TRUE(wait([&] { return !preferences->property("useDmaBuf").toBool(); }));
    EXPECT_EQ(writes.size(), 1);
    preferences->setProperty("encoder", "x264");
    preferences->setProperty("encoder", "nvenc_h264");
    EXPECT_FALSE(toggle->property("checked").toBool());
    EXPECT_FALSE(preferences->property("useDmaBuf").toBool());
    EXPECT_EQ(writes.size(), 1);
    object.reset();
  }
}

TEST_F(SharingUiTest, RecordingSizeEstimateFollowsBitrateAndEffectiveAudioLayout) {
  QFile file(QString(SHARING_QML_DIR) + "/../pages/SettingsPage.qml");
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const auto page = QString::fromUtf8(file.readAll());
  const auto marker = page.indexOf("objectName: \"recordingSizeEstimate\"");
  ASSERT_GE(marker, 0);
  const auto start = page.lastIndexOf("SettingStack {", marker);
  int depth = 0;
  QString source;
  for (auto i = page.indexOf('{', start); i < page.size(); ++i) {
    if (page[i] == '{') ++depth;
    if (page[i] == '}' && --depth == 0) {
      source = page.mid(start, i - start + 1);
      break;
    }
  }
  ASSERT_FALSE(source.isEmpty());
  const auto stackStart = page.indexOf("component SettingStack:");
  const auto stackEnd = page.indexOf("component ValuePill:", stackStart);
  ASSERT_GE(stackStart, 0); ASSERT_GT(stackEnd, stackStart);
  const auto stackType = page.mid(stackStart, stackEnd - stackStart);
  QQmlComponent component(&engine);
  component.setData((QStringLiteral(R"(
    import QtQuick
    import QtQuick.Controls
    import QtQuick.Layouts
    import "../controls"
    import "../styles"
    Item {
      width: 700; height: 240
      QtObject { id: settingsManager; objectName: "preferences"
        property string audioMode: "game"
        property bool separateAudioTracks: true
      }
      QtObject { id: bitrateSlider; objectName: "bitrate"
        property real value: 15000
      }
  )") + stackType + "ColumnLayout { width: parent.width\n" + source + "}}" ).toUtf8(),
      QUrl::fromLocalFile(QString(SHARING_QML_DIR) + "/SizeEstimateProbe.qml"));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  object.reset(component.createWithInitialProperties({{"parent", QVariant::fromValue(window.contentItem())}}));
  ASSERT_TRUE(object) << component.errorString().toStdString();
  auto *preferences = object->findChild<QObject *>("preferences");
  auto *bitrate = object->findChild<QObject *>("bitrate");
  auto *estimate = object->findChild<QObject *>("recordingSizeEstimate");
  auto *text = object->findChild<QObject *>("recordingSizeEstimateText");
  ASSERT_NE(preferences, nullptr); ASSERT_NE(bitrate, nullptr);
  ASSERT_NE(estimate, nullptr); ASSERT_NE(text, nullptr);
  EXPECT_EQ(estimate->property("audioTracks").toInt(), 3);
  EXPECT_DOUBLE_EQ(estimate->property("bytesPerMinute").toDouble(), 116820000);
  EXPECT_TRUE(text->property("text").toString().contains("1 minute: ~117 MB"));
  EXPECT_TRUE(text->property("text").toString().contains("10 minutes: ~1.2 GB"));
  EXPECT_TRUE(text->property("text").toString().contains("1 hour: ~7.0 GB"));
  preferences->setProperty("audioMode", "desktop");
  EXPECT_EQ(estimate->property("audioTracks").toInt(), 1);
  EXPECT_DOUBLE_EQ(estimate->property("bytesPerMinute").toDouble(), 113940000);
  preferences->setProperty("audioMode", "game");
  preferences->setProperty("separateAudioTracks", false);
  EXPECT_EQ(estimate->property("audioTracks").toInt(), 1);
  bitrate->setProperty("value", 2000);
  EXPECT_DOUBLE_EQ(estimate->property("bytesPerMinute").toDouble(), 16440000);
  EXPECT_TRUE(text->property("text").toString().contains("1 hour: ~986 MB"));
  preferences->setProperty("separateAudioTracks", true);
  bitrate->setProperty("value", 50000);
  EXPECT_DOUBLE_EQ(estimate->property("bytesPerMinute").toDouble(), 379320000);
  EXPECT_TRUE(text->property("text").toString().contains("1 hour: ~22.8 GB"));
}

TEST_F(SharingUiTest, AudioOptionsArePairedButtonsWithLiveStateAndRecordingLock) {
  QFile file(QString(SHARING_QML_DIR) + "/../pages/SettingsPage.qml");
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const auto page = QString::fromUtf8(file.readAll());
  auto rowSource = [&](const QString &name) {
    const auto marker = page.indexOf("objectName: \"" + name + "\"");
    if (marker < 0) return QString{};
    const auto start = page.lastIndexOf("RowLayout {", marker);
    int depth = 0;
    for (auto i = page.indexOf('{', start); i < page.size(); ++i) {
      if (page[i] == '{') ++depth;
      if (page[i] == '}' && --depth == 0) return page.mid(start, i - start + 1);
    }
    return QString{};
  };
  const auto capture = rowSource("audioCaptureMode");
  const auto tracks = rowSource("audioTrackLayout");
  ASSERT_FALSE(capture.isEmpty()); ASSERT_FALSE(tracks.isEmpty());
  QQmlComponent component(&engine);
  component.setData((QStringLiteral(R"(
    import QtQuick
    import QtQuick.Layouts
    import "../controls"
    import "../styles"
    Item {
      width: 400; height: 200
      QtObject { id: settingsManager; objectName: "preferences"
        property string audioMode: "game"
        property bool separateAudioTracks: true
      }
      QtObject { id: recordingManager; objectName: "recorder"
        property bool isRecording: false
      }
      ColumnLayout {
  )") + capture + tracks + "}}").toUtf8(),
      QUrl::fromLocalFile(QString(SHARING_QML_DIR) + "/AudioOptionsProbe.qml"));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  object.reset(component.createWithInitialProperties({{"parent", QVariant::fromValue(window.contentItem())}}));
  ASSERT_TRUE(object) << component.errorString().toStdString();
  auto *preferences = object->findChild<QObject *>("preferences");
  auto *recorder = object->findChild<QObject *>("recorder");
  ASSERT_NE(preferences, nullptr); ASSERT_NE(recorder, nullptr);
  for (const auto &name : {"audioCaptureMode", "audioTrackLayout"}) {
    auto *row = object->findChild<QQuickItem *>(name);
    ASSERT_NE(row, nullptr);
    QList<QQuickItem *> buttons;
    for (auto *child : row->childItems())
      if (child->metaObject()->indexOfSignal("clicked()") >= 0) buttons.append(child);
    ASSERT_EQ(buttons.size(), 2);
    EXPECT_EQ(buttons[0]->property("radius").toDouble(), 0);
    EXPECT_EQ(buttons[1]->property("radius").toDouble(), 0);
    EXPECT_TRUE(buttons[0]->property("checked").toBool());
    EXPECT_FALSE(buttons[1]->property("checked").toBool());
    ASSERT_TRUE(wait([&] { return buttons[1]->width() > 100; }));
    auto click = [&](QQuickItem *button) {
      QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
          button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
    };
    click(buttons[1]);
    EXPECT_TRUE(buttons[1]->property("checked").toBool());
    EXPECT_FALSE(buttons[0]->property("checked").toBool());
    recorder->setProperty("isRecording", true);
    EXPECT_FALSE(buttons[0]->isEnabled());
    click(buttons[0]);
    EXPECT_TRUE(buttons[1]->property("checked").toBool());
    recorder->setProperty("isRecording", false);
    click(buttons[0]);
    EXPECT_TRUE(buttons[0]->property("checked").toBool());
  }
  EXPECT_EQ(preferences->property("audioMode").toString(), "game");
  EXPECT_TRUE(preferences->property("separateAudioTracks").toBool());
}

TEST_F(SharingUiTest, FrameRateOffersUncappedAndRetainsRecordingLock) {
  QFile file(QString(SHARING_QML_DIR) + "/../pages/SettingsPage.qml");
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const auto page = QString::fromUtf8(file.readAll());
  const auto marker = page.indexOf("objectName: \"recordingFrameRate\"");
  ASSERT_GE(marker, 0);
  const auto start = page.lastIndexOf("Rectangle {", marker);
  QString source;
  int depth = 0;
  for (auto i = page.indexOf('{', start); i < page.size(); ++i) {
    if (page[i] == '{') ++depth;
    if (page[i] == '}' && --depth == 0) {
      source = page.mid(start, i - start + 1);
      break;
    }
  }
  ASSERT_FALSE(source.isEmpty());
  QQmlComponent component(&engine);
  component.setData((QStringLiteral(R"(
    import QtQuick
    import QtQuick.Controls
    import QtQuick.Layouts
    import "../styles"
    Item {
      width: 700; height: 100
      QtObject { id: settingsManager; objectName: "preferences"; property int fps: 60 }
      QtObject { id: recordingManager; objectName: "recorder"; property bool isRecording: false }
      RowLayout { anchors.fill: parent
  )") + source + "}}").toUtf8(),
      QUrl::fromLocalFile(QString(SHARING_QML_DIR) + "/FrameRateProbe.qml"));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  object.reset(component.createWithInitialProperties({{"parent", QVariant::fromValue(window.contentItem())}}));
  ASSERT_TRUE(object) << component.errorString().toStdString();
  auto *preferences = object->findChild<QObject *>("preferences");
  auto *recorder = object->findChild<QObject *>("recorder");
  auto *row = object->findChild<QQuickItem *>("recordingFrameRate");
  ASSERT_NE(preferences, nullptr); ASSERT_NE(recorder, nullptr); ASSERT_NE(row, nullptr);
  const auto findButton = [&](const QString &name, auto &&self, QQuickItem *item) -> QQuickItem * {
    if (item->objectName() == name) return item;
    for (auto *child : item->childItems())
      if (auto *found = self(name, self, child)) return found;
    return nullptr;
  };
  auto *uncapped = findButton("recordingFps0", findButton, row);
  auto *fixed = findButton("recordingFps60", findButton, row);
  ASSERT_NE(uncapped, nullptr); ASSERT_NE(fixed, nullptr);
  ASSERT_TRUE(wait([&] { return uncapped->width() > 0; }));
  const auto click = [&](QQuickItem *button) {
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
        button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint());
  };
  click(uncapped);
  EXPECT_EQ(preferences->property("fps").toInt(), 0);
  EXPECT_TRUE(uncapped->property("isChecked").toBool());
  recorder->setProperty("isRecording", true);
  EXPECT_FALSE(row->isEnabled());
  click(fixed);
  EXPECT_EQ(preferences->property("fps").toInt(), 0);
  recorder->setProperty("isRecording", false);
  click(fixed);
  EXPECT_EQ(preferences->property("fps").toInt(), 60);
  EXPECT_TRUE(fixed->property("isChecked").toBool());
}

TEST_F(SharingUiTest, ShareDialogHasVisibleControlsAndPassesAudioSelection) {
  QTemporaryDir output;
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"sourcePath", "/fake/Game.mkv"},
      {"excludedAudioOrdinals", QVariantList{1}}});
  ASSERT_TRUE(bool(object));
  ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "open"));
  ASSERT_TRUE(wait([&] { return exporter.inspections == 1; }));
  const auto selection = object->property("selectedAudio");
  const auto tracks = selection.canConvert<QJSValue>() ? selection.value<QJSValue>().toVariant().toList() : selection.toList();
  EXPECT_EQ(tracks, QVariantList{1}); // Muted Voice lane starts excluded.
  ASSERT_TRUE(wait([&] { return object->property("implicitHeight").toDouble() > 300; }));
  EXPECT_LE(object->property("implicitHeight").toDouble(), 760);
  object->setProperty("destination", output.path());
  object->setProperty("selectedAudio", QVariantList{1});
  auto *button = object->findChild<QObject *>("startShareExport");
  ASSERT_NE(button, nullptr);
  EXPECT_TRUE(button->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
  EXPECT_EQ(exporter.exports, 1); EXPECT_EQ(exporter.height, 1080);
  EXPECT_EQ(exporter.bytes, 0); EXPECT_EQ(exporter.selected, QVariantList{1});
  EXPECT_EQ(exporter.destination, output.path());
  ASSERT_TRUE(wait([&] { return !object->property("visible").toBool(); }));
  EXPECT_EQ(exporter.releases, 1); // Release the form, not the queued job.
  exporter.active = false; emit exporter.changed();
  ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "open"));
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  EXPECT_EQ(exporter.inspections, 2); // A completed export can be configured again.
  ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "close"));
  ASSERT_TRUE(wait([&] { return exporter.releases == 2; }));
}
TEST_F(SharingUiTest, ExportRunsInBackgroundWithoutDialogProgress) {
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"sourcePath", "/fake/Game.mkv"}});
  ASSERT_TRUE(bool(object));
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  auto *options = object->findChild<QQuickItem *>("shareExportOptions");
  ASSERT_NE(options, nullptr);
  EXPECT_EQ(object->findChild<QObject *>("shareExportProgress"), nullptr);
  EXPECT_TRUE(options->isVisible());
  object->setProperty("destination", "/fake/output");
  auto *start = object->findChild<QQuickItem *>("startShareExport");
  ASSERT_NE(start, nullptr);
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      start->mapToScene(QPointF(start->width() / 2, start->height() / 2)).toPoint());
  ASSERT_TRUE(wait([&] { return exporter.active && !object->property("visible").toBool(); }));
  EXPECT_TRUE(exporter.watermark);
  EXPECT_EQ(exporter.releases, 1);
  exporter.active = false; exporter.result = "/fake/Game_share.mp4"; emit exporter.changed();
  EXPECT_FALSE(object->property("visible").toBool());
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  ASSERT_TRUE(wait([&] { return exporter.inspections == 2; }));
  EXPECT_TRUE(options->isVisible());
  auto *close = object->findChild<QQuickItem *>("closeShareExport");
  ASSERT_NE(close, nullptr);
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      close->mapToScene(QPointF(close->width() / 2, close->height() / 2)).toPoint());
  EXPECT_TRUE(wait([&] { return !object->property("visible").toBool(); }));
}
TEST_F(SharingUiTest, BatchReviewsIndependentAudioLayoutsAndQueuesAllWithSharedSettings) {
  exporter.captureData["/fake/Second.mkv"] = {{"tracks", QVariantList{
      QVariantMap{{"index", 3}, {"label", "System"}},
      QVariantMap{{"index", 5}, {"label", "Voice"}, {"defaultExcluded", true}},
      QVariantMap{{"index", 7}, {"label", "Microphone"}}}}};
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePaths", QVariantList{"/fake/First.mkv", "/fake/Second.mkv"}}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("tracksLoaded").toBool(); }));
  EXPECT_EQ(exporter.inspections, 2);
  object->setProperty("includeWatermark", false);
  object->setProperty("selectedAudio", QVariantList{2});
  auto *picker = object->findChild<QObject *>("shareCapturePicker");
  ASSERT_NE(picker, nullptr);
  picker->setProperty("currentIndex", 1);
  ASSERT_TRUE(QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, 1)));
  EXPECT_EQ(object->property("audioSummary").toString(), "System + Microphone");
  object->setProperty("selectedAudio", QVariantList{7});
  picker->setProperty("currentIndex", 0);
  ASSERT_TRUE(QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, 0)));
  EXPECT_EQ(object->property("audioSummary").toString(), "Voice");
  auto *start = object->findChild<QObject *>("startShareExport");
  ASSERT_NE(start, nullptr);
  EXPECT_TRUE(start->property("enabled").toBool());
  QMetaObject::invokeMethod(start, "clicked");
  ASSERT_EQ(exporter.batchCaptures.size(), 2);
  EXPECT_FALSE(exporter.watermark);
  EXPECT_EQ(exporter.batchCaptures[0].toMap()["sourcePath"].toString(), "/fake/First.mkv");
  EXPECT_EQ(exporter.batchCaptures[0].toMap()["audioStreams"].toList(), QVariantList{2});
  EXPECT_EQ(exporter.batchCaptures[1].toMap()["audioStreams"].toList(), QVariantList{7});
  EXPECT_EQ(exporter.height, 1080);
  EXPECT_EQ(exporter.destination, "/fake/output");
  EXPECT_TRUE(wait([&] { return !object->property("visible").toBool(); }));
}
TEST_F(SharingUiTest, WatermarkSwitchInsideQualityRetainsExplicitOffAcrossReopenAndExportsOff) {
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", "/fake/Game.mkv"}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  auto *choice = object->findChild<QQuickItem *>("shareWatermark");
  ASSERT_NE(choice, nullptr);
  auto *quality = object->findChild<QQuickItem *>("shareQualitySection");
  auto *row = object->findChild<QQuickItem *>("shareWatermarkRow");
  auto *audio = object->findChild<QQuickItem *>("shareAudioSection");
  ASSERT_NE(quality, nullptr); ASSERT_NE(row, nullptr); ASSERT_NE(audio, nullptr);
  EXPECT_FALSE(choice->isVisible());
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      quality->mapToScene(QPointF(quality->width() / 2, quality->height() / 2)).toPoint());
  ASSERT_TRUE(wait([&] { return quality->property("expansionProgress").toDouble() >= 0.999; }));
  EXPECT_TRUE(choice->isVisible());
  EXPECT_GT(row->mapToScene(QPointF()).y(), quality->mapToScene(QPointF()).y());
  EXPECT_LE(row->mapToScene(QPointF(0, row->height())).y(), quality->mapToScene(QPointF(0, quality->height())).y());
  EXPECT_GE(audio->mapToScene(QPointF()).y(), quality->mapToScene(QPointF(0, quality->height())).y());
  EXPECT_EQ(choice->property("implicitWidth").toInt(), 44);
  EXPECT_TRUE(choice->property("checked").toBool());
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      choice->mapToScene(QPointF(choice->width() / 2, choice->height() / 2)).toPoint());
  EXPECT_FALSE(object->property("includeWatermark").toBool());
  QMetaObject::invokeMethod(object.get(), "close");
  ASSERT_TRUE(wait([&] { return !object->property("visible").toBool(); }));
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  EXPECT_FALSE(choice->property("checked").toBool());
  EXPECT_FALSE(choice->isVisible());
  QMetaObject::invokeMethod(object->findChild<QObject *>("startShareExport"), "clicked");
  EXPECT_EQ(exporter.exports, 1);
  EXPECT_FALSE(exporter.watermark);
}
TEST_F(SharingUiTest, BatchProbeFailureDoesNotQueuePartialSelection) {
  exporter.captureData["/fake/Broken.mkv"] = {{"error", "Unreadable capture"}};
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePaths", QVariantList{"/fake/First.mkv", "/fake/Broken.mkv"}}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return !object->property("preparationError").toString().isEmpty(); }));
  EXPECT_TRUE(object->property("preparationError").toString().contains("Broken.mkv"));
  EXPECT_FALSE(object->findChild<QObject *>("startShareExport")->property("enabled").toBool());
  EXPECT_EQ(exporter.exports, 0);
}
TEST_F(SharingUiTest, ClosingBatchRejectsLateProbeAndReopeningResetsSelections) {
  exporter.heldProbe = "/fake/Second.mkv";
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePaths", QVariantList{"/fake/First.mkv", "/fake/Second.mkv"}}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return exporter.inspections == 2; }));
  EXPECT_TRUE(object->property("working").toBool());
  EXPECT_FALSE(object->findChild<QObject *>("startShareExport")->property("enabled").toBool());
  QMetaObject::invokeMethod(object->findChild<QObject *>("closeShareExport"), "clicked");
  ASSERT_TRUE(wait([&] { return exporter.releases == 1; }));
  exporter.scanning = false;
  emit exporter.changed();
  EXPECT_FALSE(object->property("tracksLoaded").toBool());
  EXPECT_EQ(exporter.exports, 0);
  exporter.heldProbe.clear();
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("tracksLoaded").toBool(); }));
  EXPECT_EQ(exporter.inspections, 4);
  EXPECT_EQ(object->property("currentCapture").toInt(), 0);
  EXPECT_EQ(object->property("audioSummary").toString(), "Game + Voice");
}
TEST_F(SharingUiTest, QueuePanelShowsAllJobsAndKeepsCancelButtonsAcrossProgress) {
  queue.present = false;
  exporter.queueRows = {
      QVariantMap{{"id", "active"}, {"sourcePath", "/fake/First.mkv"}, {"active", true}, {"progress", 0.2}, {"cancelling", false}},
      QVariantMap{{"id", "waiting"}, {"sourcePath", "/fake/Second.mkv"}, {"active", false}, {"progress", 0.0}, {"cancelling", false}}};
  create("ToastStack.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"queue", QVariant::fromValue(&queue)}, {"width", 380}});
  auto *panel = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(panel, nullptr);
  ASSERT_TRUE(wait([&] { return findItem(panel, "toastAction:export:waiting") != nullptr; }));
  auto *cancel = findItem(panel, "toastAction:export:waiting");
  auto first = exporter.queueRows[0].toMap(); first["progress"] = 0.5;
  exporter.queueRows[0] = first; emit exporter.changed();
  QCoreApplication::processEvents();
  EXPECT_EQ(findItem(panel, "toastAction:export:waiting"), cancel);
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      cancel->mapToScene(QPointF(cancel->width() / 2, cancel->height() / 2)).toPoint());
  EXPECT_EQ(exporter.cancelledJob, "waiting");
  exporter.queueRows.clear(); emit exporter.changed();
  EXPECT_FALSE(panel->isVisible());
}
TEST_F(SharingUiTest, MixedToastStackRetainsGreenCompletionsWithIndependentExpiry) {
  exporter.queueRows = {
      QVariantMap{{"id", "one"}, {"sourcePath", "/fake/First.mkv"}, {"active", true}, {"progress", 0.2}, {"cancelling", false}},
      QVariantMap{{"id", "two"}, {"sourcePath", "/fake/Second.mkv"}, {"active", false}, {"progress", 0.0}, {"cancelling", false}}};
  create("ToastStack.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"queue", QVariant::fromValue(&queue)},
      {"width", 400}, {"maxToastHeight", 200}, {"successDuration", 500}});
  auto *panel = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(panel, nullptr);
  ASSERT_TRUE(wait([&] { return findItem(panel, "toast:export:one") != nullptr && findItem(panel, "toast:delete:one") != nullptr; }));
  EXPECT_EQ(object->property("count").toInt(), 3);
  EXPECT_LE(panel->implicitHeight(), 200);
  QPointer<QQuickItem> first = findItem(panel, "toast:export:one");
  auto *undo = findItem(panel, "toastAction:delete:one");
  exporter.queueRows.removeFirst();
  emit exporter.jobSettled("one", "/fake/First.mkv", "/shares/First_share.mp4", {}, false);
  emit exporter.changed();
  ASSERT_FALSE(first.isNull());
  EXPECT_EQ(findItem(panel, "toast:export:one"), first.data()); // Same card turns green.
  EXPECT_EQ(first->property("phase").toString(), "success");
  EXPECT_EQ(first->property("statusColor").value<QColor>(), QColor("#4ade80"));
  EXPECT_EQ(first->property("heading").toString(), "Exported");
  EXPECT_EQ(findItem(panel, "toastAction:delete:one"), undo);
  QTest::qWait(100);
  EXPECT_FALSE(first.isNull());
  object->setProperty("successDuration", 2000);
  exporter.queueRows.clear();
  emit exporter.jobSettled("two", "/fake/Second.mkv", "/shares/Second_share.mp4", {}, false);
  emit exporter.changed();
  ASSERT_TRUE(wait([&] { return findItem(panel, "toast:export:one") == nullptr; }));
  EXPECT_NE(findItem(panel, "toast:export:two"), nullptr);
  EXPECT_EQ(findItem(panel, "toastAction:delete:one"), undo);
  EXPECT_EQ(object->property("count").toInt(), 2);
}
TEST_F(SharingUiTest, FailedExportsTurnRedAndCancellationOnlyRemovesItsOwnToast) {
  exporter.queueRows = {
      QVariantMap{{"id", "failure"}, {"sourcePath", "/fake/Bad.mkv"}, {"active", true}, {"progress", 0.2}, {"cancelling", false}},
      QVariantMap{{"id", "cancel"}, {"sourcePath", "/fake/Cancel.mkv"}, {"active", false}, {"progress", 0.0}, {"cancelling", false}}};
  create("ToastStack.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"queue", QVariant::fromValue(&queue)}, {"width", 400}});
  auto *panel = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(panel, nullptr);
  exporter.queueRows.clear();
  emit exporter.jobSettled("failure", "/fake/Bad.mkv", {}, "Disk full", false);
  emit exporter.jobSettled("cancel", "/fake/Cancel.mkv", {}, {}, true);
  emit exporter.changed();
  auto *failed = findItem(panel, "toast:export:failure");
  ASSERT_NE(failed, nullptr);
  EXPECT_EQ(failed->property("phase").toString(), "error");
  EXPECT_EQ(failed->property("statusColor").value<QColor>(), QColor("#ef4444"));
  EXPECT_TRUE(failed->property("detail").toString().contains("Disk full"));
  EXPECT_EQ(findItem(panel, "toast:export:cancel"), nullptr);
  EXPECT_NE(findItem(panel, "toast:delete:one"), nullptr);
  auto *dismiss = findItem(panel, "toastAction:export:failure");
  ASSERT_NE(dismiss, nullptr);
  QMetaObject::invokeMethod(dismiss, "clicked");
  EXPECT_EQ(findItem(panel, "toast:export:failure"), nullptr);
  EXPECT_EQ(object->property("count").toInt(), 1);
}
TEST_F(SharingUiTest, OriginalCopyFolderAcceptanceQueuesSelectedFilesWithDecodedLocalPath) {
  QTemporaryDir directory;
  const QString folder = directory.filePath(QString::fromUtf8("Copies #1 % café"));
  ASSERT_TRUE(QDir().mkpath(folder));
  const QStringList sources{"/fake/First #1.mkv", "/fake/Second %.mp4"};
  create("OriginalCopyDialog.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"sourcePaths", sources}});
  ASSERT_TRUE(bool(object));
  ASSERT_TRUE(object->setProperty("parentWindow", QVariant::fromValue(&window)));
  QSignalSpy queued(object.get(), SIGNAL(queued()));
  QSignalSpy failed(object.get(), SIGNAL(copyFailed(QString)));
  QQmlExpression nativeOption(QQmlEngine::contextForObject(object.get()), object.get(), "FolderDialog.DontUseNativeDialog");
  const auto option = nativeOption.evaluate();
  ASSERT_FALSE(nativeOption.hasError());
  ASSERT_TRUE(object->setProperty("options", option));
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("visible").toBool(); }));
  ASSERT_TRUE(object->setProperty("selectedFolder", QUrl::fromLocalFile(folder)));
  // QML URL values do not expose QUrl's C++ toLocalFile() member.
  QQmlExpression oldConversion(QQmlEngine::contextForObject(object.get()), object.get(), "selectedFolder.toLocalFile()");
  oldConversion.evaluate();
  EXPECT_TRUE(oldConversion.hasError());
  QMetaObject::invokeMethod(object.get(), "accepted");
  EXPECT_EQ(exporter.copiedSources, sources);
  EXPECT_EQ(exporter.destination, folder);
  EXPECT_EQ(queued.count(), 1);
  EXPECT_TRUE(failed.isEmpty());
  exporter.copyAccepted = false;
  QMetaObject::invokeMethod(object.get(), "accepted");
  EXPECT_EQ(queued.count(), 1);
  ASSERT_EQ(failed.count(), 1);
  EXPECT_FALSE(failed[0][0].toString().isEmpty());
  QMetaObject::invokeMethod(object.get(), "close");
}
TEST_F(SharingUiTest, OriginalCopyUsesTheSameStackAndTurnsGreenWhenCopied) {
  queue.present = false;
  exporter.queueRows = {
      QVariantMap{{"id", "copy"}, {"sourcePath", "/fake/Original #1.mkv"}, {"active", true},
          {"progress", 0.4}, {"cancelling", false}, {"copyOriginal", true}},
      QVariantMap{{"id", "queued-copy"}, {"sourcePath", "/fake/Next.mkv"}, {"active", false},
          {"progress", 0.0}, {"cancelling", false}, {"copyOriginal", true}}};
  create("ToastStack.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"queue", QVariant::fromValue(&queue)}, {"width", 400}});
  auto *panel = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(panel, nullptr);
  auto *copy = findItem(panel, "toast:export:copy");
  auto *queued = findItem(panel, "toast:export:queued-copy");
  ASSERT_NE(copy, nullptr); ASSERT_NE(queued, nullptr);
  EXPECT_EQ(copy->property("detail").toString(), QString::fromUtf8("Copying · 40%"));
  EXPECT_EQ(queued->property("detail").toString(), "Copy queued");
  auto *cancel = findItem(panel, "toastAction:export:queued-copy");
  ASSERT_NE(cancel, nullptr);
  QMetaObject::invokeMethod(cancel, "clicked");
  EXPECT_EQ(exporter.cancelledJob, "queued-copy");
  exporter.queueRows.removeFirst();
  emit exporter.jobSettled("copy", "/fake/Original #1.mkv", "/shares/Original #1.mkv", {}, false, true);
  emit exporter.changed();
  EXPECT_EQ(findItem(panel, "toast:export:copy"), copy);
  EXPECT_EQ(copy->property("heading").toString(), "Copied");
  EXPECT_EQ(copy->property("phase").toString(), "success");
  EXPECT_EQ(copy->property("statusColor").value<QColor>(), QColor("#4ade80"));
  EXPECT_NE(findItem(panel, "toast:export:queued-copy"), nullptr);
}
TEST_F(SharingUiTest, DialogCanConfigureAnotherExportWhileQueueIsBusy) {
  exporter.active = true;
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"sourcePath", "/fake/Second.mkv"}, {"destination", "/fake/output"}});
  ASSERT_TRUE(bool(object));
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  auto *start = object->findChild<QQuickItem *>("startShareExport");
  ASSERT_NE(start, nullptr);
  EXPECT_TRUE(start->isEnabled());
  QMetaObject::invokeMethod(start, "clicked");
  EXPECT_EQ(exporter.exports, 1);
  EXPECT_TRUE(wait([&] { return !object->property("visible").toBool(); }));
}
TEST_F(SharingUiTest, FolderAcceptanceDecodesLocalUrlAndEnablesExport) {
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"sourcePath", "/fake/Game.mkv"}});
  ASSERT_TRUE(bool(object));
  ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "open"));
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  auto *picker = object->findChild<QObject *>("shareDestinationPicker");
  auto *button = object->findChild<QObject *>("startShareExport");
  ASSERT_NE(picker, nullptr);
  ASSERT_NE(button, nullptr);
  EXPECT_FALSE(button->property("enabled").toBool());
  QTemporaryDir directory;
  const QString folder = directory.filePath(QString::fromUtf8("Share #1 % café"));
  ASSERT_TRUE(QDir().mkpath(folder));
  QQmlExpression nativeOption(QQmlEngine::contextForObject(object.get()), object.get(), "FolderDialog.DontUseNativeDialog");
  const auto option = nativeOption.evaluate();
  ASSERT_FALSE(nativeOption.hasError());
  ASSERT_TRUE(picker->setProperty("options", option)); // Never contact the host portal.
  ASSERT_TRUE(QMetaObject::invokeMethod(picker, "open"));
  ASSERT_TRUE(wait([&] { return picker->property("visible").toBool(); }));
  ASSERT_TRUE(picker->setProperty("selectedFolder", QUrl::fromLocalFile(folder)));
  EXPECT_EQ(picker->property("selectedFolder").toUrl(), QUrl::fromLocalFile(folder));
  ASSERT_TRUE(QMetaObject::invokeMethod(picker, "accepted"));
  EXPECT_EQ(object->property("destination").toString(), folder);
  EXPECT_TRUE(button->property("enabled").toBool());
  auto *pathLabel = object->findChild<QObject *>("shareDestinationPath");
  ASSERT_NE(pathLabel, nullptr);
  EXPECT_EQ(pathLabel->property("text").toString(), folder);
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
  EXPECT_EQ(exporter.destination, folder);
}
TEST_F(SharingUiTest, NamedTracksPreserveOriginalIndicesAndEstimatedLabels) {
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"sourcePath", "/fake/Game.mkv"}});
  ASSERT_TRUE(bool(object));
  ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "open"));
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  auto *audioSection = object->findChild<QObject *>("shareAudioSection");
  ASSERT_NE(audioSection, nullptr);
  audioSection->setProperty("expanded", true);
  exporter.data = {{"tracks", QVariantList{
    QVariantMap{{"index", 1}, {"label", "System"}},
    QVariantMap{{"index", 2}, {"label", "Voice"}, {"defaultExcluded", true}},
    QVariantMap{{"index", 3}, {"label", "Microphone"}, {"inferred", true}}}}};
  emit exporter.changed();
  auto *content = object->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(content, nullptr);
  ASSERT_TRUE(wait([&] { return findItem(content, "shareAudioTrack3") != nullptr; }));
  ASSERT_TRUE(wait([&] { return findItem(content, "shareAudioTrack3")->isVisible(); }));
  auto *mic = findItem(content, "shareAudioTrack3");
  EXPECT_EQ(mic->property("text").toString(), "Microphone (estimated)");
  EXPECT_EQ(findItem(content, "shareAudioTrack1")->property("text").toString(), "System");
  EXPECT_FALSE(findItem(content, "shareAudioTrack2")->isVisible());
  object->setProperty("selectedAudio", QVariantList{1});
  mic->setProperty("checked", true);
  ASSERT_TRUE(QMetaObject::invokeMethod(mic, "toggled"));
  const auto selection = object->property("selectedAudio");
  const auto tracks = selection.canConvert<QJSValue>() ? selection.value<QJSValue>().toVariant().toList() : selection.toList();
  EXPECT_EQ(tracks, (QVariantList{1, 3}));
  EXPECT_EQ(object->property("audioSummary").toString(), "System + Microphone*");
}
TEST_F(SharingUiTest, DefaultViewUsesCollapsedSummariesAndRevealsOptionsOnClick) {
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"sourcePath", "/fake/Game.mkv"}});
  ASSERT_TRUE(bool(object));
  ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "open"));
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  auto *audio = object->findChild<QQuickItem *>("shareAudioSection");
  auto *quality = object->findChild<QQuickItem *>("shareQualitySection");
  ASSERT_NE(audio, nullptr);
  ASSERT_NE(quality, nullptr);
  EXPECT_FALSE(audio->property("expanded").toBool());
  EXPECT_FALSE(quality->property("expanded").toBool());
  EXPECT_EQ(audio->property("description").toString(), "Game + Voice");
  EXPECT_EQ(quality->property("description").toString(), QString::fromUtf8("1080p · High quality"));
  auto *content = object->property("contentItem").value<QQuickItem *>();
  auto *game = findItem(content, "shareAudioTrack1");
  ASSERT_NE(game, nullptr);
  EXPECT_FALSE(game->isVisible());
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, audio->mapToScene(QPointF(30, 25)).toPoint());
  ASSERT_TRUE(wait([&] { return audio->property("expanded").toBool() && game->isVisible(); }));
  QTest::qWait(250);
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, audio->mapToScene(QPointF(30, 25)).toPoint());
  ASSERT_TRUE(wait([&] { return !audio->property("expanded").toBool() && !game->isVisible(); }));
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, quality->mapToScene(QPointF(30, 25)).toPoint());
  EXPECT_TRUE(quality->property("expanded").toBool());
}
TEST_F(SharingUiTest, UndoProgressKeepsTheSameButtonAndRoutesUndo) {
  create("ToastStack.qml", {{"exporter", QVariant::fromValue(&exporter)}, {"queue", QVariant::fromValue(&queue)}, {"width", 400}, {"z", 100}});
  ASSERT_TRUE(bool(object));
  auto *visual = qobject_cast<QQuickItem *>(object.get());
  ASSERT_NE(visual, nullptr);
  // Main places toasts on the Controls overlay; test that actual parent,
  // not merely the unobscured window content item.
  QQmlExpression overlayExpression(QQmlEngine::contextForObject(object.get()), object.get(), "Overlay.overlay");
  auto *overlay = qobject_cast<QQuickItem *>(overlayExpression.evaluate().value<QObject *>());
  ASSERT_NE(overlay, nullptr);
  visual->setParentItem(overlay);
  QCoreApplication::processEvents();
  EXPECT_TRUE(visual->isVisible());
  ASSERT_TRUE(wait([&] { return findItem(visual, "toastAction:delete:one") != nullptr; }));
  auto *button = findItem(visual, "toastAction:delete:one");
  auto *toast = findItem(visual, "toast:delete:one");
  ASSERT_NE(toast, nullptr);
  queue.progress = 0.5;
  ASSERT_TRUE(wait([&] {
    return toast->property("progress").toDouble() == 0.5;
  }));
  EXPECT_EQ(findItem(visual, "toastAction:delete:one"), button);
  EXPECT_GT(object->property("implicitHeight").toDouble(), 20);
  // An unrelated modal dialog must not block this high-z Undo opportunity.
  QQmlComponent modalComponent(&engine);
  modalComponent.setData("import QtQuick\nimport QtQuick.Controls\nPopup { modal: true; dim: true; x: 600; y: 200; width: 200; height: 200; closePolicy: Popup.NoAutoClose }", QUrl());
  ASSERT_TRUE(modalComponent.isReady()) << modalComponent.errorString().toStdString();
  std::unique_ptr<QObject> modal(modalComponent.createWithInitialProperties({{"parent", QVariant::fromValue(window.contentItem())}}));
  ASSERT_NE(modal, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(modal.get(), "open"));
  ASSERT_TRUE(wait([&] { return modal->property("opened").toBool(); }));
  const auto position = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, position);
  QCoreApplication::processEvents();
  EXPECT_EQ(queue.undos, 1);
  QMetaObject::invokeMethod(modal.get(), "close");
}
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen"); qputenv("QT_QUICK_BACKEND", "software");
  QGuiApplication application(argc, argv);
  testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS();
}
#include "test_ShareUi.moc"
