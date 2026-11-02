#include <gtest/gtest.h>
#include <QGuiApplication>
#include <QQmlContext>
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
#include <QFont>
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
  int inspections = 0, exports = 0, releases = 0, lastMode = -1, lastHeight = -1;
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
  bool inspecting() const { return scanning; }
  QString error() const { return {}; }
  QString resultPath() const { return result; }
  double progress() const { return 0.25; }
  Q_INVOKABLE bool inspect(const QString &path) {
    ++inspections; source = path; result.clear();
    scanning = path == heldProbe;
    data = {{"tracks", QVariantList{QVariantMap{{"index", 1}, {"label", "Game"}}, QVariantMap{{"index", 2}, {"label", "Voice"}}}},
            {"durationSeconds", 7200.0}, {"videoCodecLabel", "H.264"},
            {"copyable", true}, {"shareable", true},
            {"estimateCopyBase", 0.0017}, {"estimateCopyPerTrack", 0.0100},
            {"estimateConvert", 0.100}};
    if (captureData.contains(path)) data = captureData.value(path);
    emit changed(); return true;
  }
  Q_INVOKABLE bool start(const QString &folder, const QVariantList &tracks,
                        int mode = 0, int height = 0) {
    ++exports; destination = folder; selected = tracks; lastMode = mode; lastHeight = height;
    active = true; emit changed(); return true;
  }
  Q_INVOKABLE bool startBatch(const QString &folder, const QVariantList &captures,
                             int mode = 0, int height = 0) {
    ++exports; destination = folder; batchCaptures = captures; lastMode = mode;
    lastHeight = height; active = true; emit changed(); return true;
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
// Minimal stand-in for RecordingLibrary: the dialog resolves the recording it
// is exporting (name, date, length, thumbnail) from the library by path.
class FakeRecordingLibrary : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList allRecordings MEMBER records CONSTANT)
public:
  QVariantList records;
  Q_INVOKABLE QUrl localFileUrl(const QString &path) const {
    return QUrl::fromLocalFile(path);
  }
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

TEST_F(SharingUiTest, ComboPopupAppearanceTracksVisibilityWithoutChangingSelection) {
  for (const bool voice : {false, true}) {
    SCOPED_TRACE(voice ? "saved voice app" : "ordinary combo");
    create(voice ? "../controls/VoiceAppComboBox.qml" : "../controls/TrophyComboBox.qml",
        voice ? QVariantMap{{"savedApp", "Discord"}, {"x", 50}, {"y", 50}}
              : QVariantMap{{"model", QStringList{"One", "Two"}}, {"currentIndex", 1}, {"x", 50}, {"y", 50}});
    ASSERT_TRUE(object);
    auto *popup = object->property("popup").value<QObject *>();
    auto *indicator = object->property("indicator").value<QQuickItem *>();
    ASSERT_NE(popup, nullptr);
    ASSERT_NE(indicator, nullptr);
    QTest::mouseMove(&window, QPoint(1000, 750));
    ASSERT_TRUE(wait([&] { return !object->property("hovered").toBool() &&
        object->property("currentIndex").toInt() == 1 && indicator->rotation() == 0; }));
    const auto closedColor = indicator->property("color").value<QColor>();
    const auto accent = object->property("_accent").value<QColor>();
    EXPECT_NE(closedColor, accent);
    QSignalSpy activated(object.get(), SIGNAL(activated(int)));
    std::unique_ptr<QSignalSpy> selected;
    if (voice) selected = std::make_unique<QSignalSpy>(object.get(), SIGNAL(appSelected(QString)));
    for (int reopen = 0; reopen < 2; ++reopen) {
      ASSERT_TRUE(QMetaObject::invokeMethod(popup, "open"));
      ASSERT_TRUE(wait([&] { return popup->property("visible").toBool() &&
          indicator->rotation() == 180 && indicator->property("color").value<QColor>() == accent; }));
      if (voice && reopen == 0) {
        object->setProperty("apps", QVariantList{QVariantMap{{"name", "Discord Voice"}, {"binary", "discord"}}});
        ASSERT_TRUE(wait([&] { return object->property("displayText").toString() == "Discord Voice"; }));
      }
      ASSERT_TRUE(QMetaObject::invokeMethod(popup, "close"));
      ASSERT_TRUE(wait([&] { return !popup->property("visible").toBool() &&
          indicator->rotation() == 0 && indicator->property("color").value<QColor>() == closedColor; }));
      EXPECT_EQ(object->property("currentIndex").toInt(), 1);
      EXPECT_TRUE(activated.isEmpty());
      if (voice) {
        EXPECT_EQ(object->property("savedApp").toString(), "Discord");
        EXPECT_TRUE(selected->isEmpty());
      }
    }
    object.reset();
    QCoreApplication::processEvents();
  }
}

TEST_F(SharingUiTest, WindowPickerDividersKeepImplicitSizeAcrossResizeAndReopen) {
  create("WindowPickerDialog.qml", {});
  ASSERT_TRUE(object);
  for (const QSize &size : {QSize(500, 600), QSize(620, 680), QSize(500, 600)}) {
    object->setProperty("width", size.width());
    object->setProperty("height", size.height());
    ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "open"));
    ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
    for (const auto *name : {"windowPickerTopDivider", "windowPickerBottomDivider"}) {
      auto *divider = object->findChild<QQuickItem *>(name);
      ASSERT_NE(divider, nullptr);
      EXPECT_EQ(divider->implicitHeight(), 1);
      ASSERT_TRUE(wait([&] { return divider->height() == 1 && divider->width() > 400; }));
    }
    ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "close"));
    ASSERT_TRUE(wait([&] { return !object->property("visible").toBool(); }));
  }
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

TEST_F(SharingUiTest, BoundSearchDelegatesKeepRolesAcrossModelReplacementAndClicks) {
  create("../controls/TrophySearchBox.qml", {{"width", 400}, {"x", 50}, {"y", 50}});
  ASSERT_TRUE(object);
  auto *popup = object->property("dropdown").value<QObject *>();
  ASSERT_NE(popup, nullptr);
  QSignalSpy selected(object.get(), SIGNAL(resultSelected(QVariant)));
  ASSERT_TRUE(selected.isValid());
  auto replace = [&](bool reverse) {
    QQmlExpression expression(qmlContext(object.get()), object.get(),
        QStringLiteral("searchResults.clear(); searchResults.append(%1); searchResults.append(%2)")
            .arg(reverse ? "{type:'setting',title:'Video',subtitle:'Settings',targetIndex:2,section:'Video Recording'}"
                         : "{type:'clip',title:'Game',subtitle:'Capture',targetIndex:7,section:''}")
            .arg(reverse ? "{type:'clip',title:'Game',subtitle:'Capture',targetIndex:7,section:''}"
                         : "{type:'setting',title:'Video',subtitle:'Settings',targetIndex:2,section:'Video Recording'}"));
    expression.evaluate();
    EXPECT_FALSE(expression.hasError()) << expression.error().toString().toStdString();
  };
  for (const bool reverse : {false, true}) {
    replace(reverse);
    ASSERT_TRUE(QMetaObject::invokeMethod(popup, "open"));
    auto *content = popup->property("contentItem").value<QQuickItem *>();
    ASSERT_NE(content, nullptr);
    ASSERT_TRUE(wait([&] { return popup->property("opened").toBool() &&
        findItem(content, "searchResult1") != nullptr; }));
    auto *row = findItem(content, "searchResult1");
    ASSERT_TRUE(wait([&] { return row->width() > 0 && row->height() == 50; }));
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
        row->mapToScene(QPointF(row->width() / 2, row->height() / 2)).toPoint());
    ASSERT_TRUE(wait([&] { return selected.size() == 1; }));
    const auto value = selected.takeFirst()[0];
    const auto result = value.canConvert<QJSValue>() ? value.value<QJSValue>().toVariant().toMap() : value.toMap();
    EXPECT_EQ(result.value("type").toString(), reverse ? "clip" : "setting");
    EXPECT_EQ(result.value("title").toString(), reverse ? "Game" : "Video");
    EXPECT_EQ(result.value("targetIndex").toInt(), reverse ? 7 : 2);
    EXPECT_EQ(result.value("section").toString(), reverse ? "" : "Video Recording");
    ASSERT_TRUE(QMetaObject::invokeMethod(popup, "close"));
    ASSERT_TRUE(wait([&] { return !popup->property("visible").toBool(); }));
  }
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

TEST_F(SharingUiTest, DetectorVersionsAppearBelowDescriptionsAndNotInStatus) {
  QFile file(QString(SHARING_QML_DIR) + "/../pages/SettingsPage.qml");
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const auto page = QString::fromUtf8(file.readAll());
  const auto start = page.indexOf("component HelperControls:");
  const auto end = page.indexOf("component WarnBanner:", start);
  ASSERT_GE(start, 0); ASSERT_GT(end, start);
  const auto rowStart = page.indexOf("component SettingRow:");
  const auto rowEnd = page.indexOf("component SettingStack:", rowStart);
  ASSERT_GE(rowStart, 0); ASSERT_GT(rowEnd, rowStart);
  QQmlComponent component(&engine);
  component.setData((QStringLiteral(R"(
    import QtQuick
    import QtQuick.Controls
    import QtQuick.Layouts
    import "../styles"
    Item { width: 700; height: 120
  )") + page.mid(start, end - start) + page.mid(rowStart, rowEnd - rowStart) + R"(
    HelperControls { objectName: "badge"; showAction: false
      statusWidth: badgeWidth; text: "Enrolled"
    }
    SettingRow { objectName: "versions"; width: 500
      label: "Vulkan Detection"; help: "Description"
      showVersions: true; bundledVersion: 12; installedVersion: 1
    }
    })").toUtf8(), QUrl::fromLocalFile(QString(SHARING_QML_DIR) + "/DetectorBadgeProbe.qml"));
  ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
  object.reset(component.createWithInitialProperties({{"parent", QVariant::fromValue(window.contentItem())}}));
  ASSERT_TRUE(object) << component.errorString().toStdString();
  auto *badge = object->findChild<QQuickItem *>("badge");
  ASSERT_NE(badge, nullptr);
  EXPECT_EQ(badge->property("text").toString(), "Enrolled");
  auto *versions = object->findChild<QQuickItem *>("versions");
  ASSERT_NE(versions, nullptr);
  EXPECT_EQ(versions->property("versionText").toString(), "Bundled: Version 12");
  versions->setProperty("helperInstalled", true);
  EXPECT_EQ(versions->property("versionText").toString(), "Bundled: Version 12 · Installed: Version 1");
  auto *label = versions->findChild<QQuickItem *>("helperVersions");
  ASSERT_NE(label, nullptr);
  EXPECT_TRUE(label->property("font").value<QFont>().bold());
  versions->setProperty("installedVersion", 0);
  EXPECT_EQ(versions->property("versionText").toString(), "Bundled: Version 12 · Installed: Version unknown");
  badge->setProperty("text", "Installed · Working");
  EXPECT_EQ(badge->property("text").toString(), "Installed · Working");
  EXPECT_GE(badge->implicitWidth(), badge->property("badgeWidth").toDouble());
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
  // Capped to the window, never taller.
  EXPECT_LE(object->property("implicitHeight").toDouble(), 784);
  object->setProperty("destination", output.path());
  object->setProperty("selectedAudio", QVariantList{1});
  auto *button = object->findChild<QObject *>("startShareExport");
  ASSERT_NE(button, nullptr);
  EXPECT_TRUE(button->property("enabled").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(button, "clicked"));
  EXPECT_EQ(exporter.exports, 1);
  EXPECT_EQ(exporter.selected, QVariantList{1});
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
  EXPECT_EQ(exporter.batchCaptures[0].toMap()["sourcePath"].toString(), "/fake/First.mkv");
  EXPECT_EQ(exporter.batchCaptures[0].toMap()["audioStreams"].toList(), QVariantList{2});
  EXPECT_EQ(exporter.batchCaptures[1].toMap()["audioStreams"].toList(), QVariantList{7});
  EXPECT_EQ(exporter.destination, "/fake/output");
  EXPECT_TRUE(wait([&] { return !object->property("visible").toBool(); }));
}
// The panel offers two square, described choices rather than a dropdown, shows
// them only when there is a real choice, and always exposes what the export
// will contain plus how long it will take.
TEST_F(SharingUiTest, OutputOffersDescribedChoicesAndAlwaysShowsTheResult) {
  exporter.captureData["/fake/Game.mkv"] = QVariantMap{
      {"tracks", QVariantList{QVariantMap{{"index", 1}, {"label", "Game"}}}},
      {"durationSeconds", 7200.0},
      {"videoCodec", "hevc"}, {"videoCodecLabel", "HEVC (H.265)"},
      {"copyable", true}, {"shareable", false},
      {"estimateCopyBase", 0.0017}, {"estimateCopyPerTrack", 0.0100},
      {"estimateConvert", 0.100}};
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", "/fake/Game.mkv"}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  ASSERT_TRUE(wait([&] { return object->property("tracksLoaded").toBool(); }));

  // Two described square choices, not a combo box.
  EXPECT_EQ(object->findChild<QObject *>("shareModeBox"), nullptr);
  auto *copy = object->findChild<QQuickItem *>("shareCopyMode");
  auto *convert = object->findChild<QQuickItem *>("shareConvertMode");
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(convert, nullptr);
  EXPECT_EQ(copy->property("title").toString(), "Copy");
  EXPECT_EQ(convert->property("title").toString(), "Convert");
  EXPECT_FALSE(copy->property("subtitle").toString().isEmpty());
  EXPECT_FALSE(convert->property("subtitle").toString().isEmpty());
  // Same corner radius as the app's normal buttons, accent-outlined when chosen.
  EXPECT_GT(copy->property("radius").toDouble(), 0.0);
  EXPECT_EQ(convert->property("selected").toBool(), true);
  EXPECT_EQ(copy->property("selected").toBool(), false);

  // What it will produce is always on screen, in the flat layout.
  auto *produces = object->findChild<QObject *>("shareProduces");
  ASSERT_NE(produces, nullptr);
  EXPECT_TRUE(produces->property("text").toString().contains("H.264"));
  EXPECT_TRUE(produces->property("text").toString().contains("AAC"));
  auto *estimate = object->findChild<QObject *>("shareEstimate");
  ASSERT_NE(estimate, nullptr);
  EXPECT_TRUE(estimate->property("text").toString().contains("12 minutes"));

  // Choosing copy switches the described output to the source codec.
  QMetaObject::invokeMethod(copy, "clicked");
  ASSERT_TRUE(wait([&] { return copy->property("selected").toBool(); }));
  EXPECT_TRUE(produces->property("text").toString().contains("HEVC (H.265)"));
  EXPECT_FALSE(convert->property("selected").toBool());
  QMetaObject::invokeMethod(convert, "clicked");
  ASSERT_TRUE(wait([&] { return convert->property("selected").toBool(); }));
  EXPECT_TRUE(produces->property("text").toString().contains("H.264"));
}

// An H.264 recording defaults to the copy, which is lossless and already
// produces H.264 + AAC. Conversion is still offered - it is the only way to
// resize - and says as much.
TEST_F(SharingUiTest, AlreadyShareableSourceDefaultsToCopyButStillAllowsResizing) {
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", "/fake/Game.mkv"}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  ASSERT_TRUE(wait([&] { return object->property("tracksLoaded").toBool(); }));
  EXPECT_TRUE(object->property("allShareable").toBool());
  // Default is the copy.
  EXPECT_FALSE(object->property("converting").toBool());

  auto *copy = object->findChild<QQuickItem *>("shareCopyMode");
  auto *convert = object->findChild<QQuickItem *>("shareConvertMode");
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(convert, nullptr);
  EXPECT_TRUE(copy->property("selected").toBool());
  // Conversion stays usable, and explains why it usually is not needed.
  EXPECT_FALSE(convert->property("disabled").toBool());
  EXPECT_TRUE(convert->property("enabled").toBool());
  const QString subtitle = convert->property("subtitle").toString();
  EXPECT_TRUE(subtitle.contains("conversion is not needed")) << subtitle.toStdString();
  EXPECT_TRUE(subtitle.contains("resize")) << subtitle.toStdString();

  // Selecting it switches the export to conversion and reveals the resolution.
  QMetaObject::invokeMethod(convert, "clicked");
  ASSERT_TRUE(wait([&] { return object->property("converting").toBool(); }));
  auto *start = object->findChild<QObject *>("startShareExport");
  ASSERT_NE(start, nullptr);
  object->setProperty("exportHeight", 720);
  QMetaObject::invokeMethod(start, "clicked");
  EXPECT_EQ(exporter.exports, 1);
  EXPECT_EQ(exporter.lastMode, 1);
  EXPECT_EQ(exporter.lastHeight, 720);
}

// HEVC/AV1 cannot be copied into something a phone will play, so conversion is
// preselected; the plain copy stays available for people who want it.
TEST_F(SharingUiTest, NonShareableSourceDefaultsToConvertAndStillOffersCopy) {
  exporter.captureData["/fake/Game.mkv"] = {
      {"tracks", QVariantList{QVariantMap{{"index", 1}, {"label", "Game"}}}},
      {"durationSeconds", 7200.0},
      {"videoCodec", "hevc"}, {"videoCodecLabel", "HEVC (H.265)"},
      {"copyable", true}, {"shareable", false},
      {"estimateCopyBase", 0.0017}, {"estimateCopyPerTrack", 0.0100},
      {"estimateConvert", 0.100}};
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", "/fake/Game.mkv"}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  ASSERT_TRUE(wait([&] { return object->property("tracksLoaded").toBool(); }));
  // The row lives inside a collapsed section card, so its effective
  // visibility is always false here; assert the property that drives it.
  auto *copyCard = object->findChild<QQuickItem *>("shareCopyMode");
  auto *convertCard = object->findChild<QQuickItem *>("shareConvertMode");
  ASSERT_NE(copyCard, nullptr);
  ASSERT_NE(convertCard, nullptr);
  EXPECT_FALSE(convertCard->property("disabled").toBool());
  EXPECT_FALSE(copyCard->property("disabled").toBool());
  // Default is conversion, and the resolution choice appears with it.
  ASSERT_TRUE(wait([&] { return object->property("exportMode").toInt() == 1; }));
  EXPECT_TRUE(object->property("converting").toBool());
  auto *start = object->findChild<QObject *>("startShareExport");
  ASSERT_NE(start, nullptr);
  object->setProperty("exportHeight", 1080);
  QMetaObject::invokeMethod(start, "clicked");
  EXPECT_EQ(exporter.exports, 1);
  EXPECT_EQ(exporter.lastMode, 1);
  EXPECT_EQ(exporter.lastHeight, 1080);

  // The plain copy is still selectable for this file.
  exporter.active = false;
  emit exporter.changed();
  object->setProperty("destination", "/fake/output");
  object->setProperty("exportMode", 0);
  QMetaObject::invokeMethod(start, "clicked");
  EXPECT_EQ(exporter.exports, 2);
  EXPECT_EQ(exporter.lastMode, 0);
  EXPECT_EQ(exporter.lastHeight, 0);
}

// Choosing three tracks and/or a conversion used to be blind: the dialog said
// "a few minutes" at best. It now shows an estimate for the current selection
// so nobody starts a ten-minute job expecting seconds.
TEST_F(SharingUiTest, EstimateFollowsModeAndSelectedTrackCount) {
  // HEVC source, so both modes are selectable and the conversion estimate is
  // reachable (an H.264 source is copy-only by design).
  exporter.captureData["/fake/Game.mkv"] = {
      {"tracks", QVariantList{QVariantMap{{"index", 1}, {"label", "Game"}}}},
      {"durationSeconds", 7200.0},
      {"videoCodec", "hevc"}, {"videoCodecLabel", "HEVC (H.265)"},
      {"copyable", true}, {"shareable", false},
      {"estimateCopyBase", 0.0017}, {"estimateCopyPerTrack", 0.0100},
      {"estimateConvert", 0.100}};
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", "/fake/Game.mkv"}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  ASSERT_TRUE(wait([&] { return object->property("tracksLoaded").toBool(); }));
  auto *estimate = object->findChild<QObject *>("shareEstimate");
  ASSERT_NE(estimate, nullptr);

  // One copied track of a two-hour recording: seconds.
  object->setProperty("exportMode", 0);
  object->setProperty("selectedAudio", QVariantList{1});
  ASSERT_TRUE(wait([&] { return !estimate->property("text").toString().isEmpty(); }));
  EXPECT_EQ(estimate->property("text").toString(), "Estimated time to complete export: about 10 seconds");

  // Three tracks means mixing, which needs an audio encode.
  object->setProperty("selectedAudio", QVariantList{1, 2, 3});
  ASSERT_TRUE(wait([&] { return estimate->property("text").toString().contains("min"); }));
  EXPECT_EQ(estimate->property("text").toString(), "Estimated time to complete export: about 3 minutes");

  // Conversion re-encodes the video: minutes, and it says so.
  object->setProperty("exportMode", 1);
  ASSERT_TRUE(wait([&] { return estimate->property("text").toString().contains("12 minutes"); }));
  EXPECT_TRUE(estimate->property("text").toString().contains("hardware encoder"));
}

TEST_F(SharingUiTest, CopySectionExplainsMergedAudioAndWarnsAboutNonH264Codecs) {
  exporter.captureData["/fake/Game.mkv"] = QVariantMap{{"tracks", QVariantList{}},
      {"videoCodec", "hevc"}, {"videoCodecLabel", "HEVC (H.265)"},
      {"copyable", true}, {"shareable", false}};
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", "/fake/Game.mkv"}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  auto *produces = object->findChild<QObject *>("shareProduces");
  auto *note = object->findChild<QQuickItem *>("shareCodecNote");
  ASSERT_NE(produces, nullptr);
  ASSERT_NE(note, nullptr);
  ASSERT_TRUE(wait([&] { return object->property("tracksLoaded").toBool(); }));

  // HEVC defaults to conversion, so the summary describes H.264 output and the
  // codec warning does not apply.
  EXPECT_TRUE(object->property("converting").toBool());
  EXPECT_TRUE(produces->property("text").toString().contains("H.264"));
  EXPECT_FALSE(note->isVisible());

  // Choosing the copy brings the warning back.
  object->setProperty("exportMode", 0);
  ASSERT_TRUE(wait([&] { return !object->property("converting").toBool(); }));
  EXPECT_TRUE(object->property("codecWarning").toBool());
  ASSERT_TRUE(wait([&] { return note->isVisible(); }));
  // Friendly labels, not raw FFmpeg names.
  EXPECT_TRUE(note->property("text").toString().contains("HEVC (H.265)"));
  EXPECT_FALSE(note->property("text").toString().contains("hevc"));
  // A copy keeps the source codec, which is what the warning is about.
  EXPECT_TRUE(produces->property("text").toString().contains("HEVC (H.265)"));
  EXPECT_TRUE(note->property("text").toString().contains("HEVC (H.265)"));
  QMetaObject::invokeMethod(object->findChild<QObject *>("startShareExport"), "clicked");
  EXPECT_EQ(exporter.exports, 1);
  EXPECT_EQ(exporter.lastMode, 0);
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
  exporter.data = {{"tracks", QVariantList{
    QVariantMap{{"index", 1}, {"label", "System"}},
    QVariantMap{{"index", 2}, {"label", "Voice"}, {"defaultExcluded", true}},
    QVariantMap{{"index", 3}, {"label", "Microphone"}, {"inferred", true}}}}};
  emit exporter.changed();
  object->setProperty("audioExpanded", true);
  auto *content = object->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(content, nullptr);
  ASSERT_TRUE(wait([&] { return findItem(content, "shareAudioTrack3") != nullptr; }));
  ASSERT_TRUE(wait([&] { return findItem(content, "shareAudioTrack3")->isVisible(); }));
  auto *mic = findItem(content, "shareAudioTrack3");
  EXPECT_EQ(mic->property("text").toString(), "Microphone (estimated)");
  EXPECT_EQ(findItem(content, "shareAudioTrack1")->property("text").toString(), "System");
  EXPECT_FALSE(findItem(content, "shareAudioTrack2")->isVisible());
  object->setProperty("selectedAudio", QVariantList{1});
  // Each source is a label plus a switch now.
  auto *micSwitch = findItem(content, "shareAudioSwitch3");
  ASSERT_NE(micSwitch, nullptr);
  micSwitch->setProperty("checked", true);
  ASSERT_TRUE(QMetaObject::invokeMethod(micSwitch, "toggled"));
  const auto selection = object->property("selectedAudio");
  const auto tracks = selection.canConvert<QJSValue>() ? selection.value<QJSValue>().toVariant().toList() : selection.toList();
  EXPECT_EQ(tracks, (QVariantList{1, 3}));
  EXPECT_EQ(object->property("audioSummary").toString(), "System + Microphone*");
}
// Two choices, a compact resolution row, one line of audio summary, a details
// box and the destination - nothing dominating the panel.
TEST_F(SharingUiTest, PanelStaysCompactUntilAudioIsOpened) {
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", "/fake/Game.mkv"}, {"destination", "/fake/output"}});
  ASSERT_TRUE(bool(object));
  ASSERT_TRUE(QMetaObject::invokeMethod(object.get(), "open"));
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  ASSERT_TRUE(wait([&] { return object->property("tracksLoaded").toBool(); }));
  auto *content = object->property("contentItem").value<QQuickItem *>();
  ASSERT_NE(content, nullptr);
  for (const char *name : {"shareCopyMode", "shareConvertMode", "shareAudioToggle",
                           "shareProduces", "shareEstimate", "shareDestinationPath"})
    EXPECT_TRUE(findItem(content, name) != nullptr) << name;
  // Audio is one summary row until asked for.
  EXPECT_FALSE(object->property("audioExpanded").toBool());
  auto *game = findItem(content, "shareAudioTrack1");
  ASSERT_NE(game, nullptr);
  EXPECT_FALSE(game->isVisible());
  auto *toggle = findItem(content, "shareAudioToggle");
  ASSERT_NE(toggle, nullptr);
  QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier,
      toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint());
  ASSERT_TRUE(wait([&] { return object->property("audioExpanded").toBool(); }));
  ASSERT_TRUE(wait([&] { return game->isVisible(); }));
  // Save to is a card of its own, and the produced/estimate box sits under it.
  auto *saveTo = findItem(content, "shareSaveToCard");
  auto *details = findItem(content, "shareExportDetails");
  auto *audioCard = findItem(content, "shareAudioCard");
  ASSERT_NE(saveTo, nullptr);
  ASSERT_NE(details, nullptr);
  ASSERT_NE(audioCard, nullptr);
  // Both are one-row cards: thin, and close enough in height to read as a pair.
  EXPECT_LT(audioCard->height(), 52.0);
  EXPECT_LT(saveTo->height(), 52.0);
  EXPECT_NEAR(audioCard->height(), saveTo->height(), 0.5);
  // Same width and same left edge as the result box below them.
  EXPECT_NEAR(audioCard->width(), saveTo->width(), 0.5);
  EXPECT_NEAR(details->width(), saveTo->width(), 0.5);
  EXPECT_NEAR(audioCard->mapToItem(content, QPointF()).x(),
              saveTo->mapToItem(content, QPointF()).x(), 0.5);
  EXPECT_GT(details->mapToItem(content, QPointF()).y(),
            saveTo->mapToItem(content, QPointF()).y());
  // Converting reveals a resolution card shaped like the other two.
  object->setProperty("exportMode", 1);
  ASSERT_TRUE(wait([&] { return object->property("converting").toBool(); }));
  auto *resolutionCard = findItem(content, "shareResolutionCard");
  ASSERT_NE(resolutionCard, nullptr);
  ASSERT_TRUE(wait([&] { return resolutionCard->width() > 0; }));
  EXPECT_NEAR(resolutionCard->width(), saveTo->width(), 0.5);
  EXPECT_NEAR(resolutionCard->height(), saveTo->height(), 0.5);
  // The picker is plain text in the card; only the folder action is a segment.
  auto *heightBox = findItem(content, "shareHeightBox");
  auto *folderSegment = findItem(content, "shareChangeFolder");
  ASSERT_NE(heightBox, nullptr);
  ASSERT_NE(folderSegment, nullptr);
  EXPECT_NEAR(folderSegment->height(), saveTo->height(), 2.0);
  EXPECT_EQ(findItem(content, "shareResolutionSegment"), nullptr);
  // The picker is only as wide as its value, so the popup carries its own
  // width - otherwise every item is elided to "Origi...".
  auto *popup = heightBox->property("popup").value<QObject *>();
  ASSERT_NE(popup, nullptr);
  EXPECT_GE(popup->property("width").toDouble(), 150.0);
  // The rule sits between the two choices and the cards, so resolution and
  // audio are adjacent.
  auto *divider = findItem(content, "shareDivider");
  auto *convertCard = findItem(content, "shareConvertMode");
  ASSERT_NE(divider, nullptr);
  ASSERT_NE(convertCard, nullptr);
  const double dividerY = divider->mapToItem(content, QPointF()).y();
  EXPECT_GT(dividerY, convertCard->mapToItem(content, QPointF()).y());
  EXPECT_LT(dividerY, resolutionCard->mapToItem(content, QPointF()).y());
  // No section cards remain.
  EXPECT_EQ(object->findChild<QObject *>("shareCopySection"), nullptr);
  EXPECT_EQ(object->findChild<QObject *>("shareAudioSection"), nullptr);
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

// Two differently sized boxes look like a mistake; the pair takes the height
// of the taller one whatever its text happens to wrap to.
TEST_F(SharingUiTest, ModeChoicesShareOneHeight) {
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", "/fake/Game.mkv"}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  ASSERT_TRUE(wait([&] { return object->property("tracksLoaded").toBool(); }));
  auto *copy = object->findChild<QQuickItem *>("shareCopyMode");
  auto *convert = object->findChild<QQuickItem *>("shareConvertMode");
  ASSERT_NE(copy, nullptr);
  ASSERT_NE(convert, nullptr);
  EXPECT_GT(copy->height(), 0.0);
  EXPECT_NEAR(copy->height(), convert->height(), 0.5)
      << "the two choices must read as a pair: " << copy->height()
      << " vs " << convert->height();
}

// The header names what is being exported rather than echoing a file name.
TEST_F(SharingUiTest, HeaderNamesTheRecordingWithDateAndLength) {
  FakeRecordingLibrary library;
  QTemporaryDir art;
  ASSERT_TRUE(art.isValid());
  const QString thumb = art.filePath("thumb.png");
  QImage frame(32, 18, QImage::Format_RGB32);
  frame.fill(Qt::darkCyan);
  ASSERT_TRUE(frame.save(thumb));
  const QString path = "/fake/Lethal Company_2026-11-02_16-21-50.mkv";
  library.records = {QVariantMap{
      {"filePath", path},
      {"gameName", "Lethal Company"},
      {"date", QDateTime(QDate(2026, 11, 2), QTime(16, 21))},
      {"duration", qint64(754)},
      {"thumbnailPath", thumb}}};
  engine.rootContext()->setContextProperty("recordingLibrary", &library);

  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", path}, {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  EXPECT_EQ(object->property("title").toString(), "Exporting Lethal Company");
  const QString detail = object->property("recordingDetail").toString();
  EXPECT_TRUE(detail.contains("2026")) << detail.toStdString();
  EXPECT_TRUE(detail.contains("16:21")) << detail.toStdString();
  EXPECT_TRUE(detail.contains("12:34")) << detail.toStdString(); // 754 s
  engine.rootContext()->setContextProperty("recordingLibrary", QVariant());
}

// An untracked file still gets a readable title and the file name as detail.
TEST_F(SharingUiTest, HeaderFallsBackToTheFileNameWhenUntracked) {
  create("ShareExportDialog.qml", {{"exporter", QVariant::fromValue(&exporter)},
      {"sourcePath", "/fake/Manual_Recording_2026-11-02_16-21-50.mkv"},
      {"destination", "/fake/output"}});
  ASSERT_TRUE(object);
  QMetaObject::invokeMethod(object.get(), "open");
  ASSERT_TRUE(wait([&] { return object->property("opened").toBool(); }));
  EXPECT_EQ(object->property("title").toString(), "Exporting Manual Recording");
  EXPECT_TRUE(object->property("recordingDetail").toString().endsWith(".mkv"));
}
