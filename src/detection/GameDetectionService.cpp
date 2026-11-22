/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "GameDetectionService.h"
#include "GameIconSource.h"
#include "GameWhitelist.h"
#include "audio/AudioApplicationRegistry.h"
#include "audio/ApplicationAudioIdentity.h"
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QDebug>
#include <QSettings>
#include <algorithm>

namespace TrophyBackend {
void GameDetectionService::setAudioApplicationRegistry(AudioApplicationRegistry *registry) {
  if (m_audioRegistry)
    disconnect(m_audioRegistry, nullptr, this, nullptr);
  m_audioRegistry = registry;
  const auto update = [this] {
    const auto streams = m_audioRegistry ? m_audioRegistry->applications() : QVariantList{};
    if (m_audioMetadata == streams)
      return;
    m_audioMetadata = streams;
    ++m_audioRevision;
    m_audioStreams.clear(); // Do not display ownership for superseded identities.
    emit instancesChanged();
  };
  if (registry) {
    connect(registry, &AudioApplicationRegistry::applicationsChanged, this, update);
    connect(registry, &QObject::destroyed, this, [this, update] {
      m_audioRegistry = nullptr;
      update();
    });
  }
  update();
}
void GameDetectionService::rescanGameLibraries() {
  queueLibraryRefresh();
}
void GameDetectionService::queueLibraryRefresh() {
  const bool alreadyPending = m_libraryRefresh;
  m_libraryRefresh = true;
  if (!alreadyPending)
    emit gameLibrariesChanged();
}
void GameDetectionService::updateLibraryWatches(const QStringList &paths) {
  const auto current = m_libraryWatcher.files() + m_libraryWatcher.directories();
  const QSet<QString> currentSet(current.cbegin(), current.cend());
  const QSet<QString> desired(paths.cbegin(), paths.cend());
  QStringList removed, added;
  for (const auto &path : current)
    if (!desired.contains(path))
      removed.append(path);
  for (const auto &path : paths)
    if (!currentSet.contains(path))
      added.append(path);
  if (!removed.isEmpty())
    m_libraryWatcher.removePaths(removed);
  if (!added.isEmpty()) {
    const auto failed = m_libraryWatcher.addPaths(added);
    if (!failed.isEmpty())
      qWarning() << "Could not watch" << failed.size()
                 << "game library paths; timed and candidate refresh remain available";
  }
}
void GameDetectionService::refreshForNewCandidates(
    const ProcessCensus::Snapshot &previous, bool requestRefresh) {
  if (!m_running)
    return;
  bool found = false;
  QSet<QStringList> liveCandidates;
  for (const auto &process : m_processes) {
    const auto payload = process.payload();
    if (payload.isEmpty() || (ProcessGames::excluded(payload) &&
                             !ProcessGames::bootstrapCandidate(payload)))
      continue;
    const bool recognized = std::any_of(m_instances.begin(), m_instances.end(),
        [&](const auto &instance) { return instance.processes.contains(process.pid); });
    if (recognized)
      continue;
    // These are refresh hints, not game evidence. Include unknown native
    // payloads too, without rescanning for every PID of a respawning helper.
    const QStringList key{payload, process.winePrefix, QString::number(process.steamId)};
    if (liveCandidates.size() < 4096)
      liveCandidates.insert(key);
    const auto old = previous.constFind(process.pid);
    if (old != previous.cend() && old->identity() == process.identity() &&
        old->payload() == payload && old->winePrefix == process.winePrefix &&
        old->steamId == process.steamId)
      continue;
    if (m_seenLibraryCandidates.contains(key))
      continue;
    found = true;
  }
  m_seenLibraryCandidates = std::move(liveCandidates);
  if (!requestRefresh || !found || m_libraryRefresh || m_libraryScanning ||
      m_candidateRefresh.isActive())
    return;
  const auto elapsed = m_libraryRefreshClock.isValid() ? m_libraryRefreshClock.elapsed() : 5000;
  m_candidateRefresh.start(int(std::max<qint64>(0, 5000 - elapsed)));
}
void GameDetectionService::saveLibraryPreferences(
    const QString &id, const QVariantMap &preference) {
  if (!QStringList{"steam", "heroic", "legendary", "nile", "lutris", "faugus"}.contains(id))
    return;
  m_libraryPreferences[id] = preference;
  ++m_libraryGeneration; // Only preference changes invalidate in-flight results.
  QSettings().setValue("detection/libraryLocations", m_libraryPreferences);
  // Reflect controls immediately while the worker validates the locations.
  for (auto &value : m_libraryStatus) {
    auto row = value.toMap();
    if (row.value("id").toString() == id) {
      row["automatic"] = preference.value("automatic", true);
      row["paths"] = preference.value("paths").toStringList();
      value = row;
    }
  }
  rescanGameLibraries();
}
void GameDetectionService::setLibraryAutomatic(const QString &id, bool automatic) {
  auto preference = m_libraryPreferences.value(id).toMap();
  preference["automatic"] = automatic;
  saveLibraryPreferences(id, preference);
}
void GameDetectionService::addLibraryLocation(const QString &id, const QUrl &directory) {
  if (!directory.isLocalFile() || !QDir::isAbsolutePath(directory.toLocalFile()))
    return;
  auto preference = m_libraryPreferences.value(id).toMap();
  auto paths = preference.value("paths").toStringList();
  const auto path = QDir::cleanPath(directory.toLocalFile());
  if (paths.contains(path) || paths.size() >= 16)
    return;
  paths.append(path);
  preference["paths"] = paths;
  saveLibraryPreferences(id, preference);
}
void GameDetectionService::removeLibraryLocation(const QString &id, const QString &directory) {
  auto preference = m_libraryPreferences.value(id).toMap();
  auto paths = preference.value("paths").toStringList();
  paths.removeAll(directory);
  preference["paths"] = paths;
  saveLibraryPreferences(id, preference);
}
void GameDetectionService::resetLibraryLocations(const QString &id) {
  saveLibraryPreferences(id, {});
}
QSet<QString> GameDetectionService::desktopApplicationExecutables(
    const QStringList &dataRoots) {
  QSet<QString> executables;
  QSet<QString> seenEntries;
  for (const auto &root : dataRoots) {
    QDirIterator entries(root + "/applications", {"*.desktop"}, QDir::Files,
                         QDirIterator::Subdirectories);
    for (int count = 0; entries.hasNext() && count < 4096; ++count) {
      const QString path = entries.next();
      const QString desktopId =
          QDir(root + "/applications").relativeFilePath(path).replace('/', '-');
      if (seenEntries.contains(desktopId))
        continue;
      seenEntries.insert(desktopId); // Respect higher-priority XDG overrides.
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
        continue;
      QMap<QString, QString> values;
      bool inEntry = false;
      for (const auto &raw : file.read(65536).split('\n')) {
        const auto line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith('[')) {
          inEntry = line == "[Desktop Entry]";
          continue;
        }
        const int equals = line.indexOf('=');
        if (inEntry && !line.startsWith('#') && equals > 0)
          values.insert(line.left(equals).trimmed(), line.mid(equals + 1).trimmed());
      }
      const auto enabled = [&](const char *key) {
        return values.value(QLatin1String(key)).compare("true", Qt::CaseInsensitive) == 0;
      };
      if (values.value("Type") != "Application" || enabled("Hidden") ||
          enabled("NoDisplay") || enabled("Terminal"))
        continue;
      auto command = QProcess::splitCommand(values.value("Exec"));
      if (command.isEmpty())
        continue;
      QString executable = command.takeFirst();
      if (QFileInfo(executable).fileName() == "env") {
        while (!command.isEmpty() && command.first().contains('=') &&
               !command.first().startsWith('-'))
          command.removeFirst();
        if (command.isEmpty() || command.first().startsWith('-'))
          continue; // Do not guess at unsupported env option syntax.
        executable = command.takeFirst();
      }
      if (!QDir::isAbsolutePath(executable))
        executable = QStandardPaths::findExecutable(executable);
      if (executable.isEmpty())
        continue;
      const auto canonical = QFileInfo(executable).canonicalFilePath();
      executables.insert(canonical.isEmpty() ? QDir::cleanPath(executable) : canonical);
    }
  }
  return executables;
}

QVariantList GameDetectionService::runningProcesses() const {
  QVariantList rows;
  for (const auto &p : m_processes) {
    const auto payload = p.payload();
    if (payload.isEmpty() ||
        (ProcessGames::excluded(payload) &&
         ProcessGames::verifiedBootstrapDirectory(p, m_catalogue).isEmpty()))
      continue;
    const auto name = QFileInfo(payload).fileName();
    const bool knownGame = std::any_of(m_instances.begin(), m_instances.end(),
        [&](const auto &game) { return game.primaryPid == p.pid; });
    // Presentation hints only, not detection or proof of a visible window.
    // Unknown native apps remain selectable through Show all processes.
    const bool likelyApplication = knownGame ||
        payload.endsWith(".exe", Qt::CaseInsensitive) ||
        m_applicationExecutables.contains(payload);
    rows.append(QVariantMap{{"pid", p.pid},
                            {"identity", p.identity()},
                            {"title", name},
                            {"path", payload},
                            {"likelyApplication", likelyApplication},
                            {"iconSource", iconSourceForGame(name)}});
  }
  return rows;
}

bool GameDetectionService::addProcessGame(qint64 pid, const QString &identity,
                                          const QString &display, bool block) {
  const auto cached = m_processes.constFind(pid);
  if (cached == m_processes.cend() || cached->identity() != identity)
    return false;
  const auto live = ProcessCensus::inspect(pid);
  const auto payload = live.payload();
  if (live.identity() != identity || live.executable != cached->executable ||
      payload != cached->payload() || payload.isEmpty() ||
      live.winePrefix != cached->winePrefix ||
      (ProcessGames::excluded(payload) &&
       ProcessGames::verifiedBootstrapDirectory(live, m_catalogue).isEmpty()))
    return false; // Exit/reuse/exec since listing: never save a stale rule.
  const auto key = payload.endsWith(".exe", Qt::CaseInsensitive)
                       ? "wine:" + live.winePrefix + ":" + payload
                       : "exe:" + payload;
  return addCustomGame(key,
                       display.trimmed().isEmpty()
                           ? QFileInfo(payload).fileName()
                           : display.trimmed(),
                       block);
}

GameDetectionService::GameDetectionService(QObject *parent)
    : QObject(parent), m_whitelist(std::make_unique<GameWhitelist>()),
      m_gamesModel(std::make_unique<GameListModel>(this)),
      m_gamesFilter(new GameListFilterModel(this)) {
  m_whitelist->load();
  m_libraryPreferences = QSettings().value("detection/libraryLocations").toMap();
  const QStringList ids{"steam", "heroic", "legendary", "nile", "lutris", "faugus"};
  const QStringList names{"Steam", "Heroic", "Legendary", "Nile", "Lutris", "Faugus"};
  for (int i = 0; i < ids.size(); ++i) {
    const auto preference = m_libraryPreferences.value(ids[i]).toMap();
    m_libraryStatus.append(QVariantMap{{"id", ids[i]}, {"name", names[i]},
        {"automatic", preference.value("automatic", true)},
        {"paths", preference.value("paths").toStringList()}, {"detected", false},
        {"count", 0}, {"locations", QVariantList{}}});
  }
  m_gamesFilter->setSourceModel(m_gamesModel.get());
  m_gamesFilter->setShowDefaultEntries(false);
  m_timer.setInterval(1000);
  connect(&m_timer, &QTimer::timeout, this, &GameDetectionService::poll);
  m_libraryFallback.setInterval(30000);
  connect(&m_libraryFallback, &QTimer::timeout, this, &GameDetectionService::queueLibraryRefresh);
  m_libraryDebounce.setSingleShot(true);
  m_libraryDebounce.setInterval(250);
  connect(&m_libraryDebounce, &QTimer::timeout, this, &GameDetectionService::queueLibraryRefresh);
  const auto changed = [this](const QString &) {
    // Coalesce a burst without indefinitely delaying a continuously busy DB.
    if (m_running && !m_libraryDebounce.isActive())
      m_libraryDebounce.start();
  };
  connect(&m_libraryWatcher, &QFileSystemWatcher::fileChanged, this, changed);
  connect(&m_libraryWatcher, &QFileSystemWatcher::directoryChanged, this, changed);
  m_candidateRefresh.setSingleShot(true);
  m_candidateRefresh.setTimerType(Qt::PreciseTimer);
  connect(&m_candidateRefresh, &QTimer::timeout, this, &GameDetectionService::queueLibraryRefresh);
  rebuildModels();
}
GameDetectionService::~GameDetectionService() {
  disconnect(this, nullptr, nullptr, nullptr);
  stop();
}
void GameDetectionService::start() {
  if (m_running)
    return;
  m_running = true;
  if (m_audioRegistry)
    m_audioMetadata = m_audioRegistry->applications();
  m_libraryRefresh = true;
  m_libraryFallback.start();
  m_timer.start();
  poll();
}
void GameDetectionService::stop() {
  m_running = false;
  m_timer.stop();
  m_libraryFallback.stop();
  m_libraryDebounce.stop();
  m_candidateRefresh.stop();
  updateLibraryWatches({});
  m_seenLibraryCandidates.clear();
  if (m_scan.valid())
    m_scan.wait();
  m_scan = {};
  m_libraryScanning = false;
  m_libraryRefresh = false;
  m_audioStreams.clear();
  m_audioMetadata.clear();
  ++m_audioRevision;
  m_instances.clear();
  m_processes.clear();
  m_applicationExecutables.clear();
  m_catalogue.clear();
  m_selectedId.clear();
  ProcessGames::publish({});
  updateCurrentGame({}, 0);
  emit instancesChanged();
  emit gameLibrariesChanged();
}
void GameDetectionService::poll() {
  if (!m_running)
    return;
  if (m_scan.valid()) {
    if (m_scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
    applyScanResult(m_scan.get());
  }
  if (!m_running)
    return;
  const bool refreshCatalogue = m_libraryRefresh;
  const auto preferences = m_libraryPreferences;
  const auto generation = m_libraryGeneration;
  const auto audioMetadata = m_audioMetadata;
  const auto audioRevision = m_audioRevision;
  if (refreshCatalogue) {
    m_libraryRefresh = false;
    m_libraryScanning = true;
    m_libraryRefreshClock.start();
    m_libraryFallback.start();
    m_candidateRefresh.stop();
    emit gameLibrariesChanged();
  }
  if (!m_running)
    return;
  m_scan = std::async(std::launch::async,
      [refreshCatalogue, preferences, generation, audioMetadata, audioRevision] {
    ScanResult result;
    result.processes = ProcessCensus::scan();
    result.audioRevision = audioRevision;
    // Re-evaluate process ownership on the census worker, never on the GUI
    // thread and never by caching a game association with a PipeWire node.
    for (const auto &stream : audioMetadata) {
      auto row = stream.toMap();
      const auto pid = detail::audioProcessId(row.value("pid").toString());
      const auto owner = ProcessGames::associate(pid, row.value("binary").toString(),
                                                row.value("applicationName").toString());
      row["gameInstance"] = owner;
      row["associationEvidence"] = owner.isEmpty() ? "unassigned"
          : pid ? "process-membership" : "unique-executable";
      result.audioStreams.append(row);
    }
    result.catalogueUpdated = refreshCatalogue;
    if (refreshCatalogue) {
      auto discovery = LauncherLibrary::discover(preferences);
      result.catalogue = std::move(discovery.games);
      result.libraryStatus = std::move(discovery.launchers);
      result.libraryWatchPaths = std::move(discovery.watchPaths);
      result.libraryGeneration = generation;
      result.applicationExecutables = desktopApplicationExecutables(
          QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation));
    }
    return result;
  });
}
void GameDetectionService::applyScanResult(ScanResult result) {
  if (result.audioRevision == m_audioRevision)
    m_audioStreams = std::move(result.audioStreams);
  if (result.catalogueUpdated && result.libraryGeneration == m_libraryGeneration) {
    m_catalogue = std::move(result.catalogue);
    m_libraryStatus = std::move(result.libraryStatus);
    m_applicationExecutables = std::move(result.applicationExecutables);
    updateLibraryWatches(result.libraryWatchPaths);
  }
  if (result.catalogueUpdated) {
    m_libraryScanning = false;
    emit gameLibrariesChanged();
  }
  // A fresh catalogue already covers this census's candidates. Other
  // censuses can request an early refresh after a missed file notification.
  const auto previousProcesses = m_processes;
  apply(result.processes);
  refreshForNewCandidates(previousProcesses, !result.catalogueUpdated);
}
void GameDetectionService::apply(const ProcessCensus::Snapshot &snapshot) {
  const auto previousProcesses = m_processes;
  const auto previousInstances = m_instances;
  m_processes = snapshot;
  QMap<QString, QString> definitions;
  for (const auto &key : m_whitelist->whitelistEntries())
    definitions[key] = m_whitelist->getWhitelistName(key);
  m_instances =
      ProcessGames::identify(snapshot, m_catalogue, definitions,
                             m_whitelist->blacklistEntries(), m_instances);
  // Lifecycle-only diagnostics: enough to distinguish an observed handoff
  // from a broker-mediated/unobserved launch, without logging argv or tokens.
  const auto sameProcess = [](const ProcessInfo &a, const ProcessInfo &b) {
    return a.pid == b.pid && a.start == b.start &&
           a.executable == b.executable && a.payload() == b.payload() &&
           a.winePrefix == b.winePrefix;
  };
  for (const auto &game : m_instances) {
    if (!game.launchContext.hasScope())
      continue;
    for (const auto &p : game.processes)
      if (!sameProcess(p, previousProcesses.value(p.pid)) ||
          std::none_of(previousInstances.begin(), previousInstances.end(),
                       [&](const auto &old) {
                         return old.id == game.id && old.processes.contains(p.pid);
                       }))
        qDebug() << "Launch tracking member:" << game.name << "PID" << p.pid
                 << "start" << p.start << "parent" << p.parent
                 << "payload" << p.payload();
  }
  auto scopes = previousInstances;
  scopes.append(m_instances);
  QSet<QString> tracedScopes;
  for (const auto &game : scopes) {
    const auto scopeKey = game.launchContext.directory + QChar('\0') + game.launchContext.prefix;
    if (!game.launchContext.hasScope() || tracedScopes.contains(scopeKey))
      continue;
    tracedScopes.insert(scopeKey);
    for (const auto &p : snapshot) {
      if (!game.launchContext.contains(p) ||
          sameProcess(p, previousProcesses.value(p.pid)) ||
          std::any_of(m_instances.begin(), m_instances.end(), [&](const auto &owner) {
            return owner.processes.contains(p.pid);
          }))
        continue;
      qDebug() << "Unowned launch-installation process:" << game.name
               << "PID" << p.pid << "start" << p.start << "parent" << p.parent
               << "payload" << p.payload() << "- no verified game membership";
    }
  }
  for (const auto &old : previousInstances)
    if (old.launchContext.hasScope() &&
        std::none_of(m_instances.begin(), m_instances.end(),
                     [&](const auto &game) { return game.id == old.id; }))
      qDebug() << "Launch tracking ended:" << old.name
               << "- no verified members remain";
  ProcessGames::publish(m_instances);
  auto selected =
      std::find_if(m_instances.begin(), m_instances.end(),
                   [&](const auto &g) { return g.id == m_selectedId; });
  if (selected == m_instances.end() && !m_instances.isEmpty())
    selected =
        m_instances.begin(); // Stable target until exit or explicit selection.
  if (selected == m_instances.end()) {
    m_selectedId.clear();
    updateCurrentGame({}, 0);
  } else {
    const auto selection = *selected;
    const bool instanceChanged = m_selectedId != selection.id;
    if (instanceChanged && !m_selectedId.isEmpty())
      updateCurrentGame({}, 0);
    m_selectedId = selection.id;
    updateCurrentGame(selection.name, selection.primaryPid, selection.key);
  }
  rebuildModels();
  emit instancesChanged();
}
QString GameDetectionService::currentGameSource() const {
  for (const auto &game : m_instances)
    if (game.id == m_selectedId)
      return game.detectionSources.join(", ");
  return {};
}

bool GameDetectionService::isGamePayloadReady() const {
  if (!isGameRunning())
    return false;
  for (const auto &game : m_instances)
    if (game.id == m_selectedId)
      return ProcessGames::verifiedBootstrapDirectory(
                 game.processes.value(game.primaryPid), m_catalogue).isEmpty();
  return true; // Direct/manual identities retain their existing behavior.
}

bool GameDetectionService::selectInstance(const QString &id) {
  const auto found = std::find_if(m_instances.begin(), m_instances.end(),
                                  [&](const auto &g) { return g.id == id; });
  if (found == m_instances.end())
    return false;
  const auto selection = *found;
  if (m_selectedId != selection.id)
    updateCurrentGame({}, 0);
  m_selectedId = selection.id;
  updateCurrentGame(selection.name, selection.primaryPid, selection.key);
  rebuildModels();
  return true;
}
QVariantList GameDetectionService::instances() const {
  QVariantList list;
  for (const auto &g : m_instances) {
    QVariantList pids;
    QVariantList audio;
    for (const auto &stream : m_audioStreams)
      if (stream.toMap().value("gameInstance").toString() == g.id)
        audio.append(stream);
    for (const auto &p : g.processes)
      pids.append(QVariantMap{{"pid", p.pid},
                              {"startTime", QVariant::fromValue(p.start)},
                              {"executable", p.payload()}});
    list.append(QVariantMap{
        {"id", g.id},
        {"key", g.key},
         {"name", g.name},
         {"detectionSources", g.detectionSources},
        {"label", QString("%1 (PID %2)").arg(g.name).arg(g.primaryPid)},
        {"pid", g.primaryPid},
        {"processes", pids},
        {"selected", g.id == m_selectedId},
        {"audioStreams", audio},
        {"audioState", audio.isEmpty() ? "waiting" : "associated"}});
  }
  return list;
}
QVariantMap
GameDetectionService::captureIdentityInfo(const QString &name) const {
  if (name != m_currentGameName || !isGameRunning())
    return {};
  return {{"detectedGameKey", m_currentGameKey},
          {"detectedGamePid", m_currentGamePid},
          {"detectedGameInstance", m_selectedId},
          {"detectedSteamAppId",
           QString::number(steamAppIdFromAppId(m_currentGameKey))}};
}
bool GameDetectionService::instanceLaunchContextAlive(const QString &id) const {
  for (const auto &game : m_instances)
    if (game.id == id && game.launchContext.runtime.pid)
      return game.launchContext.matchesRuntime(
          ProcessCensus::inspect(game.launchContext.runtime.pid));
  return false;
}

bool GameDetectionService::instanceAlive(const QString &id) const {
  for (const auto &g : m_instances)
    if (g.id == id)
      for (const auto &p : g.processes) {
        const auto live = ProcessCensus::inspect(p.pid);
        if (live.start == p.start && live.executable == p.executable &&
            live.payload() == p.payload())
          return true;
      }
  return false;
}
uint GameDetectionService::steamAppIdFromAppId(const QString &key) {
  return QRegularExpression("^steam_app_(\\d+)$")
      .match(key)
      .captured(1)
      .toUInt();
}
uint GameDetectionService::steamAppIdForPid(qint64 pid) {
  return ProcessCensus::inspect(pid).steamId;
}
bool GameDetectionService::processExists(qint64 pid) {
  return ProcessCensus::inspect(pid).pid != 0;
}
QString GameDetectionService::getSteamAppName(uint id) const {
  for (const auto &g : m_catalogue)
    if (id && g.steamId == id)
      return g.name;
  return {};
}
void GameDetectionService::updateCurrentGame(QString name, qint64 pid,
                                             QString key) {
  if (m_currentGameName == name && m_currentGamePid == pid &&
      m_currentGameKey == key)
    return;
  const auto old = m_currentGameName;
  const bool changed = old != name || m_currentGameKey != key;
  m_currentGameName = name;
  m_currentGamePid = pid;
  m_currentGameKey = key;
  emit currentGameChanged();
  if (!old.isEmpty() && changed)
    emit gameClosed(old);
  if (!name.isEmpty() && changed) {
    const auto index = m_recentKeys.indexOf(key);
    if (index >= 0) {
      m_recentKeys.removeAt(index);
      m_recentGames.removeAt(index);
    }
    m_recentKeys.prepend(key);
    m_recentGames.prepend(name);
    while (m_recentKeys.size() > 10) {
      m_recentKeys.removeLast();
      m_recentGames.removeLast();
    }
    emit recentGamesChanged();
    emit gameDetected(name, pid);
  }
}
void GameDetectionService::rulesChanged() {
  m_whitelist->save();
  // Reidentify from scratch: deleting a rule must not retain its old members.
  m_instances.clear();
  apply(m_processes);
  emit whitelistChanged();
  emit blacklistChanged();
}
bool GameDetectionService::addToWhitelist(const QString &key,
                                          const QString &display) {
  if (key.trimmed().isEmpty() || GameWhitelist::isPlaceholderReporterKey(key))
    return false;
  const auto name = display.trimmed().isEmpty()
                        ? GameWhitelist::normalizeKey(key)
                        : display.trimmed();
  const bool changed = !m_whitelist->isWhitelisted(key) ||
                       m_whitelist->getWhitelistName(key) != name;
  if (!changed)
    return false;
  m_whitelist->addToWhitelist(key, name);
  rulesChanged();
  return changed;
}
bool GameDetectionService::addToBlacklist(const QString &key) {
  if (key.trimmed().isEmpty())
    return false;
  const bool changed = !m_whitelist->isBlacklisted(key);
  m_whitelist->addToBlacklist(key);
  rulesChanged();
  return changed;
}
bool GameDetectionService::removeFromWhitelist(const QString &key) {
  if (!m_whitelist->isWhitelisted(key))
    return false;
  m_whitelist->removeFromWhitelist(key);
  rulesChanged();
  return true;
}
bool GameDetectionService::removeFromBlacklist(const QString &key) {
  if (!m_whitelist->isBlacklisted(key))
    return false;
  m_whitelist->removeFromBlacklist(key);
  rulesChanged();
  return true;
}
bool GameDetectionService::moveToWhitelist(const QString &key,
                                           const QString &display) {
  return addToWhitelist(key, display);
}
bool GameDetectionService::moveToBlacklist(const QString &key) {
  return addToBlacklist(key);
}
bool GameDetectionService::unblockGame(const QString &key) {
  return removeFromBlacklist(key);
}
bool GameDetectionService::isBlocked(const QString &key) const {
  return m_whitelist->isBlacklisted(key);
}
bool GameDetectionService::addSteamGame(uint id, const QString &display) {
  return id && addCustomGame(QString("steam_app_%1").arg(id), display, false);
}
bool GameDetectionService::addCustomGame(const QString &key,
                                         const QString &display, bool block) {
  if (key.trimmed().isEmpty())
    return false;
  m_whitelist->noteCustomKey(key);
  const bool changed =
      block ? addToBlacklist(key) : addToWhitelist(key, display);
  if (!changed) {
    // Custom-list bookkeeping can change even if the definition is identical.
    // Persist it without rebuilding live process instances during callbacks.
    m_whitelist->save();
    rebuildModels();
  }
  return changed;
}
bool GameDetectionService::forgetGame(const QString &key) {
  m_whitelist->removeFromWhitelist(key);
  m_whitelist->removeFromBlacklist(key);
  m_whitelist->forgetCustomKey(key);
  for (qsizetype i = m_recentKeys.size() - 1; i >= 0; --i)
    if (m_recentKeys[i] == GameWhitelist::normalizeKey(key)) {
      m_recentKeys.removeAt(i);
      m_recentGames.removeAt(i);
    }
  rulesChanged();
  emit recentGamesChanged();
  return true;
}
QStringList GameDetectionService::whitelist() const {
  return m_whitelist->whitelistEntries();
}
QStringList GameDetectionService::blacklist() const {
  return m_whitelist->blacklistEntries();
}
QString GameDetectionService::gameFilterText() const {
  return m_gamesFilter->filterText();
}
void GameDetectionService::setGameFilterText(const QString &text) {
  m_gamesFilter->setFilterText(text);
  emit gameFilterTextChanged();
}
bool GameDetectionService::showDefaultExclusions() const {
  return m_gamesFilter->showDefaultEntries();
}
void GameDetectionService::setShowDefaultExclusions(bool show) {
  m_gamesFilter->setShowDefaultEntries(show);
  emit showDefaultExclusionsChanged();
}
void GameDetectionService::setTokenGames(const QStringList &games) {
  const QSet<QString> next(games.begin(), games.end());
  if (next == m_tokenGames)
    return;
  m_tokenGames = next;
  rebuildModels();
  emit tokenGamesChanged();
}
QUrl GameDetectionService::iconSourceForGame(const QString &name) const {
  QUrl result;
  for (int i = 0; i < m_gamesModel->rowCount(); ++i) {
    const auto index = m_gamesModel->index(i);
    if (m_gamesModel->data(index, GameListModel::DisplayRole).toString() !=
        name)
      continue;
    const auto source =
        m_gamesModel->data(index, GameListModel::IconSourceRole).toUrl();
    if (!result.isEmpty() && result != source)
      return {};
    result = source;
  }
  return result.isEmpty() ? gameIconSource(name) : result;
}
void GameDetectionService::rebuildModels() {
  QList<GameListEntry> rows;
  QSet<QString> claimed;
  const auto append = [&](const QString &key, QString name, qint64 pid = 0) {
    if (key.isEmpty() || claimed.contains(key))
      return;
    claimed.insert(key);
    GameListEntry e;
    e.key = key;
    e.steamAppId = steamAppIdFromAppId(key);
    if (name.isEmpty())
      name = m_whitelist->getWhitelistName(key);
    if (name.isEmpty())
      name = getSteamAppName(e.steamAppId);
    e.display = name.isEmpty() ? key : name;
    e.pid = pid;
    e.running = pid > 0;
    e.listed = isBlocked(key) || isBlocked(e.display) ? 2
               : m_whitelist->isWhitelisted(key)      ? 1
                                                      : 0;
    e.isDefault = GameWhitelist::defaultBlacklistEntries().contains(key);
    if (e.isDefault)
      e.display = GameWhitelist::prettySystemName(key);
    e.tokenKey = m_tokenGames.contains(key)         ? key
                 : m_tokenGames.contains(e.display) ? e.display
                                                    : QString();
    e.hasToken = !e.tokenKey.isEmpty();
    e.section = e.running    ? "Now playing"
                : e.hasToken ? "Ready to capture"
                             : "Apps";
    rows.append(e);
  };
  for (const auto &g : m_instances)
    if (g.id == m_selectedId)
      append(g.key, g.name, g.primaryPid);
  for (const auto &g : m_instances)
    append(g.key, g.name, g.primaryPid);
  for (int i = 0; i < m_recentKeys.size(); ++i)
    append(m_recentKeys[i], m_recentGames[i]);
  for (const auto &key : whitelist())
    append(key, {});
  for (const auto &key : m_tokenGames)
    append(key, {});
  for (const auto &key : blacklist())
    append(key, {});
  for (const auto &key : m_whitelist->removedDefaults())
    append(key, {});
  for (const auto &key : m_whitelist->customKeys())
    append(key, {});
  m_gamesModel->setEntries(rows);
}
} // namespace TrophyBackend
