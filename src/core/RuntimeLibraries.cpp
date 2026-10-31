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
  return roots;
}

} // namespace

LibraryInventory collectLibraryInventory(const QString &mapsPath,
                                         const QStringList &bundleRoots) {
  LibraryInventory inventory;
  inventory.bundleRoots = bundleRoots;

  QFile maps(mapsPath);
  if (!maps.open(QIODevice::ReadOnly))
    return inventory;

  // Read explicitly instead of trusting atEnd()/readAll(): /proc reports a
  // size of 0, which makes Qt's regular-file helpers stop immediately.
  QByteArray contents;
  for (;;) {
    const QByteArray chunk = maps.read(64 * 1024);
    if (chunk.isEmpty())
      break;
    contents += chunk;
  }

  // /proc/self/maps repeats a library once per mapping (text, data, ...), so
  // deduplicate by path before classifying.
  QSet<QString> seen;
  QSet<QString> host;
  QSet<QString> bundle;
  for (const QByteArray &raw : contents.split('\n')) {
    const QString line = QString::fromLocal8Bit(raw);
    const int slash = line.indexOf(QLatin1Char('/'));
    if (slash < 0)
      continue; // anonymous mappings and pseudo entries like [vdso]
    QString path = line.mid(slash).trimmed();
    if (path.endsWith(QStringLiteral(" (deleted)")))
      path.chop(10);
    if (!path.startsWith(QLatin1Char('/')) || seen.contains(path))
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

QString runtimeLibraryReport() {
  return formatLibraryInventory(
      collectLibraryInventory(QStringLiteral("/proc/self/maps"), defaultBundleRoots()));
}

} // namespace TrophyBackend
