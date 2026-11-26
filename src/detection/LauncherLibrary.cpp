/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "LauncherLibrary.h"
#include "SteamSupportTools.h"
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
#include <QtEndian>
#include <sqlite3.h>
#include <yaml.h>
#include <zlib.h>
#include <algorithm>
#include <array>

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
bool iniConfiguration(const QString &path, const QString &expectedSection) {
  const auto contents = bytes(path);
  if (contents.isEmpty() || contents.contains('\0'))
    return false;
  bool section = false, recognized = false;
  for (const auto &raw : contents.split('\n')) {
    const auto line = raw.trimmed();
    if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
      continue;
    if (line.startsWith('[')) {
      if (!line.endsWith(']') || line.size() < 3)
        return false;
      section = true;
      recognized |= line.mid(1, line.size() - 2) == expectedSection.toUtf8();
    } else if (!section || line.indexOf('=') <= 0) {
      return false;
    }
  }
  return recognized;
}
bool steamConfiguration(const QString &path) {
  const auto contents = QString::fromUtf8(bytes(path));
  // Config VDF is a tree of quoted key/value pairs and braced objects.
  static const QRegularExpression token(
      R"vdf(\s*(?:"((?:\\.|[^"\\])*)"|([{}])|(//[^\n]*)))vdf");
  qsizetype offset = 0;
  int depth = 0;
  bool key = true, root = false;
  while (offset < contents.size()) {
    if (contents.mid(offset).trimmed().isEmpty())
      break;
    const auto match = token.match(contents, offset,
        QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
    if (!match.hasMatch())
      return false;
    offset = match.capturedEnd();
    if (!match.captured(3).isEmpty())
      continue;
    const auto brace = match.captured(2);
    if (brace == "{") {
      if (key || ++depth > 64)
        return false;
      key = true;
    } else if (brace == "}") {
      if (!key || depth-- <= 0)
        return false;
    } else {
      if (depth == 0) {
        if (root || match.captured(1) != "InstallConfigStore")
          return false;
        root = true;
      }
      key = !key;
    }
  }
  return root && key && depth == 0;
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
            const QString &directory = {}, const QString &sourceName = {},
            const QString &productId = {}) {
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
      if (old.name == game.name && old.productId == productId &&
          old.executable == game.executable &&
          old.winePrefix == game.winePrefix &&
          old.bootstrap == game.bootstrap && old.directory == game.directory) {
        duplicate = true;
        old.sources.append(game.sources);
        old.sources.removeDuplicates();
        break;
      }
    if (!duplicate && games.size() < entryLimit) {
      games.append(game);
      games.last().productId = productId;
    }
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
    append(games, game.value("title").toString(), executable, {}, directory, sourceName,
           "epic:" + it.key());
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
        append(games, info.value("name").toString(), executable, {}, directory,
               "Heroic (GOG)", "gog:" + id);
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
      append(games, titles.value(entry.value("id").toString()), executable, {}, directory,
             sourceName, "amazon:" + entry.value("id").toString());
  }
}
QString positiveId(const QJsonValue &value) {
  // itch uses JSON numbers; Heroic's app_name and Steam IDs use strings.
  const auto text = value.isString() ? value.toString()
                                    : value.isDouble() ? QString::number(value.toInteger()) : QString();
  bool ok = false;
  const auto id = text.toLongLong(&ok);
  return ok && id > 0 && QString::number(id) == text &&
      (!value.isDouble() || value.toDouble() == static_cast<double>(id)) ? text : QString();
}
void itchPayload(QList<LauncherGame> &games, const QString &name,
                 const QString &id, const QString &directory,
                 const QString &path, const QString &flavor,
                 const QString &home, const QString &source) {
  if (id.isEmpty() || directory.isEmpty() ||
      games.size() >= entryLimit || (flavor != "linux" && flavor != "windows"))
    return;
  const auto executable = absolute(path, directory, home);
  const auto canonicalDirectory = QFileInfo(directory).canonicalFilePath();
  const auto canonical = QFileInfo(executable).canonicalFilePath();
  if (canonicalDirectory.isEmpty() || canonicalDirectory == "/" ||
      !QFileInfo(executable).isFile() ||
      !executable.startsWith(directory + "/") ||
      !canonical.startsWith(canonicalDirectory + "/"))
    return;
  QFile file(executable);
  if (!file.open(QIODevice::ReadOnly))
    return;
  const auto magic = file.read(4);
  if (flavor == "linux" ? !magic.startsWith("\x7f""ELF") : !magic.startsWith("MZ"))
    return; // No scripts, browser games, shared interpreters or emulators.
  append(games, name, executable, {}, directory, source, "itch:" + id);
}
struct ItchReceiptBudget {
  qint64 compressed = 32 * 1024 * 1024;
  qint64 decoded = 64 * 1024 * 1024;
};
QJsonObject itchReceipt(const QString &path, ItchReceiptBudget &budget) {
  auto compressed = bytes(path, std::min<qint64>(4 * 1024 * 1024, budget.compressed));
  if (compressed.isEmpty() || budget.decoded <= 0)
    return {};
  budget.compressed -= compressed.size();
  const auto limit = std::min<qint64>(16 * 1024 * 1024, budget.decoded);
  z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef *>(compressed.data());
  stream.avail_in = static_cast<uInt>(compressed.size());
  if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK)
    return {};
  QByteArray decoded;
  std::array<char, 64 * 1024> buffer;
  int result = Z_OK;
  do {
    stream.next_out = reinterpret_cast<Bytef *>(buffer.data());
    stream.avail_out = static_cast<uInt>(buffer.size());
    result = inflate(&stream, Z_NO_FLUSH);
    const auto written = static_cast<qint64>(buffer.size() - stream.avail_out);
    budget.decoded = qMax<qint64>(0, budget.decoded - written);
    if (written > limit - decoded.size()) {
      result = Z_MEM_ERROR;
      break;
    }
    decoded.append(buffer.data(), written);
  } while (result == Z_OK);
  const bool complete = result == Z_STREAM_END && stream.avail_in == 0;
  inflateEnd(&stream);
  // CRC/truncation/trailing members must not turn a partial receipt into
  // installation evidence. Limits apply before parsing any JSON.
  return complete ? QJsonDocument::fromJson(decoded).object() : QJsonObject();
}
bool windowsReceiptExecutable(const QString &path) {
  if (!QFileInfo(path).isFile())
    return false;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  std::array<char, 64> dos;
  if (file.read(dos.data(), dos.size()) != static_cast<qint64>(dos.size()) ||
      dos[0] != 'M' || dos[1] != 'Z')
    return false;
  const auto offset = qFromLittleEndian<quint32>(dos.data() + 0x3c);
  if (offset < 64 || offset > 1024 * 1024 || !file.seek(offset))
    return false;
  std::array<char, 94> pe;
  if (file.read(pe.data(), pe.size()) != static_cast<qint64>(pe.size()) ||
      pe[0] != 'P' || pe[1] != 'E' || pe[2] != '\0' || pe[3] != '\0')
    return false;
  const auto optionalSize = qFromLittleEndian<quint16>(pe.data() + 20);
  const auto characteristics = qFromLittleEndian<quint16>(pe.data() + 22);
  const auto magic = qFromLittleEndian<quint16>(pe.data() + 24);
  const auto subsystem = qFromLittleEndian<quint16>(pe.data() + 92);
  return optionalSize >= 70 && file.size() >= offset + 24 + optionalSize &&
      (characteristics & 0x0002) && !(characteristics & 0x2000) &&
      (magic == 0x010b || magic == 0x020b) && (subsystem == 2 || subsystem == 3);
}
void itchReceiptPayloads(QList<LauncherGame> &games, const QString &name,
                         const QString &id, const QString &directory,
                         const QString &home, const QSet<QString> &rejected,
                         QSet<QString> *targets, ItchReceiptBudget &budget) {
  if (id.isEmpty() || directory.isEmpty() || games.size() >= entryLimit)
    return;
  const auto canonicalDirectory = QFileInfo(directory).canonicalFilePath();
  if (canonicalDirectory.isEmpty() || canonicalDirectory == "/")
    return;
  const auto metadata = directory + "/.itch";
  const auto path = metadata + "/receipt.json.gz";
  if (QFileInfo(metadata).isSymLink() || QFileInfo(path).isSymLink())
    return;
  watch(targets, path);
  const auto receipt = itchReceipt(path, budget);
  const auto game = receipt.value("game").toObject();
  const auto classification = game.value("classification").toString();
  const auto files = receipt.value("files").toArray();
  if (positiveId(game.value("id")) != id ||
      (!classification.isEmpty() && classification != "game") || files.size() > 32768)
    return;
  // Butler's verdict is host-filtered: a Windows upload on Linux can have no
  // candidates even though it can run through Proton/Wine. The matching install
  // receipt attests exact files, not arbitrary siblings found by a folder scan.
  for (const auto &value : files) {
    if (games.size() >= entryLimit)
      break;
    const auto relative = value.toString();
    if (!relative.endsWith(".exe", Qt::CaseInsensitive) || QDir::isAbsolutePath(relative))
      continue;
    const auto executable = absolute(relative, directory, home);
    const auto canonical = QFileInfo(executable).canonicalFilePath();
    if (!executable.startsWith(directory + "/") ||
        !canonical.startsWith(canonicalDirectory + "/") ||
        rejected.contains(executable) || rejected.contains(canonical) ||
        ProcessGames::excluded(executable))
      continue;
    const auto base = QFileInfo(executable).completeBaseName().toLower();
    if (QStringList{"setup", "install", "installer", "uninstall", "uninstaller"}.contains(base) ||
        base.startsWith("unins") || !windowsReceiptExecutable(executable))
      continue;
    itchPayload(games, name, id, directory, executable, "windows", home, "itch.io");
  }
}
void heroicItch(QList<LauncherGame> &games, const QString &root,
                const QString &home, QSet<QString> *targets) {
  const auto path = root + "/store_cache/itchio_library.json";
  watch(targets, path);
  const auto entries = json(path).object().value("library").toArray();
  int count = 0;
  for (const auto &value : entries) {
    if (count++ >= entryLimit)
      break;
    const auto game = value.toObject();
    const auto install = game.value("install").toObject();
    const auto app = game.value("app_name").toString();
    const auto classification = game.value("classification").toString();
    if (!game.value("is_installed").toBool() || install.value("is_dlc").toBool() ||
        !app.startsWith("itchio-") ||
        (!classification.isEmpty() && classification != "game"))
      continue;
    itchPayload(games, game.value("title").toString(), positiveId(app.mid(7)),
                absolute(install.value("install_path").toString(), {}, home),
                install.value("executable").toString(),
                install.value("platform").toString(), home, "Heroic (itch.io)");
  }
}
constexpr const char *itchQuery =
    "SELECT g.id, g.title, g.classification, c.custom_install_folder, "
    "l.path, c.install_folder_name, c.verdict FROM caves c "
    "JOIN games g ON g.id = c.game_id "
    "LEFT JOIN install_locations l ON l.id = c.install_location_id "
    "WHERE c.installed_at IS NOT NULL AND COALESCE(c.morphing,0) = 0 LIMIT 4096";
// Only game/install metadata is queried. Never read profiles, tokens or other
// credentials from Butler's database, and never run its daemon or migrations.
bool itch(QList<LauncherGame> *games, const QString &root, const QString &home,
          QSet<QString> *targets) {
  const auto path = root + "/db/butler.db";
  watch(targets, path);
  watch(targets, path + "-wal");
  if (!QFileInfo(path).isFile())
    return false;
  sqlite3 *db = nullptr;
  if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db,
                     SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return false;
  }
  sqlite3_busy_timeout(db, 25);
  sqlite3_limit(db, SQLITE_LIMIT_LENGTH, 4 * 1024 * 1024);
  int budget = 100;
  sqlite3_progress_handler(db, 1000, [](void *value) -> int {
    return --*static_cast<int *>(value) <= 0;
  }, &budget);
  sqlite3_stmt *statement = nullptr;
  const bool readable = sqlite3_prepare_v2(db, itchQuery, -1, &statement, nullptr) == SQLITE_OK;
  ItchReceiptBudget receiptBudget;
  if (readable && games) {
    while (games->size() < entryLimit && sqlite3_step(statement) == SQLITE_ROW) {
      const auto column = [&](int i) {
        const auto size = sqlite3_column_bytes(statement, i);
        return size <= 4 * 1024 * 1024
            ? QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, i)), size)
            : QString();
      };
      const auto classification = column(2);
      if (!classification.isEmpty() && classification != "game")
        continue;
      const auto custom = column(3);
      const auto location = absolute(column(4), {}, home);
      const auto directory = custom.isEmpty()
          ? absolute(column(5), location, home) : absolute(custom, {}, home);
      if (custom.isEmpty() && (location.isEmpty() || !directory.startsWith(location + "/")))
        continue;
      const auto verdict = QJsonDocument::fromJson(column(6).toUtf8()).object();
      // The stored basePath can be stale after moving an installation. Butler
      // resolves candidates against the current install location as well.
      const auto candidates = verdict.value("candidates").toArray();
      QSet<QString> rejected;
      int count = 0;
      for (const auto &value : candidates) {
        if (count++ >= entryLimit)
          break;
        const auto candidate = value.toObject();
        const auto windows = candidate.value("windowsInfo").toObject();
        if (!windows.value("installerType").toString().isEmpty() ||
            windows.value("uninstaller").toBool()) {
          const auto executable = absolute(candidate.value("path").toString(), directory, home);
          rejected.insert(executable);
          rejected.insert(QFileInfo(executable).canonicalFilePath());
          continue;
        }
        itchPayload(*games, column(1), positiveId(column(0)), directory,
                    candidate.value("path").toString(), candidate.value("flavor").toString(),
                    home, "itch.io");
      }
      itchReceiptPayloads(*games, column(1), positiveId(column(0)), directory,
                          home, rejected, targets, receiptBudget);
    }
  }
  sqlite3_finalize(statement);
  sqlite3_close(db);
  return readable;
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
void appendSteam(QList<LauncherGame> &games, uint id, const QString &name,
                 const QString &directory, const QString &source,
                 QSet<QString> *targets) {
  if (!id || name.trimmed().isEmpty() || games.size() >= entryLimit)
    return;
  watch(targets, directory + "/toolmanifest.vdf");
  const auto tool = QString::fromUtf8(bytes(directory + "/toolmanifest.vdf"));
  const auto layer = QRegularExpression("\"compatmanager_layer_name\"\\s+\"([^\"]+)\"")
      .match(tool).captured(1);
  const bool runtime = knownSteamSupportAppId(id) || layer == "container-runtime" ||
      layer == "proton" || layer == "scout-in-container";
  QStringList directories{directory};
  const auto canonical = QFileInfo(directory).canonicalFilePath();
  if (!canonical.isEmpty() && canonical != directory)
    directories.append(canonical);
  for (const auto &path : directories) {
    if (games.size() >= entryLimit)
      break;
    games.append({id, name.trimmed(), path, {}, {}, false, {source}, runtime});
    games.last().productId = "steam:" + QString::number(id);
  }
}
void heroicSteam(QList<LauncherGame> &games, const QString &root,
                 const QString &home, QSet<QString> *targets) {
  const auto installed = root + "/steam_store/installed.json";
  const auto library = root + "/store_cache/steam_library.json";
  watch(targets, installed);
  watch(targets, library);
  QMap<QString, QString> titles;
  const auto cached = json(library).object().value("games").toArray();
  int count = 0;
  for (const auto &value : cached) {
    if (count++ >= entryLimit)
      break;
    const auto game = value.toObject();
    const auto id = positiveId(game.value("app_name"));
    if (!id.isEmpty())
      titles.insert(id, game.value("title").toString());
  }
  const auto entries = json(installed).object().value("installed").toArray();
  count = 0;
  for (const auto &value : entries) {
    if (count++ >= entryLimit)
      break;
    const auto game = value.toObject();
    const auto id = positiveId(game.value("appName"));
    bool ok = false;
    const auto steamId = id.toUInt(&ok);
    const auto platform = game.value("platform").toString();
    const auto directory = absolute(game.value("install_path").toString(), {}, home);
    if (!ok || game.value("is_dlc").toBool() ||
        (platform != "linux" && platform != "windows") ||
        directory == "/" || !QDir(directory).exists())
      continue;
    appendSteam(games, steamId, titles.value(id), directory, "Heroic (Steam)", targets);
  }
}
QStringList installationIdentity(const LauncherGame &game) {
  const auto canonical = [](const QString &path) {
    const auto resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? path : resolved;
  };
  const auto prefix = game.winePrefix.isEmpty() ? QString() : canonical(game.winePrefix);
  if (!game.productId.isEmpty() && !game.directory.isEmpty())
    return {game.productId, canonical(game.directory), prefix};
  // Without a store ID, conflicting launcher names remain ambiguous and need
  // a user rule. Do not erase that safety gate just because paths coincide.
  return {game.productId, game.name, canonical(game.executable), prefix, game.directory.isEmpty()
      ? QString() : canonical(game.directory)};
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
        appendSteam(games, id, field(acf, "name"), directory, "Steam", targets);
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
  const QStringList ids{"steam", "heroic", "legendary", "nile", "lutris", "faugus", "itch"};
  const QStringList names{"Steam", "Heroic", "Legendary", "Nile", "Lutris", "Faugus", "itch.io"};
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
      else if (id == "itch")
        candidates = {{config + "/itch", {}},
                      {home + "/.var/app/io.itch.itch/config/itch", {}}};
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
    QMap<QStringList, QVariantMap> detectedGames;
    for (const auto &candidate : candidates) {
      const auto path = QDir::cleanPath(candidate.first);
      watch(&targets, path);
      if (id == "lutris") {
        watch(&targets, path + "/pga.db-wal");
        watch(&targets, path + "/games");
        if (!candidate.second.isEmpty())
          watch(&targets, candidate.second + "/games");
      }
      if (id == "itch")
        watch(&targets, path + "/db/butler.db-wal");
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
                 "nile_config/nile/installed.json", "steam_store/installed.json",
                 "store_cache/itchio_library.json"};
      else if (id == "itch")
        files = {"db/butler.db"};
      else if (id == "lutris")
        files = {"pga.db"};
      else if (id == "faugus")
        files = {"games.json"};
      else
        files = {"installed.json"};
      QStringList configurations;
      if (id == "steam")
        configurations = {"config/config.vdf"};
      else if (id == "heroic" || id == "faugus")
        configurations = {"config.json"};
      else if (id == "legendary")
        configurations = {"config.ini"};
      else if (id == "nile")
        configurations = {"current_user.json", "library.json"};
      else if (id == "lutris")
        configurations = {"lutris.conf"};
      else if (id == "itch")
        configurations = {"config.json", "preferences.json"};
      bool found = false, readable = false;
      bool invalidLibrary = false;
      for (const auto &file : files) {
        const auto filename = path + "/" + file;
        watch(&targets, filename);
        if (!QFileInfo(filename).isFile())
          continue;
        found = true;
        if (file.endsWith(".json")) {
          const auto document = json(filename);
          const bool valid = id == "faugus"
              ? document.isArray()
              : document.isObject() || document.isArray();
          readable |= valid;
          invalidLibrary |= !valid;
        } else if (file == "db/butler.db") {
          const bool valid = itch(nullptr, path, home, &targets);
          readable |= valid;
          invalidLibrary |= !valid;
        } else if (file == "pga.db") {
          sqlite3 *db = nullptr;
          if (sqlite3_open_v2(QFile::encodeName(filename).constData(), &db,
                              SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 25);
            sqlite3_stmt *statement = nullptr;
            const bool valid = sqlite3_prepare_v2(db,
                "SELECT name, runner, directory, configpath FROM games WHERE installed = 1 LIMIT 0",
                -1, &statement, nullptr) == SQLITE_OK;
            readable |= valid;
            invalidLibrary |= !valid;
            sqlite3_finalize(statement);
          } else
            invalidLibrary = true;
          sqlite3_close(db);
        } else {
          QFile input(filename);
          const bool valid = input.open(QIODevice::ReadOnly) && input.size() > 0;
          readable |= valid;
          invalidLibrary |= !valid;
        }
      }
      bool configured = false, configurationPresent = false;
      if (id == "lutris" && !candidate.second.isEmpty()) {
        const auto filename = path + "/lutris.conf";
        watch(&targets, filename);
        configurationPresent |= QFileInfo(filename).isFile();
        configured |= iniConfiguration(filename, "lutris");
      }
      for (const auto &file : configurations) {
        const auto base = id == "lutris" && !candidate.second.isEmpty()
            ? candidate.second : path;
        const auto filename = base + "/" + file;
        watch(&targets, filename);
        configurationPresent |= QFileInfo(filename).isFile();
        if (file.endsWith(".ini") || file.endsWith(".conf"))
          configured |= iniConfiguration(filename, id == "lutris" ? "lutris" : "Legendary");
        else if (file.endsWith(".vdf"))
          configured |= steamConfiguration(filename);
        else {
          const auto document = json(filename);
          configured |= file == "library.json" ? document.isArray() : document.isObject();
        }
      }
      // Configuration proves presence, never supplies installed-game entries.
      // Keep library errors visible even alongside valid configuration.
      found |= configured;
      readable |= configured;
      // Older Steam layouts may have manifests without libraryfolders.vdf.
      if (id == "steam" && !QDir(path + "/steamapps")
              .entryList({"appmanifest_*.acf"}, QDir::Files).isEmpty()) {
        found = true;
        readable = QDir(path + "/steamapps").isReadable();
      }
      const bool isCustom = std::any_of(custom.begin(), custom.end(), [&](const auto &value) {
        return expanded(value, home) == candidate.first;
      });
      if (!found && !configurationPresent && !isCustom)
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
          heroicSteam(games, path, home, &targets);
          heroicItch(games, path, home, &targets);
        } else if (id == "itch") {
          itch(&games, path, home, &targets);
        } else if (id == "legendary")
          legendary(games, path, home, "Legendary", &targets);
        else if (id == "nile")
          nile(games, path, home, "Nile", &targets);
        else if (id == "faugus")
          faugus(games, path, home, &targets);
        else
          lutris(games, candidate.second.isEmpty() ? path : candidate.second, path, home, &targets);
      }
      QSet<QStringList> entries;
      for (auto &game : games) {
        game.installation = installationIdentity(game);
        const auto key = game.installation;
        if (!game.runtimeTool) {
          entries.insert(key);
          detectedGames.insert(key, QVariantMap{{"name", game.name},
              {"directory", game.directory}, {"executable", game.executable},
              {"installation", game.installation},
              {"source", game.sources.join(", ")}});
        }
        auto old = std::find_if(result.games.begin(), result.games.end(), [&](const auto &item) {
          return item.installation == game.installation && item.executable == game.executable &&
                 item.directory == game.directory &&
                 item.winePrefix == game.winePrefix &&
                 item.bootstrap == game.bootstrap;
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
          {"status", !found && !configurationPresent ? "Not found" : invalidLibrary || !readable
              ? "Unreadable or invalid metadata" : "Detected"}});
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
      "Heroic (GOG)", "Nile", "Heroic (Amazon)", "Heroic (Steam)",
      "Heroic (itch.io)", "Lutris", "Faugus", "itch.io"};
  QMap<QStringList, QStringList> installationSources;
  QMap<QStringList, QString> installationNames;
  for (const auto &game : result.games) {
    installationSources[game.installation].append(game.sources);
    if (!installationNames.contains(game.installation))
      installationNames.insert(game.installation, game.name);
  }
  for (auto &game : result.games) {
    game.sources = installationSources.value(game.installation);
    game.sources.removeDuplicates();
    game.name = installationNames.value(game.installation);
  }
  for (auto &game : result.games)
    std::stable_sort(game.sources.begin(), game.sources.end(), [&](const auto &a, const auto &b) {
      return sourceOrder.indexOf(a) < sourceOrder.indexOf(b);
    });
  result.watchPaths = existingWatchPaths(targets);
  return result;
}
} // namespace TrophyBackend
