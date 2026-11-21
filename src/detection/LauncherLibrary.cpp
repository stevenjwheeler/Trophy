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
#include <QRegularExpression>
#include <QStandardPaths>
#include <sqlite3.h>
#include <yaml.h>

namespace TrophyBackend {
namespace {
constexpr int entryLimit = 4096;
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
                const QString &home, const QString &sourceName) {
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
void gog(QList<LauncherGame> &games, const QString &root, const QString &home) {
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
          const QString &sourceName) {
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
            const QString &data, const QString &home) {
  const auto path = data + "/lutris/pga.db";
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
      auto configRoot = config + "/lutris";
      if (!QDir(configRoot).exists())
        configRoot = data + "/lutris"; // Lutris's legacy config location.
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
void steam(QList<LauncherGame> &games, const QString &home, const QString &data) {
  const auto field = [](const QString &text, const QString &key) {
    return QRegularExpression("\"" + key + "\"\\s+\"([^\"]+)\"")
        .match(text).captured(1);
  };
  QStringList libraries;
  for (const auto &root : {home + "/.steam/steam", data + "/Steam",
       home + "/.local/share/Steam",
       home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
       home + "/.var/app/com.valvesoftware.Steam/.steam/steam"}) {
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
    for (const auto &manifest : apps.entryList({"appmanifest_*.acf"}, QDir::Files)) {
      if (games.size() >= entryLimit)
        return;
      const auto acf = QString::fromUtf8(bytes(apps.filePath(manifest)));
      const uint id = field(acf, "appid").toUInt();
      const auto install = field(acf, "installdir");
      const auto directory = QDir::cleanPath(apps.filePath("common/" + install));
      if (id && !install.isEmpty() &&
          directory.startsWith(QDir::cleanPath(apps.filePath("common")) + "/")) {
        // A container-runtime tool is installation metadata, not a game.
        // Read its role on the census worker; never infer it from the title.
        const auto tool = QString::fromUtf8(bytes(directory + "/toolmanifest.vdf"));
        const bool runtime = field(tool, "compatmanager_layer_name") == "container-runtime";
        games.append({id, field(acf, "name"), directory, {}, {}, false, {"Steam"}, runtime});
      }
    }
  }
}
} // namespace

QList<LauncherGame> LauncherLibrary::installed() {
  return read(QDir::homePath(),
              QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
              QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
}
QList<LauncherGame> LauncherLibrary::read(const QString &home, const QString &config,
                                         const QString &data) {
  QList<LauncherGame> games;
  steam(games, home, data);
  legendary(games, config + "/legendary", home, "Legendary");
  legendary(games, config + "/heroic/legendaryConfig/legendary", home, "Heroic (Epic)");
  gog(games, config + "/heroic", home);
  nile(games, config + "/nile", home, "Nile");
  nile(games, config + "/heroic/nile_config/nile", home, "Heroic (Amazon)");
  lutris(games, config, data, home);
  return games;
}
} // namespace TrophyBackend
