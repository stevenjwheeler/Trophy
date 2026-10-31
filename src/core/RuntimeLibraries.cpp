/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RuntimeLibraries.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace TrophyBackend {

namespace {

// Cap each section so a pathological/very large process cannot flood the log.
constexpr int kMaxEntriesPerSection = 400;

// Only real shared objects belong in the inventory: /proc/self/maps also lists
// device mappings, the loader cache, gconv caches and Qt's memfd JIT areas.
bool looksLikeSharedLibrary(const QString &path) {
  const QString name = QFileInfo(path).fileName();
  if (name.endsWith(QStringLiteral(".so")))
    return true;
  const int at = name.lastIndexOf(QStringLiteral(".so."));
  if (at < 0)
    return false;
  for (int i = at + 4; i < name.size(); ++i) {
    const QChar c = name.at(i);
    if (!c.isDigit() && c != QLatin1Char('.'))
      return false;
  }
  return true;
}

// /proc reports a size of 0, so Qt's regular-file helpers (atEnd/readAll)
// stop immediately; read until the kernel returns nothing instead.
QByteArray readMaps(const QString &mapsPath) {
  QFile maps(mapsPath);
  if (!maps.open(QIODevice::ReadOnly))
    return {};
  QByteArray contents;
  for (;;) {
    const QByteArray chunk = maps.read(64 * 1024);
    if (chunk.isEmpty())
      break;
    contents += chunk;
  }
  return contents;
}

// A maps line is "7f00-7f10 r-xp 00000000 08:01 42 /path/to/lib.so"; the path
// follows the inode field, so its first slash starts the absolute path.
QString pathFromMapsLine(const QByteArray &raw) {
  const QString line = QString::fromLocal8Bit(raw);
  const int slash = line.indexOf(QLatin1Char('/'));
  if (slash < 0)
    return {};
  QString path = line.mid(slash).trimmed();
  if (path.endsWith(QStringLiteral(" (deleted)")))
    path.chop(10);
  if (!path.startsWith(QLatin1Char('/')))
    return {};
  return path;
}

QStringList defaultBundleRoots() {
  QStringList roots;
  const QString appDir = QString::fromLocal8Bit(qgetenv("APPDIR"));
  if (!appDir.isEmpty())
    roots << QDir::cleanPath(appDir);
  const QString executableDir = QCoreApplication::applicationDirPath();
  if (!executableDir.isEmpty())
    roots << QDir::cleanPath(executableDir + QStringLiteral("/.."));
  roots.removeDuplicates();
  roots.removeAll(QString());
  return mergeBundleRoots(roots);
}

} // namespace

// A single tree can be named twice (the AppImage mount and its usr/ parent);
// keep only the outermost roots so the log does not repeat itself.
QStringList mergeBundleRoots(const QStringList &roots) {
  QStringList kept;
  for (const QString &root : roots) {
    bool nested = false;
    for (const QString &other : roots) {
      if (other != root && root.startsWith(other + QLatin1Char('/'))) {
        nested = true;
        break;
      }
    }
    if (!nested)
      kept << root;
  }
  return kept;
}

LibraryInventory collectLibraryInventory(const QString &mapsPath,
                                         const QStringList &bundleRoots) {
  LibraryInventory inventory;
  inventory.bundleRoots = bundleRoots;

  const QByteArray contents = readMaps(mapsPath);

  // /proc/self/maps repeats a library once per mapping (text, data, ...), so
  // deduplicate by path before classifying.
  QSet<QString> seen;
  QSet<QString> host;
  QSet<QString> bundle;
  for (const QByteArray &raw : contents.split('\n')) {
    const QString path = pathFromMapsLine(raw);
    if (path.isEmpty() || seen.contains(path))
      continue;
    if (!looksLikeSharedLibrary(path))
      continue;
    seen.insert(path);

    bool isBundle = false;
    for (const QString &root : bundleRoots) {
      // Compare with a trailing separator so /opt/app never claims
      // /opt/application/libfoo.so.
      if (path.startsWith(root + QLatin1Char('/'))) {
        isBundle = true;
        break;
      }
    }
    (isBundle ? bundle : host).insert(path);
  }

  inventory.host = QStringList(host.begin(), host.end());
  inventory.bundle = QStringList(bundle.begin(), bundle.end());
  inventory.host.sort();
  inventory.bundle.sort();
  return inventory;
}

QString formatLibraryInventory(const LibraryInventory &inventory) {
  QStringList lines;
  lines << QStringLiteral("Runtime libraries: %1 from the host, %2 from the bundle")
               .arg(inventory.host.size())
               .arg(inventory.bundle.size());

  const auto appendSection = [&lines](const QString &title, const QStringList &paths) {
    lines << QStringLiteral("  %1 (%2):").arg(title).arg(paths.size());
    const int shown = qMin(paths.size(), kMaxEntriesPerSection);
    for (int i = 0; i < shown; ++i)
      lines << QStringLiteral("    ") + paths.at(i);
    if (shown < paths.size())
      lines << QStringLiteral("    ... %1 more").arg(paths.size() - shown);
  };

  appendSection(QStringLiteral("host"), inventory.host);
  const QString bundleTitle =
      inventory.bundleRoots.isEmpty()
          ? QStringLiteral("bundle")
          : QStringLiteral("bundle under ") + inventory.bundleRoots.join(QStringLiteral(", "));
  appendSection(bundleTitle, inventory.bundle);
  return lines.join(QLatin1Char('\n'));
}

QString mappedLibraryPath(const QString &soname) {
  for (const QByteArray &raw : readMaps(QStringLiteral("/proc/self/maps")).split('\n')) {
    const QString path = pathFromMapsLine(raw);
    if (!path.isEmpty() && QFileInfo(path).fileName().startsWith(soname))
      return path;
  }
  return {};
}

QString libraryOrigin(const QString &path) {
  if (path.isEmpty())
    return QStringLiteral("unknown");
  const QString appDir = QString::fromLocal8Bit(qgetenv("APPDIR"));
  if (!appDir.isEmpty() && path.startsWith(appDir + QLatin1Char('/')))
    return QStringLiteral("bundled");
  return QStringLiteral("host");
}

QString runtimeLibraryReport() {
  return formatLibraryInventory(
      collectLibraryInventory(QStringLiteral("/proc/self/maps"), defaultBundleRoots()));
}

} // namespace TrophyBackend
