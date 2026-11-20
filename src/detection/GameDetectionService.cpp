/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "GameDetectionService.h"
#include "GameIconSource.h"
#include "GameWhitelist.h"
#include "audio/PipeWireAudioSource.h"
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QStandardPaths>
#include <QRegularExpression>
#include <algorithm>

namespace TrophyBackend {
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
    if (payload.isEmpty() || ProcessGames::excluded(payload))
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
      live.winePrefix != cached->winePrefix || ProcessGames::excluded(payload))
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
  m_gamesFilter->setSourceModel(m_gamesModel.get());
  m_gamesFilter->setShowDefaultEntries(false);
  m_timer.setInterval(1000);
  connect(&m_timer, &QTimer::timeout, this, &GameDetectionService::poll);
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
  m_scanCount = 0;
  m_timer.start();
  poll();
}
void GameDetectionService::stop() {
  m_running = false;
  m_timer.stop();
  if (m_scan.valid())
    m_scan.wait();
  if (m_audioScan.valid())
    m_audioScan.wait();
  m_scan = {};
  m_audioScan = {};
  m_audioStreams.clear();
  m_instances.clear();
  m_processes.clear();
  m_applicationExecutables.clear();
  m_selectedId.clear();
  ProcessGames::publish({});
  updateCurrentGame({}, 0);
  emit instancesChanged();
}
void GameDetectionService::poll() {
  if (!m_running)
    return;
  if (m_audioScan.valid() && m_audioScan.wait_for(std::chrono::seconds(0)) ==
                                 std::future_status::ready) {
    m_audioStreams = m_audioScan.get();
    emit instancesChanged();
  }
  if (!m_audioScan.valid())
    m_audioScan = std::async(std::launch::async, [] {
      return PipeWireAudioSource::enumerateAudioApps();
    });
  if (m_scan.valid()) {
    if (m_scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
    auto result = m_scan.get();
    if (result.catalogueUpdated) {
      m_steam = std::move(result.catalogue);
      m_applicationExecutables = std::move(result.applicationExecutables);
    }
    apply(result.processes);
  }
  const bool refreshCatalogue = m_scanCount++ % 30 == 0;
  m_scan = std::async(std::launch::async, [refreshCatalogue] {
    ScanResult result;
    result.processes = ProcessCensus::scan();
    result.catalogueUpdated = refreshCatalogue;
    if (refreshCatalogue) {
      result.catalogue = ProcessGames::steamLibrary();
      result.applicationExecutables = desktopApplicationExecutables(
          QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation));
    }
    return result;
  });
}
void GameDetectionService::apply(const ProcessCensus::Snapshot &snapshot) {
  m_processes = snapshot;
  QMap<QString, QString> definitions;
  for (const auto &key : m_whitelist->whitelistEntries())
    definitions[key] = m_whitelist->getWhitelistName(key);
  m_instances =
      ProcessGames::identify(snapshot, m_steam, definitions,
                             m_whitelist->blacklistEntries(), m_instances);
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
  for (const auto &g : m_steam)
    if (g.id == id)
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
