/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "detection/GameDetectionService.h"
#include "detection/GameWhitelist.h"
#include "detection/LegacyDetectionCleanup.h"
#include "audio/AudioApplicationRegistry.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QSettings>
#include <QScopeGuard>
#include <QSaveFile>
#include <functional>
#include <gtest/gtest.h>
#include <unistd.h>
#include <sys/wait.h>

namespace TrophyBackend {
class GameDetectionServiceTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "process-detection-test";
      static char *argv[] = {name, nullptr};
      static QCoreApplication app(argc, argv);
    }
    QStandardPaths::setTestModeEnabled(true);
  }
  static ProcessInfo process(qint64 pid, QString exe, qint64 parent = 1,
                             uint steam = 0) {
    ProcessInfo p;
    p.pid = pid;
    p.start = pid * 10;
    p.parent = parent;
    p.executable = exe;
    p.arguments = {exe};
    p.steamId = steam;
    return p;
  }
  static void apply(GameDetectionService &service,
                    const ProcessCensus::Snapshot &ps) {
    service.apply(ps);
  }
  static void define(GameDetectionService &service, const QString &key,
                     const QString &name) {
    service.m_whitelist->addToWhitelist(key, name);
  }
  static QSet<QString> desktopApplications(const QStringList &roots) {
    return GameDetectionService::desktopApplicationExecutables(roots);
  }
  static void setApplications(GameDetectionService &service, const QSet<QString> &apps) {
    service.m_applicationExecutables = apps;
  }
  static void setCatalogue(GameDetectionService &service,
                            const QList<LauncherGame> &games) {
    service.m_catalogue = games;
  }
  static void setRecentGames(GameDetectionService &service, const QStringList &keys) {
    service.m_recentKeys = keys;
    service.m_recentGames = keys;
    service.rebuildModels();
  }
  static void publishCatalogue(GameDetectionService &service, const QList<LauncherGame> &games) {
    GameDetectionService::ScanResult result;
    result.catalogueUpdated = true;
    result.libraryGeneration = service.m_libraryGeneration;
    result.catalogue = games;
    service.applyScanResult(std::move(result));
  }
  static void writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
  }
  static ProcessInfo inspectWine(const QString &root, const QString &prefix,
                                 const QStringList &arguments) {
    const auto base = root + "/proc/31";
    EXPECT_TRUE(QDir().mkpath(base));
    writeFile(root + "/wine64-preloader", "fixture");
    if (!QFileInfo(base + "/exe").isSymLink()) {
      EXPECT_TRUE(QFile::link(root + "/wine64-preloader", base + "/exe"));
      EXPECT_TRUE(QFile::link(root, base + "/cwd"));
    }
    QStringList fields{"S", "1"};
    while (fields.size() < 20)
      fields.append("0");
    fields[19] = "310";
    writeFile(base + "/stat",
              ("31 (wine64-preloader) " + fields.join(' ')).toUtf8());
    writeFile(base + "/cmdline",
              arguments.join(QChar('\0')).toLocal8Bit() + '\0');
    writeFile(base + "/environ", ("WINEPREFIX=" + prefix).toLocal8Bit() + '\0' +
                                     "SteamAppId=1973530" + '\0');
    return ProcessCensus::inspect(31, root + "/proc");
  }
  // Exercise the scheduler and real local file notifications without starting
  // /proc/audio workers or connecting to the user's desktop services.
  static void readyLibraries(GameDetectionService &service, const QStringList &paths = {}) {
    service.m_running = true;
    service.m_libraryRefresh = false;
    service.m_libraryScanning = false;
    service.updateLibraryWatches(paths);
  }
  static bool waitFor(const std::function<bool()> &condition, int timeout = 2000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeout) {
      QCoreApplication::processEvents();
      QThread::msleep(5);
    }
    return condition();
  }
  static void clearRefresh(GameDetectionService &service) {
    service.m_libraryRefresh = false;
    service.m_libraryDebounce.stop();
  }
  static void watch(GameDetectionService &service, const QStringList &paths) {
    service.updateLibraryWatches(paths);
  }
  static void census(GameDetectionService &service, const ProcessCensus::Snapshot &snapshot,
                     bool catalogueUpdated = false) {
    GameDetectionService::ScanResult result;
    result.processes = snapshot;
    result.catalogueUpdated = catalogueUpdated;
    result.libraryGeneration = service.m_libraryGeneration;
    if (catalogueUpdated)
      result.catalogue = service.m_catalogue;
    service.applyScanResult(std::move(result));
  }
  static bool watchPending(const GameDetectionService &service) {
    return service.m_libraryDebounce.isActive();
  }
  static int watchDelay(const GameDetectionService &service) {
    return service.m_libraryDebounce.remainingTime();
  }
  static void publishLibraryStatus(GameDetectionService &service, const QVariantList &status,
                                   bool stale = false) {
    GameDetectionService::ScanResult result;
    result.catalogueUpdated = true;
    result.libraryGeneration = service.m_libraryGeneration + (stale ? 1 : 0);
    result.libraryStatus = status;
    result.processes = service.m_processes;
    service.applyScanResult(std::move(result));
  }
  static bool fallbackWithoutCensus(GameDetectionService &service) {
    const bool productionInterval = service.m_libraryFallback.interval() == 60000;
    service.m_libraryFallback.start(25);
    return productionInterval;
  }
  static bool noWorker(const GameDetectionService &service) { return !service.m_scan.valid(); }
  static void publishAudio(AudioApplicationRegistry &registry, const QVariantList &rows) {
    registry.m_running = true;
    registry.publish(registry.m_generation, rows, true);
  }
  static void checkStaleAudio(GameDetectionService &service) {
    GameDetectionService::ScanResult result;
    result.audioRevision = service.m_audioRevision;
    result.audioStreams = {QVariantMap{{"nodeId", 42}, {"gameInstance", "old-owner"}}};
    ++service.m_audioRevision;
    service.applyScanResult(result);
    EXPECT_TRUE(service.m_audioStreams.isEmpty());
    result.audioRevision = service.m_audioRevision;
    service.applyScanResult(result);
    EXPECT_EQ(service.m_audioStreams.size(), 1);
  }
  static void checkQueuedRefreshResult(GameDetectionService &service) {
    service.m_libraryScanning = true;
    GameDetectionService::ScanResult result;
    result.catalogueUpdated = true;
    result.libraryGeneration = service.m_libraryGeneration;
    result.catalogue.append({123, "Queued result", "/games/queued"});
    service.rescanGameLibraries();
    service.rescanGameLibraries();
    service.applyScanResult(result);
    ASSERT_EQ(service.m_catalogue.size(), 1);
    EXPECT_EQ(service.m_catalogue[0].name, "Queued result");
    EXPECT_TRUE(service.m_libraryRefresh); // One follow-up, no lost invalidation.
    EXPECT_FALSE(service.m_libraryScanning);
    ++service.m_libraryGeneration; // A changed location invalidates the old result.
    result.catalogue[0].name = "Stale result";
    service.applyScanResult(std::move(result));
    EXPECT_EQ(service.m_catalogue[0].name, "Queued result");
  }
};

TEST_F(GameDetectionServiceTest, ApprovalChangesKeepCustomGroupAndSortReadyFirst) {
  GameDetectionService service;
  const QString first = "approval-sort-first", middle = "approval-sort-middle",
                last = "approval-sort-last";
  setRecentGames(service, {first, middle, last});
  auto *model = service.gamesModel();
  auto *filtered = qobject_cast<GameListFilterModel *>(model);
  ASSERT_NE(filtered, nullptr);
  filtered->setFilterText("approval-sort");
  const auto keys = [&] {
    QStringList result;
    for (int row = 0; row < model->rowCount(); ++row)
      result.append(model->data(model->index(row, 0), GameListModel::KeyRole).toString());
    return result;
  };
  service.setTokenGames({first, last});
  EXPECT_EQ(keys(), (QStringList{first, last, middle}));
  EXPECT_EQ(model->data(model->index(0, 0), GameListModel::SectionRole).toString(),
            "Custom Games");
  EXPECT_EQ(model->data(model->index(1, 0), GameListModel::SectionRole).toString(),
            "Custom Games");
  EXPECT_EQ(model->data(model->index(2, 0), GameListModel::SectionRole).toString(), "Custom Games");
  service.setTokenGames({middle, last});
  EXPECT_EQ(keys(), (QStringList{last, middle, first}));
  service.setTokenGames({first, middle, last});
  EXPECT_EQ(keys(), (QStringList{first, last, middle}));
  service.setTokenGames({});
  EXPECT_EQ(keys(), (QStringList{first, last, middle}));
}

TEST_F(GameDetectionServiceTest, PrelistsLibrariesWithoutApprovalAndKeepsManualRulesCustom) {
  GameDetectionService service;
  const QString native = "exe:/group-test/native";
  const QString custom = "exe:/group-test/manual";
  setCatalogue(service, {
      {424242, "Library Steam Game", "/group-test/steam", {}, {}, false, {"Steam"}},
      {0, "Library Native Game", "/group-test", "/group-test/native", {}, false, {"Heroic (GOG)", "Lutris"}},
      {0, "Library Native Game", "/group-test", "/group-test/native", {}, false, {"Heroic (GOG)"}},
      {0, "Manual Game", "/group-test", "/group-test/manual", {}, false, {"Lutris"}},
      {1, "Runtime Tool", "/group-test/tool", {}, {}, false, {"Steam"}, true}});
  service.addCustomGame(custom, "Manual Game", false);
  setRecentGames(service, {});
  auto *model = service.gamesModel();
  auto *filtered = qobject_cast<GameListFilterModel *>(model);
  ASSERT_NE(filtered, nullptr);
  filtered->setFilterText("Game");
  filtered->setShowDefaultEntries(false);
  // Only fixture rows; existing isolated test data may contain other game rules.
  QMap<QString, QModelIndex> rows;
  for (int row = 0; row < model->rowCount(); ++row) {
    const auto index = model->index(row, 0);
    rows.insert(index.data(GameListModel::KeyRole).toString(), index);
  }
  ASSERT_TRUE(rows.contains("steam_app_424242"));
  ASSERT_TRUE(rows.contains(native));
  ASSERT_TRUE(rows.contains(custom));
  EXPECT_EQ(rows["steam_app_424242"].data(GameListModel::SectionRole).toString(), "Steam");
  EXPECT_EQ(rows[native].data(GameListModel::SectionRole).toString(), "Heroic");
  EXPECT_EQ(rows[custom].data(GameListModel::SectionRole).toString(), "Custom Games");
  EXPECT_TRUE(rows[custom].data(GameListModel::LibraryRole).toString().isEmpty());
  EXPECT_FALSE(rows[native].data(GameListModel::HasTokenRole).toBool());
  EXPECT_FALSE(service.whitelist().contains(native));
  EXPECT_FALSE(rows.contains("steam_app_1"));
  filtered->setShowDefaultEntries(true);
  filtered->setFilterText("Runtime Tool");
  ASSERT_EQ(model->rowCount(), 1);
  EXPECT_TRUE(model->index(0, 0).data(GameListModel::DefaultRole).toBool());
  EXPECT_TRUE(model->index(0, 0).data(GameListModel::SupportToolRole).toBool());
  EXPECT_EQ(model->index(0, 0).data(GameListModel::SectionRole).toString(), "Default Exclusions");
  EXPECT_EQ(model->index(0, 0).data(GameListModel::DisplayRole).toString(), "Runtime Tool");
  filtered->setShowDefaultEntries(false);
  EXPECT_EQ(model->rowCount(), 0);
  service.setTokenGames({native});
  filtered->setFilterText(native);
  ASSERT_EQ(model->rowCount(), 1);
  EXPECT_TRUE(model->index(0, 0).data(GameListModel::HasTokenRole).toBool());
  EXPECT_EQ(model->index(0, 0).data(GameListModel::SectionRole).toString(), "Heroic");
  setCatalogue(service, {});
  setRecentGames(service, {});
  filtered->setFilterText(custom);
  ASSERT_EQ(model->rowCount(), 1); // Manual rule survives library disappearance.
  EXPECT_EQ(model->index(0, 0).data(GameListModel::SectionRole).toString(), "Custom Games");
}

TEST_F(GameDetectionServiceTest, RunningGamesLeadAndDeniedGamesFollowReadyAndWaitingRows) {
  GameDetectionService service;
  const QString active = "exe:/group-sort/active", ready = "exe:/group-sort/ready",
                waiting = "exe:/group-sort/waiting", denied = "exe:/group-sort/denied";
  for (const auto &key : {active, ready, waiting, denied})
    define(service, key, key);
  setRecentGames(service, {denied, waiting, ready, active});
  service.setTokenGames({ready, active});
  service.moveToBlacklist(denied);
  const auto running = process(4242, "/group-sort/active");
  apply(service, {{running.pid, running}});
  auto *model = service.gamesModel();
  auto *filtered = qobject_cast<GameListFilterModel *>(model);
  ASSERT_NE(filtered, nullptr);
  filtered->setFilterText("group-sort");
  ASSERT_EQ(model->rowCount(), 4);
  QStringList keys;
  for (int row = 0; row < model->rowCount(); ++row)
    keys.append(model->index(row, 0).data(GameListModel::KeyRole).toString());
  EXPECT_EQ(keys, (QStringList{active, ready, waiting, denied}));
  EXPECT_EQ(model->index(0, 0).data(GameListModel::SectionRole).toString(), "Now Playing");
  EXPECT_EQ(model->index(1, 0).data(GameListModel::SectionRole).toString(), "Custom Games");
}

TEST_F(GameDetectionServiceTest, CompletedLibraryScansRefreshGamesCountsAndKeepCollapsedGroups) {
  GameDetectionService service;
  service.addCustomGame("exe:/scan-refresh/manual", "scan-refresh-manual", false);
  auto *model = qobject_cast<GameListFilterModel *>(service.gamesModel());
  ASSERT_NE(model, nullptr);
  model->setFilterText("scan-refresh");
  const LauncherGame first{71001, "scan-refresh-one", "/scan-refresh/one", {}, {}, false, {"Steam"}};
  const LauncherGame second{71002, "scan-refresh-two", "/scan-refresh/two", {}, {}, false, {"Steam"}};
  publishCatalogue(service, {first});
  EXPECT_EQ(model->matchingGameCount(), 2);
  EXPECT_EQ(model->sectionCount("Steam"), 1);
  model->toggleSection("Steam");
  publishCatalogue(service, {first, second});
  EXPECT_EQ(model->matchingGameCount(), 3);
  EXPECT_EQ(model->sectionCount("Steam"), 2);
  EXPECT_EQ(model->rowCount(), 2); // One collapsed representative plus the custom rule.
  EXPECT_TRUE(model->collapsedSections().contains("Steam"));
  publishCatalogue(service, {second});
  EXPECT_EQ(model->matchingGameCount(), 2);
  EXPECT_EQ(model->sectionCount("Steam"), 1);
  EXPECT_EQ(model->sectionCount("Custom Games"), 1);
}

TEST_F(GameDetectionServiceTest, SharedItchInstallationAliasesProduceOneUnapprovedGamesRow) {
  GameDetectionService service;
  LauncherGame first{0, "itch-shared Game", "/itch-shared", "/itch-shared/game",
                     {}, false, {"Heroic (itch.io)", "itch.io"}};
  first.productId = "itch:123";
  first.installation = {first.productId, "/itch-shared", ""};
  auto alias = first;
  alias.executable = "/itch-shared/alternative";
  publishCatalogue(service, {first, alias});
  auto *model = qobject_cast<GameListFilterModel *>(service.gamesModel());
  ASSERT_NE(model, nullptr);
  model->setFilterText("itch-shared");
  ASSERT_EQ(model->matchingGameCount(), 1);
  const auto index = model->index(0, 0);
  EXPECT_EQ(index.data(GameListModel::SectionRole).toString(), "itch.io");
  EXPECT_FALSE(index.data(GameListModel::HasTokenRole).toBool());
  EXPECT_FALSE(service.whitelist().contains("exe:/itch-shared/game"));
  first.sources = {"itch.io"};
  alias.sources = first.sources;
  publishCatalogue(service, {first, alias});
  ASSERT_EQ(model->matchingGameCount(), 1);
  EXPECT_EQ(model->index(0, 0).data(GameListModel::SectionRole).toString(), "itch.io");
  auto other = first;
  other.directory = "/itch-shared-other";
  other.executable = "/itch-shared-other/game";
  other.installation = {other.productId, other.directory, ""};
  publishCatalogue(service, {first, alias, other});
  EXPECT_EQ(model->matchingGameCount(), 2);
}

TEST_F(GameDetectionServiceTest, InstallerAndAccessibilityHelpersAreReadOnlyDefaultExclusions) {
  GameDetectionService service;
  auto *model = qobject_cast<GameListFilterModel *>(service.gamesModel());
  ASSERT_NE(model, nullptr);
  model->setShowDefaultEntries(true);
  for (const auto *key : {"dxsetup", "vc_redist", "xalia"}) {
    model->setFilterText(key);
    ASSERT_EQ(model->matchingGameCount(), 1) << key;
    const auto index = model->index(0, 0);
    EXPECT_TRUE(index.data(GameListModel::SupportToolRole).toBool());
    EXPECT_TRUE(index.data(GameListModel::DefaultRole).toBool());
    EXPECT_EQ(index.data(GameListModel::SectionRole).toString(), "Default Exclusions");
    EXPECT_TRUE(ProcessGames::excluded(QString("/helpers/%1.exe").arg(key)));
  }
  EXPECT_FALSE(ProcessGames::excluded("/games/xalia-adventure"));
  EXPECT_FALSE(ProcessGames::excluded("/games/setup.exe"));
  model->setShowDefaultEntries(false);
  EXPECT_EQ(model->matchingGameCount(), 0);
}

TEST_F(GameDetectionServiceTest, PrefixFreeLibraryRowsNeverShareScopedApproval) {
  GameDetectionService service;
  const QString payload = "/group-test/shared/game.exe";
  const QString blockKey = GameWhitelist::normalizeKey("exe:" + payload);
  const QString first = GameWhitelist::normalizeKey("wine:/group-test/prefix-one:" + payload);
  const QString second = GameWhitelist::normalizeKey("wine:/group-test/prefix-two:" + payload);
  setCatalogue(service, {
      {0, "Prefix Test Game", "/group-test/shared", payload, {}, false, {"Heroic (GOG)"}}});
  setRecentGames(service, {});
  service.setTokenGames({"Prefix Test Game"});
  auto *model = service.gamesModel();
  auto *filtered = qobject_cast<GameListFilterModel *>(model);
  ASSERT_NE(filtered, nullptr);
  filtered->setFilterText(GameWhitelist::normalizeKey(payload));
  ASSERT_EQ(model->rowCount(), 1);
  EXPECT_EQ(model->index(0, 0).data(GameListModel::KeyRole).toString(), blockKey);
  EXPECT_FALSE(model->index(0, 0).data(GameListModel::HasTokenRole).toBool());
  service.setTokenGames({first});
  setRecentGames(service, {first, second});
  ASSERT_EQ(model->rowCount(), 2);
  QSet<QString> approved;
  for (int row = 0; row < model->rowCount(); ++row) {
    const auto index = model->index(row, 0);
    EXPECT_EQ(index.data(GameListModel::SectionRole).toString(), "Heroic");
    EXPECT_EQ(index.data(GameListModel::BlockKeyRole).toString(), blockKey);
    if (index.data(GameListModel::HasTokenRole).toBool())
      approved.insert(index.data(GameListModel::KeyRole).toString());
  }
  EXPECT_EQ(approved, (QSet<QString>{first}));
  service.moveToBlacklist(blockKey);
  ASSERT_EQ(model->rowCount(), 2); // A shared veto is not a third custom game.
  for (int row = 0; row < model->rowCount(); ++row)
    EXPECT_EQ(model->index(row, 0).data(GameListModel::ListedRole).toInt(), 2);
  service.unblockGame(blockKey);
  for (int row = 0; row < model->rowCount(); ++row)
    EXPECT_NE(model->index(row, 0).data(GameListModel::ListedRole).toInt(), 2);
}

TEST_F(GameDetectionServiceTest, SharedAudioMetadataInvalidatesOldAssociationsWithoutNativeQueries) {
  GameDetectionService service;
  AudioApplicationRegistry registry;
  service.setAudioApplicationRegistry(&registry);
  publishAudio(registry, {QVariantMap{{"nodeId", 1}, {"binary", "first"}}});
  checkStaleAudio(service);
  publishAudio(registry, {QVariantMap{{"nodeId", 2}, {"binary", "second"}}});
  checkStaleAudio(service);
  service.setAudioApplicationRegistry(nullptr);
  EXPECT_TRUE(service.instances().isEmpty());
}

TEST_F(GameDetectionServiceTest, MetadataWatcherHandlesInPlaceAndAtomicReplacement) {
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const auto file = root.path() + "/installed.json";
  writeFile(file, "{}");
  GameDetectionService service;
  readyLibraries(service, {root.path(), file});
  writeFile(file, "{\"changed\":true}");
  ASSERT_TRUE(waitFor([&] { return service.librariesScanning(); }));
  clearRefresh(service);
  QSaveFile replacement(file);
  ASSERT_TRUE(replacement.open(QIODevice::WriteOnly));
  replacement.write("{\"replaced\":true}");
  ASSERT_TRUE(replacement.commit());
  ASSERT_TRUE(waitFor([&] { return service.librariesScanning(); }));
  watch(service, {root.path(), file}); // A completed worker scan repairs the watch.
  clearRefresh(service);
  writeFile(file, "{\"changed_again\":true}");
  ASSERT_TRUE(waitFor([&] { return service.librariesScanning(); }));
  EXPECT_TRUE(noWorker(service));
}

TEST_F(GameDetectionServiceTest, FallbackRefreshUsesElapsedTimeNotProcessScans) {
  GameDetectionService service;
  readyLibraries(service);
  ASSERT_TRUE(fallbackWithoutCensus(service));
  ASSERT_TRUE(waitFor([&] { return service.librariesScanning(); }));
  EXPECT_TRUE(noWorker(service));
}

TEST_F(GameDetectionServiceTest, UnfamiliarProcessesNeverRequestLibraryRefresh) {
  GameDetectionService service;
  readyLibraries(service);
  QSignalSpy scanning(&service, &GameDetectionService::librariesScanningChanged);
  const auto candidate = process(31, "/games/NewGame");
  census(service, {{31, candidate}});
  EXPECT_FALSE(service.librariesScanning());
  EXPECT_TRUE(service.games().isEmpty());
  census(service, {{31, candidate}});
  auto respawn = candidate;
  respawn.start++;
  census(service, {{31, respawn}});
  census(service, {});
  census(service, {{31, respawn}});
  EXPECT_FALSE(waitFor([&] { return service.librariesScanning(); }, 350));
  EXPECT_EQ(scanning.size(), 0);
  EXPECT_TRUE(noWorker(service));
}

TEST_F(GameDetectionServiceTest, ProcessCensusUsesCachedCatalogueWithoutRequestingRefresh) {
  GameDetectionService service;
  readyLibraries(service);
  census(service, {{31, process(31, "/usr/bin/wineserver")}});
  EXPECT_FALSE(service.librariesScanning());
  census(service, {{32, process(32, "/games/FreshlyScanned")}}, true);
  EXPECT_FALSE(service.librariesScanning());
  clearRefresh(service);
  setCatalogue(service, {{123, "Known Game", "/games/known"}});
  census(service, {{33, process(33, "/games/known/Game", 1, 123)}});
  EXPECT_FALSE(service.librariesScanning());
  EXPECT_EQ(service.games().size(), 1);
}

TEST_F(GameDetectionServiceTest, RefreshRequestsCoalesceWithoutDiscardingUsableInFlightResults) {
  GameDetectionService service;
  readyLibraries(service);
  checkQueuedRefreshResult(service);
  EXPECT_TRUE(noWorker(service));
}

TEST_F(GameDetectionServiceTest, BackgroundRefreshOnlyPublishesChangedLibraryResults) {
  GameDetectionService service;
  readyLibraries(service);
  QSignalSpy results(&service, &GameDetectionService::gameLibrariesChanged);
  QSignalSpy scanning(&service, &GameDetectionService::librariesScanningChanged);
  EXPECT_FALSE(service.gameLibrariesReady());
  publishLibraryStatus(service, {});
  EXPECT_TRUE(service.gameLibrariesReady()); // A completed empty catalogue is a real result.
  EXPECT_EQ(results.size(), 1);
  results.clear();
  const QVariantList rows{QVariantMap{{"id", "steam"}, {"detected", true}, {"count", 45}}};
  publishLibraryStatus(service, rows);
  ASSERT_EQ(results.size(), 1);
  EXPECT_EQ(service.gameLibraries(), rows);
  results.clear();
  service.rescanGameLibraries();
  EXPECT_TRUE(service.librariesScanning());
  EXPECT_EQ(results.size(), 0);
  EXPECT_EQ(service.gameLibraries(), rows);
  EXPECT_TRUE(service.gameLibrariesReady());
  clearRefresh(service);
  publishLibraryStatus(service, rows);
  EXPECT_EQ(results.size(), 0); // Unchanged results do not rebuild QML models.
  auto updated = rows;
  auto steam = updated[0].toMap();
  steam["count"] = 46;
  updated[0] = steam;
  publishLibraryStatus(service, updated, true);
  EXPECT_EQ(results.size(), 0); // Obsolete location settings cannot replace the snapshot.
  EXPECT_EQ(service.gameLibraries(), rows);
  publishLibraryStatus(service, updated);
  EXPECT_EQ(results.size(), 1);
  EXPECT_EQ(service.gameLibraries(), updated);
  EXPECT_GT(scanning.size(), 0);
}

TEST_F(GameDetectionServiceTest, MetadataChangesRefreshPromptlyAndCoalesceNotificationBursts) {
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const auto file = root.path() + "/installed.json";
  writeFile(file, "{}");
  GameDetectionService service;
  readyLibraries(service, {root.path(), file});
  QSignalSpy scanning(&service, &GameDetectionService::librariesScanningChanged);
  writeFile(file, "{\"first\":true}");
  ASSERT_TRUE(waitFor([&] { return watchPending(service); }));
  const auto delay = watchDelay(service);
  EXPECT_GT(delay, 0);
  EXPECT_LE(delay, 250);
  writeFile(file, "{\"second\":true}");
  QCoreApplication::processEvents();
  EXPECT_LE(watchDelay(service), delay); // More notifications do not move the deadline.
  ASSERT_TRUE(waitFor([&] { return service.librariesScanning(); }));
  EXPECT_EQ(scanning.size(), 1);
  EXPECT_TRUE(noWorker(service));
  writeFile(file, "{\"third\":true}");
  QCoreApplication::processEvents();
  EXPECT_FALSE(watchPending(service)); // One pending refresh covers further events.
  EXPECT_EQ(scanning.size(), 1);
  clearRefresh(service); // A later file change need not wait for the fallback.
  writeFile(file, "{\"after_refresh\":true}");
  ASSERT_TRUE(waitFor([&] { return service.librariesScanning(); }));
  EXPECT_EQ(scanning.size(), 2);
  clearRefresh(service);
  service.rescanGameLibraries(); // User requests do not wait for debounce either.
  EXPECT_TRUE(service.librariesScanning());
  service.stop();
  EXPECT_FALSE(watchPending(service));
}

TEST_F(GameDetectionServiceTest, StopCancelsWatchDebounceAndFallbackRequests) {
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const auto file = root.path() + "/installed.json";
  writeFile(file, "{}");
  GameDetectionService service;
  readyLibraries(service, {file, root.path()});
  ASSERT_TRUE(fallbackWithoutCensus(service));
  writeFile(file, "{\"pending\":true}");
  service.stop();
  EXPECT_FALSE(waitFor([&] { return service.librariesScanning(); }, 350));
  EXPECT_FALSE(watchPending(service));
}

TEST_F(GameDetectionServiceTest, LibraryLocationsPersistAndCanBeReset) {
  QSettings settings;
  const auto previous = settings.value("detection/libraryLocations");
  const auto restore = qScopeGuard([&] {
    if (previous.isValid())
      settings.setValue("detection/libraryLocations", previous);
    else
      settings.remove("detection/libraryLocations");
  });
  settings.remove("detection/libraryLocations");
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  {
    GameDetectionService service;
    ASSERT_EQ(service.gameLibraries().size(), 7);
    service.setLibraryAutomatic("heroic", false);
    service.addLibraryLocation("heroic", QUrl::fromLocalFile(root.path()));
    service.addLibraryLocation("heroic", QUrl::fromLocalFile(root.path()));
    const auto heroic = service.gameLibraries()[1].toMap();
    EXPECT_FALSE(heroic.value("automatic").toBool());
    EXPECT_EQ(heroic.value("paths").toStringList(), QStringList{root.path()});
    EXPECT_TRUE(service.librariesScanning());
  }
  GameDetectionService restored;
  EXPECT_EQ(restored.gameLibraries()[1].toMap().value("paths").toStringList(),
            QStringList{root.path()});
  restored.removeLibraryLocation("heroic", root.path());
  EXPECT_TRUE(restored.gameLibraries()[1].toMap().value("paths").toStringList().isEmpty());
  restored.resetLibraryLocations("heroic");
  EXPECT_TRUE(restored.gameLibraries()[1].toMap().value("automatic").toBool());
  restored.setLibraryAutomatic("faugus", false);
  restored.addLibraryLocation("faugus", QUrl::fromLocalFile(root.path()));
  EXPECT_FALSE(restored.gameLibraries()[5].toMap().value("automatic").toBool());
  EXPECT_EQ(restored.gameLibraries()[5].toMap().value("paths").toStringList(),
             QStringList{root.path()});
  restored.setLibraryAutomatic("itch", false);
  restored.addLibraryLocation("itch", QUrl::fromLocalFile(root.path()));
  EXPECT_FALSE(restored.gameLibraries()[6].toMap().value("automatic").toBool());
  EXPECT_EQ(restored.gameLibraries()[6].toMap().value("paths").toStringList(),
            QStringList{root.path()});
  restored.resetLibraryLocations("itch");
  EXPECT_TRUE(restored.gameLibraries()[6].toMap().value("automatic").toBool());
  const auto before = settings.value("detection/libraryLocations");
  restored.addLibraryLocation("invalid-launcher", QUrl::fromLocalFile(root.path()));
  restored.addLibraryLocation("heroic", QUrl("https://example.invalid/metadata"));
  EXPECT_EQ(settings.value("detection/libraryLocations"), before);
}

TEST_F(GameDetectionServiceTest,
       WineCustomDriveAndZDriveIdentifySameSteamGame) {
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const auto prefix = root.path() + "/pfx";
  const auto library = root.path() + "/SteamLibrary";
  const auto directory = library + "/steamapps/common/Limbus Company";
  ASSERT_TRUE(QDir().mkpath(prefix + "/dosdevices"));
  ASSERT_TRUE(QDir().mkpath(directory));
  ASSERT_TRUE(QFile::link("../../SteamLibrary", prefix + "/dosdevices/s:"));
  ASSERT_TRUE(QFile::link("/", prefix + "/dosdevices/z:"));
  const auto executable = directory + "/LimbusCompany.exe";
  writeFile(executable, "fixture");
  for (const auto &argument :
       QStringList{"S:\\steamapps\\common\\Limbus Company\\LimbusCompany.exe",
                   "s:/steamapps/common/Limbus Company/LimbusCompany.exe",
                   "Z:" + executable}) {
    const auto p = inspectWine(root.path(), prefix, {argument});
    ASSERT_EQ(p.pid, 31);
    EXPECT_EQ(p.payload(), executable);
    const auto games = ProcessGames::identify(
        {{p.pid, p}}, {{1973530, "Limbus Company", directory}}, {}, {});
    ASSERT_EQ(games.size(), 1);
    EXPECT_EQ(games[0].key, "steam_app_1973530");
    EXPECT_EQ(games[0].primaryPid, p.pid);
  }
  const auto p =
      inspectWine(root.path(), prefix,
                  {root.path() + "/wine64-preloader",
                   "S:\\steamapps\\common\\Limbus Company\\LimbusCompany.exe"});
  EXPECT_EQ(p.payload(), executable);
  ASSERT_TRUE(QFile::remove(prefix + "/dosdevices/s:"));
  EXPECT_EQ(p.payload(), executable); // Existing snapshot stays immutable.
  const auto missing = inspectWine(root.path(), prefix, p.arguments);
  EXPECT_TRUE(missing.payload().isEmpty());
}

TEST_F(GameDetectionServiceTest,
       WineDriveOverridesArePrefixSpecificAndFailSafe) {
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  const auto first = root.path() + "/first";
  const auto second = root.path() + "/second";
  const auto games = root.path() + "/games";
  ASSERT_TRUE(QDir().mkpath(first + "/dosdevices"));
  ASSERT_TRUE(QDir().mkpath(second + "/dosdevices"));
  ASSERT_TRUE(QDir().mkpath(games));
  for (const auto drive : {"c:", "z:", "s:"}) {
    ASSERT_TRUE(QFile::link(games, first + "/dosdevices/" + drive));
    ASSERT_TRUE(QFile::link("../../missing", second + "/dosdevices/" + drive));
    const QString argument = QString(drive).toUpper() + "\\Game.exe";
    const auto mapped = inspectWine(root.path(), first, {argument});
    EXPECT_EQ(mapped.payload(), games + "/Game.exe");
    const auto broken = inspectWine(root.path(), second, {argument});
    EXPECT_TRUE(
        broken.payload().isEmpty()); // No C:/Z: fallback over a bad map.
  }
  for (const auto &argument : {"R:\\Game.exe", "S:Game.exe"}) {
    const auto p = inspectWine(root.path(), first, {argument});
    EXPECT_TRUE(p.payload().isEmpty()); // Never borrow the process's CWD.
    EXPECT_TRUE(ProcessGames::identify({{p.pid, p}},
                                       {{1973530, "Wrong game", root.path()}},
                                       {}, {})
                    .isEmpty());
  }
}

TEST_F(GameDetectionServiceTest,
       ProcessPickerListsPayloadsAndRejectsStaleRules) {
  GameDetectionService service;
  EXPECT_TRUE(service.runningProcesses().isEmpty());
  QProcess child;
  child.start("sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted());
  auto p = ProcessCensus::inspect(child.processId());
  ASSERT_GT(p.pid, 0);
  apply(service, {{p.pid, p}});
  auto rows = service.runningProcesses();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].toMap()["pid"].toLongLong(), p.pid);
  EXPECT_EQ(rows[0].toMap()["path"].toString(), p.payload());
  EXPECT_EQ(rows[0].toMap()["identity"].toString(), p.identity());
  EXPECT_FALSE(rows[0].toMap()["likelyApplication"].toBool());
  EXPECT_FALSE(
      service.addProcessGame(p.pid, p.identity() + "0", "Fixture", false));
  ASSERT_TRUE(service.addProcessGame(p.pid, p.identity(), "Fixture", false));
  const auto key = "exe:" + p.payload();
  EXPECT_EQ(service.currentGameName(), "Fixture");
  EXPECT_TRUE(service.removeFromWhitelist(key));
  EXPECT_TRUE(service.addProcessGame(p.pid, p.identity(), "Fixture", true));
  EXPECT_TRUE(service.isBlocked(key));
  EXPECT_TRUE(service.removeFromBlacklist(key));
  auto changed = p;
  changed.executable += "-different";
  apply(service, {{p.pid, changed}});
  EXPECT_FALSE(
      service.addProcessGame(p.pid, p.identity(), "Wrong payload", false));
  apply(service, {{p.pid, p}});
  child.kill();
  ASSERT_TRUE(child.waitForFinished());
  EXPECT_FALSE(service.addProcessGame(p.pid, p.identity(), "Exited", false));
}

TEST_F(GameDetectionServiceTest, PickerLikelihoodDoesNotHideUnknownsOrChangeDetection) {
  GameDetectionService service;
  auto wine = process(20, "/usr/bin/wine64-preloader");
  wine.arguments = {"C:\\Games\\Unknown.exe"};
  wine.winePrefix = "/prefix";
  define(service, "exe:/games/known", "Known");
  setApplications(service, {"/opt/desktop-app/app"});
  apply(service, {{20, wine},
                  {21, process(21, "/usr/bin/rclone")},
                  {22, process(22, "/opt/desktop-app/app")},
                  {23, process(23, "/games/unknown-native")},
                  {24, process(24, "/games/known")},
                  {25, process(25, "/opt/desktop-app/app")}});
  const auto games = service.games();
  const auto rows = service.runningProcesses();
  ASSERT_EQ(rows.size(), 6); // All selectable processes remain available.
  const QList<bool> expected{true, false, true, false, true, true};
  for (int i = 0; i < rows.size(); ++i) {
    const auto row = rows[i].toMap();
    EXPECT_EQ(row["likelyApplication"].toBool(), expected[i]);
    EXPECT_EQ(row["pid"].toLongLong(), 20 + i);
    EXPECT_FALSE(row["identity"].toString().isEmpty());
  }
  EXPECT_EQ(service.games().size(), games.size());
  ASSERT_EQ(service.games().size(), 1); // Desktop/Wine hints do not detect games.
  EXPECT_EQ(service.games().first().key, "exe:/games/known");
}

TEST_F(GameDetectionServiceTest, UnlistedNativeGameRequiresAnExplicitRule) {
  GameDetectionService service;
  const auto p = process(5000, "/games/unlisted/terraria.bin.x86_64");
  apply(service, {{p.pid, p}});
  EXPECT_TRUE(service.games().isEmpty());
  define(service, "exe:" + p.payload(), "My standalone game");
  apply(service, {{p.pid, p}});
  ASSERT_EQ(service.games().size(), 1);
  EXPECT_EQ(service.games().first().name, "My standalone game");
}

TEST_F(GameDetectionServiceTest, LauncherCatalogueFeedsSelectionAndPicker) {
  GameDetectionService service;
  const auto p = process(5000, "/games/non-steam/native");
  setCatalogue(service, {{0, "A Launcher Game", {}, p.payload(), {}}});
  apply(service, {{p.pid, p}});
  ASSERT_EQ(service.games().size(), 1);
  EXPECT_EQ(service.currentGameName(), "A Launcher Game");
  EXPECT_EQ(service.currentGameKey(), "exe:/games/non-steam/native");
  ASSERT_EQ(service.runningProcesses().size(), 1);
  EXPECT_TRUE(service.runningProcesses().first().toMap()["likelyApplication"].toBool());
  define(service, "exe:/games/non-steam/native", "My Override");
  apply(service, {{p.pid, p}});
  EXPECT_EQ(service.currentGameName(), "My Override");
  service.stop();
  EXPECT_TRUE(service.games().isEmpty());
  EXPECT_TRUE(service.runningProcesses().isEmpty());
}

TEST_F(GameDetectionServiceTest, VerifiedBootstrapHandoffKeepsCaptureIdentityAndPrompt) {
  GameDetectionService service;
  const auto bootstrap = process(5000, "/games/fixture/Launcher.exe");
  auto child = process(5001, "/games/fixture/Retail/Game.exe", bootstrap.pid);
  setCatalogue(service, {{0, "A Launcher Game", "/games/fixture",
                          bootstrap.payload(), {}, true, {"Heroic (Epic)"}}});
  QSignalSpy detected(&service, &GameDetectionService::gameDetected);
  QSignalSpy closed(&service, &GameDetectionService::gameClosed);
  apply(service, {{bootstrap.pid, bootstrap}, {child.pid, child}});
  ASSERT_EQ(service.games().size(), 1);
  EXPECT_EQ(service.currentGameName(), "A Launcher Game");
  EXPECT_EQ(service.currentGameSource(), "Heroic (Epic)");
  EXPECT_EQ(detected.count(), 1);
  const auto identity = service.captureIdentityInfo("A Launcher Game");
  const auto rows = service.runningProcesses();
  ASSERT_EQ(rows.size(), 2);
  EXPECT_TRUE(rows[0].toMap()["likelyApplication"].toBool());
  child.parent = 1;
  apply(service, {{child.pid, child}});
  EXPECT_EQ(service.currentGamePid(), child.pid);
  const auto next = service.captureIdentityInfo("A Launcher Game");
  EXPECT_EQ(next["detectedGameInstance"], identity["detectedGameInstance"]);
  EXPECT_EQ(next["detectedGameKey"], identity["detectedGameKey"]);
  EXPECT_EQ(detected.count(), 1);
  EXPECT_EQ(closed.count(), 0);
  EXPECT_EQ(service.currentGameSource(), "Heroic (Epic)");
}

TEST_F(GameDetectionServiceTest, PickerDesktopHintsRespectExecAndHiddenOverrides) {
  QTemporaryDir root;
  const auto user = root.path() + "/user";
  const auto system = root.path() + "/system";
  ASSERT_TRUE(QDir().mkpath(user + "/applications"));
  ASSERT_TRUE(QDir().mkpath(system + "/applications"));
  writeFile(user + "/applications/gui.desktop",
      "[Desktop Entry]\nType=Application\nExec=\"/games/A Game/game\" --play %U\n");
  writeFile(user + "/applications/env.desktop",
      "[Desktop Entry]\nType=Application\nExec=env KEY=value /games/env-game %U\n");
  writeFile(user + "/applications/hidden.desktop",
      "[Desktop Entry]\nType=Application\nHidden=true\nExec=/games/hidden\n");
  writeFile(system + "/applications/hidden.desktop",
      "[Desktop Entry]\nType=Application\nExec=/games/hidden\n");
  writeFile(user + "/applications/terminal.desktop",
      "[Desktop Entry]\nType=Application\nTerminal=true\nExec=/usr/bin/rclone\n");
  writeFile(user + "/applications/helper.desktop",
      "[Desktop Entry]\nType=Application\nNoDisplay=true\nExec=/games/helper\n");
  writeFile(user + "/applications/action.desktop",
      "[Desktop Entry]\nType=Link\nExec=/games/not-an-app\n"
      "[Desktop Action Fake]\nType=Application\nExec=/games/action\n");
  const auto apps = desktopApplications({user, system});
  EXPECT_EQ(apps, (QSet<QString>{"/games/A Game/game", "/games/env-game"}));
}

TEST_F(GameDetectionServiceTest, SteamRuntimeToolsAreNotGamesOrInstanceMembers) {
  const QString runtimeDirectory = "/library/steamapps/common/SteamLinuxRuntime4.0";
  const QList<LauncherGame> catalogue{
      {424004, "Steam Linux Runtime 4.0", runtimeDirectory},
      {424005, "Actual Game", "/library/steamapps/common/ActualGame"}};
  for (const auto &name : QStringList{
           "srt-logger", "srt-client-container", "srt-launcher-service",
           "srt-launch-client", "x86_64-linux-gnu-srt-launcher-service",
           "i386-linux-gnu-srt-launch-client", "steam-runtime-supervisor",
           "steam-runtime-launcher-service", "pressure-vessel-wrap",
           "pressure-vessel-unruntime", "srt-bwrap", "pv-adverb"}) {
    SCOPED_TRACE(name.toStdString());
    GameDetectionService service;
    setCatalogue(service, catalogue);
    auto helper = process(110, runtimeDirectory + "/pressure-vessel/bin/" + name,
                          1, 424004);
    const auto game = process(120, "/library/steamapps/common/ActualGame/game",
                              helper.pid, 424005);
    EXPECT_TRUE(ProcessGames::excluded(helper.payload()));
    apply(service, {{helper.pid, helper}, {game.pid, game}});
    ASSERT_EQ(service.games().size(), 1);
    EXPECT_EQ(service.currentGameName(), "Actual Game");
    EXPECT_EQ(service.currentGamePid(), game.pid);
    EXPECT_FALSE(service.games().first().processes.contains(helper.pid));
    apply(service, {{helper.pid, helper}});
    EXPECT_FALSE(service.isGameRunning()); // Helpers alone cannot keep it alive.
  }
}

TEST_F(GameDetectionServiceTest, SteamApprovalReportsSteamAfterRuntimeHelperStartsFirst) {
  GameDetectionService service;
  const QString directory = "/library/steamapps/common/Terraria";
  setCatalogue(service, {
      {105600, "Terraria", directory},
      {424004, "Steam Linux Runtime 4.0",
       "/library/steamapps/common/SteamLinuxRuntime4.0"}});
  define(service, "steam_app_105600", "Terraria");
  auto logger = process(110, "/library/steamapps/common/SteamLinuxRuntime4.0/"
                             "pressure-vessel/bin/srt-logger", 1, 105600);
  auto game = process(120, directory + "/Terraria.bin.x86_64", logger.pid, 105600);
  QStringList announcedSources;
  QObject::connect(&service, &GameDetectionService::gameDetected,
                   [&](const QString &, qint64) {
    announcedSources.append(service.currentGameSource());
  });
  apply(service, {{logger.pid, logger}});
  EXPECT_FALSE(service.isGameRunning());
  EXPECT_TRUE(announcedSources.isEmpty());
  apply(service, {{logger.pid, logger}, {game.pid, game}});
  ASSERT_EQ(service.games().size(), 1);
  EXPECT_EQ(service.currentGamePid(), game.pid);
  EXPECT_EQ(service.currentGameSource(), "Steam");
  EXPECT_EQ(announcedSources, QStringList{"Steam"});
  apply(service, {{logger.pid, logger}, {game.pid, game}});
  EXPECT_EQ(announcedSources.size(), 1);
}

TEST_F(GameDetectionServiceTest, SteamAppRuleWithoutManifestDoesNotClaimInstallationMatch) {
  GameDetectionService service;
  define(service, "steam_app_105600", "Terraria");
  const auto game = process(120, "/outside/known/libraries/Terraria.bin.x86_64",
                            1, 105600);
  apply(service, {{game.pid, game}});
  ASSERT_TRUE(service.isGameRunning());
  EXPECT_EQ(service.currentGameSource(), "Steam App ID rule");
}

TEST_F(GameDetectionServiceTest,
       DetectedIdentitySurvivesSynchronousRuleRebuild) {
  GameDetectionService service;
  const QString key = "exe:/games/reentrant/game";
  define(service, key, "Reentrant Game");
  service.removeFromWhitelist("exe:/games/another/game");
  QString first, afterRebuild, second;
  QObject::connect(&service, &GameDetectionService::gameDetected,
                   [&](const QString &name, qint64) {
                     first = name;
                     // Force a real rebuild without changing the current game.
                     service.addToWhitelist("exe:/games/another/game",
                                            "Another");
                     afterRebuild = name;
                   });
  QObject::connect(&service, &GameDetectionService::gameDetected,
                   [&](const QString &name, qint64) { second = name; });
  apply(service, {{30, process(30, "/games/reentrant/game")}});
  EXPECT_EQ(first, "Reentrant Game");
  EXPECT_EQ(afterRebuild, first);
  EXPECT_EQ(second, first);
  EXPECT_EQ(service.currentGameKey(), key);
}

TEST_F(GameDetectionServiceTest,
       IdenticalWhitelistDefinitionDoesNotReidentify) {
  GameDetectionService service;
  service.removeFromWhitelist("exe:/games/noop/game");
  ASSERT_TRUE(service.addToWhitelist("exe:/games/noop/game", "Noop"));
  apply(service, {{30, process(30, "/games/noop/game")}});
  const auto id = service.games().first().id;
  QSignalSpy instances(&service, &GameDetectionService::instancesChanged);
  QSignalSpy whitelist(&service, &GameDetectionService::whitelistChanged);
  EXPECT_FALSE(service.addToWhitelist("exe:/games/noop/game", " Noop "));
  EXPECT_TRUE(instances.isEmpty());
  EXPECT_TRUE(whitelist.isEmpty());
  EXPECT_EQ(service.games().first().id, id);
}

TEST_F(GameDetectionServiceTest, ProcessPickerExposesWinePayloadNotLoader) {
  auto p = process(20, "/usr/bin/wine64-preloader");
  p.arguments = {"C:\\Games\\Game.exe"};
  p.winePrefix = "/prefix";
  auto browser = process(21, "/usr/bin/firefox");
  GameDetectionService service;
  apply(service, {{20, p}, {21, browser}});
  const auto rows = service.runningProcesses();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].toMap()["path"].toString(),
            "/prefix/drive_c/Games/Game.exe");
  EXPECT_FALSE(
      service.addProcessGame(20, p.identity(), "Not a live process", false));
  const QList<LauncherGame> steam{{42, "Fixture", "/prefix/drive_c/Games"}};
  const auto blocked =
      GameWhitelist::normalizeKey("wine:/prefix:" + p.payload());
  EXPECT_TRUE(
      ProcessGames::identify({{20, p}}, steam, {}, {blocked}).isEmpty());
  EXPECT_EQ(ProcessGames::identify(
                {{20, p}}, steam, {},
                {GameWhitelist::normalizeKey("wine:/other:" + p.payload())})
                .size(),
            1);
}

TEST_F(GameDetectionServiceTest,
       NativeSteamRequiresPayloadNotInheritedEnvironment) {
  ProcessCensus::Snapshot ps;
  ps[10] = process(10, "/usr/bin/steam");
  ps[20] = process(20, "/usr/bin/python3", 10, 42);
  ps[30] = process(30, "/games/Native/bin/game", 20, 42);
  ps[31] = process(31, "/games/Native/audio-helper", 30, 42);
  ps[40] = process(40, "/usr/bin/firefox", 20, 42);
  const auto games =
      ProcessGames::identify(ps, {{42, "Native", "/games/Native"}}, {}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].name, "Native");
  EXPECT_EQ(games[0].processes.keys(), (QList<qint64>{30, 31}));
}
TEST_F(GameDetectionServiceTest, NativeStandaloneExactUserRule) {
  const auto p = process(10, "/opt/standalone/game");
  const auto games =
      ProcessGames::identify({{10, p}}, {}, {{"game", "Standalone"}}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].name, "Standalone");
  EXPECT_TRUE(
      ProcessGames::identify({{10, p}}, {}, {{"gam", "Wrong"}}, {}).isEmpty());
}

TEST_F(GameDetectionServiceTest, InstalledNativeGameWithoutSteamEnvironment) {
  const auto p = process(10, "/games/Native/game");
  const auto games = ProcessGames::identify(
      {{10, p}}, {{42, "Native", "/games/Native"}}, {}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].key, "steam_app_42");
}

TEST_F(GameDetectionServiceTest,
       NativeInterpretedGamesUseConcretePayloadNotAllJavaOrPython) {
  auto java = process(10, "/usr/bin/java");
  java.arguments = {"java", "-jar", "/games/minecraft.jar"};
  auto python = process(20, "/usr/bin/python3");
  python.arguments = {"python3", "/games/standalone.py"};
  auto unrelated = process(30, "/usr/bin/python3");
  unrelated.arguments = {"python3", "-c", "print('not a game')"};
  const auto games = ProcessGames::identify(
      {{10, java}, {20, python}, {30, unrelated}}, {},
      {{"minecraft.jar", "Minecraft"}, {"standalone.py", "Standalone"}}, {});
  ASSERT_EQ(games.size(), 2);
  EXPECT_EQ(games[0].processes.size(), 1);
  EXPECT_EQ(games[1].processes.size(), 1);
}

TEST_F(GameDetectionServiceTest, RapidNativeProcessExitDuringInspectionIsSafe) {
  for (int i = 0; i < 25; ++i) {
    QProcess child;
    child.start("/usr/bin/true");
    ASSERT_TRUE(child.waitForStarted());
    const auto pid = child.processId();
    const auto p = ProcessCensus::inspect(pid);
    EXPECT_TRUE(p.pid == 0 || p.pid == pid);
    ASSERT_TRUE(child.waitForFinished());
    EXPECT_EQ(ProcessCensus::inspect(pid).pid, 0);
  }
}
TEST_F(GameDetectionServiceTest, ProtonWinePayloadAndServices) {
  auto game = process(30, "/usr/bin/wine64-preloader", 20, 42);
  game.arguments = {"Z:\\games\\Windows\\Game.exe"};
  game.directory = "/games/Windows";
  auto service = game;
  service.pid = 31;
  service.arguments = {"C:\\windows\\system32\\services.exe"};
  auto audio = game;
  audio.pid = 32;
  audio.parent = 30;
  audio.arguments = {"Z:\\games\\Windows\\Audio.exe"};
  const auto games =
      ProcessGames::identify({{30, game}, {31, service}, {32, audio}},
                             {{42, "Windows", "/games/Windows"}}, {}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].processes.keys(), (QList<qint64>{30, 32}));
}
TEST_F(GameDetectionServiceTest, TwoInstancesOfSameGameRemainSeparate) {
  auto a = process(10, "/games/game");
  auto b = process(20, "/games/game");
  const auto games =
      ProcessGames::identify({{10, a}, {20, b}}, {}, {{"game", "Game"}}, {});
  ASSERT_EQ(games.size(), 2);
  EXPECT_NE(games[0].id, games[1].id);
  auto service = process(10, "/usr/bin/wine64-preloader", 1, 42);
  service.arguments = {"C:\\windows\\system32\\services.exe"};
  auto wineOne = process(20, "/usr/bin/wine64-preloader", 10, 42);
  wineOne.arguments = {"/games/Windows/Game.exe"};
  auto wineTwo = process(30, "/usr/bin/wine64-preloader", 10, 42);
  wineTwo.arguments = wineOne.arguments;
  const auto wineGames =
      ProcessGames::identify({{10, service}, {20, wineOne}, {30, wineTwo}},
                             {{42, "Windows", "/games/Windows"}}, {}, {});
  EXPECT_EQ(wineGames.size(), 2); // A shared Wine service must not merge games.
}
TEST_F(GameDetectionServiceTest, MatchingGrandchildrenStayInTheirRootInstance) {
  const auto root = process(10, "/opt/game");
  const auto child = process(20, "/opt/game", 10);
  const auto grandchild = process(30, "/opt/game", 20);
  const auto games = ProcessGames::identify(
      {{10, root}, {20, child}, {30, grandchild}}, {}, {{"game", "Game"}}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].processes.size(), 3);
}
TEST_F(GameDetectionServiceTest, LauncherDoesNotAdoptUnrelatedSiblings) {
  auto launcher = process(10, "/opt/launcher");
  auto a = process(20, "/opt/game-one", 10);
  auto b = process(30, "/opt/game-two", 10);
  auto browser = process(40, "/usr/bin/firefox", 20);
  auto discord = process(50, "/usr/bin/Discord", 20);
  auto games = ProcessGames::identify(
      {{10, launcher}, {20, a}, {30, b}, {40, browser}, {50, discord}}, {},
      {{"game-one", "One"}, {"game-two", "Two"}}, {});
  ASSERT_EQ(games.size(), 2);
  EXPECT_EQ(games[0].processes.size(), 1);
  EXPECT_EQ(games[1].processes.size(), 1);
}
TEST_F(GameDetectionServiceTest,
       ReparentingPreservesIdentityButPidReuseAndExecDoNot) {
  auto a = process(10, "/opt/game");
  auto helper = process(20, "/opt/audio", 10);
  auto games = ProcessGames::identify({{10, a}, {20, helper}}, {},
                                      {{"game", "Game"}}, {});
  ASSERT_EQ(games.size(), 1);
  const auto id = games[0].id;
  helper.parent = 1;
  games =
      ProcessGames::identify({{20, helper}}, {}, {{"game", "Game"}}, {}, games);
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].id, id);
  helper.start++;
  EXPECT_TRUE(
      ProcessGames::identify({{20, helper}}, {}, {}, {}, games).isEmpty());
  helper.start--;
  helper.executable = "/usr/bin/firefox";
  EXPECT_TRUE(
      ProcessGames::identify({{20, helper}}, {}, {}, {}, games).isEmpty());
}
TEST_F(GameDetectionServiceTest, BlockRetiresEntireInstance) {
  const auto p = process(10, "/opt/game");
  EXPECT_TRUE(
      ProcessGames::identify({{10, p}}, {}, {{"game", "Game"}}, {"game"})
          .isEmpty());
}
TEST_F(GameDetectionServiceTest, StatParserHandlesSpacesParenthesesAndZombie) {
  ProcessInfo p;
  QByteArray stat = "123 (game ) with (parens)) S 42";
  for (int field = 5; field <= 21; ++field)
    stat += " 0";
  stat += " 123456 0";
  ASSERT_TRUE(ProcessCensus::parseStat(stat, p));
  EXPECT_EQ(p.parent, 42);
  EXPECT_EQ(p.start, 123456);
  for (const char state : {'Z', 'X', 'x'}) {
    auto dead = stat;
    dead[dead.lastIndexOf(')') + 2] = state;
    EXPECT_FALSE(ProcessCensus::parseStat(dead, p));
  }
  EXPECT_FALSE(ProcessCensus::parseStat("123 (gone) Z 1", p));
  EXPECT_FALSE(ProcessCensus::parseStat("truncated", p));
}
TEST_F(GameDetectionServiceTest, UnreapedZombieIsNotALiveGameProcess) {
  const auto pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0)
    ::_exit(0);
  siginfo_t info{};
  const auto waited = ::waitid(P_PID, pid, &info, WEXITED | WNOWAIT);
  EXPECT_EQ(waited, 0);
  if (waited == 0)
    EXPECT_FALSE(GameDetectionService::processExists(pid));
  int status = 0;
  EXPECT_EQ(::waitpid(pid, &status, 0), pid);
}
TEST_F(GameDetectionServiceTest, RealProcEnumerationAndExitRace) {
  QProcess child;
  child.start("/usr/bin/sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted());
  const auto pid = child.processId();
  auto ps = ProcessCensus::scan();
  ASSERT_TRUE(ps.contains(pid));
  EXPECT_GT(ps[pid].start, 0);
  EXPECT_EQ(ps[pid].parent, ::getpid());
  child.kill();
  ASSERT_TRUE(child.waitForFinished());
  EXPECT_FALSE(ProcessCensus::scan().contains(pid));
  EXPECT_EQ(ProcessCensus::inspect(pid).pid, 0);
}
TEST_F(GameDetectionServiceTest,
       RealAudioIdentityIsUniqueAndNeverUsesDisplaySubstring) {
  QProcess a, b;
  a.start("/usr/bin/sleep", {"30"});
  b.start("/usr/bin/sleep", {"30"});
  ASSERT_TRUE(a.waitForStarted());
  ASSERT_TRUE(b.waitForStarted());
  const auto pa = ProcessCensus::inspect(a.processId()),
             pb = ProcessCensus::inspect(b.processId());
  auto games = ProcessGames::identify({{pa.pid, pa}, {pb.pid, pb}}, {},
                                      {{"sleep", "Fixture"}}, {});
  ASSERT_EQ(games.size(), 2);
  ProcessGames::publish(games);
  EXPECT_EQ(ProcessGames::associate(pa.pid, "sleep"), games[0].id);
  EXPECT_TRUE(ProcessGames::associate(0, "sleep").isEmpty());
  EXPECT_TRUE(ProcessGames::associate(pa.pid, "firefox").isEmpty());
  EXPECT_TRUE(ProcessGames::associate(::getpid(), "sleep").isEmpty());
  EXPECT_TRUE(ProcessGames::associate(0, {}, "Fixture").isEmpty());
  a.kill();
  ASSERT_TRUE(a.waitForFinished());
  EXPECT_TRUE(ProcessGames::associate(pa.pid, "sleep").isEmpty());
  EXPECT_EQ(ProcessGames::associate(0, "sleep"), games[1].id);
  b.kill();
  ASSERT_TRUE(b.waitForFinished());
  ProcessGames::publish({});
}
TEST_F(GameDetectionServiceTest,
       ServicePublishesMultipleGamesWithoutWindowMonitor) {
  GameDetectionService service;
  define(service, "native-one", "One");
  define(service, "native-two", "Two");
  apply(service, {{10, process(10, "/opt/native-one")},
                  {20, process(20, "/opt/native-two")}});
  EXPECT_EQ(service.games().size(), 2);
  EXPECT_EQ(service.currentGameName(), "One");
  ASSERT_TRUE(service.selectInstance(service.games()[1].id));
  EXPECT_EQ(service.currentGameName(), "Two");
  EXPECT_FALSE(service.captureIdentityInfo("Two")
                   .value("detectedGameInstance")
                   .toString()
                   .isEmpty());
  apply(service, {});
  EXPECT_FALSE(service.isGameRunning());
}
TEST_F(GameDetectionServiceTest,
       ManualProcessPinDoesNotRegisterAGameOrSurviveExit) {
  QProcess child;
  child.start("/usr/bin/sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted());
  const auto pid = child.processId();
  ProcessGames::publish({});
  const auto pin = ProcessGames::pinProcess(pid);
  ASSERT_FALSE(pin.isEmpty());
  EXPECT_TRUE(ProcessGames::snapshot().isEmpty());
  EXPECT_TRUE(ProcessGames::belongsTo(pin, pid, "sleep"));
  EXPECT_FALSE(ProcessGames::belongsTo(pin, pid, "firefox"));
  EXPECT_FALSE(ProcessGames::belongsTo(pin, 0, "sleep"));
  child.kill();
  ASSERT_TRUE(child.waitForFinished());
  EXPECT_FALSE(ProcessGames::belongsTo(pin, pid, "sleep"));
}

TEST_F(GameDetectionServiceTest, LiveCensusDoesNotNeedCompositorOrAudioServer) {
  QProcess child;
  child.start("/usr/bin/sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted());
  const auto desktop = qgetenv("XDG_CURRENT_DESKTOP");
  const auto wayland = qgetenv("WAYLAND_DISPLAY");
  const auto remote = qgetenv("PIPEWIRE_REMOTE");
  struct Restore {
    QByteArray desktop, wayland, remote;
    ~Restore() {
      const auto set = [](const char *key, const QByteArray &value) {
        if (value.isNull())
          qunsetenv(key);
        else
          qputenv(key, value);
      };
      set("XDG_CURRENT_DESKTOP", desktop);
      set("WAYLAND_DISPLAY", wayland);
      set("PIPEWIRE_REMOTE", remote);
    }
  } restore{desktop, wayland, remote};
  qputenv("WAYLAND_DISPLAY", "trophy-test-no-compositor");
  qputenv("PIPEWIRE_REMOTE", "trophy-test-no-audio-server");
  for (const auto *name : {"KDE", "GNOME", "Hyprland"}) {
    qputenv("XDG_CURRENT_DESKTOP", name);
    GameDetectionService service;
    define(service, "sleep", "Native process fixture");
    service.start();
    bool found = false;
    QElapsedTimer timer;
    timer.start();
    while (!found && timer.elapsed() < 4000) {
      QCoreApplication::processEvents();
      for (const auto &g : service.games())
        if (g.processes.contains(child.processId()))
          found = true;
      QThread::msleep(10);
    }
    EXPECT_TRUE(found) << name;
    service.stop();
  }
  child.kill();
  ASSERT_TRUE(child.waitForFinished());
}
TEST_F(GameDetectionServiceTest,
       LegacyEnrollmentCleanupPreservesForeignSettings) {
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  struct Restore {
    QByteArray config = qgetenv("XDG_CONFIG_HOME"),
               data = qgetenv("XDG_DATA_HOME");
    ~Restore() {
      if (config.isNull())
        qunsetenv("XDG_CONFIG_HOME");
      else
        qputenv("XDG_CONFIG_HOME", config);
      if (data.isNull())
        qunsetenv("XDG_DATA_HOME");
      else
        qputenv("XDG_DATA_HOME", data);
      QStandardPaths::setTestModeEnabled(true);
    }
  } restore;
  QStandardPaths::setTestModeEnabled(false);
  qputenv("XDG_CONFIG_HOME", (root.path() + "/config").toUtf8());
  qputenv("XDG_DATA_HOME", (root.path() + "/data").toUtf8());
  ASSERT_TRUE(QDir().mkpath(root.path() + "/config/environment.d"));
  ASSERT_TRUE(QDir().mkpath(root.path() + "/data/vulkan/implicit_layer.d"));
  QFile config(root.path() + "/config/environment.d/trophy-gl-shim.conf");
  ASSERT_TRUE(config.open(QIODevice::WriteOnly));
  config.write("# retained "
               "comment\nLD_PRELOAD=/foreign.so:/old/"
               "libtrophy_gl_shim.so\nOTHER=value\n");
  config.close();
  QFile manifest(root.path() +
                 "/data/vulkan/implicit_layer.d/trophy_layer.json");
  ASSERT_TRUE(manifest.open(QIODevice::WriteOnly));
  manifest.write("{}");
  manifest.close();
  removeLegacyDetectionEnrollment();
  ASSERT_TRUE(config.open(QIODevice::ReadOnly));
  const auto remaining = config.readAll();
  EXPECT_TRUE(remaining.contains("/foreign.so"));
  EXPECT_TRUE(remaining.contains("OTHER=value"));
  EXPECT_TRUE(remaining.contains("# retained comment"));
  EXPECT_FALSE(remaining.contains("libtrophy_gl_shim"));
  EXPECT_FALSE(manifest.exists());
}
TEST_F(GameDetectionServiceTest,
       MixedSnapshotCannotAdoptChildOfReusedParentPid) {
  auto replacement = process(10, "/opt/game");
  replacement.start = 1000;
  auto oldChild = process(20, "/opt/audio-helper", 10);
  oldChild.start = 200; // Cannot be the child of this newly reused parent PID.
  const auto games = ProcessGames::identify({{10, replacement}, {20, oldChild}},
                                            {}, {{"game", "Game"}}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].processes.keys(), (QList<qint64>{10}));
}
} // namespace TrophyBackend
