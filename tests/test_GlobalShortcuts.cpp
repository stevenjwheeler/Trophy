/*
 * Trophy - Unit Tests
 * GlobalShortcutManager: catalogue, routing and bound-map logic.
 * Fake request/session state exercises real response handlers without
 * requesting or configuring shortcuts on the user's live portal.
 */

#include <QCoreApplication>
#include <QDBusObjectPath>
#include <QSignalSpy>
#include <gtest/gtest.h>

#include "core/GlobalShortcutManager.h"

using namespace TrophyBackend;

class GlobalShortcutTestEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
  }
};

static auto *env =
    testing::AddGlobalTestEnvironment(new GlobalShortcutTestEnvironment);

class GlobalShortcutTest : public ::testing::Test {
protected:
  static void injectRequest(GlobalShortcutManager &mgr,
                            const QString &session = {}) {
    mgr.m_sessionHandle = session;
    mgr.m_pendingRequestPath = "/org/freedesktop/portal/desktop/request/trophy_test/1";
    mgr.m_requestInFlight = true;
    mgr.m_bindingRequest = !session.isEmpty();
    mgr.m_requestTimeout.start();
  }

  static void expireRequest(GlobalShortcutManager &mgr) {
    mgr.onRequestTimeout();
  }

  static void bindResponse(GlobalShortcutManager &mgr, unsigned int response,
                           const QVariantMap &results = {}) {
    mgr.onBindResponse(response, results);
  }

  static QString session(const GlobalShortcutManager &mgr) {
    return mgr.m_sessionHandle;
  }

  static QString pendingRequest(const GlobalShortcutManager &mgr) {
    return mgr.m_pendingRequestPath;
  }

  static bool timerActive(const GlobalShortcutManager &mgr) {
    return mgr.m_requestTimeout.isActive();
  }

  // Mirrors requestShortcuts' early-return gates without issuing a live
  // CreateSession call (which could open a system hotkey dialog).
  static bool readyForFreshRequest(const GlobalShortcutManager &mgr) {
    return !mgr.m_bound && !mgr.m_requestInFlight && mgr.m_pendingRequestPath.isEmpty() &&
           mgr.m_sessionHandle.isEmpty();
  }

  static void setAvailable(GlobalShortcutManager &mgr, bool available) {
    mgr.setAvailable(available);
  }

  static bool cachedAvailability(const GlobalShortcutManager &mgr) {
    return mgr.m_available;
  }
};

TEST_F(GlobalShortcutTest, EnsureSetupDuringPendingRequestIsSafeNoOp) {
  GlobalShortcutManager mgr;
  injectRequest(mgr);
  const QString pending = pendingRequest(mgr);
  mgr.ensureSetup();
  EXPECT_EQ(pendingRequest(mgr), pending);
  EXPECT_TRUE(timerActive(mgr));
  EXPECT_FALSE(mgr.isBound());
}

TEST_F(GlobalShortcutTest, DisabledManagerSwallowsActivations) {
  GlobalShortcutManager mgr;
  EXPECT_TRUE(mgr.isEnabled());
  QSignalSpy toggleSpy(&mgr,
                       &GlobalShortcutManager::toggleRecordingRequested);
  QSignalSpy clipSpy(&mgr, &GlobalShortcutManager::saveClipRequested);

  QSignalSpy enabledSpy(&mgr, &GlobalShortcutManager::enabledChanged);
  mgr.setEnabled(false);
  EXPECT_FALSE(mgr.isEnabled());
  EXPECT_EQ(enabledSpy.count(), 1);
  mgr.setEnabled(false);
  EXPECT_EQ(enabledSpy.count(), 1); // same value: no repeat signal

  mgr.simulateActivation("toggle-recording");
  mgr.simulateActivation("save-clip");
  EXPECT_EQ(toggleSpy.count(), 0);
  EXPECT_EQ(clipSpy.count(), 0);

  mgr.setEnabled(true);
  mgr.simulateActivation("toggle-recording");
  mgr.simulateActivation("save-clip");
  EXPECT_EQ(toggleSpy.count(), 1);
  EXPECT_EQ(clipSpy.count(), 1);
}

TEST_F(GlobalShortcutTest, CatalogueHasBothActions) {
  const auto defs = GlobalShortcutManager::shortcutDefinitions();
  ASSERT_EQ(defs.size(), 2);
  EXPECT_EQ(defs[0].id, "toggle-recording");
  EXPECT_EQ(defs[0].options.value("preferred_trigger").toString(), "F9");
  EXPECT_FALSE(
      defs[0].options.value("description").toString().isEmpty());
  EXPECT_EQ(defs[1].id, "save-clip");
  EXPECT_EQ(defs[1].options.value("preferred_trigger").toString(), "F10");
}

TEST_F(GlobalShortcutTest, SimulateActivationRoutesById) {
  GlobalShortcutManager mgr;
  QSignalSpy toggleSpy(&mgr,
                       &GlobalShortcutManager::toggleRecordingRequested);
  QSignalSpy clipSpy(&mgr, &GlobalShortcutManager::saveClipRequested);

  mgr.simulateActivation("toggle-recording");
  EXPECT_EQ(toggleSpy.count(), 1);
  EXPECT_EQ(clipSpy.count(), 0);

  mgr.simulateActivation("save-clip");
  EXPECT_EQ(toggleSpy.count(), 1);
  EXPECT_EQ(clipSpy.count(), 1);

  // Unknown ids are ignored.
  mgr.simulateActivation("nope");
  EXPECT_EQ(toggleSpy.count(), 1);
  EXPECT_EQ(clipSpy.count(), 1);
}

TEST_F(GlobalShortcutTest, HandleActivatedRequiresSessionMatch) {
  GlobalShortcutManager mgr; // no session: everything ignored
  QSignalSpy toggleSpy(&mgr,
                       &GlobalShortcutManager::toggleRecordingRequested);
  QSignalSpy clipSpy(&mgr, &GlobalShortcutManager::saveClipRequested);

  mgr.handleActivated(QDBusObjectPath("/some/session"), "toggle-recording");
  mgr.handleActivated(QDBusObjectPath(), "save-clip");
  EXPECT_EQ(toggleSpy.count(), 0);
  EXPECT_EQ(clipSpy.count(), 0);
  EXPECT_FALSE(mgr.isBound());
}

TEST_F(GlobalShortcutTest, BoundMapExtractsTriggers) {
  PortalShortcut a;
  a.id = "toggle-recording";
  a.options["trigger_description"] = "F9";
  PortalShortcut b;
  b.id = "save-clip";
  b.options["trigger_description"] = "Shift+F10";
  const QVariantMap m =
      GlobalShortcutManager::boundMap({a, b});
  EXPECT_EQ(m.value("toggle-recording").toString(), "F9");
  EXPECT_EQ(m.value("save-clip").toString(), "Shift+F10");
  EXPECT_TRUE(GlobalShortcutManager::boundMap({}).isEmpty());
}

TEST_F(GlobalShortcutTest, RemapWhileBoundStillNotifiesUi) {
  GlobalShortcutManager mgr;
  QSignalSpy spy(&mgr, &GlobalShortcutManager::boundChanged);

  auto withTriggers = [](const QString &toggle, const QString &clip) {
    PortalShortcut a;
    a.id = "toggle-recording";
    a.options["trigger_description"] = toggle;
    PortalShortcut b;
    b.id = "save-clip";
    b.options["trigger_description"] = clip;
    return QList<PortalShortcut>{a, b};
  };

  mgr.applyBoundShortcuts(withTriggers("F9", "F10"));
  EXPECT_TRUE(mgr.isBound());
  EXPECT_EQ(spy.count(), 1);

  // Identical list: nothing the UI renders changed.
  mgr.applyBoundShortcuts(withTriggers("F9", "F10"));
  EXPECT_EQ(spy.count(), 1);

  // Remap while still bound: the boolean does not flip, but the UI shows
  // triggers, so this must still notify (was stale until restart).
  mgr.applyBoundShortcuts(withTriggers("F8", "F7"));
  EXPECT_EQ(spy.count(), 2);
  EXPECT_TRUE(mgr.isBound());
  EXPECT_EQ(mgr.boundShortcuts().value("toggle-recording").toString(), "F8");

  // A key being cleared drops back to unbound and notifies.
  mgr.applyBoundShortcuts(withTriggers("", "F7"));
  EXPECT_EQ(spy.count(), 3);
}

TEST_F(GlobalShortcutTest, RequestDuringPendingRequestIsSafeNoOp) {
  GlobalShortcutManager mgr;
  injectRequest(mgr);
  const QString pending = pendingRequest(mgr);
  // Both entry points hit the pending-request gate before any portal call.
  mgr.requestShortcuts();
  mgr.reconfigureShortcuts();
  EXPECT_EQ(pendingRequest(mgr), pending);
  EXPECT_TRUE(mgr.boundShortcuts().isEmpty());
  EXPECT_FALSE(mgr.isBound());
}

TEST_F(GlobalShortcutTest, TimeoutClearsCreateAndBindRequestsForFreshRetry) {
  for (const QString &handle : {QString(), QString("/trophy_test/session")}) {
    SCOPED_TRACE(handle.toStdString());
    GlobalShortcutManager mgr;
    QSignalSpy boundSpy(&mgr, &GlobalShortcutManager::boundChanged);
    QSignalSpy availabilitySpy(&mgr, &GlobalShortcutManager::availabilityChanged);
    injectRequest(mgr, handle);
    ASSERT_FALSE(readyForFreshRequest(mgr));
    ASSERT_TRUE(timerActive(mgr));

    expireRequest(mgr);
    EXPECT_TRUE(pendingRequest(mgr).isEmpty());
    EXPECT_TRUE(session(mgr).isEmpty());
    EXPECT_FALSE(timerActive(mgr));
    EXPECT_TRUE(readyForFreshRequest(mgr));
    EXPECT_EQ(boundSpy.count(), 0);
    EXPECT_EQ(availabilitySpy.count(), 0);

    // A duplicate timeout must be harmless after the request is retired.
    expireRequest(mgr);
    EXPECT_TRUE(readyForFreshRequest(mgr));
    EXPECT_EQ(boundSpy.count(), 0);
    EXPECT_EQ(availabilitySpy.count(), 0);
  }
}

TEST_F(GlobalShortcutTest, CancelledOrFailedBindClearsSessionForFreshRetry) {
  for (unsigned int response : {1u, 2u}) {
    SCOPED_TRACE(response);
    GlobalShortcutManager mgr;
    QSignalSpy toggleSpy(&mgr, &GlobalShortcutManager::toggleRecordingRequested);
    QSignalSpy boundSpy(&mgr, &GlobalShortcutManager::boundChanged);
    const QString oldSession = "/trophy_test/session";
    injectRequest(mgr, oldSession);

    bindResponse(mgr, response);
    EXPECT_TRUE(pendingRequest(mgr).isEmpty());
    EXPECT_TRUE(session(mgr).isEmpty());
    EXPECT_FALSE(timerActive(mgr));
    EXPECT_TRUE(readyForFreshRequest(mgr));
    EXPECT_EQ(boundSpy.count(), 0);
    mgr.handleActivated(QDBusObjectPath(oldSession), "toggle-recording");
    EXPECT_EQ(toggleSpy.count(), 0);
  }
}

TEST_F(GlobalShortcutTest, AvailabilityNotifiesExactlyOncePerFlip) {
  GlobalShortcutManager mgr;
  // Normalize before installing the spy: independent of the live portal.
  setAvailable(mgr, false);
  QSignalSpy spy(&mgr, &GlobalShortcutManager::availabilityChanged);
  setAvailable(mgr, false);
  EXPECT_EQ(spy.count(), 0);
  setAvailable(mgr, true);
  EXPECT_TRUE(cachedAvailability(mgr));
  EXPECT_EQ(spy.count(), 1);
  setAvailable(mgr, true);
  EXPECT_EQ(spy.count(), 1);
  setAvailable(mgr, false);
  EXPECT_FALSE(cachedAvailability(mgr));
  EXPECT_EQ(spy.count(), 2);
  setAvailable(mgr, false);
  EXPECT_EQ(spy.count(), 2);
}

TEST_F(GlobalShortcutTest, EmptyBindResponseSafelyClearsOldBindingsOnce) {
  GlobalShortcutManager mgr;
  PortalShortcut shortcut;
  shortcut.id = "toggle-recording";
  shortcut.options["trigger_description"] = "F8";
  mgr.applyBoundShortcuts({shortcut});
  ASSERT_TRUE(mgr.isBound());
  QSignalSpy spy(&mgr, &GlobalShortcutManager::boundChanged);
  const QString handle = "/trophy_test/session";
  injectRequest(mgr, handle);

  bindResponse(mgr, 0); // successful response missing the shortcuts field
  EXPECT_FALSE(mgr.isBound());
  EXPECT_TRUE(mgr.boundShortcuts().isEmpty());
  EXPECT_TRUE(pendingRequest(mgr).isEmpty());
  EXPECT_FALSE(timerActive(mgr));
  EXPECT_TRUE(session(mgr).isEmpty()); // unusable bind permits a full retry
  EXPECT_TRUE(readyForFreshRequest(mgr));
  EXPECT_EQ(spy.count(), 1);

  injectRequest(mgr, handle);
  bindResponse(mgr, 0, {{"shortcuts", QString("malformed")}});
  EXPECT_FALSE(mgr.isBound());
  EXPECT_TRUE(mgr.boundShortcuts().isEmpty());
  EXPECT_FALSE(timerActive(mgr));
  EXPECT_EQ(spy.count(), 1);
}
