/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "LauncherLibrary.h"
#include "ProcessGames.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QRegularExpression>
#include <QStandardPaths>
#include <sqlite3.h>
#include <yaml.h>
#include <algorithm>

namespace TrophyBackend {
namespace {
constexpr int entryLimit = 4096;
constexpr int watchLimit = 8192;
void watch(QSet<QString> *targets, const QString &path) {
  if (targets && targets->size() < watchLimit && QDir::isAbsolutePath(path))
    targets->insert(QDir::cleanPath(path));
}
QStringList existingWatchPaths(const QSet<QString> &targets) {
  QSet<QString> paths;
  for (const auto &target : targets) {
    QFileInfo info(target);
    const bool existed = info.exists();
    while (!info.exists() && info.absoluteFilePath() != "/")
      info.setFile(info.absolutePath());
    const auto resolved = info.canonicalFilePath();
    if (!resolved.isEmpty())
      paths.insert(resolved);
    if (existed) {
      // File watches disappear on atomic replacement; directory watches repair
      // them. Parents also notice removal/recreation of a metadata directory.
      const auto parent = QFileInfo(QFileInfo(target).absolutePath()).canonicalFilePath();
      if (!parent.isEmpty())
        paths.insert(parent);
    }
  }
  auto result = paths.values();
  std::sort(result.begin(), result.end());
  return result.mid(0, watchLimit);
}
QByteArray bytes(const QString &path, qint64 limit = 4 * 1024 * 1024) {
  QFile file(path);
  if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) ||
      file.size() > limit)
    return {};
  const auto result = file.read(limit + 1);
  return result.size() <= limit ? result : QByteArray();
}
QJsonDocument json(const QString &path) {
  return QJsonDocument::fromJson(bytes(path));
}
QString expanded(QString path, const QString &home) {
  if (path.startsWith("~/"))
    path.replace(0, 1, home);
  return path;
}
QString absolute(QString path, const QString &base, const QString &home) {
  path = expanded(path, home);
  path.replace('\\', '/');
  // Do not interpret commands, variables, URLs or Windows drive paths as
  // relative POSIX files. Wine configurations must provide a host path.
  if (path.isEmpty() || path.contains('\0') || path.contains('$') ||
      path.contains("://") || (path.size() > 1 && path[1] == ':'))
    return {};
  if (!QDir::isAbsolutePath(path)) {
    if (!QDir::isAbsolutePath(base))
      return {};
    path = QDir(base).filePath(path);
  }
  return QDir::cleanPath(path);
}
void append(QList<LauncherGame> &games, const QString &name,
            const QString &executable, const QString &prefix = {},
            const QString &directory = {}, const QString &sourceName = {}) {
  const QFileInfo file(executable);
  const bool bootstrap = ProcessGames::bootstrapCandidate(executable);
  if (games.size() >= entryLimit || name.trimmed().isEmpty() ||
      executable.isEmpty() || !file.isFile() ||
      (ProcessGames::excluded(executable) && !bootstrap))
    return;
  // Executable scripts identify an interpreter, not the game it eventually
  // launches. Never expand a script into directory-wide ownership.
  QFile source(executable);
  if (!source.open(QIODevice::ReadOnly))
    return;
  const auto magic = source.read(4);
  if (!magic.startsWith("\x7f""ELF") && !magic.startsWith("MZ"))
    return;
  const auto canonical = file.canonicalFilePath();
  if (canonical.isEmpty() ||
      (ProcessGames::excluded(canonical) &&
       !ProcessGames::bootstrapCandidate(canonical)))
    return;
  const auto canonicalDirectory = QFileInfo(directory).canonicalFilePath();
  // A filename exception needs explicit installation evidence. Do not follow
  // a bootstrap symlink out of the declared installation.
  if (bootstrap &&
      (directory.isEmpty() || canonicalDirectory.isEmpty() ||
       !executable.startsWith(directory + "/") ||
       !canonical.startsWith(canonicalDirectory + "/")))
    return;
  // /proc/exe resolves symlinks, while a Wine payload can retain its
  // configured spelling. Cache both exact paths on this worker.
  QStringList paths{executable, canonical};
  paths.removeDuplicates();
  for (const auto &path : paths) {
    const auto scope = path == canonical ? canonicalDirectory : directory;
    const LauncherGame game{0, name.trimmed(), scope, path, prefix, bootstrap,
                            sourceName.isEmpty() ? QStringList{} : QStringList{sourceName}};
    bool duplicate = false;
    for (auto &old : games)
      if (old.name == game.name && old.executable == game.executable &&
          old.winePrefix == game.winePrefix &&
          old.bootstrap == game.bootstrap && old.directory == game.directory) {
        duplicate = true;
        old.sources.append(game.sources);
        old.sources.removeDuplicates();
        break;
      }
    if (!duplicate && games.size() < entryLimit)
      games.append(game);
  }
}
void legendary(QList<LauncherGame> &games, const QString &root,
                const QString &home, const QString &sourceName,
                QSet<QString> *targets = nullptr) {
  watch(targets, root + "/installed.json");
  const auto entries = json(root + "/installed.json").object();
  int count = 0;
  for (auto it = entries.begin(); it != entries.end() && count++ < entryLimit; ++it) {
    const auto game = it.value().toObject();
    if (game.value("is_dlc").toBool() || game.value("is_preloaded").toBool() ||
        game.value("needs_verification").toBool())
      continue;
    const auto platform = game.value("platform").toString();
    if (!platform.isEmpty() && platform != "Windows" && platform != "Linux")
      continue;
    const auto directory = absolute(game.value("install_path").toString(), {}, home);
    const auto executable = absolute(game.value("executable").toString(), directory, home);
    // Legendary executables are relative to their installation, never to an
    // arbitrary current working directory.
    if (directory.isEmpty() || !executable.startsWith(directory + "/"))
      continue;
    append(games, game.value("title").toString(), executable, {}, directory, sourceName);
  }
}
void faugus(QList<LauncherGame> &games, const QString &root, const QString &home,
            QSet<QString> *targets) {
  watch(targets, root + "/games.json");
  const auto entries = json(root + "/games.json").array();
  int count = 0;
  for (const auto &value : entries) {
    if (count++ >= entryLimit)
      break;
    const auto game = value.toObject();
    if (game.contains("runner") && !game.value("runner").isString())
      continue;
    const auto runner = game.value("runner").toString();
    // Steam entries hold an App ID, not a payload. Additional-app mode runs
    // a generated batch file instead of the configured main path.
    const auto additional = game.value("addapp_enabled");
    if (runner == "Steam" || additional.toBool() || !additional.toString().isEmpty() ||
        additional.isArray() || additional.isObject())
      continue;
    if (QStringList{"amazon", "battle", "ea", "epic", "gog", "rockstar",
                    "ubisoft", "wargaming"}.contains(game.value("gameid").toString()))
      continue;
    const auto executable = absolute(game.value("path").toString(), {}, home);
    if (executable.isEmpty() || !QFileInfo(executable).isFile())
      continue;
    const auto name = QFileInfo(executable).fileName().toLower();
    if (QStringList{"amazon games.exe", "wgc.exe", "rockstargameslauncher.exe"}
            .contains(name))
      continue;
    const bool native = runner == "Linux-Native";
    const auto prefix = absolute(game.value("prefix").toString(), {}, home);
    if (!native && prefix.isEmpty())
      continue;
    QFile payload(executable);
    if (!payload.open(QIODevice::ReadOnly))
      continue;
    const auto magic = payload.read(4);
    if (native ? !magic.startsWith("\x7f""ELF") : !magic.startsWith("MZ"))
      continue;
    // Faugus attests a configured executable, not an installation root. Never
    // turn its parent directory or Wine prefix into bootstrap/sibling scope.
    append(games, game.value("title").toString(), executable,
           native ? QString() : prefix, {}, "Faugus");
  }
}
void gog(QList<LauncherGame> &games, const QString &root, const QString &home,
         QSet<QString> *targets = nullptr) {
  watch(targets, root + "/gog_store/installed.json");
  const auto entries = json(root + "/gog_store/installed.json")
                           .object().value("installed").toArray();
  int count = 0;
  for (const auto &value : entries) {
    if (count++ >= entryLimit)
      break;
    const auto entry = value.toObject();
    if (entry.value("is_dlc").toBool())
      continue;
    const auto id = entry.value("appName").toString();
    bool numeric = false;
    id.toULongLong(&numeric);
    if (!numeric)
      continue;
    const auto directory = absolute(entry.value("install_path").toString(), {}, home);
    if (directory.isEmpty())
      continue;
    watch(targets, directory + "/goggame-" + id + ".info");
    const auto info = json(directory + "/goggame-" + id + ".info").object();
    // Bind the launch task to this installed product, not another game's
    // .info file or an arbitrary executable in the same Wine prefix.
    if (info.value("gameId").toString() != id)
      continue;
    const auto tasks = info.value("playTasks").toArray();
    for (const auto &taskValue : tasks) {
      const auto task = taskValue.toObject();
      if (!task.value("isPrimary").toBool() ||
          task.value("type").toString() != "FileTask")
        continue;
      const auto working = task.value("workingDir").toString();
      const auto base = working.isEmpty() ? directory : absolute(working, directory, home);
      const auto executable = absolute(task.value("path").toString(), base, home);
      if (executable.startsWith(directory + "/"))
        append(games, info.value("name").toString(), executable, {}, directory, "Heroic (GOG)");
    }
  }
}
void nile(QList<LauncherGame> &games, const QString &root, const QString &home,
           const QString &sourceName, QSet<QString> *targets = nullptr) {
  watch(targets, root + "/installed.json");
  watch(targets, root + "/library.json");
  QMap<QString, QString> titles;
  const auto library = json(root + "/library.json").array();
  int libraryCount = 0;
  for (const auto &value : library) {
    if (libraryCount++ >= entryLimit)
      break;
    const auto product = value.toObject().value("product").toObject();
    const auto id = product.value("id").toString();
    if (!id.isEmpty())
      titles.insert(id, product.value("title").toString());
  }
  const auto entries = json(root + "/installed.json").array();
  int count = 0;
  for (const auto &value : entries) {
    if (count++ >= entryLimit)
      break;
    const auto entry = value.toObject();
    const auto directory = absolute(entry.value("path").toString(), {}, home);
    if (directory.isEmpty())
      continue;
    // Nile records the executable in its per-install Fuel metadata.
    watch(targets, directory + "/fuel.json");
    const auto fuel = json(directory + "/fuel.json").object();
    const auto executable = absolute(fuel.value("Main").toObject()
                                         .value("Command").toString(), directory, home);
    if (executable.startsWith(directory + "/"))
      append(games, titles.value(entry.value("id").toString()), executable, {}, directory, sourceName);
  }
}
QString scalar(yaml_document_t &document, int index) {
  const auto *node = yaml_document_get_node(&document, index);
  if (!node || node->type != YAML_SCALAR_NODE)
    return {};
  return QString::fromUtf8(reinterpret_cast<const char *>(node->data.scalar.value),
                           node->data.scalar.length);
}
// Only inspect a mapping's immediate fields; never follow recursive aliases
// or execute YAML tags. Duplicate keys are ambiguous and rejected.
int member(yaml_document_t &document, yaml_node_t *node, const QString &key) {
  if (!node || node->type != YAML_MAPPING_NODE)
    return 0;
  int result = 0;
  for (auto *pair = node->data.mapping.pairs.start;
       pair != node->data.mapping.pairs.top; ++pair) {
    if (scalar(document, pair->key) != key)
      continue;
    if (result)
      return 0;
    result = pair->value;
  }
  return result;
}
void lutris(QList<LauncherGame> &games, const QString &config,
             const QString &data, const QString &home,
             QSet<QString> *targets = nullptr) {
  const auto path = data + "/pga.db";
  watch(targets, path);
  watch(targets, path + "-wal");
  watch(targets, config + "/games");
  watch(targets, data + "/games");
  if (!QFileInfo(path).isFile())
    return; // sqlite3_open must never create a missing library.
  sqlite3 *db = nullptr;
  if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return;
  }
  sqlite3_busy_timeout(db, 25);
  int budget = 100;
  sqlite3_progress_handler(db, 1000, [](void *value) -> int {
    return --*static_cast<int *>(value) <= 0;
  }, &budget);
  sqlite3_stmt *statement = nullptr;
  const char *query = "SELECT name, runner, directory, configpath FROM games "
                      "WHERE installed = 1 LIMIT 4096";
  if (sqlite3_prepare_v2(db, query, -1, &statement, nullptr) == SQLITE_OK) {
    while (sqlite3_step(statement) == SQLITE_ROW) {
      const auto column = [&](int i) {
        return QString::fromUtf8(
            reinterpret_cast<const char *>(sqlite3_column_text(statement, i)));
      };
      const auto runner = column(1);
      if (runner != "linux" && runner != "wine")
        continue; // No inference from shared emulators/interpreters.
      const auto id = column(3);
      if (id.isEmpty() || id.contains('/') || id.contains('\\') ||
          id == "." || id == "..")
        continue;
      auto configRoot = config;
      if (!QDir(configRoot).exists())
        configRoot = data; // Lutris's legacy config location.
      watch(targets, configRoot + "/games/" + id + ".yml");
      const auto input = bytes(configRoot + "/games/" + id + ".yml", 65536);
      if (input.isEmpty())
        continue;
      yaml_parser_t parser;
      if (!yaml_parser_initialize(&parser))
        continue;
      yaml_parser_set_input_string(&parser,
          reinterpret_cast<const unsigned char *>(input.constData()), input.size());
      yaml_document_t document;
      if (yaml_parser_load(&parser, &document)) {
        auto *root = yaml_document_get_root_node(&document);
        auto *game = yaml_document_get_node(&document, member(document, root, "game"));
        const auto exe = scalar(document, member(document, game, "exe"));
        const auto prefixValue = scalar(document, member(document, game, "prefix"));
        const auto directory = absolute(column(2), {}, home);
        const auto executable = absolute(exe, directory, home);
        const auto prefix = absolute(prefixValue, {}, home);
        if (runner == "linux" || !prefix.isEmpty())
          append(games, column(0), executable, runner == "wine" ? prefix : QString(), directory, "Lutris");
        yaml_document_delete(&document);
      }
      yaml_parser_delete(&parser);
    }
  }
  sqlite3_finalize(statement);
  sqlite3_close(db);
}
void steam(QList<LauncherGame> &games, const QStringList &roots,
           QSet<QString> *targets = nullptr) {
  const auto field = [](const QString &text, const QString &key) {
    return QRegularExpression("\"" + key + "\"\\s+\"([^\"]+)\"")
        .match(text).captured(1);
  };
  QStringList libraries;
  for (const auto &root : roots) {
    watch(targets, root + "/steamapps/libraryfolders.vdf");
    if (!QDir(root + "/steamapps").exists())
      continue;
    libraries.append(QFileInfo(root).canonicalFilePath());
    auto matches = QRegularExpression("\"path\"\\s+\"([^\"]+)\"")
        .globalMatch(QString::fromUtf8(bytes(root + "/steamapps/libraryfolders.vdf")));
    while (matches.hasNext() && libraries.size() < entryLimit)
      libraries.append(matches.next().captured(1).replace("\\\\", "\\"));
  }
  libraries.removeDuplicates();
  for (const auto &library : libraries) {
    QDir apps(library + "/steamapps");
    watch(targets, apps.path());
    for (const auto &manifest : apps.entryList({"appmanifest_*.acf"}, QDir::Files)) {
      if (games.size() >= entryLimit)
        return;
      watch(targets, apps.filePath(manifest));
      const auto acf = QString::fromUtf8(bytes(apps.filePath(manifest)));
      const uint id = field(acf, "appid").toUInt();
      const auto install = field(acf, "installdir");
      const auto directory = QDir::cleanPath(apps.filePath("common/" + install));
      if (id && !install.isEmpty() &&
          directory.startsWith(QDir::cleanPath(apps.filePath("common")) + "/")) {
        // A container-runtime tool is installation metadata, not a game.
        // Read its role on the census worker; never infer it from the title.
        watch(targets, directory + "/toolmanifest.vdf");
        const auto tool = QString::fromUtf8(bytes(directory + "/toolmanifest.vdf"));
        const bool runtime = field(tool, "compatmanager_layer_name") == "container-runtime";
        games.append({id, field(acf, "name"), directory, {}, {}, false, {"Steam"}, runtime});
      }
    }
  }
}
} // namespace

QList<LauncherGame> LauncherLibrary::installed() {
  return discover().games;
}
LauncherLibrary::Discovery LauncherLibrary::discover(const QVariantMap &preferences) {
  return discover(QDir::homePath(),
              QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
              QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation),
              preferences, qEnvironmentVariable("LEGENDARY_CONFIG_PATH"));
}
QList<LauncherGame> LauncherLibrary::read(const QString &home, const QString &config,
                                          const QString &data) {
  return discover(home, config, data).games;
}
LauncherLibrary::Discovery LauncherLibrary::discover(
    const QString &home, const QString &config, const QString &data,
    const QVariantMap &preferences, const QString &legendaryConfigPath) {
  Discovery result;
  QSet<QString> targets;
  const QString heroicFlatpak = home + "/.var/app/com.heroicgameslauncher.hgl/config/heroic";
  const QString lutrisFlatpak = home + "/.var/app/net.lutris.Lutris";
  const QStringList ids{"steam", "heroic", "legendary", "nile", "lutris", "faugus"};
  const QStringList names{"Steam", "Heroic", "Legendary", "Nile", "Lutris", "Faugus"};
  for (int i = 0; i < ids.size(); ++i) {
    const auto id = ids[i];
    const auto preference = preferences.value(id).toMap();
    const bool automatic = preference.value("automatic", true).toBool();
    const auto custom = preference.value("paths").toStringList().mid(0, 16);
    // Lutris has separate data and config roots; preserve their pairing.
    QList<QPair<QString, QString>> candidates;
    if (automatic) {
      if (id == "steam")
        for (const auto &path : QStringList{home + "/.steam/steam", data + "/Steam",
             home + "/.local/share/Steam",
             home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
             home + "/.var/app/com.valvesoftware.Steam/.steam/steam"})
          candidates.append({path, {}});
      else if (id == "heroic")
        candidates = {{config + "/heroic", {}}, {heroicFlatpak, {}}};
      else if (id == "legendary")
        candidates = {{QDir::isAbsolutePath(legendaryConfigPath)
                           ? legendaryConfigPath : config + "/legendary", {}}};
      else if (id == "nile")
        candidates = {{config + "/nile", {}}};
      else if (id == "faugus") {
        const auto flatpak = home + "/.var/app/io.github.Faugus.faugus-launcher";
        candidates = {{data + "/faugus-launcher", {}},
                      {config + "/faugus-launcher", {}},
                      {flatpak + "/data/faugus-launcher", {}},
                      {flatpak + "/config/faugus-launcher", {}}};
      }
      else
        candidates = {{data + "/lutris", config + "/lutris"},
                      {lutrisFlatpak + "/data/lutris", lutrisFlatpak + "/config/lutris"}};
    }
    for (const auto &path : custom)
      candidates.append({expanded(path, home), {}});
    QVariantList locations;
    QSet<QString> seen;
    bool detected = false;
    QMap<QString, QVariantMap> detectedGames;
    for (const auto &candidate : candidates) {
      const auto path = QDir::cleanPath(candidate.first);
      watch(&targets, path);
      if (id == "lutris") {
        watch(&targets, path + "/pga.db-wal");
        watch(&targets, path + "/games");
        if (!candidate.second.isEmpty())
          watch(&targets, candidate.second + "/games");
      }
      const auto canonical = QFileInfo(path).canonicalFilePath();
      const auto key = canonical.isEmpty() ? path : canonical;
      if (seen.contains(key))
        continue;
      seen.insert(key);
      QStringList files;
      if (id == "steam")
        files = {"steamapps/libraryfolders.vdf"};
      else if (id == "heroic")
        files = {"legendaryConfig/legendary/installed.json", "gog_store/installed.json",
                 "nile_config/nile/installed.json"};
      else if (id == "lutris")
        files = {"pga.db"};
      else if (id == "faugus")
        files = {"games.json"};
      else
        files = {"installed.json"};
      bool found = false, readable = false;
      for (const auto &file : files) {
        const auto filename = path + "/" + file;
        watch(&targets, filename);
        if (!QFileInfo(filename).isFile())
          continue;
        found = true;
        if (file.endsWith(".json")) {
          const auto document = json(filename);
          readable |= id == "faugus" ? document.isArray()
                                    : document.isObject() || document.isArray();
        } else if (file == "pga.db") {
          sqlite3 *db = nullptr;
          if (sqlite3_open_v2(QFile::encodeName(filename).constData(), &db,
                              SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 25);
            sqlite3_stmt *statement = nullptr;
            readable |= sqlite3_prepare_v2(db,
                "SELECT name, runner, directory, configpath FROM games WHERE installed = 1 LIMIT 0",
                -1, &statement, nullptr) == SQLITE_OK;
            sqlite3_finalize(statement);
          }
          sqlite3_close(db);
        } else {
          QFile input(filename);
          readable |= input.open(QIODevice::ReadOnly) && input.size() > 0;
        }
      }
      // Older Steam layouts may have manifests without libraryfolders.vdf.
      if (id == "steam" && !QDir(path + "/steamapps")
              .entryList({"appmanifest_*.acf"}, QDir::Files).isEmpty()) {
        found = true;
        readable = QDir(path + "/steamapps").isReadable();
      }
      const bool isCustom = std::any_of(custom.begin(), custom.end(), [&](const auto &value) {
        return expanded(value, home) == candidate.first;
      });
      if (!found && !isCustom)
        continue;
      detected |= found;
      QList<LauncherGame> games;
      if (readable) {
        if (id == "steam")
          steam(games, {path}, &targets);
        else if (id == "heroic") {
          legendary(games, path + "/legendaryConfig/legendary", home, "Heroic (Epic)", &targets);
          gog(games, path, home, &targets);
          nile(games, path + "/nile_config/nile", home, "Heroic (Amazon)", &targets);
        } else if (id == "legendary")
          legendary(games, path, home, "Legendary", &targets);
        else if (id == "nile")
          nile(games, path, home, "Nile", &targets);
        else if (id == "faugus")
          faugus(games, path, home, &targets);
        else
          lutris(games, candidate.second.isEmpty() ? path : candidate.second, path, home, &targets);
      }
      QSet<QString> entries;
      for (const auto &game : games) {
        const auto key = game.steamId ? QString::number(game.steamId)
                                     : game.name + "\n" + game.directory;
        if (!game.runtimeTool) {
          entries.insert(key);
          detectedGames.insert(key, QVariantMap{{"name", game.name},
              {"directory", game.directory}, {"executable", game.executable},
              {"source", game.sources.join(", ")}});
        }
        auto old = std::find_if(result.games.begin(), result.games.end(), [&](const auto &item) {
          return item.steamId == game.steamId && item.name == game.name &&
                 item.directory == game.directory && item.executable == game.executable &&
                 item.winePrefix == game.winePrefix;
        });
        if (old == result.games.end()) {
          if (result.games.size() < entryLimit)
            result.games.append(game);
        } else {
          old->sources.append(game.sources);
          old->sources.removeDuplicates();
        }
      }
      locations.append(QVariantMap{{"path", path}, {"count", entries.size()},
          {"status", !found ? "Not found" : !readable ? "Unreadable or invalid metadata" : "Detected"}});
    }
    auto sortedGames = detectedGames.values();
    std::sort(sortedGames.begin(), sortedGames.end(), [](const auto &a, const auto &b) {
      return QString::localeAwareCompare(a.value("name").toString(),
                                        b.value("name").toString()) < 0;
    });
    QVariantList gameList;
    for (const auto &game : sortedGames)
      gameList.append(game);
    result.launchers.append(QVariantMap{{"id", id}, {"name", names[i]},
        {"automatic", automatic}, {"paths", custom}, {"locations", locations},
        {"detected", detected}, {"count", gameList.size()}, {"games", gameList}});
  }
  // Keep recognition provenance ordering stable across discovery/UI ordering.
  const QStringList sourceOrder{"Steam", "Legendary", "Heroic (Epic)",
      "Heroic (GOG)", "Nile", "Heroic (Amazon)", "Lutris", "Faugus"};
  for (auto &game : result.games)
    std::stable_sort(game.sources.begin(), game.sources.end(), [&](const auto &a, const auto &b) {
      return sourceOrder.indexOf(a) < sourceOrder.indexOf(b);
    });
  result.watchPaths = existingWatchPaths(targets);
  return result;
}
} // namespace TrophyBackend
