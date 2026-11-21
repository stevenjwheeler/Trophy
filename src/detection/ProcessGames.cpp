/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ProcessGames.h"
#include "GameWhitelist.h"
#include "PrerequisiteInstallers.h"
#include <QMutexLocker>
#include <QSet>
#include <algorithm>

namespace TrophyBackend {
namespace {
QMutex mutex;
QList<GameInstance> published;
QString base(QString path) {
  path.replace('\\', '/');
  return path.section('/', -1);
}
QString normalized(const QString &s) { return GameWhitelist::normalizeKey(s); }
bool under(const QString &path, const QString &directory) {
  return !directory.isEmpty() && path.startsWith(directory + "/");
}

} // namespace

namespace {
bool alwaysExcluded(const QString &payload) {
  const auto name = normalized(base(payload));
  return name.isEmpty() || name.endsWith("updater") ||
         prerequisiteInstallerNames().contains(name) ||
         QStringList{"steam",
                     "heroic",
                     "lutris",
                     "legendary",
                     "nile",
                     "bottles",
                     "galaxyclient",
                     "battle.net",
                     "eadesktop",
                     "ubisoftconnect",
                     "upc",
                     "epicgameslauncher",
                     "ealauncher",
                     "origin",
                     "originthinsetupinternal",
                     "ubisoftgamelauncher",
                     "battlenet",
                     "steamwebhelper",
                     "steamservice",
                     "proton",
                     "wine",
                     "wine64",
                     "wine-preloader",
                     "wine64-preloader",
                     "wineserver",
                     "services",
                     "winedevice",
                     "wineboot",
                     "explorer",
                     "rundll32",
                     "conhost",
                     "rpcss",
                     "plugplay",
                     "svchost",
                     "start",
                     "cmd",
                     "python",
                     "python3",
                     "sh",
                     "bash",
                     "pressure-vessel-wrap",
                     "pv-bwrap",
                     "bwrap",
                     "pv-adverb",
                     "srt-bwrap",
                     "reaper",
                     "steam-runtime-launcher-service",
                     "steam-runtime-launch-client",
                     "crashpad_handler",
                     "unitycrashhandler64",
                     "unitycrashhandler32",
                     "crashreportclient",
                     "discord",
                     "firefox",
                     "chrome",
                     "chromium"}
              .contains(name);
}
} // namespace

bool ProcessGames::excluded(const QString &payload) {
  return alwaysExcluded(payload) ||
         normalized(base(payload)).endsWith("launcher");
}

bool ProcessGames::bootstrapCandidate(const QString &payload) {
  return !alwaysExcluded(payload) &&
         normalized(base(payload)).endsWith("launcher");
}

QString ProcessGames::verifiedBootstrapDirectory(
    const ProcessInfo &process, const QList<LauncherGame> &catalogue) {
  const auto payload = process.payload();
  if (!bootstrapCandidate(payload))
    return {};
  QSet<QString> names, directories;
  for (const auto &game : catalogue) {
    if (game.steamId || payload != game.executable ||
        (!game.winePrefix.isEmpty() && process.winePrefix != game.winePrefix))
      continue;
    names.insert(game.name);
    if (game.bootstrap && under(payload, game.directory))
      directories.insert(game.directory);
  }
  return names.size() == 1 && directories.size() == 1
             ? *directories.begin() : QString();
}

QString ProcessGames::verifiedInstallationDirectory(
    const ProcessInfo &process, const QList<LauncherGame> &catalogue) {
  const auto payload = process.payload();
  QSet<QString> steamDirectories, directories, names;
  for (const auto &game : catalogue) {
    if (game.steamId) {
      if ((!process.steamId || process.steamId == game.steamId) &&
          under(payload, game.directory))
        steamDirectories.insert(game.directory);
    } else if (payload == game.executable &&
               (game.winePrefix.isEmpty() || process.winePrefix == game.winePrefix)) {
      names.insert(game.name);
      if (under(payload, game.directory))
        directories.insert(game.directory);
    }
  }
  // Match the existing Steam precedence, but never choose an ambiguous scope.
  if (!steamDirectories.isEmpty())
    return steamDirectories.size() == 1 ? *steamDirectories.begin() : QString();
  return names.size() == 1 && directories.size() == 1
             ? *directories.begin() : QString();
}

QList<GameInstance> ProcessGames::identify(
    const ProcessCensus::Snapshot &ps, const QList<LauncherGame> &catalogue,
    const QMap<QString, QString> &definitions, const QStringList &blocked,
    const QList<GameInstance> &previous) {
  struct Seed {
    QString key, name, anchor, previousId;
    GameLaunchContext launchContext;
    bool explicitRule = false;
    QStringList sources;
  };
  QMap<qint64, Seed> seeds;
  const auto veto = [&](const ProcessInfo &p, const QString &key) {
    return blocked.contains(normalized(key)) ||
           blocked.contains(normalized(base(p.payload()))) ||
           blocked.contains(normalized("exe:" + p.payload())) ||
           blocked.contains(
               normalized("wine:" + p.winePrefix + ":" + p.payload()));
  };
  for (const auto &p : ps) {
    const auto payload = p.payload();
    const auto bootstrapDirectory = verifiedBootstrapDirectory(p, catalogue);
    if (excluded(payload) && bootstrapDirectory.isEmpty())
      continue;
    QString key, name;
    QStringList sources;
    const QString pathKey = payload.endsWith(".exe", Qt::CaseInsensitive)
                                ? "wine:" + p.winePrefix + ":" + payload
                                : "exe:" + payload;
    for (auto d = definitions.begin(); d != definitions.end(); ++d) {
      if (d.key() == normalized(pathKey) || d.key() == normalized(payload) ||
          d.key() == normalized(base(payload)) ||
          (p.steamId && d.key() == QString("steam_app_%1").arg(p.steamId))) {
        key = d.key();
        name = d.value();
        break;
      }
    }
    const bool explicitRule = !key.isEmpty();
    // Environment is inherited by launchers/services. Require a payload in
    // the matching installed game's directory, not any "steamapps" path.
    for (const auto &game : catalogue) {
      if (!game.steamId || (p.steamId && p.steamId != game.steamId) ||
          !under(payload, game.directory))
        continue;
      key = QString("steam_app_%1").arg(game.steamId);
      if (name.isEmpty())
        name = game.name;
      sources.append(game.sources.isEmpty() ? QStringList{"Steam"} : game.sources);
      break;
    }
    if (sources.isEmpty()) {
      QSet<QString> names;
      QStringList launcherSources;
      for (const auto &game : catalogue)
        if (!game.steamId && payload == game.executable &&
            (game.winePrefix.isEmpty() || p.winePrefix == game.winePrefix)) {
          names.insert(game.name);
          launcherSources.append(game.sources.isEmpty()
                                     ? QStringList{"local launcher metadata"} : game.sources);
        }
      // Conflicting metadata must not silently choose a title. Exact user
      // rules and Steam metadata above remain authoritative.
      if (names.size() == 1) {
        if (key.isEmpty()) {
          key = normalized(pathKey);
          name = *names.begin();
        }
        sources.append(launcherSources);
      }
    }
    if (key.isEmpty() || veto(p, key) || blocked.contains(normalized(name)))
      continue;
    if (name.isEmpty())
      name = base(payload);
    if (explicitRule)
      sources.append("user rule");
    sources.removeDuplicates();
    auto context = GameLaunchContext::attest(
        p, bootstrapDirectory.isEmpty()
               ? verifiedInstallationDirectory(p, catalogue) : bootstrapDirectory, ps);
    // Ordinary direct/descendant detection retains its existing policy when
    // no private runtime is attested. Bootstraps still require scoped children.
    if (!context.runtime.pid && bootstrapDirectory.isEmpty())
      context = {};
    auto anchor = p;
    QSet<qint64> visited{p.pid};
    // Steam launch context groups sibling Wine payloads, but never crosses
    // into Steam itself or another App ID. Standalone instances use the
    // matched executable root, not an entire Wine prefix.
    if (p.steamId && key.startsWith("steam_app_") && !context.hasScope()) {
      while (ps.contains(anchor.parent) && !visited.contains(anchor.parent)) {
        const auto parent = ps.value(anchor.parent);
        const auto parentPayload = normalized(base(parent.payload()));
        if (parentPayload.isEmpty() ||
            QStringList{"steam", "steamwebhelper", "wineserver", "services",
                        "explorer", "winedevice", "firefox", "discord",
                        "chrome", "chromium"}
                .contains(parentPayload))
          break; // Unresolved parents/shared brokers cannot prove a launch.
        if (parent.start > anchor.start || parent.steamId != p.steamId ||
            normalized(base(parent.executable)) == "steam")
          break;
        visited.insert(parent.pid);
        anchor = parent;
      }
    }
    seeds.insert(p.pid, {key, name,
                         context.runtime.pid ? context.runtime.identity() : anchor.identity(),
                         {}, context, explicitRule, sources});
  }
  // A sibling may itself match a catalogue entry. Resolve it to the observed
  // launch before assigning fresh instance IDs; explicit different user rules
  // remain separate. Names alone never establish the relationship.
  const auto originalSeeds = seeds;
  QSet<qint64> deniedSeeds;
  for (auto it = seeds.begin(); it != seeds.end(); ++it) {
    auto &seed = it.value();
    const auto p = ps.value(it.key());
    QMap<QString, Seed> candidates;
    const auto compatible = [&](const QString &key, const QString &name) {
      return seed.key == key || (!seed.explicitRule && seed.name == name);
    };
    for (const auto &old : previous) {
      const auto prior = old.processes.value(p.pid);
      const bool unchanged = prior.pid == p.pid && prior.start == p.start &&
                             prior.executable == p.executable &&
                             prior.payload() == p.payload() &&
                             prior.winePrefix == p.winePrefix;
      if (!old.launchContext.hasScope() || !compatible(old.key, old.name) ||
          !(unchanged ? old.launchContext.contains(p)
                      : old.launchContext.ownsSibling(p, ps)))
        continue;
      candidates.insert(old.id, {old.key, old.name, {}, old.id,
                                  old.launchContext, seed.explicitRule,
                                  old.detectionSources});
    }
    // An unchanged member's prior identity wins over a freshly formed root.
    if (candidates.isEmpty())
      for (auto root = originalSeeds.begin(); root != originalSeeds.end(); ++root) {
        const auto &owner = root.value();
        if (root.key() == p.pid || !owner.launchContext.runtime.pid ||
            !compatible(owner.key, owner.name) ||
            !owner.launchContext.ownsSibling(p, ps))
          continue;
        candidates.insert("game:" + owner.key + "@" + owner.anchor, owner);
      }
    if (candidates.size() == 1) {
      const auto &owner = candidates.first();
      if (veto(p, owner.key) || blocked.contains(normalized(owner.name))) {
        deniedSeeds.insert(p.pid);
        continue;
      }
      const bool userRule = seed.explicitRule;
      const auto currentSources = seed.sources;
      seed = owner;
      seed.explicitRule = userRule;
      seed.sources.append(currentSources);
      seed.sources.removeDuplicates();
    }
  }
  for (const auto pid : deniedSeeds)
    seeds.remove(pid);
  QList<GameInstance> result;
  QMap<qint64, QString> owners;
  for (auto it = seeds.begin(); it != seeds.end(); ++it) {
    auto seed = it.value();
    const auto p = ps.value(it.key());
    // A matched descendant with the same key belongs to its matched root.
    auto parent = p.parent;
    auto childStart = p.start;
    QSet<qint64> visited;
    while (ps.contains(parent) && !visited.contains(parent)) {
      if (ps.value(parent).start > childStart)
        break;
      childStart = ps.value(parent).start;
      visited.insert(parent);
      if (seeds.contains(parent)) {
        if (seeds.value(parent).key != seed.key)
          break;
        seed.anchor = seeds.value(parent).anchor;
      }
      parent = ps.value(parent).parent;
    }
    QString id = seed.previousId.isEmpty()
                     ? "game:" + seed.key + "@" + seed.anchor : seed.previousId;
    for (const auto &old : previous) {
      const auto prior = old.processes.value(p.pid);
      if (old.key == seed.key && prior.start == p.start &&
          prior.executable == p.executable && prior.payload() == p.payload() &&
          prior.winePrefix == p.winePrefix) {
        id = old.id;
        break;
      }
    }
    auto found = std::find_if(result.begin(), result.end(),
                              [&](const auto &g) { return g.id == id; });
    if (found == result.end()) {
      result.append({id, seed.key, seed.name, p.pid, {},
                     seed.launchContext, seed.sources});
      found = std::prev(result.end());
    }
    found->detectionSources.append(seed.sources);
    found->detectionSources.removeDuplicates();
    found->processes.insert(p.pid, p);
    owners[p.pid] = id;
  }
  // Retain only surviving, unchanged members across reparenting. Never carry
  // ownership across PID reuse or exec into a different application.
  for (const auto &old : previous) {
    for (const auto &prior : old.processes) {
      const auto p = ps.value(prior.pid);
      if (!p.pid || p.start != prior.start ||
          p.executable != prior.executable || p.payload() != prior.payload() ||
          owners.contains(p.pid) ||
          (excluded(p.payload()) && verifiedBootstrapDirectory(p, catalogue).isEmpty()) ||
          veto(p, old.key) || blocked.contains(normalized(old.name)) ||
          (old.launchContext.hasScope() && !old.launchContext.contains(p)))
        continue;
      auto found = std::find_if(result.begin(), result.end(),
                                [&](const auto &g) { return g.id == old.id; });
      if (found == result.end()) {
        result.append({old.id, old.key, old.name, p.pid, {},
                       old.launchContext, old.detectionSources});
        found = std::prev(result.end());
      }
      found->processes.insert(p.pid, p);
      owners[p.pid] = old.id;
    }
  }
  // Adopt only installation payloads whose ancestry resolves to the exact
  // private runtime attested while an identified seed was present. The runtime
  // itself is never a member and cannot keep a finished game alive.
  auto contexts = result;
  contexts.append(previous);
  for (const auto &p : ps) {
    if (owners.contains(p.pid) || excluded(p.payload()))
      continue;
    QMap<QString, GameInstance> candidates;
    for (const auto &game : contexts) {
      if (veto(p, game.key) ||
          blocked.contains(normalized(game.name)) ||
          !game.launchContext.ownsSibling(p, ps))
        continue;
      candidates.insert(game.id, game);
    }
    if (candidates.size() != 1)
      continue;
    const auto owner = candidates.first();
    auto found = std::find_if(result.begin(), result.end(),
                              [&](const auto &game) { return game.id == owner.id; });
    if (found == result.end()) {
      result.append({owner.id, owner.key, owner.name, p.pid, {},
                     owner.launchContext, owner.detectionSources});
      found = std::prev(result.end());
    }
    found->processes.insert(p.pid, p);
    owners[p.pid] = owner.id;
  }
  // Descendants can produce audio without matching a game definition. Never
  // walk up to Steam/Wine and adopt siblings by shared launcher alone.
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &p : ps) {
      if (owners.contains(p.pid) || !owners.contains(p.parent) ||
          excluded(p.payload()) || ps.value(p.parent).start > p.start)
        continue;
      const auto owner = owners.value(p.parent);
      for (auto &g : result) {
        if (g.id != owner || veto(p, g.key) ||
            (g.launchContext.hasScope() && !g.launchContext.contains(p)) ||
            (p.steamId && p.steamId != ps.value(p.parent).steamId))
          continue;
        g.processes.insert(p.pid, p);
        owners[p.pid] = owner;
        changed = true;
        break;
      }
    }
  }
  // A metadata-backed bootstrap establishes launch identity, not a capture
  // target. Prefer its verified payload as soon as one joins the same
  // instance, even when the launcher stays open (and has the lower PID).
  for (auto &game : result) {
    const auto primary = game.processes.value(game.primaryPid);
    if (verifiedBootstrapDirectory(primary, catalogue).isEmpty())
      continue;
    for (const auto &member : game.processes)
      if (verifiedBootstrapDirectory(member, catalogue).isEmpty()) {
        game.primaryPid = member.pid;
        break;
      }
  }
  return result;
}

void ProcessGames::publish(const QList<GameInstance> &games) {
  QMutexLocker lock(&mutex);
  published = games;
}
QList<GameInstance> ProcessGames::snapshot() {
  QMutexLocker lock(&mutex);
  return published;
}
QString ProcessGames::instanceForPid(qint64 pid) { return associate(pid, {}); }
QString ProcessGames::pinProcess(qint64 pid) {
  const auto p = ProcessCensus::inspect(pid);
  return p.pid ? "process@" + p.identity() : QString();
}
bool ProcessGames::belongsTo(const QString &instance, qint64 pid,
                             const QString &binary,
                             const QString &application) {
  if (!instance.startsWith("process@"))
    return !instance.isEmpty() &&
           associate(pid, binary, application) == instance;
  const auto p = ProcessCensus::inspect(pid);
  return pid > 0 && p.pid > 0 && instance == "process@" + p.identity() &&
         (binary.isEmpty() ||
          base(binary).compare(base(p.executable), Qt::CaseInsensitive) == 0 ||
          base(binary).compare(base(p.payload()), Qt::CaseInsensitive) == 0);
}
QString ProcessGames::associate(qint64 pid, const QString &binary,
                                const QString &application) {
  const auto games = snapshot();
  QSet<QString> candidates;
  // Application names are not general identity. Only accept an exact payload
  // basename (including .exe), which Wine commonly supplies as app.name.
  const auto evidence = !binary.isEmpty() ? binary : application;
  for (const auto &g : games) {
    for (const auto &p : g.processes) {
      if (pid && p.pid != pid)
        continue;
      if (!pid &&
          (evidence.isEmpty() ||
           base(p.payload()).compare(base(evidence), Qt::CaseInsensitive) != 0))
        continue;
      const auto live = ProcessCensus::inspect(p.pid);
      if (live.start != p.start || live.executable != p.executable ||
          live.payload() != p.payload())
        continue;
      // A contradictory binary is never repaired by a matching PID.
      if (pid && !binary.isEmpty() &&
          base(p.payload()).compare(base(binary), Qt::CaseInsensitive) != 0 &&
          base(p.executable).compare(base(binary), Qt::CaseInsensitive) != 0)
        continue;
      candidates.insert(g.id);
    }
  }
  return candidates.size() == 1 ? *candidates.begin() : QString();
}
} // namespace TrophyBackend
