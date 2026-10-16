/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "GameIconProvider.h"
#include "DefaultAppCatalog.h"

#include <QDir>
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QProcess>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>

namespace TrophyBackend {
namespace {
QImage readIcon(const QString &path, int extent, QString *foundPath = nullptr) {
  // A miss must not invoke every image-format plugin for every candidate.
  if (!QFileInfo::exists(path))
    return {};
  QImageReader reader(path);
  const QSize size = reader.size();
  // Treat artwork as untrusted input; don't decode giant/non-image files.
  if (!size.isValid() || size.width() > 4096 || size.height() > 4096)
    return {};
  reader.setScaledSize(size.scaled(extent, extent, Qt::KeepAspectRatio));
  QImage image = reader.read();
  if (!image.isNull() && foundPath)
    *foundPath = path;
  return image;
}

QStringList listValue(const QVariant &value) {
  // QSettings parses comma-separated theme lists as QStringList.
  return value.toStringList();
}

QHash<QString, QString> desktopEntry(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 65536)
    return {};
  QHash<QString, QString> values;
  bool inEntry = false;
  for (const auto &raw : file.read(65536).split('\n')) {
    const QString line = QString::fromUtf8(raw).trimmed();
    if (line.startsWith('[')) {
      inEntry = line == "[Desktop Entry]";
      continue;
    }
    if (!inEntry || line.startsWith('#'))
      continue;
    const int equals = line.indexOf('=');
    if (equals > 0)
      values.insert(line.left(equals).trimmed(), line.mid(equals + 1).trimmed());
  }
  // Not QSettings: its INI parser strips quotes from Exec, breaking paths
  // containing spaces. Desktop entries have their own string grammar.
  return values;
}

QImage themedIcon(const QString &name, const QStringList &roots,
                  const QString &theme, int extent, QString *foundPath) {
  if (QDir::isAbsolutePath(name))
    return readIcon(name, extent, foundPath);
  if (name.isEmpty() || name.contains('/') || name == "." || name == "..")
    return {};

  QStringList themes{theme, "hicolor"};
  QStringList visited;
  for (int i = 0; i < themes.size() && visited.size() < 16; ++i) {
    const QString current = themes.at(i);
    if (current.isEmpty() || current.contains('/') || current == "." ||
        current == ".." || visited.contains(current))
      continue;
    visited.append(current);
    for (const QString &root : roots) {
      const QString base = root + '/' + current + '/';
      if (!QDir(base).exists())
        continue;
      QSettings index(base + "index.theme", QSettings::IniFormat);
      index.beginGroup("Icon Theme");
      for (const QString &parent : listValue(index.value("Inherits"))) {
        if (!themes.contains(parent) && themes.size() < 32)
          themes.append(parent);
      }
      QStringList directories;
      // User-installed Steam icons often have no local index.theme.
      for (const char *dir : {"48x48/apps", "64x64/apps", "32x32/apps",
                              "96x96/apps", "128x128/apps", "256x256/apps",
                              "scalable/apps", "24x24/apps", "16x16/apps"})
        directories.append(QString::fromLatin1(dir));
      directories += listValue(index.value("Directories"));
      directories += listValue(index.value("ScaledDirectories"));
      // Prefer compact app icons over large/irrelevant contexts.
      directories.removeDuplicates();
      index.endGroup();
      for (const QString &dir : directories) {
        const auto parts = dir.split('/');
        if (QDir::isAbsolutePath(dir) || parts.contains(".."))
          continue;
        // An executable name can also be an action (notably "zoom").
        // Searching every theme context silently substitutes a toolbar glyph
        // for the application's logo. Respect explicit theme contexts; common
        // apps/ directories also work without index.theme (Steam installs).
        const QString context = index.value(dir + "/Context").toString();
        const bool applicationDirectory =
            parts.contains("apps") || parts.contains("applications");
        if (context.isEmpty() ? !applicationDirectory :
            context.compare("Applications", Qt::CaseInsensitive) != 0)
          continue;
        if (!QDir(base + dir).exists())
          continue;
        for (const QString &suffix : {QString(".png"), QString(".svg"),
                                      QString(".xpm"), QString()}) {
          QImage image = readIcon(base + dir + '/' + name + suffix, extent, foundPath);
          if (!image.isNull())
            return image;
        }
      }
    }
  }
  // Unthemed icons and the traditional pixmaps directory.
  for (const QString &root : roots) {
    for (const QString &suffix : {QString(".png"), QString(".svg"),
                                  QString(".xpm"), QString()}) {
      QImage image = readIcon(root + '/' + name + suffix, extent, foundPath);
      if (!image.isNull())
        return image;
    }
  }
  return {};
}

QString sourceCacheFile() {
  const QString root =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  return root.isEmpty() ? QString() : root + "/game-icons-v1.json";
}
} // namespace

GameIconProvider::GameIconProvider()
    : GameIconProvider(
          QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation),
          QIcon::themeSearchPaths() +
              QStringList{QDir::homePath() + "/.icons", "/usr/share/pixmaps"},
          QIcon::themeName(),
          {QDir::homePath() + "/.steam/steam",
           QDir::homePath() + "/.local/share/Steam",
           QDir::homePath() + "/.var/app/com.valvesoftware.Steam/.steam/steam"},
          sourceCacheFile()) {}

GameIconProvider::GameIconProvider(QStringList dataRoots, QStringList iconRoots,
                                 QString theme, QStringList steamRoots,
                                 QString cacheFile)
    : QQuickImageProvider(QQuickImageProvider::Image,
                          QQmlImageProviderBase::ForceAsynchronousImageLoading),
      m_dataRoots(std::move(dataRoots)), m_iconRoots(std::move(iconRoots)),
      m_theme(std::move(theme)), m_steamRoots(std::move(steamRoots)),
      m_cacheFile(std::move(cacheFile)) {
  for (const QString &root : m_dataRoots) {
    if (!m_iconRoots.contains(root + "/icons"))
      m_iconRoots.append(root + "/icons");
    if (!m_iconRoots.contains(root + "/pixmaps"))
      m_iconRoots.append(root + "/pixmaps");
  }
  // Changes to roots, theme or our built-in alias/artwork catalogue invalidate
  // prior selections. Stable game identities and settings are not modified.
  QJsonArray context{2, m_theme, QJsonArray::fromStringList(m_dataRoots),
                     QJsonArray::fromStringList(m_iconRoots),
                     QJsonArray::fromStringList(m_steamRoots)};
  for (const auto &app : defaultAppCatalog())
    context.append(QJsonArray{app.key, app.artwork,
                             QJsonArray::fromStringList(app.aliases),
                             QJsonArray::fromStringList(app.icons)});
  m_cacheFingerprint = QString::fromLatin1(QCryptographicHash::hash(
      QJsonDocument(context).toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256).toHex());
}

void GameIconProvider::loadSourceCache() {
  if (m_sourceCacheLoaded)
    return;
  m_sourceCacheLoaded = true;
  QFile file(m_cacheFile);
  if (m_cacheFile.isEmpty() || !file.open(QIODevice::ReadOnly) ||
      file.size() > 512 * 1024)
    return;
  const auto root = QJsonDocument::fromJson(file.read(512 * 1024)).object();
  if (root.value("fingerprint").toString() != m_cacheFingerprint)
    return;
  const auto entries = root.value("entries").toObject();
  for (auto it = entries.begin(); it != entries.end() && m_sources.size() < 256; ++it) {
    const auto entry = it.value().toObject();
    const QString path = entry.value("path").toString();
    if (it.key().size() > 16384 || path.size() > 16384 ||
        (!path.isEmpty() && !QDir::isAbsolutePath(path) &&
         !path.startsWith(":/icons/default-apps/")))
      continue;
    m_sources.insert(it.key(), {path,
        entry.value("modified").toString().toLongLong(),
        entry.value("bytes").toString().toLongLong(),
        entry.value("discovered").toString().toLongLong()});
  }
}

void GameIconProvider::saveSourceCache() const {
  if (m_cacheFile.isEmpty())
    return;
  QJsonObject entries;
  for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
    entries.insert(it.key(), QJsonObject{
        {"path", it->path}, {"modified", QString::number(it->modifiedMs)},
        {"bytes", QString::number(it->bytes)},
        {"discovered", QString::number(it->discoveredMs)}});
  if (!QDir().mkpath(QFileInfo(m_cacheFile).absolutePath()))
    return;
  QSaveFile file(m_cacheFile);
  const QByteArray json = QJsonDocument(QJsonObject{
      {"fingerprint", m_cacheFingerprint}, {"entries", entries}})
                              .toJson(QJsonDocument::Compact);
  if (json.size() > 512 * 1024)
    return;
  if (file.open(QIODevice::WriteOnly) && file.write(json) == json.size())
    file.commit(); // best effort: cache failure must never hide an icon
}

QImage GameIconProvider::requestImage(const QString &id, QSize *size,
                                     const QSize &requestedSize) {
  if (id.size() > 16384) {
    if (size)
      *size = {};
    return {};
  }
  const int extent =
      qBound(16, qMax(requestedSize.width(), requestedSize.height()), 256);
  const QString cacheKey = QString::number(extent) + '/' + id;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  // All I/O stays on Qt's image worker. Serialize cache updates as well as
  // discovery so simultaneous size requests cannot race the on-disk snapshot.
  QMutexLocker lock(&m_cacheMutex);
  if (const auto *cached = m_cache.object(cacheKey)) {
    if (now >= cached->loadedMs && now - cached->loadedMs < 60000) {
      if (size)
        *size = cached->image.size();
      return cached->image;
    }
  }
  loadSourceCache();
  QImage image;
  bool resolved = false;
  const auto source = m_sources.constFind(id);
  if (source != m_sources.constEnd()) {
    const qint64 ttl = source->path.isEmpty() ? 15 * 60000 :
        source->path.startsWith(":/") ? 60 * 60000 : 24 * 60 * 60000;
    if (now >= source->discoveredMs && now - source->discoveredMs < ttl) {
      if (source->path.isEmpty()) {
        resolved = true;
      } else {
        const QFileInfo file(source->path);
        if (file.exists() && file.isFile() && file.size() == source->bytes &&
            file.lastModified().toMSecsSinceEpoch() == source->modifiedMs) {
          image = readIcon(source->path, extent);
          resolved = !image.isNull();
        }
      }
    }
  }
  if (!resolved) {
    QString path;
    image = loadImage(id, extent, &path);
    const QFileInfo file(path);
    if (!m_sources.contains(id) && m_sources.size() >= 256)
      m_sources.erase(m_sources.begin());
    m_sources.insert(id, {path, path.isEmpty() ? 0 : file.lastModified().toMSecsSinceEpoch(),
                         path.isEmpty() ? 0 : file.size(), now});
    saveSourceCache();
  }
  if (size)
    *size = image.size();
  m_cache.insert(cacheKey, new ImageEntry{image, now});
  return image;
}

QImage GameIconProvider::loadImage(const QString &id, int extent, QString *foundPath) const {
  const int split = id.indexOf('/');
  if (split < 0 || id.size() > 16384)
    return {};
  bool validAppId = false;
  const uint appId = id.left(split).toUInt(&validAppId);
  if (!validAppId)
    return {};
  const QString key = QString::fromUtf8(QByteArray::fromBase64(
      id.mid(split + 1).toLatin1(), QByteArray::Base64UrlEncoding));
  if (!appId && key.isEmpty())
    return {};
  QImage image;
  if (appId) {
    image = themedIcon(QStringLiteral("steam_icon_%1").arg(appId),
                       m_iconRoots, m_theme, extent, foundPath);
    if (image.isNull()) {
      for (const QString &root : m_steamRoots) {
        image = readIcon(
            root + QString("/appcache/librarycache/%1_icon.jpg").arg(appId), extent, foundPath);
        if (!image.isNull())
          break;
      }
    }
  } else {
    const auto *app = defaultAppInfo(key);
    QStringList desktopKeys{key};
    QStringList iconNames{key};
    if (app) {
      desktopKeys.append(app->key);
      desktopKeys.append(app->aliases);
      iconNames.append(app->key);
      iconNames.append(app->icons);
      iconNames.append(app->aliases);
    }
    // Match desktop IDs, StartupWMClass or exact native executable paths,
    // never fuzzy display names:
    // unrelated games can share a title. Never execute desktop entries.
    for (const QString &root : m_dataRoots) {
      const QDir apps(root + "/applications");
      const auto files = apps.entryInfoList({"*.desktop"}, QDir::Files, QDir::Name);
      for (const QFileInfo &file : files) {
        if (file.size() > 65536)
          continue;
        const auto desktop = desktopEntry(file.absoluteFilePath());
        bool matches = false;
        for (const QString &desktopKey : desktopKeys) {
          if (file.completeBaseName().compare(desktopKey, Qt::CaseInsensitive) == 0 ||
              desktop.value("StartupWMClass").compare(desktopKey, Qt::CaseInsensitive) == 0) {
            matches = true;
            break;
          }
        }
        if (!matches && key.startsWith("exe:")) {
          const auto command =
              QProcess::splitCommand(desktop.value("Exec"));
          if (!command.isEmpty()) {
            QString executable = command.first();
            if (!QDir::isAbsolutePath(executable))
              executable = QStandardPaths::findExecutable(executable);
            QString identity = "exe:" + executable.trimmed().toLower();
            if (identity.endsWith(".exe"))
              identity.chop(4);
            matches = !executable.isEmpty() && identity == key;
          }
        }
        if (!matches)
          continue;
        image = themedIcon(desktop.value("Icon"), m_iconRoots,
                            m_theme, extent, foundPath);
        if (!image.isNull())
          break;
      }
      if (!image.isNull())
        break;
    }
    if (image.isNull()) {
      iconNames.removeDuplicates();
      for (const QString &name : iconNames) {
        image = themedIcon(name, m_iconRoots, m_theme, extent, foundPath);
        if (!image.isNull())
          break;
      }
    }
    if (image.isNull() && app)
      image = readIcon(":/icons/default-apps/" + app->artwork + ".svg", extent, foundPath);
  }
  return image;
}
} // namespace TrophyBackend
