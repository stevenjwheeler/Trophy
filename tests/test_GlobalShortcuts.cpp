/*
 * Trophy - Unit Tests
 * GlobalShortcutManager: catalogue, routing and bound-map logic.
 * No portal needed (D-Bus calls are only made on requestShortcuts).
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

class GlobalShortcutTest : public ::testing::Test {};

TEST_F(GlobalShortcutTest, EnsureSetupWithoutPortalIsSafeNoOp) {
  GlobalShortcutManager mgr; // no portal here: neither path can act
  mgr.ensureSetup();
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

TEST_F(GlobalShortcutTest, RequestWithoutPortalIsSafeNoOp) {
  GlobalShortcutManager mgr;
  // Must never crash or emit, portal or not.
  mgr.requestShortcuts();
  mgr.reconfigureShortcuts(); // no session: falls back to bind attempt
  EXPECT_TRUE(mgr.boundShortcuts().isEmpty());
  EXPECT_FALSE(mgr.isBound());
}
