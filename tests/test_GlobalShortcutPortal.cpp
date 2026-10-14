/* Actual GlobalShortcuts D-Bus exchanges on a private dbus-run-session bus.
 * The fake portal never registers keys with a compositor or a live portal.
 */
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QProcess>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <gtest/gtest.h>

#include "core/GlobalShortcutManager.h"

using namespace TrophyBackend;

class FakeGlobalShortcuts : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.GlobalShortcuts")
  Q_PROPERTY(uint version READ version CONSTANT)
public:
  uint version() const { return 1; }
public slots:
  QDBusObjectPath CreateSession(const QVariantMap &) {
    ++m_creates;
    return request("CreateSession");
  }
  QDBusObjectPath BindShortcuts(const QDBusObjectPath &,
                               const QList<PortalShortcut> &shortcuts,
                               const QString &, const QVariantMap &) {
    m_definitions = shortcuts;
    ++m_binds;
    return request("BindShortcuts");
  }
  void ConfigureShortcuts(const QDBusObjectPath &, const QString &,
                          const QVariantMap &) {
    ++m_configures;
    request("ConfigureShortcuts");
  }
  void Configure(const QString &mode) { m_mode = mode; }
  bool HasHeld(const QString &stage) { return m_held.contains(stage); }
  QString HeldPath(const QString &stage) { return m_heldPaths.value(stage); }
  void ReleaseHeld(const QString &stage, bool error) {
    if (!m_held.contains(stage)) return;
    const auto held = m_held.take(stage);
    const QString path = m_heldPaths.take(stage);
    if (error)
      connection().send(held.createErrorReply(QDBusError::Failed, "late method error"));
    else
      connection().send(held.createReply(stage == "ConfigureShortcuts" ? QVariantList{} :
          QVariantList{QVariant::fromValue(QDBusObjectPath(path))}));
  }
  QVariantMap Counts() {
    return {{"create", m_creates}, {"bind", m_binds},
            {"configure", m_configures}, {"definitions", m_definitions.size()}};
  }
  void Respond(const QString &path, uint response, const QVariantMap &results) {
    auto signal = QDBusMessage::createSignal(
        path, "org.freedesktop.portal.Request", "Response");
    signal.setArguments({response, results});
    connection().send(signal);
  }
  void Activate(const QDBusObjectPath &session) {
    auto signal = QDBusMessage::createSignal(
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.GlobalShortcuts", "Activated");
    signal.setArguments({QVariant::fromValue(session), "toggle-recording", 1ull, QVariantMap{}});
    connection().send(signal);
  }
  void Remap(const QDBusObjectPath &session, const QList<PortalShortcut> &shortcuts) {
    auto signal = QDBusMessage::createSignal(
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.GlobalShortcuts", "ShortcutsChanged");
    signal.setArguments({QVariant::fromValue(session), QVariant::fromValue(shortcuts)});
    connection().send(signal);
  }
private:
  QDBusObjectPath request(const QString &stage) {
    if (m_mode == stage + "Hold") {
      setDelayedReply(true);
      m_held.insert(stage, message());
      const QString path = QString("/org/freedesktop/portal/desktop/request/fake/r%1")
                               .arg(++m_requests);
      m_heldPaths.insert(stage, path);
      return QDBusObjectPath("/");
    }
    if (m_mode == stage + "Error") {
      sendErrorReply(QDBusError::Failed, "injected method error");
      return QDBusObjectPath("/");
    }
    if (m_mode == stage + "Empty" || m_mode == stage + "WrongType" ||
        m_mode == stage + "Extra") {
      setDelayedReply(true);
      QVariantList arguments;
      if (m_mode.endsWith("WrongType"))
        arguments = {QString("not a path")};
      else if (m_mode.endsWith("Extra"))
        arguments = {QVariant::fromValue(QDBusObjectPath("/fake/request")), 42};
      connection().send(message().createReply(arguments));
      return QDBusObjectPath("/");
    }
    if (m_mode == stage + "Root")
      return QDBusObjectPath("/");
    return QDBusObjectPath(QString("/org/freedesktop/portal/desktop/request/fake/r%1")
                              .arg(++m_requests));
  }
  QString m_mode;
  int m_requests = 0;
  int m_creates = 0;
  int m_binds = 0;
  int m_configures = 0;
  QList<PortalShortcut> m_definitions;
  QMap<QString, QDBusMessage> m_held;
  QMap<QString, QString> m_heldPaths;
};

// A virtual object counts any introspection. It deliberately rejects it;
// explicit method/property dispatch must still work without consulting XML.
class NoIntrospectionPortal : public QDBusVirtualObject {
public:
  QString introspect(const QString &) const override {
    ++introspections;
    return {};
  }
  bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override {
    const QString method = message.member();
    if (message.interface() == "org.freedesktop.DBus.Introspectable") {
      ++introspections;
      bus.send(message.createErrorReply(QDBusError::Failed, "introspection forbidden"));
    } else if (message.interface() == "org.freedesktop.DBus.Properties" && method == "Get") {
      ++gets;
      if (holdAvailability)
        held = message;
      else
        bus.send(message.createReply({QVariant::fromValue(QDBusVariant(1u))}));
    } else if (method == "Configure") {
      holdAvailability = message.arguments().first().toString() == "HoldAvailability";
      bus.send(message.createReply());
    } else if (method == "ReleaseAvailability") {
      if (held.type() == QDBusMessage::MethodCallMessage) {
        bus.send(held.createReply({QVariant::fromValue(
            QDBusVariant(message.arguments().first()))}));
        held = {};
      }
      bus.send(message.createReply());
    } else if (method == "Counts") {
      bus.send(message.createReply(QVariantList{QVariantMap{{"introspection", introspections},
          {"get", gets}, {"create", creates}, {"bind", binds}, {"configure", configures}}}));
    } else if (method == "CreateSession" || method == "BindShortcuts") {
      method == "CreateSession" ? ++creates : ++binds;
      bus.send(message.createReply({QVariant::fromValue(QDBusObjectPath(
          QString("/org/freedesktop/portal/desktop/request/fake/no_introspection_%1")
              .arg(creates + binds)))}));
    } else if (method == "Respond") {
      auto response = QDBusMessage::createSignal(message.arguments()[0].toString(),
          "org.freedesktop.portal.Request", "Response");
      response.setArguments({message.arguments()[1],
          qdbus_cast<QVariantMap>(message.arguments()[2])});
      bus.send(response);
      bus.send(message.createReply());
    } else if (method == "ConfigureShortcuts") {
      ++configures;
      bus.send(message.createReply());
    } else {
      bus.send(message.createErrorReply(QDBusError::UnknownMethod, "unsupported"));
    }
    return true;
  }
private:
  mutable int introspections = 0;
  int gets = 0, creates = 0, binds = 0, configures = 0;
  bool holdAvailability = false;
  QDBusMessage held;
};

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  qDBusRegisterMetaType<PortalShortcut>();
  qDBusRegisterMetaType<QList<PortalShortcut>>();
  if (app.arguments().contains("--fake-portal")) {
    if (qgetenv("TROPHY_PRIVATE_PORTAL_TESTS") != "1")
      return 2;
    FakeGlobalShortcuts portal;
    NoIntrospectionPortal noIntrospection;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.freedesktop.portal.Desktop"))
      return 2;
    const bool registered = app.arguments().contains("--no-introspection")
        ? bus.registerVirtualObject("/org/freedesktop/portal/desktop", &noIntrospection)
        : bus.registerObject("/org/freedesktop/portal/desktop", &portal,
                             QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties);
    if (!registered)
      return 2;
    return app.exec();
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

class GlobalShortcutPortalTest : public ::testing::Test {
protected:
  QProcess fake;
  virtual QStringList fakeArguments() const { return {"--fake-portal"}; }
  void SetUp() override {
    if (qgetenv("TROPHY_PRIVATE_PORTAL_TESTS") != "1")
      GTEST_SKIP() << "requires the private portal test launcher";
    const QString address = QString::fromUtf8(qgetenv("DBUS_SESSION_BUS_ADDRESS"));
    if (address.isEmpty() || address.contains("/run/user/"))
      GTEST_SKIP() << "requires a private dbus-run-session bus";
    const auto bus = QDBusConnection::sessionBus();
    ASSERT_TRUE(bus.isConnected());
    if (bus.interface()->isServiceRegistered("org.freedesktop.portal.Desktop").value())
      GTEST_SKIP() << "refusing to contact an existing portal";
    fake.start(QCoreApplication::applicationFilePath(), fakeArguments());
    ASSERT_TRUE(fake.waitForStarted(3000));
    ASSERT_TRUE(waitUntil([] {
      return QDBusConnection::sessionBus().interface()->isServiceRegistered(
          "org.freedesktop.portal.Desktop").value();
    }));
    // Even if another process won the name between the check and launch,
    // never let the test body issue calls to anything except our child.
    const auto owner = bus.interface()->serviceOwner("org.freedesktop.portal.Desktop");
    ASSERT_TRUE(owner.isValid());
    const auto pid = bus.interface()->servicePid(owner.value());
    ASSERT_TRUE(pid.isValid());
    ASSERT_EQ(pid.value(), static_cast<uint>(fake.processId()));
  }
  void TearDown() override {
    if (fake.state() != QProcess::NotRunning) {
      fake.terminate();
      if (!fake.waitForFinished(3000)) {
        fake.kill();
        fake.waitForFinished(3000);
      }
    }
  }
  template<class Predicate> static bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
      QCoreApplication::processEvents();
      if (predicate()) return true;
      QThread::msleep(1);
    }
    return false;
  }
  static QDBusMessage control(const QString &method, const QVariantList &args = {}) {
    auto call = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.GlobalShortcuts", method);
    call.setArguments(args);
    return QDBusConnection::sessionBus().call(call, QDBus::Block, 3000);
  }
  static QString pending(const GlobalShortcutManager &manager) {
    return manager.m_pendingRequestPath;
  }
  static QString session(const GlobalShortcutManager &manager) {
    return manager.m_sessionHandle;
  }
  static bool timerActive(const GlobalShortcutManager &manager) {
    return manager.m_requestTimeout.isActive();
  }
  static bool busy(const GlobalShortcutManager &manager) {
    return manager.m_requestInFlight;
  }
  static bool hasHeld(const QString &stage) {
    const auto reply = control("HasHeld", {stage});
    return reply.type() == QDBusMessage::ReplyMessage &&
           !reply.arguments().isEmpty() && reply.arguments().first().toBool();
  }
  static void shortenTimeout(GlobalShortcutManager &manager) {
    QObject::connect(&manager.m_requestTimeout, &QTimer::timeout, &manager,
                     [&manager] { manager.m_requestTimeout.setInterval(10000); },
                     Qt::SingleShotConnection);
    manager.m_requestTimeout.start(1);
  }
  static void deliverQueuedStale(GlobalShortcutManager &manager, const QString &path) {
    const auto stale = QDBusMessage::createSignal(
        path, "org.freedesktop.portal.Request", "Response");
    manager.onSessionResponse(0, {{"session_handle", "/fake/stale"}}, stale);
    manager.onBindResponse(1, {}, stale);
  }
  static QList<PortalShortcut> shortcuts(const QString &trigger = "F8") {
    return {{"toggle-recording", {{"trigger_description", trigger}}},
            {"save-clip", {{"trigger_description", "F7"}}}};
  }
  static QVariantMap bindings() {
    return {{"shortcuts", QVariant::fromValue(shortcuts())}};
  }
  static void respond(const QString &path, uint response, const QVariantMap &results = {}) {
    ASSERT_EQ(control("Respond", {path, response, results}).type(), QDBusMessage::ReplyMessage);
  }
  static void createSession(GlobalShortcutManager &manager, const QString &handle) {
    manager.ensureSetup();
    ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
    const QString create = pending(manager);
    respond(create, 0, {{"session_handle", handle}});
    ASSERT_TRUE(waitUntil([&] {
      return (!pending(manager).isEmpty() && pending(manager) != create) || !busy(manager);
    }));
  }
  static void finishRetry(GlobalShortcutManager &manager) {
    createSession(manager, "/fake/retry");
    ASSERT_FALSE(pending(manager).isEmpty());
    respond(pending(manager), 0, bindings());
    ASSERT_TRUE(waitUntil([&] { return manager.isBound(); }));
    EXPECT_FALSE(timerActive(manager));
    EXPECT_EQ(manager.boundShortcuts().value("toggle-recording").toString(), "F8");
  }
};

class GlobalShortcutNoIntrospectionTest : public GlobalShortcutPortalTest {
protected:
  QStringList fakeArguments() const override { return {"--fake-portal", "--no-introspection"}; }
  static QVariantMap counts() {
    return qdbus_cast<QVariantMap>(control("Counts").arguments().first());
  }
};

TEST_F(GlobalShortcutNoIntrospectionTest, ConstructorGetterAndMethodsNeverIntrospect) {
  GlobalShortcutManager manager;
  ASSERT_TRUE(waitUntil([&] { return manager.isAvailable(); }));
  for (int i = 0; i < 1000; ++i)
    EXPECT_TRUE(manager.isAvailable());
  EXPECT_EQ(counts().value("get").toInt(), 1);
  finishRetry(manager);
  manager.reconfigureShortcuts();
  ASSERT_TRUE(waitUntil([] { return counts().value("configure").toInt() == 1; }));
  EXPECT_EQ(counts().value("introspection").toInt(), 0);
}

TEST_F(GlobalShortcutNoIntrospectionTest, HeldAvailabilityDoesNotBlockOrOverrideSupportedRequest) {
  control("Configure", {"HoldAvailability"});
  GlobalShortcutManager manager;
  bool heartbeat = false;
  QTimer::singleShot(0, [&] { heartbeat = true; });
  ASSERT_TRUE(waitUntil([&] { return heartbeat && counts().value("get").toInt() == 1; }));
  EXPECT_FALSE(manager.isAvailable());
  manager.requestShortcuts();
  ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
  EXPECT_TRUE(manager.isAvailable());
  control("ReleaseAvailability", {0u});
  QCoreApplication::processEvents();
  EXPECT_TRUE(manager.isAvailable());
  EXPECT_EQ(counts().value("introspection").toInt(), 0);
}

TEST_F(GlobalShortcutPortalTest, RealCreateBindRemapAndActivation) {
  GlobalShortcutManager manager;
  QSignalSpy toggle(&manager, &GlobalShortcutManager::toggleRecordingRequested);
  QSignalSpy changed(&manager, &GlobalShortcutManager::boundChanged);
  finishRetry(manager);
  EXPECT_EQ(changed.count(), 1);
  const auto reply = control("Counts");
  ASSERT_EQ(reply.arguments().size(), 1);
  const auto counts = qdbus_cast<QVariantMap>(reply.arguments().first());
  EXPECT_EQ(counts.value("create").toInt(), 1);
  EXPECT_EQ(counts.value("bind").toInt(), 1);
  EXPECT_EQ(counts.value("definitions").toInt(), 2);
  control("Activate", {QVariant::fromValue(QDBusObjectPath("/fake/retry"))});
  ASSERT_TRUE(waitUntil([&] { return toggle.count() == 1; }));
  control("Remap", {QVariant::fromValue(QDBusObjectPath("/fake/retry")),
                     QVariant::fromValue(shortcuts("Ctrl+F8"))});
  ASSERT_TRUE(waitUntil([&] { return changed.count() == 2; }));
  EXPECT_EQ(manager.boundShortcuts().value("toggle-recording").toString(), "Ctrl+F8");
}

TEST_F(GlobalShortcutPortalTest, BindTimeoutThenRetryRejectsOldResponseAndSessionSignals) {
  GlobalShortcutManager manager;
  QSignalSpy toggle(&manager, &GlobalShortcutManager::toggleRecordingRequested);
  createSession(manager, "/fake/old");
  const QString oldBind = pending(manager);
  ASSERT_FALSE(oldBind.isEmpty());
  // ensureSetup must not open ConfigureShortcuts while Bind is pending.
  manager.ensureSetup();
  manager.reconfigureShortcuts();
  const auto counts = qdbus_cast<QVariantMap>(control("Counts").arguments().first());
  EXPECT_EQ(counts.value("configure").toInt(), 0);
  shortenTimeout(manager);
  ASSERT_TRUE(waitUntil([&] { return pending(manager).isEmpty(); }));
  EXPECT_TRUE(session(manager).isEmpty());
  manager.ensureSetup();
  ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
  const QString retryCreate = pending(manager);
  ASSERT_FALSE(retryCreate.isEmpty());
  respond(oldBind, 0, bindings());
  respond(retryCreate, 0, {{"session_handle", "/fake/retry"}});
  ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty() && pending(manager) != retryCreate; }));
  ASSERT_FALSE(pending(manager).isEmpty());
  respond(pending(manager), 0, bindings());
  ASSERT_TRUE(waitUntil([&] { return manager.isBound(); }));
  control("Activate", {QVariant::fromValue(QDBusObjectPath("/fake/old"))});
  control("Remap", {QVariant::fromValue(QDBusObjectPath("/fake/old")),
                     QVariant::fromValue(shortcuts("stale"))});
  control("Activate", {QVariant::fromValue(QDBusObjectPath("/fake/retry"))});
  ASSERT_TRUE(waitUntil([&] { return toggle.count() != 0; }));
  EXPECT_EQ(toggle.count(), 1);
  EXPECT_EQ(manager.boundShortcuts().value("toggle-recording").toString(), "F8");
}

TEST_F(GlobalShortcutPortalTest, CreateTimeoutThenRetryRejectsOldResponse) {
  GlobalShortcutManager manager;
  manager.ensureSetup();
  ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
  const QString oldCreate = pending(manager);
  shortenTimeout(manager);
  ASSERT_TRUE(waitUntil([&] { return pending(manager).isEmpty(); }));
  manager.ensureSetup();
  ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
  const QString retry = pending(manager);
  respond(oldCreate, 0, {{"session_handle", "/fake/old"}});
  respond(retry, 0, {{"session_handle", "/fake/retry"}});
  ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty() && pending(manager) != retry; }));
  EXPECT_EQ(session(manager), "/fake/retry");
  ASSERT_FALSE(pending(manager).isEmpty());
  respond(pending(manager), 0, bindings());
  ASSERT_TRUE(waitUntil([&] { return manager.isBound(); }));
}

TEST_F(GlobalShortcutPortalTest, CancelledCreateAndBindAllowFullRetry) {
  for (const bool bind : {false, true}) {
    for (const uint response : {1u, 2u}) {
      SCOPED_TRACE(bind);
      SCOPED_TRACE(response);
      GlobalShortcutManager manager;
      if (bind) createSession(manager, "/fake/cancelled");
      else manager.ensureSetup();
      ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
      respond(pending(manager), response);
      ASSERT_TRUE(waitUntil([&] { return !busy(manager); }));
      EXPECT_TRUE(session(manager).isEmpty());
      EXPECT_FALSE(timerActive(manager));
      finishRetry(manager);
    }
  }
}

TEST_F(GlobalShortcutPortalTest, MalformedAndErrorMethodRepliesAllowFullRetry) {
  for (const QString &stage : {QString("CreateSession"), QString("BindShortcuts")}) {
    for (const QString &mode : {QString("Empty"), QString("WrongType"),
                                QString("Extra"), QString("Root"), QString("Error")}) {
      SCOPED_TRACE((stage + mode).toStdString());
      control("Configure", {stage + mode});
      GlobalShortcutManager manager;
      if (stage == "BindShortcuts") createSession(manager, "/fake/bad_bind");
      else manager.ensureSetup();
      ASSERT_TRUE(waitUntil([&] { return !busy(manager); }));
      EXPECT_TRUE(pending(manager).isEmpty());
      EXPECT_TRUE(session(manager).isEmpty());
      EXPECT_FALSE(timerActive(manager));
      EXPECT_FALSE(manager.isBound());
      control("Configure", {""});
      finishRetry(manager);
    }
  }
}

TEST_F(GlobalShortcutPortalTest, MissingAndInvalidSessionHandlesAllowRetry) {
  for (const QString &handle : {QString(), QString("/"), QString("not/a/path"),
                                QString("/fake\n"), QString("/fake//session"),
                                QString("/fake-session")}) {
    SCOPED_TRACE(handle.toStdString());
    GlobalShortcutManager manager;
    manager.ensureSetup();
    ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
    respond(pending(manager), 0, {{"session_handle", handle}});
    ASSERT_TRUE(waitUntil([&] { return pending(manager).isEmpty(); }));
    EXPECT_TRUE(session(manager).isEmpty());
    EXPECT_FALSE(timerActive(manager));
    finishRetry(manager);
  }
}

TEST_F(GlobalShortcutPortalTest, EmptyOrMalformedBindingsRetireSessionAndAllowRetry) {
  for (const QVariantMap &results : {
           QVariantMap{}, QVariantMap{{"shortcuts", QString("invalid")}},
           QVariantMap{{"shortcuts", QVariant::fromValue(QList<PortalShortcut>{})}}}) {
    GlobalShortcutManager manager;
    createSession(manager, "/fake/empty_bind");
    ASSERT_FALSE(pending(manager).isEmpty());
    respond(pending(manager), 0, results);
    ASSERT_TRUE(waitUntil([&] { return !busy(manager); }));
    EXPECT_FALSE(manager.isBound());
    ASSERT_TRUE(session(manager).isEmpty());
    finishRetry(manager);
  }
}

TEST_F(GlobalShortcutPortalTest, AlreadyQueuedOldResponseCannotConsumeNewRequest) {
  GlobalShortcutManager manager;
  manager.ensureSetup();
  ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
  const QString old = pending(manager);
  shortenTimeout(manager);
  ASSERT_TRUE(waitUntil([&] { return pending(manager).isEmpty(); }));
  manager.ensureSetup();
  ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
  const QString current = pending(manager);
  // Qt can have queued the old slot before its subscription was disconnected.
  deliverQueuedStale(manager, old);
  EXPECT_EQ(pending(manager), current);
  EXPECT_TRUE(timerActive(manager));
  EXPECT_TRUE(session(manager).isEmpty());
  respond(current, 0, {{"session_handle", "/fake/retry"}});
  ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty() && pending(manager) != current; }));
  ASSERT_FALSE(pending(manager).isEmpty());
  respond(pending(manager), 0, bindings());
  ASSERT_TRUE(waitUntil([&] { return manager.isBound(); }));
}

TEST_F(GlobalShortcutPortalTest, HeldMethodRepliesDoNotBlockAndCannotReplaceRetry) {
  for (const QString &stage : {QString("CreateSession"), QString("BindShortcuts")}) {
    for (const bool lateError : {false, true}) {
      SCOPED_TRACE(stage.toStdString());
      SCOPED_TRACE(lateError);
      control("Configure", {stage + "Hold"});
      GlobalShortcutManager manager;
      manager.ensureSetup();
      ASSERT_TRUE(busy(manager));
      ASSERT_TRUE(timerActive(manager));
      if (stage == "BindShortcuts") {
        ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
        respond(pending(manager), 0, {{"session_handle", "/fake/old"}});
      }
      // The method remains unanswered while both our event loop and the
      // fake child's control interface make progress. No duration assertion.
      bool ticked = false;
      QTimer::singleShot(0, &manager, [&] { ticked = true; });
      ASSERT_TRUE(waitUntil([&] { return ticked && hasHeld(stage); }));
      EXPECT_TRUE(busy(manager));
      EXPECT_TRUE(timerActive(manager));
      EXPECT_TRUE(pending(manager).isEmpty());
      const auto oldPathReply = control("HeldPath", {stage});
      ASSERT_EQ(oldPathReply.arguments().size(), 1);
      const QString oldPath = oldPathReply.arguments().first().toString();
      const auto before = qdbus_cast<QVariantMap>(control("Counts").arguments().first());
      manager.ensureSetup();
      manager.requestShortcuts();
      manager.reconfigureShortcuts();
      const auto after = qdbus_cast<QVariantMap>(control("Counts").arguments().first());
      EXPECT_EQ(after, before);

      if (stage == "BindShortcuts") {
        control("Remap", {QVariant::fromValue(QDBusObjectPath("/fake/old")),
                           QVariant::fromValue(shortcuts("early"))});
        ASSERT_TRUE(waitUntil([&] { return manager.isBound(); }));
      }

      shortenTimeout(manager);
      ASSERT_TRUE(waitUntil([&] { return !busy(manager); }));
      EXPECT_TRUE(session(manager).isEmpty());
      EXPECT_TRUE(pending(manager).isEmpty());
      EXPECT_FALSE(timerActive(manager));
      EXPECT_FALSE(manager.isBound());
      EXPECT_TRUE(manager.boundShortcuts().isEmpty());

      control("Configure", {""});
      manager.ensureSetup();
      ASSERT_TRUE(waitUntil([&] { return !pending(manager).isEmpty(); }));
      const QString retryCreate = pending(manager);
      // This finishes the old call's watcher while the retry is active.
      control("ReleaseHeld", {stage, lateError});
      respond(oldPath, 0, stage == "CreateSession"
          ? QVariantMap{{"session_handle", "/fake/stale"}} : bindings());
      respond(retryCreate, 0, {{"session_handle", "/fake/retry"}});
      ASSERT_TRUE(waitUntil([&] {
        return !pending(manager).isEmpty() && pending(manager) != retryCreate;
      }));
      EXPECT_EQ(session(manager), "/fake/retry");
      respond(pending(manager), 0, bindings());
      ASSERT_TRUE(waitUntil([&] { return manager.isBound(); }));
      EXPECT_FALSE(busy(manager));
      EXPECT_FALSE(timerActive(manager));
    }
  }
}

TEST_F(GlobalShortcutPortalTest, HeldConfigureMethodDoesNotBlockOrDisturbBoundSession) {
  GlobalShortcutManager manager;
  finishRetry(manager);
  control("Configure", {"ConfigureShortcutsHold"});
  manager.reconfigureShortcuts();
  bool ticked = false;
  QTimer::singleShot(0, &manager, [&] { ticked = true; });
  ASSERT_TRUE(waitUntil([&] { return ticked && hasHeld("ConfigureShortcuts"); }));
  EXPECT_TRUE(manager.isBound());
  EXPECT_FALSE(busy(manager));
  EXPECT_FALSE(timerActive(manager));
  EXPECT_EQ(session(manager), "/fake/retry");
  EXPECT_EQ(control("ReleaseHeld", {"ConfigureShortcuts", false}).type(),
            QDBusMessage::ReplyMessage);
}

#include "test_GlobalShortcutPortal.moc"
