/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ProcessGames.h"
#include "GameWhitelist.h"
#include "PrerequisiteInstallers.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
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
QString text(const QString &path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(1024LL * 1024))
                                     : QString();
}
QString field(const QString &s, const QString &name) {
  return QRegularExpression("\"" + name + "\"\\s+\"([^\"]+)\"")
      .match(s)
      .captured(1);
}
bool under(const QString &path, const QString &directory) {
  return !directory.isEmpty() && path.startsWith(directory + "/");
}
} // namespace

QList<SteamGame> ProcessGames::steamLibrary() {
  const auto home = QDir::homePath();
  QStringList libraries;
  for (const auto &root :
       {home + "/.steam/steam", home + "/.local/share/Steam",
        home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
        home + "/.var/app/com.valvesoftware.Steam/.steam/steam"}) {
    if (!QDir(root + "/steamapps").exists())
      continue;
    libraries.append(QFileInfo(root).canonicalFilePath());
    const auto vdf = text(root + "/steamapps/libraryfolders.vdf");
    auto matches =
        QRegularExpression("\"path\"\\s+\"([^\"]+)\"").globalMatch(vdf);
    while (matches.hasNext())
      libraries.append(matches.next().captured(1).replace("\\\\", "\\"));
  }
  libraries.removeDuplicates();
  QList<SteamGame> result;
  for (const auto &library : libraries) {
    QDir apps(library + "/steamapps");
    for (const auto &manifest :
         apps.entryList({"appmanifest_*.acf"}, QDir::Files)) {
      const auto acf = text(apps.filePath(manifest));
      const uint id = field(acf, "appid").toUInt();
      const auto install = field(acf, "installdir");
      if (id && !install.isEmpty())
        result.append({id, field(acf, "name"),
                       QDir::cleanPath(apps.filePath("common/" + install))});
    }
  }
  return result;
}

bool ProcessGames::excluded(const QString &payload) {
  const auto name = normalized(base(payload));
  return name.isEmpty() || name.endsWith("launcher") ||
         name.endsWith("updater") ||
         prerequisiteInstallerNames().contains(name) ||
         QStringList{"steam",
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

QList<GameInstance> ProcessGames::identify(
    const ProcessCensus::Snapshot &ps, const QList<SteamGame> &steam,
    const QMap<QString, QString> &definitions, const QStringList &blocked,
    const QList<GameInstance> &previous) {
  struct Seed {
    QString key, name, anchor;
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
    if (excluded(payload))
      continue;
    QString key, name;
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
    // Environment is inherited by launchers/services. Require a payload in
    // the matching installed game's directory, not any "steamapps" path.
    for (const auto &game : steam) {
      if ((p.steamId && p.steamId != game.id) ||
          !under(payload, game.directory))
        continue;
      key = QString("steam_app_%1").arg(game.id);
      if (name.isEmpty())
        name = game.name;
      break;
    }
    if (key.isEmpty() || veto(p, key) || blocked.contains(normalized(name)))
      continue;
    if (name.isEmpty())
      name = base(payload);
    auto anchor = p;
    QSet<qint64> visited{p.pid};
    // Steam launch context groups sibling Wine payloads, but never crosses
    // into Steam itself or another App ID. Standalone instances use the
    // matched executable root, not an entire Wine prefix.
    if (p.steamId) {
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
    seeds.insert(p.pid, {key, name, anchor.identity()});
  }
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
    QString id = "game:" + seed.key + "@" + seed.anchor;
    for (const auto &old : previous) {
      const auto prior = old.processes.value(p.pid);
      if (old.key == seed.key && prior.start == p.start &&
          prior.executable == p.executable) {
        id = old.id;
        break;
      }
    }
    auto found = std::find_if(result.begin(), result.end(),
                              [&](const auto &g) { return g.id == id; });
    if (found == result.end()) {
      result.append({id, seed.key, seed.name, p.pid, {}});
      found = std::prev(result.end());
    }
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
          owners.contains(p.pid) || excluded(p.payload()) || veto(p, old.key))
        continue;
      auto found = std::find_if(result.begin(), result.end(),
                                [&](const auto &g) { return g.id == old.id; });
      if (found == result.end()) {
        result.append({old.id, old.key, old.name, p.pid, {}});
        found = std::prev(result.end());
      }
      found->processes.insert(p.pid, p);
      owners[p.pid] = old.id;
    }
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
            (p.steamId && p.steamId != ps.value(p.parent).steamId))
          continue;
        g.processes.insert(p.pid, p);
        owners[p.pid] = owner;
        changed = true;
        break;
      }
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
